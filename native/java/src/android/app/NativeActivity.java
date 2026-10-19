package android.app;

import android.content.pm.ActivityInfo;
import android.content.pm.PackageManager;
import android.content.res.Configuration;
import android.os.Build;
import android.os.Bundle;
import android.view.InputQueue;
import android.view.SurfaceHolder;
import android.view.ViewTreeObserver;

import java.io.File;

import refract.app.NativeActivityHost;

/**
 * AOSP's NativeActivity: loads the library named by the android.app.lib_name meta-data, calls its
 * ANativeActivity_onCreate and forwards the lifecycle to the callbacks it registers (UE4's GameActivity extends
 * this). Input queues aren't provided; window callbacks only reach the library through the SurfaceHolder methods.
 */
public class NativeActivity extends Activity
        implements SurfaceHolder.Callback2, InputQueue.Callback, ViewTreeObserver.OnGlobalLayoutListener {
    public static final String META_DATA_LIB_NAME = "android.app.lib_name";
    public static final String META_DATA_FUNC_NAME = "android.app.func_name";
    private static final String KEY_NATIVE_SAVED_STATE = "android:native_state";

    private long mNativeHandle;

    public NativeActivity() {}

    private static String path(File f) {
        return f != null ? f.getPath() : null;
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        String libname = "main";
        String funcname = "ANativeActivity_onCreate";
        try {
            ActivityInfo ai = getPackageManager().getActivityInfo(getIntent().getComponent(), PackageManager.GET_META_DATA);
            if (ai.metaData != null) {
                String ln = ai.metaData.getString(META_DATA_LIB_NAME);
                if (ln != null) libname = ln;
                ln = ai.metaData.getString(META_DATA_FUNC_NAME);
                if (ln != null) funcname = ln;
            }
        } catch (PackageManager.NameNotFoundException e) {
            throw new RuntimeException("Error getting activity info", e);
        }
        String lib = refract.Runtime.NATIVE_LIBRARY_DIR + "/lib" + libname + ".so";
        byte[] saved = savedInstanceState != null ? savedInstanceState.getByteArray(KEY_NATIVE_SAVED_STATE) : null;
        mNativeHandle = NativeActivityHost.create(lib, funcname, this, path(getFilesDir()), path(getObbDir()),
                path(getExternalFilesDir(null)), Build.VERSION.SDK_INT, getAssets(), saved);
        if (mNativeHandle == 0) throw new UnsatisfiedLinkError("Unable to load native library \"" + lib + "\"");
        super.onCreate(savedInstanceState);
    }

    private void callback(int index, int arg) {
        if (mNativeHandle != 0) NativeActivityHost.callback(mNativeHandle, index, arg);
    }

    @Override
    protected void onStart() {
        super.onStart();
        callback(NativeActivityHost.ON_START, 0);
    }

    @Override
    protected void onResume() {
        super.onResume();
        callback(NativeActivityHost.ON_RESUME, 0);
    }

    @Override
    protected void onPause() {
        super.onPause();
        callback(NativeActivityHost.ON_PAUSE, 0);
    }

    @Override
    protected void onStop() {
        super.onStop();
        callback(NativeActivityHost.ON_STOP, 0);
    }

    @Override
    protected void onDestroy() {
        callback(NativeActivityHost.ON_DESTROY, 0);
        mNativeHandle = 0;
        super.onDestroy();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        callback(NativeActivityHost.ON_WINDOW_FOCUS_CHANGED, hasFocus ? 1 : 0);
    }

    @Override
    public void onConfigurationChanged(Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        callback(NativeActivityHost.ON_CONFIGURATION_CHANGED, 0);
    }

    @Override
    public void onLowMemory() {
        super.onLowMemory();
        callback(NativeActivityHost.ON_LOW_MEMORY, 0);
    }

    private void window(int index, SurfaceHolder holder) {
        if (mNativeHandle != 0) NativeActivityHost.window(mNativeHandle, index, holder != null ? holder.getSurface() : null);
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        window(NativeActivityHost.ON_NATIVE_WINDOW_CREATED, holder);
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        window(NativeActivityHost.ON_NATIVE_WINDOW_RESIZED, holder);
    }

    @Override
    public void surfaceRedrawNeeded(SurfaceHolder holder) {
        window(NativeActivityHost.ON_NATIVE_WINDOW_REDRAW_NEEDED, holder);
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        window(NativeActivityHost.ON_NATIVE_WINDOW_DESTROYED, holder);
    }

    @Override
    public void onInputQueueCreated(InputQueue queue) {}

    @Override
    public void onInputQueueDestroyed(InputQueue queue) {}

    @Override
    public void onGlobalLayout() {}
}
