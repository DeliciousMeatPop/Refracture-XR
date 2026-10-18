# VrApi games

Quest games built before OpenXR use Meta's VrApi (the Oculus Mobile SDK, or Unity's and Unreal's Oculus
plugins on top of it). Refract runs them through `android-runtime/vrapi` (`libvrapiimpl.so`), a VrApi
implementation on top of Refract's own OpenXR runtime. It is experimental.

## How a VrApi game reaches Refract

A VrApi game ships Meta's loader, `libvrapi.so`. On a Quest, its `vrapi_Initialize` creates a package context
for `com.oculus.systemdriver`, loads `com.oculus.systemdriver.DriverLoader` from it and calls
`load64(context, context, product, major, minor, patch, build)` (or `load64Ext`, which adds API layers). That
loads the system's `libvrapiimpl.so` and returns a function accessor, `void* accessor(const char* name)`,
through which the loader resolves every `vrapi_*` call.

Refract's systemdriver stand-in (`android-runtime-apk/systemdriver`), which Meta's OpenXR loader already uses,
answers the same way: `DriverLoader.load64` loads Refract's `libvrapiimpl.so` and returns its accessor. Games are
not modified. Loaders 1.1.40 (2020) and 1.1.56 (2022, the last one) are tested.

## What the layer does

- **Session:** one OpenXR session on the game's EGL context (or Vulkan device), created when the game makes its
  first swapchain or enters VR mode, and kept until `vrapi_Shutdown`.
- **Swapchains:** each VrApi swapchain is an OpenXR swapchain. VrApi games choose which image to render into;
  the runtime hands images out round-robin, so at submit each swapchain is cycled (acquire, wait, release) until
  the submitted image is the released one. Nothing is copied. Depth and cube-map swapchains are plain GL
  textures that are never displayed.
- **Frames:** `vrapi_GetPredictedDisplayTime` and `vrapi_WaitFrame` wait on `xrWaitFrame`, once per frame;
  `vrapi_SubmitFrame2` (and the legacy `vrapi_SubmitFrame`) becomes `xrBeginFrame` + `xrEndFrame`.
- **Projection layers:** each eye's FOV comes from its `TexCoordsFromTanAngles` matrix and texture rectangle;
  its pose is the layer's head pose plus the runtime's eye offset. 2D-array (multiview) swapchains use one
  layer per eye.
- **Cylinder layers** (curved menus, HUDs) are shown as flat quads on the middle of their arc.
- **Tracking:** `vrapi_GetPredictedTracking2` locates the view and eyes in the current tracking space (LOCAL,
  LOCAL_FLOOR or STAGE) and builds VrApi's projection (near 0.1 m, infinite far) and view matrices.
- **Input:** two Touch controllers through the runtime's `/interaction_profiles/oculus/touch_controller`
  actions: buttons, touches, triggers, grips, thumbsticks, the aim pose, and simple haptics.
- **Events:** focus and visibility events on entering and leaving VR mode.

### EGL configs on the emulator

Meta's native samples (VrCubeWorld), and the native engines built on them, pick the first EGL config that is
RGBA8 with no depth, stencil or multisampling. The emulator's RGBA8 configs all have depth 24 and stencil 8,
so these games stop with `eglChooseConfig() failed`. For libraries that link `libvrapi.so` themselves (native
engines, not Unity), the layer redirects the library's `eglGetConfigAttrib` import so depth and stencil read
as 0. Those games only make a 16x16 pbuffer with the config. `setprop debug.refract.vrapi_egl_compat 0` turns
this off.

## Not supported yet

- Cube, equirect, fisheye and loading-icon layers (skipped), and quads placed with unit-square tan-angle
  matrices.
- Android surface swapchains (`vrapi_CreateAndroidSurfaceSwapChain`, used for video).
- Hand tracking, body tracking, keyboard overlays.
- Vulkan games: implemented (queue family 0 assumed) but untested.
- 32-bit games (armeabi-v7a only, common on the first Quest): Refract translates ARM64 only. The launcher
  marks them "32-bit, not supported".

## Testing

- `tests/vrapi_smoke` is a small VrApi app: cubes around the player, a cube on each controller (red while its
  trigger is held, which also vibrates it). It links Meta's loader, which is not part of Refract:
  `tests/vrapi_smoke/build_apk.ps1 -VrApiLoader <libvrapi.so>`, taken from a VrApi game's APK
  (`lib/arm64-v8a/libvrapi.so`) or a Quest (`/system_ext/lib64/libvrapi.so`). Request the loader's own API
  version in `vrapi_Initialize` (the smoke test asks for 1.1.56): a loader rejects other versions.
- QuakeQuest 1.4.11 (Team Beef, GPL, on GitHub, includes the shareware episode) is a VrApi 1.1.40 game that
  runs: menus on cylinder layers, the 3D game on projection layers, controllers and firing.
  `/sdcard/QuakeQuest/commandline.txt` with `quake +map e1m1` starts in a level.

Logs are under `Refract.VrApi` (the layer) and `RefractDriverLoader` (the Java side).
