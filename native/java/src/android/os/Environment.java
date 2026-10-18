package android.os;

import java.io.File;

import refract.app.GuestFile;

/**
 * Storage paths as a Quest reports them (the android.jar stub returns null). Games build OBB and asset paths
 * from getExternalStorageDirectory(): Iron Man VR opened "Android/obb/..." relative paths without this.
 */
public class Environment {
    public static final String MEDIA_UNKNOWN = "unknown";
    public static final String MEDIA_REMOVED = "removed";
    public static final String MEDIA_UNMOUNTED = "unmounted";
    public static final String MEDIA_CHECKING = "checking";
    public static final String MEDIA_NOFS = "nofs";
    public static final String MEDIA_MOUNTED = "mounted";
    public static final String MEDIA_MOUNTED_READ_ONLY = "mounted_ro";
    public static final String MEDIA_SHARED = "shared";
    public static final String MEDIA_BAD_REMOVAL = "bad_removal";
    public static final String MEDIA_UNMOUNTABLE = "unmountable";
    public static final String MEDIA_EJECTING = "ejecting";

    public static String DIRECTORY_MUSIC = "Music";
    public static String DIRECTORY_PODCASTS = "Podcasts";
    public static String DIRECTORY_RINGTONES = "Ringtones";
    public static String DIRECTORY_ALARMS = "Alarms";
    public static String DIRECTORY_NOTIFICATIONS = "Notifications";
    public static String DIRECTORY_PICTURES = "Pictures";
    public static String DIRECTORY_MOVIES = "Movies";
    public static String DIRECTORY_DOWNLOADS = "Download";
    public static String DIRECTORY_DCIM = "DCIM";
    public static String DIRECTORY_DOCUMENTS = "Documents";
    public static String DIRECTORY_SCREENSHOTS = "Screenshots";
    public static String DIRECTORY_AUDIOBOOKS = "Audiobooks";
    public static String DIRECTORY_RECORDINGS = "Recordings";

    private static final String EXTERNAL = "/storage/emulated/0";

    public Environment() {}

    public static File getRootDirectory() { return new GuestFile("/system"); }
    public static File getStorageDirectory() { return new GuestFile("/storage"); }
    public static File getDataDirectory() { return new GuestFile("/data"); }
    public static File getDownloadCacheDirectory() { return new GuestFile("/data/cache"); }
    public static File getExternalStorageDirectory() { return new GuestFile(EXTERNAL); }
    public static File getExternalStoragePublicDirectory(String type) { return new GuestFile(EXTERNAL + "/" + type); }

    public static String getExternalStorageState() { return MEDIA_MOUNTED; }
    public static String getExternalStorageState(File path) { return MEDIA_MOUNTED; }
    @Deprecated public static String getStorageState(File path) { return MEDIA_MOUNTED; }
    public static boolean isExternalStorageRemovable() { return false; }
    public static boolean isExternalStorageRemovable(File path) { return false; }
    public static boolean isExternalStorageEmulated() { return true; }
    public static boolean isExternalStorageEmulated(File path) { return true; }
    public static boolean isExternalStorageLegacy() { return true; }
    public static boolean isExternalStorageLegacy(File path) { return true; }
    public static boolean isExternalStorageManager() { return false; }
    public static boolean isExternalStorageManager(File path) { return false; }
}
