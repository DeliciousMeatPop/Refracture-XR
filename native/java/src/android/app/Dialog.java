package android.app;

import android.content.Context;
import android.view.Window;

import refract.view.WindowImpl;

/** Dialogs are never shown, but games configure their window before show() (UE4's virtual keyboard dialog). */
public class Dialog {
    private Window mWindow;
    private Context mContext;

    public Dialog(Context context) {
        mContext = context;
    }

    public Dialog(Context context, int themeResId) {
        this(context);
    }

    public final Context getContext() {
        return mContext;
    }

    public Window getWindow() {
        if (mWindow == null) mWindow = new WindowImpl(mContext);
        return mWindow;
    }

    public void show() {}

    public void dismiss() {}

    public void setCancelable(boolean flag) {}
}
