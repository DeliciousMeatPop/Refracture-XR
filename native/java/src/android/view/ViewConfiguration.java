package android.view;

import android.content.Context;

/** AOSP's default touch configuration at a Quest's density (the android.jar stub's get() returns null). */
public class ViewConfiguration {
    private static final ViewConfiguration INSTANCE = new ViewConfiguration();
    private static final float DENSITY = 2.0f;

    public ViewConfiguration() {}

    public static ViewConfiguration get(Context context) {
        return INSTANCE;
    }

    private static int scaled(int dp) {
        return (int) (dp * DENSITY + 0.5f);
    }

    public static int getTouchSlop() { return 8; }
    public int getScaledTouchSlop() { return scaled(8); }
    public int getScaledPagingTouchSlop() { return scaled(16); }
    public int getScaledDoubleTapSlop() { return scaled(100); }
    public static int getWindowTouchSlop() { return 16; }
    public int getScaledWindowTouchSlop() { return scaled(16); }
    public static int getMinimumFlingVelocity() { return 50; }
    public int getScaledMinimumFlingVelocity() { return scaled(50); }
    public static int getMaximumFlingVelocity() { return 8000; }
    public int getScaledMaximumFlingVelocity() { return scaled(8000); }
    public static int getEdgeSlop() { return 12; }
    public int getScaledEdgeSlop() { return scaled(12); }
    public static int getTapTimeout() { return 100; }
    public static int getDoubleTapTimeout() { return 300; }
    public static int getLongPressTimeout() { return 400; }
    public static int getScrollBarSize() { return 4; }
    public int getScaledScrollBarSize() { return scaled(4); }
}
