// A minimal VrApi game for testing Refract's VrApi driver (android-runtime/vrapi) without a VrApi game. It
// links Meta's own loader, libvrapi.so (taken from a Quest or a VrApi game, see build_apk.ps1), so the whole
// path a real game takes is exercised: loader -> com.oculus.systemdriver.DriverLoader -> libvrapiimpl.so.
//
// The scene: a ring of cubes at eye height and a floor grid in LOCAL_FLOOR space, plus a cube on each Touch
// controller (red while its trigger is held, which also vibrates it; blue while A/X is held). Logs once a
// second under the tag RefractVrApiSmoke: frame rate, head position, controller state.
#include "../../android-runtime/vrapi/vrapi_abi.h"

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/log.h>
#include <android_native_app_glue.h>

#include <cmath>
#include <cstring>

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "RefractVrApiSmoke", __VA_ARGS__)

extern "C" {
ovrInitializeStatus vrapi_Initialize(const ovrInitParms*);
void vrapi_Shutdown();
int vrapi_GetSystemPropertyInt(const ovrJava*, ovrSystemProperty);
ovrTextureSwapChain* vrapi_CreateTextureSwapChain3(ovrTextureType, int64_t, int, int, int, int);
int vrapi_GetTextureSwapChainLength(ovrTextureSwapChain*);
unsigned int vrapi_GetTextureSwapChainHandle(ovrTextureSwapChain*, int);
ovrMobile* vrapi_EnterVrMode(const ovrModeParms*);
void vrapi_LeaveVrMode(ovrMobile*);
double vrapi_GetPredictedDisplayTime(ovrMobile*, long long);
ovrTracking2 vrapi_GetPredictedTracking2(ovrMobile*, double);
ovrResult vrapi_SubmitFrame2(ovrMobile*, const ovrSubmitFrameDescription2*);
ovrResult vrapi_SetTrackingSpace(ovrMobile*, ovrTrackingSpace);
ovrResult vrapi_PollEvent(ovrEventHeader*);
ovrResult vrapi_EnumerateInputDevices(ovrMobile*, uint32_t, ovrInputCapabilityHeader*);
ovrResult vrapi_GetInputDeviceCapabilities(ovrMobile*, ovrInputCapabilityHeader*);
ovrResult vrapi_GetCurrentInputState(ovrMobile*, ovrDeviceID, ovrInputStateHeader*);
ovrResult vrapi_GetInputTrackingState(ovrMobile*, ovrDeviceID, double, ovrTracking*);
ovrResult vrapi_SetHapticVibrationSimple(ovrMobile*, ovrDeviceID, float);
double vrapi_GetTimeInSeconds();
}

namespace {

ovrMatrix4f multiply(const ovrMatrix4f& a, const ovrMatrix4f& b) {
    ovrMatrix4f out{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            for (int k = 0; k < 4; ++k) out.M[i][j] += a.M[i][k] * b.M[k][j];
    return out;
}

ovrMatrix4f model(const ovrPosef& pose, float scale) {
    const auto& q = pose.Orientation;
    const float ww = q.w * q.w, xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    ovrMatrix4f m{};
    m.M[0][0] = (ww + xx - yy - zz) * scale; m.M[0][1] = 2 * (q.x * q.y - q.w * q.z) * scale; m.M[0][2] = 2 * (q.x * q.z + q.w * q.y) * scale;
    m.M[1][0] = 2 * (q.x * q.y + q.w * q.z) * scale; m.M[1][1] = (ww - xx + yy - zz) * scale; m.M[1][2] = 2 * (q.y * q.z - q.w * q.x) * scale;
    m.M[2][0] = 2 * (q.x * q.z - q.w * q.y) * scale; m.M[2][1] = 2 * (q.y * q.z + q.w * q.x) * scale; m.M[2][2] = (ww - xx - yy + zz) * scale;
    m.M[0][3] = pose.Position.x; m.M[1][3] = pose.Position.y; m.M[2][3] = pose.Position.z; m.M[3][3] = 1;
    return m;
}

// VrApi's TexCoordsFromTanAngles for an eye rendered with `projection`.
ovrMatrix4f tan_angles_from_projection(const ovrMatrix4f& p) {
    ovrMatrix4f t{};
    t.M[0][0] = 0.5f * p.M[0][0]; t.M[0][2] = 0.5f * p.M[0][2] - 0.5f;
    t.M[1][1] = 0.5f * p.M[1][1]; t.M[1][2] = 0.5f * p.M[1][2] - 0.5f;
    t.M[2][2] = -1.0f;
    t.M[3][0] = p.M[2][2]; t.M[3][1] = p.M[2][3]; t.M[3][2] = p.M[3][2]; t.M[3][3] = 1.0f;
    return t;
}

const char* kVertexShader = R"(#version 300 es
uniform mat4 uMvp;
uniform vec3 uColor;
in vec3 aPosition;
out vec3 vColor;
void main() {
    gl_Position = uMvp * vec4(aPosition, 1.0);
    vColor = uColor * (0.65 + 0.35 * clamp(aPosition.y + 0.5, 0.0, 1.0));
})";
const char* kFragmentShader = R"(#version 300 es
precision mediump float;
in vec3 vColor;
out vec4 outColor;
void main() { outColor = vec4(vColor, 1.0); })";

struct Renderer {
    GLuint program = 0, vao = 0, vbo = 0, ibo = 0;
    GLint mvp = -1, color = -1;
    ovrTextureSwapChain* chains[2] = {};
    int length = 0, width = 0, height = 0;
    GLuint framebuffers[2][3] = {}, depth[2] = {};

    bool create(const ovrJava& java) {
        width = vrapi_GetSystemPropertyInt(&java, VRAPI_SYS_PROP_SUGGESTED_EYE_TEXTURE_WIDTH);
        height = vrapi_GetSystemPropertyInt(&java, VRAPI_SYS_PROP_SUGGESTED_EYE_TEXTURE_HEIGHT);
        LOG("suggested eye size %dx%d", width, height);
        const auto compile = [](GLenum type, const char* source) {
            const GLuint shader = glCreateShader(type);
            glShaderSource(shader, 1, &source, nullptr);
            glCompileShader(shader);
            return shader;
        };
        program = glCreateProgram();
        glAttachShader(program, compile(GL_VERTEX_SHADER, kVertexShader));
        glAttachShader(program, compile(GL_FRAGMENT_SHADER, kFragmentShader));
        glBindAttribLocation(program, 0, "aPosition");
        glLinkProgram(program);
        mvp = glGetUniformLocation(program, "uMvp");
        color = glGetUniformLocation(program, "uColor");
        const float vertices[] = {-.5f, -.5f, -.5f, .5f, -.5f, -.5f, .5f, .5f, -.5f, -.5f, .5f, -.5f,
                                  -.5f, -.5f, .5f, .5f, -.5f, .5f, .5f, .5f, .5f, -.5f, .5f, .5f};
        const uint16_t indices[] = {0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4,
                                    3, 6, 2, 3, 7, 6, 0, 4, 7, 0, 7, 3, 1, 2, 6, 1, 6, 5};
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        glGenBuffers(1, &vbo);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
        glGenBuffers(1, &ibo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
        glBindVertexArray(0);
        for (int eye = 0; eye < 2; ++eye) {
            chains[eye] = vrapi_CreateTextureSwapChain3(VRAPI_TEXTURE_TYPE_2D, GL_SRGB8_ALPHA8, width, height, 1, 3);
            if (!chains[eye]) { LOG("swapchain creation failed"); return false; }
            length = vrapi_GetTextureSwapChainLength(chains[eye]);
            glGenRenderbuffers(1, &depth[eye]);
            glBindRenderbuffer(GL_RENDERBUFFER, depth[eye]);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
            for (int i = 0; i < length && i < 3; ++i) {
                glGenFramebuffers(1, &framebuffers[eye][i]);
                glBindFramebuffer(GL_FRAMEBUFFER, framebuffers[eye][i]);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, vrapi_GetTextureSwapChainHandle(chains[eye], i), 0);
                glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth[eye]);
                if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { LOG("incomplete framebuffer"); return false; }
            }
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        LOG("swapchains ready: %d images per eye", length);
        return true;
    }

    void cube(const ovrMatrix4f& viewProjection, const ovrPosef& pose, float scale, float r, float g, float b) {
        const ovrMatrix4f m = multiply(viewProjection, model(pose, scale));
        glUniformMatrix4fv(mvp, 1, GL_TRUE, &m.M[0][0]);  // Row-major.
        glUniform3f(color, r, g, b);
        glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, nullptr);
    }
};

struct Controller {
    ovrDeviceID id = 0;
    bool left = false;
    ovrTracking tracking{};
    ovrInputStateTrackedRemote input{};
};

struct App {
    android_app* android = nullptr;
    ovrJava java{};
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;
    ANativeWindow* window = nullptr;
    bool resumed = false;
    ovrMobile* ovr = nullptr;
    Renderer renderer;
    long long frameIndex = 1;
    Controller controllers[2];
    int controllerCount = 0;
};

void handle_command(android_app* android, int32_t command) {
    auto* app = static_cast<App*>(android->userData);
    switch (command) {
    case APP_CMD_RESUME: app->resumed = true; break;
    case APP_CMD_PAUSE: app->resumed = false; break;
    case APP_CMD_INIT_WINDOW: app->window = android->window; break;
    case APP_CMD_TERM_WINDOW: app->window = nullptr; break;
    default: break;
    }
}

bool create_egl(App& app) {
    app.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(app.display, nullptr, nullptr);
    const EGLint configAttributes[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                       EGL_RENDERABLE_TYPE, 0x40 /* EGL_OPENGL_ES3_BIT_KHR */, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE};
    EGLConfig config = nullptr;
    EGLint count = 0;
    if (!eglChooseConfig(app.display, configAttributes, &config, 1, &count) || !count) { LOG("no EGL config"); return false; }
    const EGLint contextAttributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    app.context = eglCreateContext(app.display, config, EGL_NO_CONTEXT, contextAttributes);
    const EGLint surfaceAttributes[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    app.surface = eglCreatePbufferSurface(app.display, config, surfaceAttributes);
    if (app.context == EGL_NO_CONTEXT || !eglMakeCurrent(app.display, app.surface, app.surface, app.context)) {
        LOG("EGL context failed: 0x%x", eglGetError());
        return false;
    }
    LOG("GL renderer: %s", reinterpret_cast<const char*>(glGetString(GL_RENDERER)));
    return true;
}

void update_vr_mode(App& app) {
    if (app.resumed && app.window && !app.ovr) {
        ovrModeParms parms{};
        parms.Type = VRAPI_STRUCTURE_TYPE_MODE_PARMS;
        parms.Flags = VRAPI_MODE_FLAG_NATIVE_WINDOW | 0x0000FF00;  // ...| VRAPI_MODE_FLAG_RESET_WINDOW_FULLSCREEN
        parms.Java = app.java;
        parms.Display = reinterpret_cast<uintptr_t>(app.display);
        parms.WindowSurface = reinterpret_cast<uintptr_t>(app.window);
        parms.ShareContext = reinterpret_cast<uintptr_t>(app.context);
        app.ovr = vrapi_EnterVrMode(&parms);
        LOG("vrapi_EnterVrMode -> %p", static_cast<void*>(app.ovr));
        if (app.ovr) vrapi_SetTrackingSpace(app.ovr, VRAPI_TRACKING_SPACE_LOCAL_FLOOR);
        app.controllerCount = 0;
        for (uint32_t i = 0; app.ovr && app.controllerCount < 2; ++i) {
            ovrInputCapabilityHeader header{};
            if (vrapi_EnumerateInputDevices(app.ovr, i, &header) < 0) break;
            if (header.Type != ovrControllerType_TrackedRemote) continue;
            ovrInputTrackedRemoteCapabilities caps{};
            caps.Header = header;
            if (vrapi_GetInputDeviceCapabilities(app.ovr, &caps.Header) < 0) continue;
            auto& controller = app.controllers[app.controllerCount++];
            controller.id = header.DeviceID;
            controller.left = (caps.ControllerCapabilities & ovrControllerCaps_LeftHand) != 0;
            LOG("controller id=0x%x %s caps=0x%x buttons=0x%x", header.DeviceID, controller.left ? "left" : "right",
                caps.ControllerCapabilities, caps.ButtonCapabilities);
        }
    } else if ((!app.resumed || !app.window) && app.ovr) {
        vrapi_LeaveVrMode(app.ovr);
        app.ovr = nullptr;
        LOG("vrapi_LeaveVrMode");
    }
}

void render_frame(App& app) {
    const double displayTime = vrapi_GetPredictedDisplayTime(app.ovr, app.frameIndex);
    const ovrTracking2 tracking = vrapi_GetPredictedTracking2(app.ovr, displayTime);
    for (int i = 0; i < app.controllerCount; ++i) {
        auto& controller = app.controllers[i];
        vrapi_GetInputTrackingState(app.ovr, controller.id, displayTime, &controller.tracking);
        controller.input.Header.ControllerType = ovrControllerType_TrackedRemote;
        vrapi_GetCurrentInputState(app.ovr, controller.id, &controller.input.Header);
        vrapi_SetHapticVibrationSimple(app.ovr, controller.id, controller.input.IndexTrigger > 0.5f ? 0.5f : 0.0f);
    }

    auto& r = app.renderer;
    const int image = static_cast<int>(app.frameIndex % r.length);
    glUseProgram(r.program);
    glBindVertexArray(r.vao);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    for (int eye = 0; eye < 2; ++eye) {
        glBindFramebuffer(GL_FRAMEBUFFER, r.framebuffers[eye][image]);
        glViewport(0, 0, r.width, r.height);
        glClearColor(0.05f, 0.07f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        const ovrMatrix4f viewProjection = multiply(tracking.Eye[eye].ProjectionMatrix, tracking.Eye[eye].ViewMatrix);
        for (int i = 0; i < 12; ++i) {  // A ring of cubes at eye height, 2 m away.
            const float angle = i * 3.14159265f / 6.0f;
            const ovrPosef pose{{0, std::sin(angle / 2), 0, std::cos(angle / 2)}, {2.0f * std::sin(angle), 1.5f, -2.0f * std::cos(angle)}};
            r.cube(viewProjection, pose, 0.3f, 0.5f + 0.5f * std::sin(angle), 0.5f + 0.5f * std::cos(angle), 0.6f);
        }
        for (int x = -4; x <= 4; ++x)  // Floor tiles.
            for (int z = -4; z <= 4; ++z) {
                const ovrPosef pose{{0, 0, 0, 1}, {x * 1.0f, -0.05f, z * 1.0f}};
                const float shade = ((x + z) & 1) ? 0.35f : 0.2f;
                r.cube(viewProjection, pose, 0.9f, shade, shade, shade + 0.05f);
            }
        for (int i = 0; i < app.controllerCount; ++i) {
            const auto& controller = app.controllers[i];
            if (!(controller.tracking.Status & VRAPI_TRACKING_STATUS_POSITION_VALID)) continue;
            const bool trigger = controller.input.IndexTrigger > 0.5f;
            const bool primary = (controller.input.Buttons & (ovrButton_A | ovrButton_X)) != 0;
            r.cube(viewProjection, controller.tracking.HeadPose.Pose, 0.06f, trigger ? 1.0f : 0.9f, trigger ? 0.1f : 0.9f, primary ? 1.0f : 0.2f);
        }
        // Discard depth: VrApi games invalidate it before submitting.
        const GLenum attachments[] = {GL_DEPTH_ATTACHMENT};
        glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, attachments);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glFlush();

    ovrLayerProjection2 layer{};
    layer.Header.Type = VRAPI_LAYER_TYPE_PROJECTION2;
    layer.Header.ColorScale = {1, 1, 1, 1};
    layer.Header.SrcBlend = 1;  // VRAPI_FRAME_LAYER_BLEND_ONE
    layer.Header.DstBlend = 0;  // VRAPI_FRAME_LAYER_BLEND_ZERO
    layer.HeadPose = tracking.HeadPose;
    for (int eye = 0; eye < 2; ++eye) {
        layer.Textures[eye].ColorSwapChain = r.chains[eye];
        layer.Textures[eye].SwapChainIndex = image;
        layer.Textures[eye].TexCoordsFromTanAngles = tan_angles_from_projection(tracking.Eye[eye].ProjectionMatrix);
        layer.Textures[eye].TextureRect = {0, 0, 1, 1};
    }
    const ovrLayerHeader2* layers[] = {&layer.Header};
    ovrSubmitFrameDescription2 frame{};
    frame.SwapInterval = 1;
    frame.FrameIndex = static_cast<uint64_t>(app.frameIndex);
    frame.DisplayTime = displayTime;
    frame.LayerCount = 1;
    frame.Layers = layers;
    const ovrResult result = vrapi_SubmitFrame2(app.ovr, &frame);

    static double windowStart = 0;
    static int windowFrames = 0;
    ++windowFrames;
    const double now = vrapi_GetTimeInSeconds();
    if (windowStart == 0) windowStart = now;
    if (now - windowStart >= 1.0) {
        const auto& head = tracking.HeadPose.Pose;
        LOG("frame %lld: %.1f fps submit=%d status=0x%x head=(%.2f %.2f %.2f) q=(%.2f %.2f %.2f %.2f) predict=%.1f ms",
            app.frameIndex, windowFrames / (now - windowStart), result, tracking.Status, head.Position.x, head.Position.y,
            head.Position.z, head.Orientation.x, head.Orientation.y, head.Orientation.z, head.Orientation.w, (displayTime - now) * 1000);
        for (int i = 0; i < app.controllerCount; ++i) {
            const auto& c = app.controllers[i];
            const auto& p = c.tracking.HeadPose.Pose.Position;
            LOG("  %s: status=0x%x pos=(%.2f %.2f %.2f) buttons=0x%x touches=0x%x trigger=%.2f grip=%.2f stick=(%.2f %.2f)",
                c.left ? "left" : "right", c.tracking.Status, p.x, p.y, p.z, c.input.Buttons, c.input.Touches,
                c.input.IndexTrigger, c.input.GripTrigger, c.input.Joystick.x, c.input.Joystick.y);
        }
        windowStart = now;
        windowFrames = 0;
    }
    ++app.frameIndex;
}

}  // namespace

void android_main(android_app* android) {
    App app;
    app.android = android;
    android->userData = &app;
    android->onAppCmd = handle_command;
    JNIEnv* env = nullptr;
    android->activity->vm->AttachCurrentThread(&env, nullptr);
    app.java = {android->activity->vm, env, android->activity->clazz};

    ovrInitParms init{};
    init.Type = VRAPI_STRUCTURE_TYPE_INIT_PARMS;
    init.ProductVersion = 1;
    init.MajorVersion = 1;
    init.MinorVersion = 56;  // Must match the bundled loader (the one in Horizon OS dumps is 1.1.56).
    init.PatchVersion = 0;
    init.GraphicsAPI = static_cast<ovrGraphicsAPI>(VRAPI_GRAPHICS_API_TYPE_OPENGL_ES | 0x0300);
    init.Java = app.java;
    const ovrInitializeStatus status = vrapi_Initialize(&init);
    LOG("vrapi_Initialize -> %d", status);
    if (status != VRAPI_INITIALIZE_SUCCESS || !create_egl(app) || !app.renderer.create(app.java)) {
        LOG("startup failed");
        ANativeActivity_finish(android->activity);
    }
    const bool ready = status == VRAPI_INITIALIZE_SUCCESS && app.renderer.length > 0;

    while (!android->destroyRequested) {
        int events = 0;
        android_poll_source* source = nullptr;
        while (ALooper_pollOnce(app.ovr || !ready ? 0 : -1, nullptr, &events, reinterpret_cast<void**>(&source)) >= 0) {
            if (source) source->process(android, source);
            if (android->destroyRequested) break;
            if (ready) update_vr_mode(app);
        }
        if (ready) update_vr_mode(app);
        struct { ovrEventHeader header; unsigned char data[4000]; } event{};  // ovrEventDataBuffer
        while (vrapi_PollEvent(&event.header) == ovrSuccess) LOG("event %d", event.header.EventType);
        if (app.ovr) render_frame(app);
    }
    if (app.ovr) vrapi_LeaveVrMode(app.ovr);
    if (status == VRAPI_INITIALIZE_SUCCESS) vrapi_Shutdown();
    android->activity->vm->DetachCurrentThread();
}
