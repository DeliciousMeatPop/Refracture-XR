package android.content.pm;

/** AOSP's FeatureInfo fields and toString (UE4 parses the Vulkan version out of "v=..."). */
public class FeatureInfo {
    public static final int FLAG_REQUIRED = 1;
    public static final int GL_ES_VERSION_UNDEFINED = 0;

    public String name;
    public int version;
    public int reqGlEsVersion;
    public int flags;

    public FeatureInfo() {}

    public FeatureInfo(FeatureInfo orig) {
        name = orig.name;
        version = orig.version;
        reqGlEsVersion = orig.reqGlEsVersion;
        flags = orig.flags;
    }

    public String getGlEsVersion() {
        return (reqGlEsVersion >> 16) + "." + (reqGlEsVersion & 0xffff);
    }

    @Override
    public String toString() {
        if (name != null) {
            return "FeatureInfo{" + Integer.toHexString(System.identityHashCode(this)) + " " + name + " v=" + version
                    + " fl=0x" + Integer.toHexString(flags) + "}";
        }
        return "FeatureInfo{" + Integer.toHexString(System.identityHashCode(this)) + " glEsVers=" + getGlEsVersion()
                + " fl=0x" + Integer.toHexString(flags) + "}";
    }
}
