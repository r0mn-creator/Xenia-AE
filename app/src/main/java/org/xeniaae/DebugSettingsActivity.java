package org.xeniaae;

// ============================================================================
// CANARY-AE-ONLY DEBUG MODULE  —  DELETE THIS FILE FOR MAINLINE XENIA AE
// ============================================================================
// Self-contained UI for every diagnostic probe in the emulator. Nothing outside
// this module (plus one row in SettingsFragment and its layout include) knows it
// exists, so producing mainline Xenia AE is:
//   1. delete DebugSettingsActivity.java + activity_debug_settings.xml
//   2. delete the row_debug entry from SettingsFragment.java + fragment_settings.xml
//   3. remove the <activity> entry from AndroidManifest.xml
//   4. strip the TESTRIG(...)-tagged native blocks (git grep -c TESTRIG)
// See feedback_xenia_ae_debug_module_architecture in memory.
//
// All toggles drive Android system properties, which the native side reads
// live (no restart needed for the testrig ones - see testrig_debug_server.h).
// ============================================================================

import android.app.AlertDialog;
import android.os.Bundle;
import android.view.View;
import android.view.ViewGroup;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;

import androidx.appcompat.app.AppCompatActivity;

import java.io.BufferedReader;
import java.io.InputStreamReader;

public class DebugSettingsActivity extends AppCompatActivity {

    /** A probe = one system property plus human-readable explanation. */
    private static final class Probe {
        final String prop;
        final String title;
        final String summary;
        Probe(String prop, String title, String summary) {
            this.prop = prop;
            this.title = title;
            this.summary = summary;
        }
    }

    // Master switch first; per-area switches only take effect while it is on.
    private static final Probe MASTER = new Probe(
            "debug.canary.testrig.master",
            "Master diagnostics switch",
            "Enables all probes below. COSTS MOST OF THE FRAMERATE - leave off to play.");

    private static final Probe[] AREA_PROBES = {
            new Probe("debug.canary.testrig.gpu", "GPU probes",
                    "Draw/fetch/memexport logging: VTXDIST, VALSHAPE, RECDUMP, SENTINEL, "
                            + "STREAMFMT, MEMEXPORT_PATHSPLIT. Logs to xe.log."),
            new Probe("debug.canary.testrig.audio", "Audio probes",
                    "XMA decode and AAudio driver state."),
            new Probe("debug.canary.testrig.jit", "CPU / JIT probes",
                    "arm64 backend translation and guest thread state."),
            new Probe("debug.canary.testrig.mem", "Memory probes",
                    "Guest memory mapping and shared-memory traffic."),
            new Probe("debug.canary.testrig.kernel", "Kernel-thread probes",
                    "XThread scheduling and kernel object state."),
    };

    // Driver-level probe: changes how Mesa/Turnip DRIVES the GPU, not what Xenia
    // asks it to do. Read at driver load, so it needs a game restart.
    private static final String[][] TU_DEBUG_OPTIONS = {
            {"", "Default", "Normal Turnip behaviour."},
            {"sysmem", "sysmem - no tiled rendering",
                    "Disables tiled/GMEM rendering, so there is no binning pass."},
            {"gmem", "gmem - force tiled", "Forces GMEM/tiled rendering."},
            {"nobin", "nobin - no binning", "Disables the binning pass only."},
            {"forcebin", "forcebin - always bin", "Forces binning for every render pass."},
            {"noubwc", "noubwc - no compression", "Disables UBWC bandwidth compression."},
            {"flushall", "flushall - flush every draw", "Heavy cache flushing between draws."},
            {"syncdraw", "syncdraw - serialise draws", "Serialises draws; slow but deterministic."},
    };

    // Register-array initialiser. Multi-valued rather than a switch: the point of
    // the experiment is WHICH value, and "none" (no initializer at all, matching
    // upstream) is a distinct third state, not the off position of a toggle.
    private static final String[][] REGINIT_OPTIONS = {
            {"", "0.0 - zeroed", "Default. Shader registers start at zero."},
            {"none", "none - no initializer",
                    "Matches upstream xenia-canary, which leaves the register array "
                            + "INDETERMINATE per the SPIR-V spec. This is what the desktop "
                            + "RADV oracle does when it renders Halo 3 correctly."},
            {"1", "1.0", "Registers start at 1.0 - shakes out code that reads an "
                    + "uninitialised register and happens to work when it reads zero."},
            {"-1", "-1.0", "Registers start at -1.0, same purpose as 1.0."},
    };

    // Log verbosity. Needed alongside "Trace ALL kernel calls": PrintKernelCall
    // emits at DEBUG unless the export is tagged kImportant, and the shipped
    // level (2 = info) throws those away, so kernel traffic stays invisible
    // until this is raised too.
    private static final String[][] LOG_LEVEL_OPTIONS = {
            {"", "Default (2 - info)", "Shipped level. Kernel calls are NOT shown."},
            {"3", "3 - debug",
                    "Shows every kernel call that Trace ALL kernel calls enables. "
                            + "This is the setting that makes 'the game never called X' a "
                            + "real observation instead of a logging artifact."},
            {"4", "4 - trace", "Everything. Enormous; expect the log to dominate I/O."},
            {"1", "1 - warning", "Quieter than shipped - warnings and errors only."},
    };

    // Audio system override. Must be a launch arg, not config: `apu` is consumed
    // in Emulator::Setup, before the per-game config is read.
    private static final String[][] APU_OPTIONS = {
            {"", "Default", "Use whatever the config selects."},
            {"nop", "nop - silence",
                    "No audio backend at all. Tells you whether a hang or a stutter is "
                            + "coming from the audio path."},
            {"aaudio", "aaudio", "Force the AAudio driver."},
            {"sdl", "sdl", "Force the SDL driver."},
    };

    /**
     * Engine switches that change emulation behaviour (not just logging).
     * Read once at GPU init, so a game restart is needed for a change to apply.
     * Default OFF - these are experiments, not features.
     */
    private static final Probe[] ENGINE_PROBES = {
            new Probe("debug.canary.fifo_semaphore", "Strict-FIFO guest semaphores",
                    "Ported from XenDroid, which runs NFS Carbon past where we stall. "
                            + "Our semaphore is a bare count + notify_all, so a thread "
                            + "arriving AFTER a release can steal the token from one "
                            + "already parked - Windows never does that. Guest job systems "
                            + "that expect one-wake-per-release then starve, which looks "
                            + "exactly like our freeze: rendering continues, game logic "
                            + "does not. Restart the game to apply."),
            new Probe("debug.canary.xnaddr_online", "Report an ONLINE network address",
                    "XNetGetTitleXnAddr normally returns a loopback address with NO online "
                            + "identity (inaOnline 0, abOnline all zeros) and a status of "
                            + "STATIC only. NFS Carbon's main thread polls it thousands of "
                            + "times at the menu and never proceeds. This reports a "
                            + "fully-configured online-looking address instead. May just "
                            + "move the stall to the first real Live request - that would "
                            + "still prove this poll is the blocker."),
            new Probe("debug.canary.memexport_compute", "Compute memexport",
                    "Emulates vertex-shader memory export with a compute dispatch instead of "
                            + "vertex-stage stores. Did NOT fix Halo 3 geometry and is the "
                            + "suspected cause of an NFS Carbon load-screen freeze. "
                            + "Restart the game to apply."),
    };

    /**
     * AE-only engine changes that can be switched OFF to bisect a regression.
     *
     * Everything here is ON in a shipped build; the switch DISABLES it. They exist
     * because AE carries fixes upstream does not, and a fix added for one title can
     * break another - NFS Carbon worked on 2026-07-04 and the Halo 3 GPU work landed
     * 2026-07-10..13, so being able to flip each one at runtime turns a 15-minute
     * rebuild into a setprop.
     *
     * All are read when a game starts, so restart the game after changing one, and
     * clear the shader cache for the ones that alter generated SPIR-V.
     */
    private static final Probe[] AE_FIXES = {
            new Probe("debug.canary.fix_rsq", "RSQ via sqrt+div (c14047bc)",
                    "ON in shipped builds. Turn OFF to restore the driver's approximate "
                            + "GLSLstd450InverseSqrt. Added because Adreno's approximation "
                            + "diverges from RADV enough to flip floor()-based export-slot "
                            + "math. Changes SPIR-V - clear the shader cache."),
            new Probe("debug.canary.fix_sincos", "Cody-Waite SIN/COS (a0b2f29e)",
                    "ON in shipped builds. Turn OFF to restore the driver's "
                            + "GLSLstd450Sin/Cos. Added because this Adreno's native sin/cos "
                            + "has real error at large arguments. Changes SPIR-V - clear the "
                            + "shader cache."),
            new Probe("debug.canary.fix_wclip", "Degenerate W clip (6a4b9932)",
                    "ON in shipped builds. Turn OFF to let a guest W of 0 reciprocate to "
                            + "+Infinity as upstream does. Changes SPIR-V - clear the shader "
                            + "cache."),
            new Probe("debug.canary.headless", "Headless mode (no emulator dialogs)",
                    "ON in shipped builds, and REQUIRED on Android. Without it Xenia "
                            + "tries to show its own ImGui prompts (sign-in, storage) "
                            + "which this front-end cannot display or dismiss, so the "
                            + "guest waits on them forever - that was NFS Carbon's "
                            + "main-menu freeze. Turn OFF only to reproduce that bug."),
            new Probe("debug.canary.profile_local_only", "Local-only profile",
                    "ON in shipped builds: the guest profile reports as LOCAL ONLY, which "
                            + "suppresses Xbox Live attempts. aX360e - the base this project "
                            + "came from - reports local AND online, and got FURTHER in NFS "
                            + "Carbon (past the menu, stalling later at network). Turn OFF "
                            + "to restore that, so XamUserGetXUID can answer a request for "
                            + "an online XUID instead of returning NO_SUCH_USER."),
            new Probe("debug.canary.fix_rt_1010102", "10:10:10:2 render target (d04910e2)",
                    "ON in shipped builds. Corrects the 10:10:10:2 colour render target "
                            + "from an 8:8:8:8 mapping. It is the right format, but it "
                            + "changes the render-target layout for every title that uses "
                            + "it, so turn it OFF to test against the older behaviour."),
            new Probe("debug.canary.fix_sampler", "Sampler min/mip filter (d04910e2)",
                    "ON in shipped builds. Before this, minFilter and mipmapMode both "
                            + "keyed off the MAGnification bit. Turn OFF to restore that."),
            new Probe("debug.canary.fix_stencil_discard", "Stencil-bit discard (6daf1479)",
                    "ON in shipped builds. Makes the stencil kill-check actually run "
                            + "during EDRAM ownership transfers from a depth/stencil "
                            + "source; before it, every sample passed through unfiltered."),
            new Probe("debug.canary.fix_swap_renderpass", "Swap render-pass tracking (74a4ebfe)",
                    "ON in shipped builds. Keeps render-pass tracking in step through "
                            + "IssueSwap's gamma pass. Known load-bearing - reverting it "
                            + "once made the menu render fully black - so expect this one "
                            + "to break rendering rather than fix it."),
            new Probe("debug.canary.gpu_3d_to_2d", "3D-as-2D textures (d04910e2)",
                    "ON in shipped builds. Part of the texture-cache port done for Halo 3; "
                            + "it changes texture handling for EVERY game, so it is a prime "
                            + "suspect for cross-game regressions. Turn OFF to skip creating "
                            + "a 2D view of slice 0 for shaders that sample 3D textures."),
    };

    /** Diagnostic switches that only add logging or readback - no behaviour change. */
    private static final Probe[] TRACE_PROBES = {
            new Probe("debug.canary.readback_memexport", "Readback memexport",
                    "Copies GPU memexport output back to guest RAM so CPU-side probes can "
                            + "read it. Without this those probes read ZEROS and look like a "
                            + "total fill failure. Expensive - diagnostics only."),
            new Probe("debug.canary.dump_shaders", "Dump shaders",
                    "Writes guest ucode and translated SPIR-V to <storage>/shaderdump for "
                            + "byte-comparison against the desktop RADV oracle."),
            new Probe("debug.canary.stuck_wait", "Report stuck waits",
                    "Logs any KeWaitForSingleObject that blocks for over a second, with the "
                            + "object type and waiting guest thread. Finds deadlocks; note it "
                            + "will NOT catch a short-timeout poll loop."),
            new Probe("debug.canary.log_all_kernel_calls", "Trace ALL kernel calls",
                    "Ignores the per-export kLog tag. Needed because whole subsystems "
                            + "lack it - no xam_net export has it - so without this, a "
                            + "missing call in the log does NOT mean the game never made "
                            + "it. Use this before concluding a subsystem is unused."),
            new Probe("debug.canary.log_kernel_calls", "Trace kernel calls",
                    "Logs high-frequency guest kernel calls. Extremely verbose - use it to "
                            + "catch a guest poll loop, then turn it straight back off."),
    };

    private LinearLayout container;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setTitle("Debug");

        ScrollView scroll = new ScrollView(this);
        container = new LinearLayout(this);
        container.setOrientation(LinearLayout.VERTICAL);
        int pad = dp(16);
        container.setPadding(pad, pad, pad, pad);
        scroll.addView(container, new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        setContentView(scroll);

        addHeader("Diagnostics");
        addNote("These are development tools for Canary AE. They write to xe.log and "
                + "significantly reduce performance while enabled.");
        addSwitch(MASTER);

        addHeader("Probe areas");
        for (Probe p : AREA_PROBES) {
            addSwitch(p);
        }

        addHeader("Engine experiments");
        addNote("These change how the emulator works, not just what it logs. They are read "
                + "when a game starts, so restart the game after changing one.");
        for (Probe p : ENGINE_PROBES) {
            addSwitch(p);
        }

        addHeader("AE fix bisect");
        addNote("Unlike every other switch here these start ON, because they are shipped "
                + "behaviour. Turn one OFF to remove that fix and see whether a game that "
                + "regressed starts working again. Restart the game after changing one, and "
                + "use Clear Shader Cache for the ones that alter SPIR-V.");
        for (Probe p : AE_FIXES) {
            addDefaultOnSwitch(p);
        }

        addHeader("Tracing");
        addNote("Logging and readback only - these do not change how the emulator behaves.");
        for (Probe p : TRACE_PROBES) {
            addSwitch(p);
        }

        addHeader("GPU driver (Turnip)");
        addNote("Changes how the Mesa/Turnip driver executes commands, rather than what "
                + "the emulator asks for. Requires a custom Turnip driver to be selected, "
                + "and takes effect when the game is restarted.");
        addTuDebugRow();

        addHeader("Shader registers");
        addNote("Changes generated SPIR-V - clear the shader cache after changing it, or "
                + "stale pipelines are reused and the test measures nothing.");
        addPickerRow("Register initialiser", "debug.canary.reginit", REGINIT_OPTIONS);

        addPickerRow("Log level (at launch)", "debug.canary.log_level", LOG_LEVEL_OPTIONS);
        addPickerRow("Log level (LIVE, while running)", "debug.canary.log_level_live",
                LOG_LEVEL_OPTIONS);
        addNote("The LIVE row takes effect within about a second WITHOUT restarting the "
                + "game. Boot at full speed, then raise it the moment the bug happens and "
                + "drop it back afterwards. Launching at level 3 makes the emulator so "
                + "I/O-bound it looks frozen, which is useless when a freeze is what you "
                + "are trying to measure.");

        addHeader("Audio");
        addPickerRow("Audio system override", "debug.canary.apu", APU_OPTIONS);

        addHeader("Extra launch arguments");
        addNote("Set debug.canary.extra_args to pass any cvar straight to the emulator, "
                + "e.g.  --clear_memory_page_state=true --readback_resolve=uma\n"
                + "Launch args beat both config files, so this reaches settings a "
                + "per-game config is read too late to affect. Current value:\n"
                + orDash(getProp("debug.canary.extra_args")));

        addHeader("Utilities");
        addClickRow("Clear shader cache",
                "Deletes the compiled pipeline cache so shaders are retranslated. REQUIRED "
                        + "after changing anything that alters SPIR-V.", v -> clearShaderCache());
        addClickRow("Clear xe.log", "Empties the emulator log file.", v -> clearLog());
        addClickRow("Show current flags", "Dumps every debug property now set.",
                v -> showCurrentFlags());
    }

    // ---------------------------------------------------------------- widgets

    private void addHeader(String text) {
        TextView tv = new TextView(this);
        tv.setText(text);
        tv.setAllCaps(true);
        tv.setTextSize(13);
        tv.setPadding(0, dp(20), 0, dp(6));
        container.addView(tv);
    }

    private void addNote(String text) {
        TextView tv = new TextView(this);
        tv.setText(text);
        tv.setTextSize(12);
        tv.setPadding(0, 0, 0, dp(8));
        container.addView(tv);
    }

    private void addSwitch(final Probe probe) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.VERTICAL);
        row.setPadding(0, dp(10), 0, dp(10));

        Switch sw = new Switch(this);
        sw.setText(probe.title);
        sw.setTextSize(16);
        sw.setChecked("1".equals(getProp(probe.prop)));
        sw.setOnCheckedChangeListener((buttonView, isChecked) -> {
            setProp(probe.prop, isChecked ? "1" : "0");
            Toast.makeText(this, probe.title + (isChecked ? " ON" : " OFF"),
                    Toast.LENGTH_SHORT).show();
        });
        row.addView(sw);

        TextView sub = new TextView(this);
        sub.setText(probe.summary);
        sub.setTextSize(12);
        row.addView(sub);

        container.addView(row);
    }

    /**
     * Switch for a fix that is ON unless its property is explicitly "0".
     *
     * The native side treats "unset" as enabled (see XeProbeFixEnabled), so this
     * cannot use addSwitch: an unset property has to read as CHECKED, and turning
     * the switch off has to write an explicit "0" rather than clearing the value.
     */
    private void addDefaultOnSwitch(final Probe probe) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.VERTICAL);
        row.setPadding(0, dp(10), 0, dp(10));

        Switch sw = new Switch(this);
        sw.setText(probe.title);
        sw.setTextSize(16);
        sw.setChecked(!"0".equals(getProp(probe.prop)));
        sw.setOnCheckedChangeListener((buttonView, isChecked) -> {
            setProp(probe.prop, isChecked ? "1" : "0");
            Toast.makeText(this,
                    probe.title + (isChecked ? " ON" : " DISABLED")
                            + " - restart the game to apply",
                    Toast.LENGTH_LONG).show();
        });
        row.addView(sw);

        TextView sub = new TextView(this);
        sub.setText(probe.summary);
        sub.setTextSize(12);
        row.addView(sub);

        container.addView(row);
    }

    private void addClickRow(String title, String summary, View.OnClickListener onClick) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.VERTICAL);
        row.setPadding(0, dp(12), 0, dp(12));
        row.setClickable(true);
        row.setOnClickListener(onClick);

        TextView tv = new TextView(this);
        tv.setText(title);
        tv.setTextSize(16);
        row.addView(tv);

        TextView sub = new TextView(this);
        sub.setText(summary);
        sub.setTextSize(12);
        row.addView(sub);

        container.addView(row);
    }

    /**
     * Row for a property that has more than two useful values.
     *
     * options[i] = {value written, label, explanation}; an empty value means
     * "leave the property unset", i.e. the emulator's normal behaviour.
     */
    private void addPickerRow(final String title, final String prop,
                              final String[][] options) {
        addClickRow(title, pickerLabel(prop, options), v -> {
            final String[] labels = new String[options.length];
            for (int i = 0; i < options.length; i++) {
                labels[i] = options[i][1] + "\n" + options[i][2];
            }
            new AlertDialog.Builder(this)
                    .setTitle(title)
                    .setItems(labels, (dialog, which) -> {
                        setProp(prop, options[which][0]);
                        Toast.makeText(this,
                                title + ": " + options[which][1]
                                        + " - restart the game to apply",
                                Toast.LENGTH_LONG).show();
                        recreate();
                    })
                    .setNegativeButton(android.R.string.cancel, null)
                    .show();
        });
    }

    /** Current value of a picker property, shown as its human-readable label. */
    private String pickerLabel(String prop, String[][] options) {
        String cur = getProp(prop);
        if (cur == null) {
            cur = "";
        }
        for (String[] option : options) {
            if (option[0].equals(cur)) {
                return option[1];
            }
        }
        return cur.isEmpty() ? options[0][1] : cur;
    }

    private void addTuDebugRow() {
        addClickRow("TU_DEBUG flags", tuDebugLabel(), v -> showTuDebugPicker());
    }

    private String tuDebugLabel() {
        String cur = getProp("debug.canary.tu_debug");
        if (cur == null || cur.isEmpty()) {
            return "Default";
        }
        return cur;
    }

    private void showTuDebugPicker() {
        final String[] labels = new String[TU_DEBUG_OPTIONS.length];
        for (int i = 0; i < TU_DEBUG_OPTIONS.length; i++) {
            labels[i] = TU_DEBUG_OPTIONS[i][1] + "\n" + TU_DEBUG_OPTIONS[i][2];
        }
        new AlertDialog.Builder(this)
                .setTitle("TU_DEBUG")
                .setItems(labels, (dialog, which) -> {
                    setProp("debug.canary.tu_debug", TU_DEBUG_OPTIONS[which][0]);
                    Toast.makeText(this,
                            "TU_DEBUG=" + tuDebugLabel() + " - restart the game to apply",
                            Toast.LENGTH_LONG).show();
                    recreate();
                })
                .setNegativeButton(android.R.string.cancel, null)
                .show();
    }

    // ------------------------------------------------------------- utilities

    /**
     * Deletes the per-title Vulkan pipeline caches under <storage>/cache.
     *
     * Only pipelines_*.bin: the sibling modules/ directory is the CPU JIT's code
     * cache, which has nothing to do with shader translation and is expensive to
     * rebuild.
     */
    private void clearShaderCache() {
        java.io.File cacheDir =
                new java.io.File(getExternalFilesDir(null), "xeniaae/cache");
        java.io.File[] files = cacheDir.listFiles(
                (dir, name) -> name.startsWith("pipelines_") && name.endsWith(".bin"));
        if (files == null || files.length == 0) {
            Toast.makeText(this, "No shader cache to clear", Toast.LENGTH_SHORT).show();
            return;
        }
        int deleted = 0;
        for (java.io.File f : files) {
            if (f.delete()) {
                deleted++;
            }
        }
        Toast.makeText(this, "Cleared " + deleted + " of " + files.length
                + " shader caches", Toast.LENGTH_LONG).show();
    }

    private void clearLog() {
        try {
            java.io.File log = new java.io.File(getExternalFilesDir(null), "xeniaae/xe.log");
            new java.io.FileWriter(log, false).close();
            Toast.makeText(this, "xe.log cleared", Toast.LENGTH_SHORT).show();
        } catch (Exception e) {
            Toast.makeText(this, "Could not clear log: " + e.getMessage(),
                    Toast.LENGTH_LONG).show();
        }
    }

    private void showCurrentFlags() {
        StringBuilder sb = new StringBuilder();
        sb.append(MASTER.prop).append(" = ").append(orDash(getProp(MASTER.prop))).append('\n');
        for (Probe p : AREA_PROBES) {
            sb.append(p.prop).append(" = ").append(orDash(getProp(p.prop))).append('\n');
        }
        for (Probe p : ENGINE_PROBES) {
            sb.append(p.prop).append(" = ").append(orDash(getProp(p.prop))).append('\n');
        }
        for (Probe p : AE_FIXES) {
            // These default ON, so spell out what "unset" actually means here.
            String v = getProp(p.prop);
            sb.append(p.prop).append(" = ")
                    .append((v == null || v.isEmpty()) ? "(unset - ON)" : v).append('\n');
        }
        for (Probe p : TRACE_PROBES) {
            sb.append(p.prop).append(" = ").append(orDash(getProp(p.prop))).append('\n');
        }
        sb.append("debug.canary.reginit = ")
                .append(orDash(getProp("debug.canary.reginit"))).append('\n');
        sb.append("debug.canary.apu = ")
                .append(orDash(getProp("debug.canary.apu"))).append('\n');
        sb.append("debug.canary.tu_debug = ")
                .append(orDash(getProp("debug.canary.tu_debug")));
        new AlertDialog.Builder(this)
                .setTitle("Current debug flags")
                .setMessage(sb.toString())
                .setPositiveButton(android.R.string.ok, null)
                .show();
    }

    private static String orDash(String s) {
        return (s == null || s.isEmpty()) ? "(unset)" : s;
    }

    /** Reads an Android system property via getprop. */
    private static String getProp(String key) {
        try {
            Process p = new ProcessBuilder("/system/bin/getprop", key)
                    .redirectErrorStream(true).start();
            BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream()));
            String line = r.readLine();
            r.close();
            p.waitFor();
            return line == null ? "" : line.trim();
        } catch (Exception e) {
            return "";
        }
    }

    /**
     * Writes an Android system property. `debug.*` properties are writable by
     * normal apps on most builds; falls back to `su` when the device is rooted
     * (both dev devices are).
     */
    private void setProp(String key, String value) {
        if (trySetProp(new String[]{"/system/bin/setprop", key, value})) {
            return;
        }
        if (trySetProp(new String[]{"su", "-c", "setprop " + key + " '" + value + "'"})) {
            return;
        }
        Toast.makeText(this,
                "Could not set " + key + " - use: adb shell setprop " + key + " " + value,
                Toast.LENGTH_LONG).show();
    }

    private static boolean trySetProp(String[] cmd) {
        try {
            Process p = new ProcessBuilder(cmd).redirectErrorStream(true).start();
            return p.waitFor() == 0;
        } catch (Exception e) {
            return false;
        }
    }

    private int dp(int v) {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }
}
