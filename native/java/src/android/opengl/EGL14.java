package android.opengl;

/**
 * Java EGL as far as games probe it. Refract Native has no GLES; games render with Vulkan. UE4's GameActivity
 * still makes a pbuffer context in onCreate to read the GL vendor/renderer strings (GLES20 overlay), so every
 * call here succeeds with placeholder objects and nothing is drawn.
 */
public class EGL14 {
    public static EGLContext EGL_NO_CONTEXT = new EGLContext();
    public static EGLDisplay EGL_NO_DISPLAY = new EGLDisplay();
    public static EGLSurface EGL_NO_SURFACE = new EGLSurface();

    private static final int EGL_SUCCESS = 0x3000;
    private static final int EGL_VENDOR = 0x3053;
    private static final int EGL_VERSION = 0x3054;
    private static final int EGL_EXTENSIONS = 0x3055;
    private static final int EGL_CLIENT_APIS = 0x308D;
    private static final int EGL_NONE = 0x3038;

    private static final EGLDisplay DISPLAY = new EGLDisplay();
    private static final EGLConfig CONFIG = new EGLConfig();
    private static EGLContext current = null;
    private static EGLSurface currentDraw = null, currentRead = null;

    public EGL14() {}

    public static int eglGetError() {
        return EGL_SUCCESS;
    }

    public static EGLDisplay eglGetDisplay(int displayId) {
        return DISPLAY;
    }

    public static EGLDisplay eglGetDisplay(long displayId) {
        return DISPLAY;
    }

    public static boolean eglInitialize(EGLDisplay dpy, int[] major, int majorOffset, int[] minor, int minorOffset) {
        if (major != null) major[majorOffset] = 1;
        if (minor != null) minor[minorOffset] = 4;
        return true;
    }

    public static boolean eglTerminate(EGLDisplay dpy) {
        return true;
    }

    public static String eglQueryString(EGLDisplay dpy, int name) {
        switch (name) {
            case EGL_VENDOR: return "Qualcomm";
            case EGL_VERSION: return "1.4";
            case EGL_CLIENT_APIS: return "OpenGL_ES";
            case EGL_EXTENSIONS: return "";
            default: return null;
        }
    }

    public static boolean eglGetConfigs(EGLDisplay dpy, EGLConfig[] configs, int configsOffset, int configSize,
            int[] numConfig, int numConfigOffset) {
        return eglChooseConfig(dpy, null, 0, configs, configsOffset, configSize, numConfig, numConfigOffset);
    }

    public static boolean eglChooseConfig(EGLDisplay dpy, int[] attribList, int attribListOffset, EGLConfig[] configs,
            int configsOffset, int configSize, int[] numConfig, int numConfigOffset) {
        int n = configs != null && configSize > 0 ? 1 : 0;
        if (n > 0) configs[configsOffset] = CONFIG;
        if (numConfig != null) numConfig[numConfigOffset] = configs == null ? 1 : n;
        return true;
    }

    public static boolean eglGetConfigAttrib(EGLDisplay dpy, EGLConfig config, int attribute, int[] value, int offset) {
        int v;
        switch (attribute) {
            case 0x3020: v = 32; break;                       // EGL_BUFFER_SIZE
            case 0x3021: case 0x3022: case 0x3023: case 0x3024: v = 8; break; // ALPHA/BLUE/GREEN/RED
            case 0x3025: v = 24; break;                       // EGL_DEPTH_SIZE
            case 0x3026: v = 8; break;                        // EGL_STENCIL_SIZE
            case 0x3033: v = 0x5; break;                      // EGL_SURFACE_TYPE: PBUFFER | WINDOW
            case 0x3040: v = 0x44; break;                     // EGL_RENDERABLE_TYPE: ES2 | ES3
            default: v = 0;
        }
        value[offset] = v;
        return true;
    }

    public static EGLSurface eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config, Object win, int[] attribList, int offset) {
        return new EGLSurface();
    }

    public static EGLSurface eglCreatePbufferSurface(EGLDisplay dpy, EGLConfig config, int[] attribList, int offset) {
        return new EGLSurface();
    }

    public static boolean eglDestroySurface(EGLDisplay dpy, EGLSurface surface) {
        return true;
    }

    public static boolean eglBindAPI(int api) {
        return true;
    }

    public static int eglQueryAPI() {
        return 0x30A0; // EGL_OPENGL_ES_API
    }

    public static EGLContext eglCreateContext(EGLDisplay dpy, EGLConfig config, EGLContext shareContext, int[] attribList, int offset) {
        return new EGLContext();
    }

    public static boolean eglDestroyContext(EGLDisplay dpy, EGLContext ctx) {
        if (current == ctx) current = null;
        return true;
    }

    public static boolean eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx) {
        current = ctx == EGL_NO_CONTEXT ? null : ctx;
        currentDraw = draw;
        currentRead = read;
        return true;
    }

    public static EGLContext eglGetCurrentContext() {
        return current != null ? current : EGL_NO_CONTEXT;
    }

    public static EGLSurface eglGetCurrentSurface(int readdraw) {
        EGLSurface s = readdraw == 0x305A ? currentRead : currentDraw; // EGL_READ
        return current != null && s != null ? s : EGL_NO_SURFACE;
    }

    public static EGLDisplay eglGetCurrentDisplay() {
        return current != null ? DISPLAY : EGL_NO_DISPLAY;
    }

    public static boolean eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) {
        return true;
    }

    public static boolean eglSwapInterval(EGLDisplay dpy, int interval) {
        return true;
    }

    public static boolean eglReleaseThread() {
        current = null;
        return true;
    }

    public static boolean eglWaitGL() {
        return true;
    }

    public static boolean eglWaitNative(int engine) {
        return true;
    }
}
