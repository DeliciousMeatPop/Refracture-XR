package android.view;

/** No Android input devices: controllers reach games through OpenXR/VrApi (the stub returned null ids). */
public final class InputDevice {
    private InputDevice() {}

    public static int[] getDeviceIds() {
        return new int[0];
    }

    public static InputDevice getDevice(int id) {
        return null;
    }
}
