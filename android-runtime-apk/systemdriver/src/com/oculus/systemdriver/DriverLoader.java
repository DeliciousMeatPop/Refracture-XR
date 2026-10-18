package com.oculus.systemdriver;

import android.content.Context;
import android.util.Log;

import java.lang.reflect.Method;

// The entry points Meta's loaders call in com.oculus.systemdriver:
// - the OpenXR loader: loadXrRuntime(), then xrNegotiateLoaderRuntimeInterface and friends through getProcAddr
//   (refract_driver.c);
// - the VrApi loader (libvrapi.so in games built before OpenXR): load32/load64[Ext](), which return the
//   function accessor of libvrapiimpl.so (android-runtime/vrapi), Refract's VrApi on top of the same runtime.
public final class DriverLoader {
    private static final String TAG = "RefractDriverLoader";

    private DriverLoader() {}

    public static void loadXrRuntime() {
        Log.i(TAG, "Loading Refract OpenXR runtime");
        System.loadLibrary("openxr_runtime");
        System.loadLibrary("refract_driver");
        // The runtime reaches its pose and image brokers through the game's application context.
        try {
            Class<?> activityThread = Class.forName("android.app.ActivityThread");
            Method currentApplication = activityThread.getDeclaredMethod("currentApplication");
            currentApplication.setAccessible(true);
            Context context = (Context) currentApplication.invoke(null);
            if (context != null) {
                setContext(context);
                Log.i(TAG, "Passed application context to Refract runtime");
            } else {
                Log.w(TAG, "Current application context is null");
            }
        } catch (Exception ex) {
            Log.w(TAG, "Cannot acquire application context", ex);
        }
    }

    // VrApi: (game context, driver context, VrApi product/major/minor/patch/build version[, API layers]).
    public static long load32(Context app, Context driver, int product, int major, int minor, int patch, int build) {
        return loadVrApi(major, minor, patch);
    }

    public static long load64(Context app, Context driver, int product, int major, int minor, int patch, int build) {
        return loadVrApi(major, minor, patch);
    }

    public static long load32Ext(Context app, Context driver, int product, int major, int minor, int patch, int build,
                                 long layers) {
        return loadVrApi(major, minor, patch);
    }

    public static long load64Ext(Context app, Context driver, int product, int major, int minor, int patch, int build,
                                 long layers) {
        return loadVrApi(major, minor, patch);
    }

    private static long loadVrApi(int major, int minor, int patch) {
        Log.i(TAG, "Loading Refract VrApi driver for VrApi " + major + "." + minor + "." + patch);
        System.loadLibrary("openxr_runtime");
        System.loadLibrary("vrapiimpl");
        return nativeGetVrApiAccessor();
    }

    private static native void setContext(Context context);
    public static native long getProcAddr(String name);
    private static native long nativeGetVrApiAccessor();
}
