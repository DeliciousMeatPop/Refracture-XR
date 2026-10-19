package android.os;

import java.util.HashMap;
import java.util.Map;

/**
 * Debug.MemoryInfo with values (the android.jar stub returns null from getMemoryStats, which UE4's
 * MemoryUsageThread dereferences). The host process's committed memory stands in for the guest's PSS.
 */
public class Debug {
    public Debug() {}

    public static class MemoryInfo {
        public int dalvikPrivateDirty;
        public int dalvikPss;
        public int dalvikSharedDirty;
        public int nativePrivateDirty;
        public int nativePss;
        public int nativeSharedDirty;
        public int otherPrivateDirty;
        public int otherPss;
        public int otherSharedDirty;

        public MemoryInfo() {
            Runtime rt = Runtime.getRuntime();
            dalvikPss = dalvikPrivateDirty = (int) ((rt.totalMemory() - rt.freeMemory()) / 1024);
            long committed = 0;
            try {
                var os = (com.sun.management.OperatingSystemMXBean) java.lang.management.ManagementFactory.getOperatingSystemMXBean();
                committed = os.getCommittedVirtualMemorySize() / 1024;
            } catch (Throwable t) {
            }
            nativePss = nativePrivateDirty = (int) Math.min(Integer.MAX_VALUE, Math.max(0, committed - dalvikPss));
        }

        public int getTotalPss() {
            return dalvikPss + nativePss + otherPss;
        }

        public int getTotalSwappablePss() {
            return 0;
        }

        public int getTotalPrivateDirty() {
            return dalvikPrivateDirty + nativePrivateDirty + otherPrivateDirty;
        }

        public int getTotalSharedDirty() {
            return dalvikSharedDirty + nativeSharedDirty + otherSharedDirty;
        }

        public int getTotalPrivateClean() {
            return 0;
        }

        public int getTotalSharedClean() {
            return 0;
        }

        public String getMemoryStat(String statName) {
            return getMemoryStats().get(statName);
        }

        public Map<String, String> getMemoryStats() {
            Map<String, String> m = new HashMap<>();
            m.put("summary.java-heap", Integer.toString(dalvikPrivateDirty));
            m.put("summary.native-heap", Integer.toString(nativePrivateDirty));
            m.put("summary.code", "0");
            m.put("summary.stack", "0");
            m.put("summary.graphics", "0");
            m.put("summary.private-other", Integer.toString(otherPrivateDirty));
            m.put("summary.system", "0");
            m.put("summary.total-pss", Integer.toString(getTotalPss()));
            m.put("summary.total-swap", "0");
            return m;
        }
    }
}
