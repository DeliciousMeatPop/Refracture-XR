// The binary interface of Meta's VrApi (Oculus Mobile SDK 1.x), as far as Refract implements it: the
// structures games pass to libvrapi.so, which forwards every call to the driver's libvrapiimpl.so. Layouts
// match the 64-bit Android ABI (sizes are checked below); names follow the SDK so the code reads like the
// games that call it. Only what vrapi_impl.cpp needs is declared.
#pragma once

#include <jni.h>
#include <stdint.h>

extern "C" {

using ovrResult = int32_t;
enum : ovrResult {
    ovrSuccess = 0,
    ovrSuccess_BoundaryInvalid = 1001,
    ovrSuccess_EventUnavailable = 1002,
    ovrError_NotInitialized = -1004,
    ovrError_InvalidParameter = -1005,
    ovrError_InvalidOperation = -1015,
    ovrError_NoDevice = -1051,
    ovrError_NotImplemented = -1052,
    ovrError_Unavailable = -1054,
};

struct ovrJava {
    JavaVM* Vm;
    JNIEnv* Env;
    jobject ActivityObject;
};

struct ovrVector2f { float x, y; };
struct ovrVector3f { float x, y, z; };
struct ovrVector4f { float x, y, z, w; };
struct ovrQuatf { float x, y, z, w; };
struct ovrMatrix4f { float M[4][4]; };  // Row-major, column vectors (translation in M[i][3]).
struct ovrPosef {
    ovrQuatf Orientation;
    ovrVector3f Position;
};
struct ovrRectf { float x, y, width, height; };

enum ovrStructureType : int32_t {
    VRAPI_STRUCTURE_TYPE_INIT_PARMS = 1,
    VRAPI_STRUCTURE_TYPE_MODE_PARMS = 2,
    VRAPI_STRUCTURE_TYPE_FRAME_PARMS = 3,
    VRAPI_STRUCTURE_TYPE_MODE_PARMS_VULKAN = 5,
};

enum : int32_t {
    VRAPI_DEVICE_TYPE_OCULUSQUEST2 = 320,
};

enum ovrSystemProperty : int32_t {
    VRAPI_SYS_PROP_DEVICE_TYPE = 0,
    VRAPI_SYS_PROP_MAX_FULLSPEED_FRAMEBUFFER_SAMPLES = 1,
    VRAPI_SYS_PROP_DISPLAY_PIXELS_WIDE = 2,
    VRAPI_SYS_PROP_DISPLAY_PIXELS_HIGH = 3,
    VRAPI_SYS_PROP_DISPLAY_REFRESH_RATE = 4,
    VRAPI_SYS_PROP_SUGGESTED_EYE_TEXTURE_WIDTH = 5,
    VRAPI_SYS_PROP_SUGGESTED_EYE_TEXTURE_HEIGHT = 6,
    VRAPI_SYS_PROP_SUGGESTED_EYE_FOV_DEGREES_X = 7,
    VRAPI_SYS_PROP_SUGGESTED_EYE_FOV_DEGREES_Y = 8,
    VRAPI_SYS_PROP_DEVICE_REGION = 10,
    VRAPI_SYS_PROP_DOMINANT_HAND = 15,
    VRAPI_SYS_PROP_HAS_ORIENTATION_TRACKING = 16,
    VRAPI_SYS_PROP_HAS_POSITION_TRACKING = 17,
    VRAPI_SYS_PROP_NUM_SUPPORTED_DISPLAY_REFRESH_RATES = 64,
    VRAPI_SYS_PROP_SUPPORTED_DISPLAY_REFRESH_RATES = 65,
    VRAPI_SYS_PROP_NUM_SUPPORTED_SWAPCHAIN_FORMATS = 66,
    VRAPI_SYS_PROP_SUPPORTED_SWAPCHAIN_FORMATS = 67,
    VRAPI_SYS_PROP_MULTIVIEW_AVAILABLE = 128,  // Dropped from later SDK headers (always true); old games ask.
    VRAPI_SYS_PROP_SRGB_LAYER_SOURCE_AVAILABLE = 129,
    VRAPI_SYS_PROP_FOVEATION_AVAILABLE = 130,
};

enum ovrProperty : int32_t {
    VRAPI_FOVEATION_LEVEL = 15,
    VRAPI_EAT_NATIVE_GAMEPAD_EVENTS = 20,
    VRAPI_ACTIVE_INPUT_DEVICE_ID = 24,
    VRAPI_DEVICE_EMULATION_MODE = 29,
    VRAPI_DYNAMIC_FOVEATION_ENABLED = 30,
};

enum ovrSystemStatus : int32_t {
    VRAPI_SYS_STATUS_MOUNTED = 1,
    VRAPI_SYS_STATUS_THROTTLED = 2,
    VRAPI_SYS_STATUS_RENDER_LATENCY_MILLISECONDS = 5,
    VRAPI_SYS_STATUS_TIMEWARP_LATENCY_MILLISECONDS = 6,
    VRAPI_SYS_STATUS_SCANOUT_LATENCY_MILLISECONDS = 7,
    VRAPI_SYS_STATUS_APP_FRAMES_PER_SECOND = 8,
    VRAPI_SYS_STATUS_RECENTER_COUNT = 13,
    VRAPI_SYS_STATUS_USER_RECENTER_COUNT = 15,
    VRAPI_SYS_STATUS_FRONT_BUFFER_SRGB = 130,
};

enum ovrInitializeStatus : int32_t {
    VRAPI_INITIALIZE_SUCCESS = 0,
    VRAPI_INITIALIZE_UNKNOWN_ERROR = -1,
};

enum ovrGraphicsAPI : int32_t {
    VRAPI_GRAPHICS_API_TYPE_OPENGL_ES = 0x10000,
    VRAPI_GRAPHICS_API_TYPE_VULKAN = 0x40000,
};
constexpr int32_t kVrApiGraphicsTypeMask = 0xFFFF0000;

// ovrFrameLayerBlend values used to tell an opaque layer (ONE, ZERO) from a blended one.
constexpr int32_t kVrApiBlendZero = 0;
constexpr int32_t kVrApiBlendOne = 1;

struct ovrInitParms {
    ovrStructureType Type;
    int ProductVersion;
    int MajorVersion;
    int MinorVersion;
    int PatchVersion;
    ovrGraphicsAPI GraphicsAPI;
    ovrJava Java;
};

enum : uint32_t {
    VRAPI_MODE_FLAG_NATIVE_WINDOW = 0x00010000,
    VRAPI_MODE_FLAG_FRONT_BUFFER_SRGB = 0x00080000,
};

struct ovrModeParms {
    ovrStructureType Type;
    uint32_t Flags;
    ovrJava Java;
    unsigned long long Display;        // EGLDisplay
    unsigned long long WindowSurface;  // ANativeWindow* or EGLSurface
    unsigned long long ShareContext;   // EGLContext
};

struct ovrModeParmsVulkan {
    ovrModeParms ModeParms;
    unsigned long long SynchronizationQueue;  // VkQueue
};

struct ovrMobile;
struct ovrTextureSwapChain;

struct ovrRigidBodyPosef {
    ovrPosef Pose;
    ovrVector3f AngularVelocity;
    ovrVector3f LinearVelocity;
    ovrVector3f AngularAcceleration;
    ovrVector3f LinearAcceleration;
    uint32_t Padding;
    double TimeInSeconds;
    double PredictionInSeconds;
};

enum : uint32_t {
    VRAPI_TRACKING_STATUS_ORIENTATION_TRACKED = 1 << 0,
    VRAPI_TRACKING_STATUS_POSITION_TRACKED = 1 << 1,
    VRAPI_TRACKING_STATUS_ORIENTATION_VALID = 1 << 2,
    VRAPI_TRACKING_STATUS_POSITION_VALID = 1 << 3,
    VRAPI_TRACKING_STATUS_HMD_CONNECTED = 1 << 7,
};

struct ovrTracking2 {
    uint32_t Status;
    uint32_t Padding;
    ovrRigidBodyPosef HeadPose;
    struct {
        ovrMatrix4f ProjectionMatrix;
        ovrMatrix4f ViewMatrix;
    } Eye[2];
};

struct ovrTracking {
    uint32_t Status;
    uint32_t Padding;
    ovrRigidBodyPosef HeadPose;
};

enum ovrTrackingTransform : int32_t {
    VRAPI_TRACKING_TRANSFORM_IDENTITY = 0,
    VRAPI_TRACKING_TRANSFORM_CURRENT = 1,
    VRAPI_TRACKING_TRANSFORM_SYSTEM_CENTER_EYE_LEVEL = 2,
    VRAPI_TRACKING_TRANSFORM_SYSTEM_CENTER_FLOOR_LEVEL = 3,
};

enum ovrTrackingSpace : int32_t {
    VRAPI_TRACKING_SPACE_LOCAL = 0,
    VRAPI_TRACKING_SPACE_LOCAL_FLOOR = 1,
    VRAPI_TRACKING_SPACE_LOCAL_TILTED = 2,
    VRAPI_TRACKING_SPACE_STAGE = 3,
    VRAPI_TRACKING_SPACE_LOCAL_FIXED_YAW = 7,
};

enum ovrTrackedDeviceTypeId : int32_t {};

struct ovrBoundaryTriggerResult {
    ovrVector3f ClosestPoint;
    ovrVector3f ClosestPointNormal;
    float ClosestDistance;
    bool IsTriggering;
};

enum ovrTextureType : int32_t {
    VRAPI_TEXTURE_TYPE_2D = 0,
    VRAPI_TEXTURE_TYPE_2D_ARRAY = 2,
    VRAPI_TEXTURE_TYPE_CUBE = 3,
};

// The pre-CreateTextureSwapChain3 format enum.
enum ovrTextureFormat : int32_t {
    VRAPI_TEXTURE_FORMAT_NONE = 0,
    VRAPI_TEXTURE_FORMAT_565 = 1,
    VRAPI_TEXTURE_FORMAT_5551 = 2,
    VRAPI_TEXTURE_FORMAT_4444 = 3,
    VRAPI_TEXTURE_FORMAT_8888 = 4,
    VRAPI_TEXTURE_FORMAT_8888_sRGB = 5,
    VRAPI_TEXTURE_FORMAT_RGBA16F = 6,
    VRAPI_TEXTURE_FORMAT_DEPTH_16 = 7,
    VRAPI_TEXTURE_FORMAT_DEPTH_24 = 8,
    VRAPI_TEXTURE_FORMAT_DEPTH_24_STENCIL_8 = 9,
    VRAPI_TEXTURE_FORMAT_RG16 = 10,
};

// Placeholder swapchain "pointers" games put in layers for the runtime's built-in images.
constexpr uintptr_t VRAPI_DEFAULT_TEXTURE_SWAPCHAIN = 0x1;
constexpr uintptr_t VRAPI_DEFAULT_TEXTURE_SWAPCHAIN_LOADING_ICON = 0x2;

struct ovrTextureSamplerState {
    int32_t MinFilter;
    int32_t MagFilter;
    int32_t WrapModeS;
    int32_t WrapModeT;
    float BorderColor[4];
    float MaxAnisotropy;
};

struct ovrSwapChainCreateInfo {
    int64_t Format;
    int Width;
    int Height;
    int Levels;
    int FaceCount;
    int ArraySize;
    int BufferCount;
    uint64_t CreateFlags;
    uint64_t UsageFlags;
};

enum : uint32_t {
    VRAPI_FRAME_FLAG_FLUSH = 1 << 1,
    VRAPI_FRAME_FLAG_FINAL = 1 << 2,
};

enum : uint32_t {
    VRAPI_FRAME_LAYER_FLAG_FIXED_TO_VIEW = 1 << 2,
};

// Legacy vrapi_SubmitFrame structures.
struct ovrFrameLayerTexture {
    ovrTextureSwapChain* ColorTextureSwapChain;
    ovrTextureSwapChain* DepthTextureSwapChain;
    int TextureSwapChainIndex;
    ovrMatrix4f TexCoordsFromTanAngles;
    ovrRectf TextureRect;
    uint32_t Padding;
    ovrRigidBodyPosef HeadPose;
    unsigned char Pad[8];
};

struct ovrFrameLayer {
    ovrFrameLayerTexture Textures[2];
    float SpinSpeed;
    float SpinScale;
    float ColorScale;
    uint32_t Padding;
    int32_t SrcBlend;
    int32_t DstBlend;
    int Flags;
};

struct ovrPerformanceParms {
    int CpuLevel;
    int GpuLevel;
    int MainThreadTid;
    int RenderThreadTid;
};

constexpr int kVrApiLegacyMaxLayers = 4;
struct ovrFrameParms {
    ovrStructureType Type;
    uint32_t Padding;
    ovrFrameLayer Layers[kVrApiLegacyMaxLayers];
    int LayerCount;
    int Flags;
    long long FrameIndex;
    int SwapInterval;
    int32_t ExtraLatencyMode;
    ovrMatrix4f Reserved;
    void* Reserved1;
    ovrPerformanceParms PerformanceParms;
    ovrJava Java;
};

// vrapi_SubmitFrame2 layers.
enum ovrLayerType2 : int32_t {
    VRAPI_LAYER_TYPE_PROJECTION2 = 1,
    VRAPI_LAYER_TYPE_CYLINDER2 = 3,
    VRAPI_LAYER_TYPE_CUBE2 = 4,
    VRAPI_LAYER_TYPE_EQUIRECT2 = 5,
    VRAPI_LAYER_TYPE_LOADING_ICON2 = 6,
    VRAPI_LAYER_TYPE_FISHEYE2 = 7,
    VRAPI_LAYER_TYPE_EQUIRECT3 = 10,
};

struct ovrLayerHeader2 {
    ovrLayerType2 Type;
    uint32_t Flags;
    ovrVector4f ColorScale;
    int32_t SrcBlend;
    int32_t DstBlend;
    void* Reserved;
};

struct ovrLayerProjection2 {
    ovrLayerHeader2 Header;
    ovrRigidBodyPosef HeadPose;
    struct {
        ovrTextureSwapChain* ColorSwapChain;
        int SwapChainIndex;
        ovrMatrix4f TexCoordsFromTanAngles;
        ovrRectf TextureRect;
    } Textures[2];
};

struct ovrLayerCylinder2 {
    ovrLayerHeader2 Header;
    ovrRigidBodyPosef HeadPose;
    struct {
        ovrTextureSwapChain* ColorSwapChain;
        int SwapChainIndex;
        ovrMatrix4f TexCoordsFromTanAngles;  // inverse(eye view * cylinder transform)
        ovrRectf TextureRect;
        ovrMatrix4f TextureMatrix;           // Scale and bias from the 180-degree unit hemicylinder to the image.
    } Textures[2];
};

struct ovrSubmitFrameDescription2 {
    uint32_t Flags;
    uint32_t SwapInterval;
    uint64_t FrameIndex;
    double DisplayTime;
    unsigned char Pad[8];
    uint32_t LayerCount;
    const ovrLayerHeader2* const* Layers;
};

enum ovrPerfThreadType : int32_t {};
enum ovrExtraLatencyMode : int32_t {};

enum ovrColorSpace : int32_t {
    VRAPI_COLORSPACE_REC_2020 = 1,
};
struct ovrHmdColorDesc {
    ovrColorSpace ColorSpace;
    uint32_t Padding;
};

enum ovrEventType : int32_t {
    VRAPI_EVENT_NONE = 0,
    VRAPI_EVENT_DATA_LOST = 1,
    VRAPI_EVENT_VISIBILITY_GAINED = 2,
    VRAPI_EVENT_VISIBILITY_LOST = 3,
    VRAPI_EVENT_FOCUS_GAINED = 4,
    VRAPI_EVENT_FOCUS_LOST = 5,
    VRAPI_EVENT_DISPLAY_REFRESH_RATE_CHANGE = 11,
};
struct ovrEventHeader {
    ovrEventType EventType;
};

enum ovrSystemUIType : int32_t {};

// Input.
enum : uint32_t {
    ovrButton_A = 0x00000001,
    ovrButton_B = 0x00000002,
    ovrButton_RThumb = 0x00000004,
    ovrButton_X = 0x00000100,
    ovrButton_Y = 0x00000200,
    ovrButton_LThumb = 0x00000400,
    ovrButton_Enter = 0x00100000,
    ovrButton_Back = 0x00200000,
    ovrButton_GripTrigger = 0x04000000,
    ovrButton_Trigger = 0x20000000,
    ovrButton_Joystick = 0x80000000,
};

enum : uint32_t {
    ovrTouch_A = 0x00000001,
    ovrTouch_B = 0x00000002,
    ovrTouch_X = 0x00000004,
    ovrTouch_Y = 0x00000008,
    ovrTouch_Joystick = 0x00000020,
    ovrTouch_IndexTrigger = 0x00000040,
    ovrTouch_ThumbUp = 0x00000100,
    ovrTouch_IndexPointing = 0x00000200,
    ovrTouch_LThumb = 0x00000400,
    ovrTouch_RThumb = 0x00000800,
    ovrTouch_ThumbRest = 0x00001000,
    ovrTouch_LThumbRest = 0x00002000,
    ovrTouch_RThumbRest = 0x00004000,
};

enum ovrControllerType : int32_t {
    ovrControllerType_None = 0,
    ovrControllerType_TrackedRemote = 1 << 2,
    ovrControllerType_Gamepad = 1 << 4,
    ovrControllerType_Hand = 1 << 5,
    ovrControllerType_StandardPointer = 1 << 7,
};

using ovrDeviceID = uint32_t;

struct ovrInputCapabilityHeader {
    ovrControllerType Type;
    ovrDeviceID DeviceID;
};

enum : uint32_t {
    ovrControllerCaps_HasOrientationTracking = 0x00000001,
    ovrControllerCaps_HasPositionTracking = 0x00000002,
    ovrControllerCaps_LeftHand = 0x00000004,
    ovrControllerCaps_RightHand = 0x00000008,
    ovrControllerCaps_HasAnalogIndexTrigger = 0x00000040,
    ovrControllerCaps_HasAnalogGripTrigger = 0x00000080,
    ovrControllerCaps_HasSimpleHapticVibration = 0x00000200,
    ovrControllerCaps_HasJoystick = 0x00002000,
    ovrControllerCaps_ModelOculusTouch = 0x00004000,
};

struct ovrInputTrackedRemoteCapabilities {
    ovrInputCapabilityHeader Header;
    uint32_t ControllerCapabilities;
    uint32_t ButtonCapabilities;
    uint16_t TrackpadMaxX;
    uint16_t TrackpadMaxY;
    float TrackpadSizeX;
    float TrackpadSizeY;
    uint32_t HapticSamplesMax;
    uint32_t HapticSampleDurationMS;
    uint32_t TouchCapabilities;
    uint32_t Reserved4;
    uint32_t Reserved5;
};

struct ovrHapticBuffer {
    double BufferTime;
    uint32_t NumSamples;
    bool Terminated;
    uint8_t* HapticBuffer;
};

struct ovrInputStateHeader {
    ovrControllerType ControllerType;
    double TimeInSeconds;
};

struct ovrInputStateTrackedRemote {
    ovrInputStateHeader Header;
    uint32_t Buttons;
    uint32_t TrackpadStatus;
    ovrVector2f TrackpadPosition;
    uint8_t BatteryPercentRemaining;
    uint8_t RecenterCount;
    uint16_t Reserved;
    float IndexTrigger;
    float GripTrigger;
    uint32_t Touches;
    uint32_t Reserved5a;
    ovrVector2f Joystick;
    ovrVector2f JoystickNoDeadZone;
};

enum ovrHandedness : int32_t {
    VRAPI_HAND_UNKNOWN = 0,
    VRAPI_HAND_LEFT = 1,
    VRAPI_HAND_RIGHT = 2,
};

struct ovrHandPoseHeader;
struct ovrHandSkeletonHeader;
struct ovrHandMeshHeader;

// Vulkan.
struct VkInstance_T;
struct VkPhysicalDevice_T;
struct VkDevice_T;
struct ovrSystemCreateInfoVulkan {
    VkInstance_T* Instance;
    VkPhysicalDevice_T* PhysicalDevice;
    VkDevice_T* Device;
};

}  // extern "C"

static_assert(sizeof(void*) != 8 || sizeof(ovrInitParms) == 48, "ovrInitParms");
static_assert(sizeof(void*) != 8 || sizeof(ovrModeParms) == 56, "ovrModeParms");
static_assert(sizeof(void*) != 8 || sizeof(ovrModeParmsVulkan) == 64, "ovrModeParmsVulkan");
static_assert(sizeof(ovrPosef) == 28, "ovrPosef");
static_assert(sizeof(ovrRigidBodyPosef) == 96, "ovrRigidBodyPosef");
static_assert(sizeof(ovrTracking2) == 360, "ovrTracking2");
static_assert(sizeof(ovrTracking) == 104, "ovrTracking");
static_assert(sizeof(ovrTextureSamplerState) == 36, "ovrTextureSamplerState");
static_assert(sizeof(ovrSwapChainCreateInfo) == 48, "ovrSwapChainCreateInfo");
static_assert(sizeof(void*) != 8 || sizeof(ovrFrameLayerTexture) == 208, "ovrFrameLayerTexture");
static_assert(sizeof(void*) != 8 || sizeof(ovrFrameLayer) == 448, "ovrFrameLayer");
static_assert(sizeof(void*) != 8 || sizeof(ovrLayerHeader2) == 40, "ovrLayerHeader2");
static_assert(sizeof(void*) != 8 || sizeof(ovrLayerProjection2) == 328, "ovrLayerProjection2");
static_assert(sizeof(void*) != 8 || sizeof(ovrLayerCylinder2) == 456, "ovrLayerCylinder2");
static_assert(sizeof(void*) != 8 || sizeof(ovrSubmitFrameDescription2) == 48, "ovrSubmitFrameDescription2");
static_assert(sizeof(ovrBoundaryTriggerResult) == 32, "ovrBoundaryTriggerResult");
static_assert(sizeof(ovrInputTrackedRemoteCapabilities) == 48, "ovrInputTrackedRemoteCapabilities");
static_assert(sizeof(ovrInputStateTrackedRemote) == 72, "ovrInputStateTrackedRemote");
