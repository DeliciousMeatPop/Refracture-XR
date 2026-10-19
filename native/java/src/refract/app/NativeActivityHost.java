package refract.app;

/** Host side of android.app.NativeActivity (android_hle.cpp): the guest's ANativeActivity and its callbacks. */
public final class NativeActivityHost {
    // ANativeActivityCallbacks slots.
    public static final int ON_START = 0, ON_RESUME = 1, ON_PAUSE = 3, ON_STOP = 4, ON_DESTROY = 5,
            ON_WINDOW_FOCUS_CHANGED = 6, ON_NATIVE_WINDOW_CREATED = 7, ON_NATIVE_WINDOW_RESIZED = 8,
            ON_NATIVE_WINDOW_REDRAW_NEEDED = 9, ON_NATIVE_WINDOW_DESTROYED = 10, ON_CONFIGURATION_CHANGED = 14,
            ON_LOW_MEMORY = 15;

    private NativeActivityHost() {}

    /** dlopens lib, builds the ANativeActivity and calls func (ANativeActivity_onCreate); 0 on failure. */
    public static native long create(String lib, String func, Object activity, String internalDataPath,
            String obbPath, String externalDataPath, int sdkVersion, Object assetManager, byte[] savedState);

    public static native void callback(long activity, int index, int arg);

    /** surface is an android.view.Surface (only read for ON_NATIVE_WINDOW_CREATED). */
    public static native void window(long activity, int index, Object surface);
}
