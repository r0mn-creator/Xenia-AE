package org.xeniaae;

import android.content.Context;
import android.os.Build;
import android.os.PerformanceHintManager;
import android.util.Log;

import java.io.File;
import java.io.FileInputStream;
import java.util.ArrayList;
import java.util.List;

/**
 * Android Dynamic Performance Framework (ADPF) hints for the emulator threads.
 *
 * <p>Measured on an Odin 2 (Snapdragon 8 Gen 2): during gameplay the two guest
 * threads sit pinned at ~105% of a core each and the GPU command thread at ~95%,
 * while the prime core is already at its full 3187 MHz. Core placement is
 * already optimal — nothing ever lands on an efficiency core — so there is no
 * clock left to unlock by asking politely.
 *
 * <p>What is still missing is telling the scheduler <b>which</b> threads matter
 * and <b>how far behind</b> they are. {@code appCategory="game"} and sustained
 * performance mode are passive hints; ADPF is specific: a session names the
 * critical thread ids and carries a target work duration, so the governor can
 * treat those threads as latency-critical rather than merely busy.
 *
 * <p>⚠️ Honest scope: this creates the session and sets the target. It does not
 * yet call {@code reportActualWorkDuration} every frame, which is where most of
 * ADPF's benefit normally comes from — that needs the emulator's own frame loop
 * to report timings across JNI. Expect a small effect from this alone; if it
 * shows nothing, the reporting half is the part worth building, not more hints.
 */
public final class PerformanceHints {

    private static final String TAG = "XeniaAE";

    /** 30 FPS — the frame rate essentially every Xbox 360 title targeted. */
    private static final long TARGET_WORK_NANOS = 33_333_333L;

    /**
     * Threads worth boosting, matched against /proc/self/task/<tid>/comm.
     * These are exactly the ones measured pinned during gameplay; adding idle
     * threads would dilute the hint.
     */
    private static final String[] CRITICAL_THREADS = {
            "Main XThread",   // guest main thread, ~107% of a core
            "MainThread",     // guest render thread, ~102%
            "GPU Commands",   // PM4 -> Vulkan translation, ~95%
    };

    private static PerformanceHintManager.Session sSession;

    private PerformanceHints() {}

    /** Creates the hint session. Safe to call repeatedly. */
    public static void start(Context ctx) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) {
            return;  // PerformanceHintManager is API 31+
        }
        if (sSession != null) {
            return;
        }
        try {
            PerformanceHintManager mgr =
                    (PerformanceHintManager) ctx.getSystemService(
                            Context.PERFORMANCE_HINT_SERVICE);
            if (mgr == null) {
                Log.i(TAG, "ADPF: PerformanceHintManager unavailable");
                return;
            }
            int[] tids = criticalThreadIds();
            if (tids.length == 0) {
                // The emulator threads are created after the activity starts, so
                // an early call finds nothing. Caller retries.
                return;
            }
            sSession = mgr.createHintSession(tids, TARGET_WORK_NANOS);
            if (sSession == null) {
                Log.i(TAG, "ADPF: session not created (unsupported device?)");
            } else {
                StringBuilder sb = new StringBuilder();
                for (int t : tids) sb.append(t).append(' ');
                Log.i(TAG, "ADPF: hint session for tids [" + sb.toString().trim()
                        + "] target=" + (TARGET_WORK_NANOS / 1_000_000) + "ms");
            }
        } catch (Exception e) {
            Log.i(TAG, "ADPF unavailable: " + e);
        }
    }

    /**
     * Reports how long the last frame actually took.
     *
     * <p>This is the half that makes ADPF work: the governor compares it against
     * the target and boosts when we are behind. Currently unused pending a hook
     * in the emulator's swap path.
     */
    public static void reportFrameNanos(long actualNanos) {
        if (sSession == null || actualNanos <= 0) {
            return;
        }
        try {
            sSession.reportActualWorkDuration(actualNanos);
        } catch (Exception ignored) {
        }
    }

    public static void stop() {
        if (sSession != null) {
            try {
                sSession.close();
            } catch (Exception ignored) {
            }
            sSession = null;
        }
    }

    /** Finds the tids of the threads named in {@link #CRITICAL_THREADS}. */
    private static int[] criticalThreadIds() {
        List<Integer> found = new ArrayList<>();
        File taskDir = new File("/proc/self/task");
        File[] tasks = taskDir.listFiles();
        if (tasks == null) {
            return new int[0];
        }
        for (File t : tasks) {
            String comm = readComm(t);
            if (comm == null) continue;
            for (String want : CRITICAL_THREADS) {
                // comm is truncated to 15 chars by the kernel, so compare on a
                // prefix rather than for equality.
                String key = want.length() > 14 ? want.substring(0, 14) : want;
                if (comm.startsWith(key)) {
                    try {
                        found.add(Integer.parseInt(t.getName()));
                    } catch (NumberFormatException ignored) {
                    }
                    break;
                }
            }
        }
        int[] out = new int[found.size()];
        for (int i = 0; i < out.length; i++) out[i] = found.get(i);
        return out;
    }

    private static String readComm(File taskDir) {
        try (FileInputStream in = new FileInputStream(new File(taskDir, "comm"))) {
            byte[] buf = new byte[64];
            int n = in.read(buf);
            return n > 0 ? new String(buf, 0, n).trim() : null;
        } catch (Exception e) {
            return null;
        }
    }
}
