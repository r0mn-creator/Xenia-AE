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

    /**
     * Engine switches that change emulation behaviour (not just logging).
     * Read once at GPU init, so a game restart is needed for a change to apply.
     * Default OFF - these are experiments, not features.
     */
    private static final Probe[] ENGINE_PROBES = {
            new Probe("debug.canary.memexport_compute", "Compute memexport",
                    "Emulates vertex-shader memory export with a compute dispatch instead of "
                            + "vertex-stage stores. Did NOT fix Halo 3 geometry and is the "
                            + "suspected cause of an NFS Carbon load-screen freeze. "
                            + "Restart the game to apply."),
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

        addHeader("GPU driver (Turnip)");
        addNote("Changes how the Mesa/Turnip driver executes commands, rather than what "
                + "the emulator asks for. Requires a custom Turnip driver to be selected, "
                + "and takes effect when the game is restarted.");
        addTuDebugRow();

        addHeader("Utilities");
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
