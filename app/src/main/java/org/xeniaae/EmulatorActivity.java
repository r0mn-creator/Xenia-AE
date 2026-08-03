package org.xeniaae;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.Dialog;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.net.Uri;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.os.Message;
import android.os.ParcelFileDescriptor;
import android.os.SystemClock;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.preference.PreferenceManager;
import android.util.Log;
import android.util.SparseIntArray;
import android.view.InputDevice;
import android.view.InputEvent;
import android.view.KeyEvent;
import android.view.LayoutInflater;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.widget.ProgressBar;
import android.widget.TextView;
import android.widget.Toast;

import androidx.annotation.NonNull;
import androidx.documentfile.provider.DocumentFile;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.RandomAccessFile;

// Created by aenu on 2025/7/29.
// SPDX-License-Identifier: WTFPL
public class EmulatorActivity extends Activity implements SurfaceHolder.Callback, View.OnGenericMotionListener {

    static final int DELAY_ON_CREATE=0xaeae0001;
    public static final String EXTRA_GAME_URI="game_uri";
    public static final String EXTRA_GAME_TITLE="game_title";
    public static final String EXTRA_GAME_TITLE_ID="game_title_id";
    public static final String EXTRA_PRECACHE_MODE="precache_mode";

    // How long a "Pre-cache Shaders" session runs before auto-stopping and saving.
    private static final long PRECACHE_DURATION_MS = 120_000;

    /** Builds the intent used to launch a game, matching the manifest's EMULATE action. */
    static Intent createInternalIntent(Context context, String gameUri, String gameTitle){
        return createInternalIntent(context, gameUri, gameTitle, null);
    }

    /** Same as above, but also threads through the game's XEX title ID (e.g. "4D5307E6")
     *  when the caller already has it, so the in-game Settings menu can open the correct
     *  per-game config file without re-deriving it. Pass null if not known yet. */
    static Intent createInternalIntent(Context context, String gameUri, String gameTitle, String titleId){
        Intent intent=new Intent("org.xeniaae.intent.action.EMULATE");
        intent.setPackage(context.getPackageName());
        intent.putExtra(EXTRA_GAME_URI,gameUri);
        intent.putExtra(EXTRA_GAME_TITLE,gameTitle);
        intent.putExtra(EXTRA_GAME_TITLE_ID,titleId);
        return intent;
    }

    /** Same as {@link #createInternalIntent}, but boots into a timed shader pre-cache session. */
    static Intent createPrecacheIntent(Context context, String gameUri, String gameTitle){
        Intent intent = createInternalIntent(context, gameUri, gameTitle);
        intent.putExtra(EXTRA_PRECACHE_MODE, true);
        return intent;
    }
    static SurfaceView sf=null;
    private SparseIntArray keysMap = new SparseIntArray();
    private Vibrator vibrator=null;
    private VibrationEffect vibrationEffect=null;
    boolean started=false;
    Dialog delay_dialog=null;

    // In-game pause menu (Resume / Settings / Exit).
    private String game_uri_;
    private String game_title_id_;
    private AlertDialog pause_dialog_;
    private boolean returning_from_game_settings_;

    private TextView status_overlay;
    // Stat reads below block on /proc and log-file I/O, so they run on a
    // dedicated background thread — not the UI thread — to avoid stealing
    // CPU time from rendering/JIT once per second during gameplay.
    private HandlerThread overlay_thread;
    private Handler overlay_bg_handler;
    private Handler overlay_ui_handler;
    private static final int OVERLAY_INTERVAL_MS = 1000;

    private long[] prev_cpu_ticks = null;

    // ---- FPS counter ---------------------------------------------------
    // Deliberately its own small unit, separate from status_overlay above:
    // different toggle, different position, and no file I/O. status_overlay
    // tails xe.log and reads /proc, which is far too heavy for something meant
    // to be on screen while judging performance.
    //
    // Toggle: debug.canary.fps (the same property that enables the native
    // counter, so one switch turns on both the display and the log lines that
    // scripts/fps_bench.sh aggregates).
    private TextView fps_counter;
    private Handler fps_handler;
    private static final int FPS_INTERVAL_MS = 500;

    private boolean fps_counter_enabled() {
        return "1".equals(debugProp("debug.canary.fps"));
    }

    private final Runnable fps_updater = new Runnable() {
        @Override
        public void run() {
            // Re-checked every tick so the counter can be switched on and off
            // mid-game with setprop, matching the native side's behaviour.
            if (!fps_counter_enabled()) {
                fps_counter.setVisibility(View.GONE);
                fps_handler.postDelayed(this, FPS_INTERVAL_MS);
                return;
            }
            float fps = 0.0f;
            try {
                fps = Emulator.get.current_fps();
            } catch (Throwable ignored) {
                // Emulator not up yet, or an older native lib without the
                // export - show nothing rather than crash the game.
            }
            if (fps > 0.0f) {
                fps_counter.setText(String.format(java.util.Locale.US, "%.1f FPS", fps));
                fps_counter.setVisibility(View.VISIBLE);
            } else {
                // Counter on but no window closed yet.
                fps_counter.setText("-- FPS");
                fps_counter.setVisibility(View.VISIBLE);
            }
            fps_handler.postDelayed(this, FPS_INTERVAL_MS);
        }
    };

    // TESTRIG(perf): the overlay reads /proc/self/stat, /proc/stat and tails
    // xe.log once a second. Small, but it is real work and real file I/O during
    // gameplay, and an FPS measurement should not include the cost of the thing
    // displaying the FPS. debug.canary.overlay=0 disables it.
    private boolean overlay_enabled() {
        return !"0".equals(debugProp("debug.canary.overlay"));
    }

    private final Runnable overlay_updater = new Runnable() {
        @Override
        public void run() {
            final String ram = read_ram_mb();
            final String cpu = read_cpu_pct();
            final String last_log = read_last_log_line();
            final String text = "RAM:" + ram + "MB  CPU:" + cpu + "%\n" + last_log;
            overlay_ui_handler.post(() -> status_overlay.setText(text));
            overlay_bg_handler.postDelayed(this, OVERLAY_INTERVAL_MS);
        }
    };

    private static String read_ram_mb() {
        try (BufferedReader br = new BufferedReader(new FileReader("/proc/self/status"))) {
            String line;
            while ((line = br.readLine()) != null) {
                if (line.startsWith("VmRSS:")) {
                    String[] parts = line.trim().split("\\s+");
                    if (parts.length >= 2) return String.valueOf(Long.parseLong(parts[1]) / 1024);
                }
            }
        } catch (Exception ignored) {}
        return "?";
    }

    // Returns process CPU% across all cores since last call (reads /proc/self/stat + /proc/stat).
    private String read_cpu_pct() {
        try (BufferedReader proc = new BufferedReader(new FileReader("/proc/self/stat"));
             BufferedReader sys  = new BufferedReader(new FileReader("/proc/stat"))) {
            String[] p = proc.readLine().trim().split("\\s+");
            long proc_ticks = Long.parseLong(p[13]) + Long.parseLong(p[14]); // utime + stime
            String[] s = sys.readLine().trim().split("\\s+"); // "cpu  ..."
            long total_ticks = 0;
            for (int i = 1; i < s.length; i++) total_ticks += Long.parseLong(s[i]);
            long[] cur = {proc_ticks, total_ticks};
            if (prev_cpu_ticks != null) {
                long d_proc  = cur[0] - prev_cpu_ticks[0];
                long d_total = cur[1] - prev_cpu_ticks[1];
                prev_cpu_ticks = cur;
                if (d_total > 0) return String.valueOf((int)(100L * d_proc / d_total));
            }
            prev_cpu_ticks = cur;
        } catch (Exception ignored) {}
        return "?";
    }

    private String read_last_log_line() {
        String log_path = Application.get_app_data_dir().getAbsolutePath() + "/xe.log";
        try (RandomAccessFile raf = new RandomAccessFile(log_path, "r")) {
            long len = raf.length();
            if (len == 0) return "";
            long pos = Math.max(0, len - 512);
            raf.seek(pos);
            byte[] buf = new byte[(int) (len - pos)];
            raf.readFully(buf);
            String[] lines = new String(buf).split("\n");
            for (int i = lines.length - 1; i >= 0; i--) {
                String l = lines[i].trim();
                if (!l.isEmpty()) return l;
            }
        } catch (Exception ignored) {}
        return "";
    }
    // TESTRIG(toggles): every diagnostic launch arg below is gated on a
    // `debug.canary.*` system property so a test can be switched from the Debug
    // screen (or `adb shell setprop`) instead of a 15-minute rebuild. Properties
    // rather than the config file because hand-writing app config corrupts its
    // ownership, and launch args because several of these cvars are consumed in
    // Emulator::Setup - before any per-game config is read.

    /** Reads a debug system property; "" when unset or unreadable. */
    private static String debugProp(String key) {
        try {
            Process p = new ProcessBuilder("/system/bin/getprop", key)
                    .redirectErrorStream(true).start();
            java.io.BufferedReader r = new java.io.BufferedReader(
                    new java.io.InputStreamReader(p.getInputStream()));
            String line = r.readLine();
            r.close();
            p.waitFor();
            return line == null ? "" : line.trim();
        } catch (Exception e) {
            // Unreadable property means the feature stays off, which is the safe
            // default for everything gated this way.
            return "";
        }
    }

    /** True only when a debug property is explicitly turned on. */
    private static boolean debugFlagSet(String key) {
        String v = debugProp(key);
        return v.equals("1") || v.equals("true");
    }

    final Handler delay_on_create=new Handler(new Handler.Callback(){
        @Override
        public boolean handleMessage(@NonNull Message msg) {

            if(msg.what!=DELAY_ON_CREATE) return false;
            if(delay_dialog!=null){
                delay_dialog.dismiss();
                delay_dialog=null;
            }
            on_create();
            return true;
        }
    });
    void on_create(){
        String uri=getIntent().getStringExtra(EXTRA_GAME_URI);
        final String game_title = getIntent().getStringExtra(EXTRA_GAME_TITLE);
        // The library may hold a stale MediaStore URI: those are row ids, and
        // MediaStore reassigns them on re-index (reboot, media scan, file move).
        // The ISO is still there, the saved URI just points at nothing. Before
        // this, the open failed inside nc_open_uri_fd, which logged and returned
        // -1, so the emulator started against no file and sat there silently -
        // it looked exactly like "tapping the game does nothing".
        if (!GameUriResolver.canOpen(this, uri)) {
            String resolved = GameUriResolver.resolve(this, uri, game_title);
            if (GameUriResolver.canOpen(this, resolved)) {
                uri = resolved;
            } else {
                show_missing_game_dialog(game_title);
                return;
            }
        }
        game_uri_=uri;
        game_title_id_=getIntent().getStringExtra(EXTRA_GAME_TITLE_ID);
        org.xeniaae.emulator.Emulator.Path path=org.xeniaae.emulator.Emulator.Path.from(uri,-1);
        Emulator.get.setup_context(this);
        android.net.Uri gameDirUri = MainActivity.load_pref_game_dir(this);
        Emulator.get.setup_document_file_tree(gameDirUri != null ? DocumentFile.fromTreeUri(this, gameDirUri) : null);
        Emulator.get.setup_game_path(path);
        // Per-game GPU driver override, injected as a LAUNCH ARGUMENT rather than
        // written into a config file. The driver is loaded during
        // Emulator::Setup (emulator.cc:325), long before the per-game config is
        // read (emulator.cc:1688), so a per-game TOML entry would be ignored.
        // Launch args win over both config files (base/cvar.h
        // ConfigVar::UpdateValue: commandline > game_config > config), and this
        // leaves the global config untouched.
        final java.util.ArrayList<String> launch_args = new java.util.ArrayList<>();
        final String per_game_driver =
                GameDriverStore.launchArgValue(this, game_title_id_);
        if (per_game_driver != null) {
            launch_args.add("--vulkan_lib_path=" + per_game_driver);
            android.util.Log.i("XeniaAE",
                    "Per-game driver for " + game_title_id_ + ": " + per_game_driver);
        }
        // TESTRIG(readback-memexport): CPU-side probes that scan the memexport
        // target buffer (RECFIELD0/VTXDIST) read GUEST RAM, which the GPU's
        // memexport writes never reach unless readback_memexport copies them
        // back. Without it those probes read ZEROS and look like a total fill
        // failure - an artifact, not a finding. Exposed as a runtime property so
        // it can be matched against the desktop oracle's --readback_memexport
        // without a rebuild, and it stays OFF by default because the copy-back
        // is expensive.
        if (debugFlagSet("debug.canary.readback_memexport")) {
            launch_args.add("--readback_memexport=true");
            android.util.Log.i("XeniaAE", "readback_memexport ENABLED via property");
        }
        // TESTRIG(shader-dump): dump translated SPIR-V when the property is set,
        // so it can be byte-compared against the desktop RADV oracle. Off by
        // default; passed as a launch arg rather than written into the config,
        // because hand-editing the config file corrupts its ownership.
        if (debugFlagSet("debug.canary.dump_shaders")) {
            launch_args.add("--dump_shaders="
                    + Application.get_app_data_dir().getAbsolutePath() + "/shaderdump");
            android.util.Log.i("XeniaAE", "dump_shaders ENABLED via property");
        }
        // TESTRIG(kernel-call-trace): log high-frequency kernel calls when the
        // property is set. Needed to catch a guest POLL LOOP - e.g. NFS Carbon's
        // main-menu freeze, where the guest spins with zero ordinary log output
        // because the poll lands on an unimplemented stub that returns without
        // logging. Off by default; this is extremely verbose.
        // TESTRIG(kernel-call-trace): PrintKernelCall logs at DEBUG unless the
        // export is tagged kImportant, and the shipped log_level is 2 (info) -
        // so kernel calls are invisible even with log_all_kernel_calls set.
        // This is the second half of that blind spot; both are needed to see a
        // subsystem's traffic.
        // TESTRIG(extra-args): arbitrary cvars, whitespace-separated, e.g.
        //   adb shell setprop debug.canary.extra_args \
        //     "--clear_memory_page_state=true --readback_resolve=uma"
        //
        // Every other toggle here needed a rebuild to add. This one does not:
        // any cvar the emulator has becomes testable immediately, which is what
        // made comparing against a WORKING third-party build (XenDroid) - whose
        // config differs from ours in 14 settings - a minutes-long job instead
        // of one rebuild per setting.
        //
        // Launch args beat both config files (base/cvar.h: commandline >
        // game_config > config), so this also reaches cvars consumed during
        // Emulator::Setup that a per-game TOML is read too late to affect.
        // ★ headless=true is REQUIRED on Android.
        //
        // Xenia's non-headless path tries to display its own ImGui dialogs for
        // things like sign-in and storage-device prompts. There is no ImGui
        // overlay on this front-end, so such a dialog can never be shown OR
        // dismissed - the guest asks for it, waits for a result, and waits
        // forever. That is what froze NFS Carbon at its main menu: pressing A
        // on CAREER made the title attempt an Xbox Live connection, and the
        // resulting prompt never completed, so the game's main thread sat in a
        // Sleep(0) poll loop at guest 0x824E98AC waiting on a flag nothing
        // would ever set. With headless, the request returns a default
        // immediately, the title gets its answer ("Cannot connect to Xbox
        // Live!"), and career loading proceeds.
        //
        // Both known-working Android forks ship this: aX360e and XenDroid both
        // have headless = true. AE had false.
        //
        // debug.canary.headless=0 restores the old behaviour for A/B testing.
        if (!"0".equals(debugProp("debug.canary.headless"))) {
            launch_args.add("--headless=true");
        }
        // TESTRIG(perf): log I/O off by default.
        //
        // flush_log writes every batch straight to disk and log_to_stdout mirrors
        // every line into logcat. Both are per-line costs paid at ~40 log sites
        // in the GPU hot paths alone, on threads that saturate during gameplay.
        // Canary AE is a test bed, so this has to be switchable rather than
        // removed - debug.canary.logging=1 turns it back on when a trace is
        // actually wanted.
        if (!"1".equals(debugProp("debug.canary.logging"))) {
            launch_args.add("--flush_log=false");
            launch_args.add("--log_to_stdout=false");
        }
        String extra_args = debugProp("debug.canary.extra_args");
        if (!extra_args.isEmpty()) {
            for (String arg : extra_args.trim().split("\\s+")) {
                if (!arg.isEmpty()) {
                    launch_args.add(arg);
                }
            }
            android.util.Log.i("XeniaAE", "extra launch args: " + extra_args);
        }
        String log_level = debugProp("debug.canary.log_level");
        if (!log_level.isEmpty()) {
            launch_args.add("--log_level=" + log_level);
            android.util.Log.i("XeniaAE", "log_level override: " + log_level);
        }
        if (debugFlagSet("debug.canary.log_all_kernel_calls")) {
            launch_args.add("--log_all_kernel_calls=true");
            android.util.Log.i("XeniaAE", "log_all_kernel_calls ENABLED");
        }
        if (debugFlagSet("debug.canary.log_kernel_calls")) {
            launch_args.add("--log_high_frequency_kernel_calls=true");
            android.util.Log.i("XeniaAE", "log_high_frequency_kernel_calls ENABLED");
        }
        // TESTRIG(apu-override): force the audio system via launch arg.
        //
        // ⚠️ The per-game config CANNOT do this - same timing trap as
        // vulkan_lib_path: `apu` is consumed while the audio system is created in
        // Emulator::Setup, but the per-game config is not read until
        // CompleteLaunch, a phase later. Setting apu in <TITLEID>.config.toml is
        // silently ignored (verified: the AudioTrack thread still spawned with
        // apu='nop' in the per-game config). Launch args win over both config
        // files, so this is the only route that applies in time.
        String apu_override = debugProp("debug.canary.apu");
        if (!apu_override.isEmpty()) {
            launch_args.add("--apu=" + apu_override);
            android.util.Log.i("XeniaAE", "apu override: " + apu_override);
        }
        java.util.Collections.addAll(launch_args,
                "--storage_root="+Application.get_app_data_dir().getAbsolutePath(),
                "--config="+Application.get_global_config_file().getAbsolutePath(),
                "--log_file="+Application.get_app_data_dir().getAbsolutePath()+"/xe.log");
        Emulator.get.setup_launch_args(launch_args.toArray(new String[0]));
        Emulator.get.setup_uri_info_list_file(Application.get_uri_info_list_file().getAbsolutePath());
        setContentView(R.layout.activity_emulator);
        sf = (SurfaceView) findViewById(R.id.surface_view);
        sf.getHolder().addCallback(EmulatorActivity.this);

        sf.setFocusable(true);
        sf.setFocusableInTouchMode(true);
        sf.requestFocus();
        sf.setOnGenericMotionListener(this);

        // FPS counter: independent of the status overlay below. Always polled
        // (cheaply, on the UI thread - it is a single atomic read via JNI, no
        // I/O) so that setprop debug.canary.fps 1 takes effect mid-game without
        // restarting. The view stays GONE until the toggle is on.
        fps_counter = (TextView) findViewById(R.id.fps_counter);
        fps_handler = new Handler(Looper.getMainLooper());
        fps_handler.postDelayed(fps_updater, FPS_INTERVAL_MS);

        status_overlay = (TextView) findViewById(R.id.status_overlay);
        final SharedPreferences sPrefs2 = PreferenceManager.getDefaultSharedPreferences(this);
        // Position the status overlay at the bottom (default) or top, per the
        // "status_overlay_bottom" preference, so it doesn't overlap other
        // top-of-screen overlays. Kept on the left (start) either way.
        {
            final boolean overlay_bottom = sPrefs2.getBoolean("status_overlay_bottom", true);
            final android.widget.FrameLayout.LayoutParams lp =
                    (android.widget.FrameLayout.LayoutParams) status_overlay.getLayoutParams();
            lp.gravity = (overlay_bottom ? android.view.Gravity.BOTTOM
                                         : android.view.Gravity.TOP)
                    | android.view.Gravity.START;
            status_overlay.setLayoutParams(lp);
        }
        // Already opt-in via the setting; debug.canary.overlay=0 also kills it
        // for a clean-room FPS measurement without changing the user's setting.
        if (sPrefs2.getBoolean("show_status_overlay", false) && overlay_enabled()) {
            status_overlay.setVisibility(View.VISIBLE);
            overlay_ui_handler = new Handler(Looper.getMainLooper());
            overlay_thread = new HandlerThread("EmulatorOverlayStats");
            overlay_thread.start();
            overlay_bg_handler = new Handler(overlay_thread.getLooper());
            overlay_bg_handler.post(overlay_updater);
        }

        if (getIntent().getBooleanExtra(EXTRA_PRECACHE_MODE, false)) {
            start_precache_session(getIntent().getStringExtra(EXTRA_GAME_TITLE));
        }

        load_key_map_and_vibrator();
    }

    private Handler precache_handler;
    private long precache_start_time_ms;

    private void start_precache_session(String game_title) {
        final View overlay = findViewById(R.id.precache_overlay);
        final TextView title_view = (TextView) findViewById(R.id.precache_title);
        final ProgressBar progress = (ProgressBar) findViewById(R.id.precache_progress);

        title_view.setText(game_title != null ? game_title : "");
        overlay.setVisibility(View.VISIBLE);

        precache_start_time_ms = SystemClock.elapsedRealtime();
        precache_handler = new Handler(Looper.getMainLooper());

        final Runnable ticker = new Runnable() {
            @Override
            public void run() {
                final long elapsed = SystemClock.elapsedRealtime() - precache_start_time_ms;
                if (elapsed >= PRECACHE_DURATION_MS) {
                    finish_precache_session(overlay, game_title);
                    return;
                }
                progress.setProgress((int) (100 * elapsed / PRECACHE_DURATION_MS));
                precache_handler.postDelayed(this, 250);
            }
        };
        precache_handler.post(ticker);
    }

    private void finish_precache_session(View overlay, String game_title) {
        overlay.setVisibility(View.GONE);
        final int seconds_ran = (int) (PRECACHE_DURATION_MS / 1000);
        new AlertDialog.Builder(this)
                .setTitle(R.string.precache_complete_title)
                .setMessage(getString(R.string.precache_complete_message, game_title, seconds_ran))
                .setCancelable(false)
                .setPositiveButton(android.R.string.ok, (dialog, which) -> {
                    dialog.dismiss();
                    finish();
                })
                .show();
    }
    void vibrator(){
        if(vibrator!=null) {
            vibrator.vibrate(vibrationEffect);
        }
    }

    // Auto-detect controllers while a game is running. A pad plugged in
    // mid-session must work immediately - making the player quit to Settings to
    // re-bind is the friction this removes - and a pad that drops out mid-race
    // needs to be obvious, otherwise it reads as the game hanging.
    private android.hardware.input.InputManager input_manager;

    private final android.hardware.input.InputManager.InputDeviceListener
            input_device_listener =
            new android.hardware.input.InputManager.InputDeviceListener() {
                @Override
                public void onInputDeviceAdded(int deviceId) {
                    ControllerAutoMap.Pad pad =
                            ControllerAutoMap.autoMapNewDevices(EmulatorActivity.this);
                    // Reload either way: an already-known pad still needs its
                    // bindings live in keysMap for this session.
                    load_key_map_and_vibrator();
                    android.view.InputDevice dev =
                            android.view.InputDevice.getDevice(deviceId);
                    String name = dev != null ? dev.getName() : "Controller";
                    String msg = pad != null
                            ? name + " connected \u2014 "
                                    + ControllerAutoMap.profileLabel(pad.profile)
                            : name + " connected";
                    Toast.makeText(EmulatorActivity.this, msg, Toast.LENGTH_SHORT).show();
                }

                @Override
                public void onInputDeviceRemoved(int deviceId) {
                    Toast.makeText(EmulatorActivity.this, "Controller disconnected",
                            Toast.LENGTH_SHORT).show();
                }

                @Override
                public void onInputDeviceChanged(int deviceId) {
                    load_key_map_and_vibrator();
                }
            };

    /**
     * Asks Android for stable sustained clocks rather than burst-then-throttle.
     *
     * <p>Measured on this device: the two guest threads sit pinned at ~105% of a
     * core each for the whole session, so this is not a bursty workload - it is
     * a sustained one, and the default DVFS behaviour of boosting then backing
     * off is the wrong shape for it. Sustained mode asks the governor for a
     * clock it can hold indefinitely.
     *
     * <p>Particularly worth it on a handheld with ACTIVE COOLING, where the
     * thermal ceiling is higher than the governor's default assumptions.
     *
     * <p>Paired with android:appCategory="game" in the manifest, which is what
     * tells Android - and OEM performance profiles - what this app actually is.
     */
    private void request_sustained_performance() {
        try {
            if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.N
                    && getWindow() != null) {
                getWindow().setSustainedPerformanceMode(true);
            }
        } catch (Exception e) {
            // Not supported on this device - harmless, just skip it.
            android.util.Log.i("XeniaAE", "sustained performance mode unavailable: " + e);
        }
    }

    /**
     * Registers the emulator's hot threads with ADPF.
     *
     * <p>Retried on a delay because the threads it needs to name - the guest
     * threads and the GPU command thread - do not exist yet when the activity
     * resumes; they are created as the emulator spins up. Looking once would
     * always find nothing.
     */
    private void start_performance_hints() {
        final Handler h = new Handler(Looper.getMainLooper());
        final int[] attempts = {0};
        h.postDelayed(new Runnable() {
            @Override
            public void run() {
                PerformanceHints.start(EmulatorActivity.this);
                if (++attempts[0] < 10) {
                    h.postDelayed(this, 2000);
                }
            }
        }, 3000);
    }

    /** Starts controller detection; safe to call more than once. */
    private void start_controller_detection() {
        if (input_manager != null) {
            return;
        }
        input_manager = (android.hardware.input.InputManager)
                getSystemService(INPUT_SERVICE);
        if (input_manager == null) {
            return;
        }
        // Map anything already attached before the listener existed.
        ControllerAutoMap.autoMapNewDevices(this);
        load_key_map_and_vibrator();
        input_manager.registerInputDeviceListener(input_device_listener, null);
    }

    void load_key_map_and_vibrator() {
        // Repair maps saved before the trigger/thumb keycodes were corrected.
        ControllerAutoMap.migrateLegacyKeyMap(this);
        final SharedPreferences sPrefs = PreferenceManager.getDefaultSharedPreferences(this);
        keysMap.clear();
        for (int i = 0; i < KeyMapConfig.KEY_NAMEIDS.length; i++) {
            String keyName = Integer.toString(KeyMapConfig.KEY_NAMEIDS[i]);
            int keyCode = sPrefs.getInt(keyName, KeyMapConfig.DEFAULT_KEYMAPPERS[i]);
            keysMap.put(keyCode, KeyMapConfig.KEY_VALUES[i]);
        }
        if(sPrefs.getBoolean("enable_vibrator",false)){
            vibrator = (Vibrator) getSystemService(VIBRATOR_SERVICE);
            vibrationEffect = VibrationEffect.createOneShot(25, VibrationEffect.DEFAULT_AMPLITUDE);
        }
    }
    @Override
    protected void onCreate(android.os.Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        if(!Application.should_delay_load()){
            on_create();
            return;
        }

        delay_dialog=ProgressTask.create_progress_dialog( this,getString(R.string.loading));
        delay_dialog.show();
        new Thread() {
            @Override
            public void run() {
                try {
                    Thread.sleep(500);
                    Emulator.load_library();
                    Thread.sleep(100);
                    delay_on_create.sendEmptyMessage(DELAY_ON_CREATE);
                } catch (InterruptedException e) {
                    throw new RuntimeException(e);
                }
            }
        }.start();
        return;
    }

    @Override
    public void onBackPressed()
    {
        if(delay_dialog!=null)
            return;

        if(pause_dialog_!=null && pause_dialog_.isShowing())
            return;

        showPauseMenu();
    }

    /** Dolphin-style pause menu: Resume Game / Settings / Exit Game (red). */
    private void showPauseMenu()
    {
        if(Emulator.get.is_running())
            Emulator.get.pause();

        View view=LayoutInflater.from(this).inflate(R.layout.dialog_pause_menu,null);
        ((TextView) view.findViewById(R.id.pause_menu_title))
                .setText(getIntent().getStringExtra(EXTRA_GAME_TITLE));

        final boolean[] suppress_resume_on_dismiss={false};

        pause_dialog_=new AlertDialog.Builder(this)
                .setView(view)
                .setCancelable(true)
                .setOnDismissListener(d->{
                    pause_dialog_=null;
                    if(!suppress_resume_on_dismiss[0] && Emulator.get.is_paused())
                        Emulator.get.resume();
                })
                .create();

        view.findViewById(R.id.row_resume).setOnClickListener(v->pause_dialog_.dismiss());

        view.findViewById(R.id.row_settings).setOnClickListener(v->{
            suppress_resume_on_dismiss[0]=true;
            pause_dialog_.dismiss();
            openGameSettings();
        });

        view.findViewById(R.id.row_exit).setOnClickListener(v->{
            suppress_resume_on_dismiss[0]=true;
            pause_dialog_.dismiss();
            return_to_library();
        });

        pause_dialog_.show();
    }

    /** Opens the existing raw-cvar Settings screen pointed at this game's own
     *  per-title config file (created empty on first use), so changes only
     *  affect this game and are saved automatically when the screen closes -
     *  the native engine already layers <title_id>.config.toml on top of the
     *  global config on boot (see config::LoadGameConfig in the engine). */
    private void openGameSettings()
    {
        String title_id=game_title_id_;
        if(title_id==null && game_uri_!=null){
            // Rare fallback: caller didn't already know the title ID (e.g. launched
            // via a raw VIEW intent rather than the library grid) - derive it now.
            title_id=GameScanner.peekTitleId(this,Uri.parse(game_uri_));
            game_title_id_=title_id;
        }
        if(title_id==null){
            Toast.makeText(this,"Couldn't identify this game yet - try again shortly.",
                    Toast.LENGTH_SHORT).show();
            return;
        }

        File config_file=Application.ensure_game_config_file(title_id);

        Intent intent=new Intent(this,EmulatorSettings.class);
        intent.putExtra(EmulatorSettings.EXTRA_CONFIG_PATH,config_file.getAbsolutePath());
        intent.putExtra(EmulatorSettings.EXTRA_GAME_TITLE,getIntent().getStringExtra(EXTRA_GAME_TITLE));
        returning_from_game_settings_=true;
        startActivity(intent);
    }

    @Override
    protected void onResume()
    {
        super.onResume();
        start_controller_detection();
        request_sustained_performance();
        start_performance_hints();
        if(returning_from_game_settings_){
            returning_from_game_settings_=false;
            // Still paused from before - reopen the pause menu so the user
            // explicitly chooses Resume rather than snapping back into gameplay.
            showPauseMenu();
        }
    }

    /**
     * Tells the user the game file is gone, instead of stalling on a black
     * screen. Silent failure here previously read as "the emulator is broken".
     */
    private void show_missing_game_dialog(String title) {
        new AlertDialog.Builder(this)
                .setTitle("Can't open game")
                .setMessage((title == null ? "This game" : title)
                        + " couldn't be opened. Its file may have been moved,"
                        + " renamed, or re-indexed by Android.\n\n"
                        + "Refreshing your library usually fixes this.")
                .setCancelable(false)
                .setPositiveButton("Refresh library", (d, w) -> {
                    Intent i = new Intent(this, MainActivity.class);
                    i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK
                            | Intent.FLAG_ACTIVITY_CLEAR_TOP);
                    i.putExtra(MainActivity.EXTRA_REFRESH_LIBRARY, true);
                    startActivity(i);
                    finish();
                })
                .setNegativeButton("Back", (d, w) -> return_to_library())
                .show();
    }

    /**
     * Leaves a game and returns to the library.
     *
     * <p>The emulator runs in its own {@code :emu} process and {@link #onDestroy}
     * kills that process outright, because the native core cannot be torn down
     * and re-initialised in place - a second game launched into the same process
     * would fail. That kill is deliberate and has to stay.
     *
     * <p>What was missing is that nothing brought the library back first, so
     * exiting a game killed the only visible process and dropped the user on the
     * home screen instead of the game list. Starting MainActivity here hands the
     * intent to the system before we exit, so it comes up in the main process
     * whether or not that process is still alive - if Android reclaimed it while
     * the emulator was using its memory, this relaunches it.
     */
    private void return_to_library() {
        Intent intent = new Intent(this, MainActivity.class);
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK
                | Intent.FLAG_ACTIVITY_CLEAR_TOP
                | Intent.FLAG_ACTIVITY_SINGLE_TOP);
        startActivity(intent);
        finish();
    }

    @Override
    protected void onDestroy()
    {
        if (fps_handler != null) fps_handler.removeCallbacks(fps_updater);
        if (overlay_bg_handler != null) overlay_bg_handler.removeCallbacks(overlay_updater);
        if (overlay_thread != null) overlay_thread.quitSafely();
        if (precache_handler != null) precache_handler.removeCallbacksAndMessages(null);
        if (input_manager != null) {
            input_manager.unregisterInputDeviceListener(input_device_listener);
            input_manager = null;
        }
        PerformanceHints.stop();
        super.onDestroy();
        System.exit(0);
    }

    final int KEY_NO_MAPPED = -1;

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        int gameKey = keysMap.get(keyCode, KEY_NO_MAPPED);
        if (gameKey == KEY_NO_MAPPED) return super.onKeyDown(keyCode, event);
        if (event.getRepeatCount() == 0){
            vibrator();
            Emulator.get.key_event(gameKey, true,VirtualControl.KEY_VALUE_UNUSED);
            return true;
        }
        return super.onKeyDown(keyCode, event);
    }

    @Override
    public boolean onKeyUp(int keyCode, KeyEvent event) {
        int gameKey = keysMap.get(keyCode, KEY_NO_MAPPED);
        if (gameKey != KEY_NO_MAPPED) {
            Emulator.get.key_event(gameKey, false,VirtualControl.KEY_VALUE_UNUSED);
            return true;
        }
        return super.onKeyUp(keyCode, event);
    }
    @Override
    public void surfaceCreated(@NonNull SurfaceHolder holder) {

        if(!started){
            started=true;

            Emulator.get.setup_surface(holder.getSurface());
            try {
                Emulator.get.boot();
            } catch (org.xeniaae.emulator.Emulator.BootException e) {
                throw new RuntimeException(e);
            }
        }
        else{
            Emulator.get.setup_surface(holder.getSurface());
            // Don't auto-resume if the pause menu (or the Settings screen launched
            // from it) is the reason we're paused - only the user's explicit
            // "Resume Game" tap should do that in that case.
            if(Emulator.get.is_paused() && pause_dialog_==null && !returning_from_game_settings_)
                Emulator.get.resume();
        }


    }

    @Override
    public void surfaceChanged(@NonNull SurfaceHolder holder, int format, int width, int height) {
        if(!started) return;
        if(width==0||height==0) return;
        Emulator.get.change_surface(width,height);
    }

    @Override
    public void surfaceDestroyed(@NonNull SurfaceHolder holder) {
        if(!started) return;
        Emulator.get.setup_surface(null);
    }


    boolean handle_dpad(InputEvent event) {

        boolean pressed=false;
        if (event instanceof MotionEvent) {

            // Use the hat axis value to find the D-pad direction
            MotionEvent motionEvent = (MotionEvent) event;
            float xaxis = motionEvent.getAxisValue(MotionEvent.AXIS_HAT_X);
            float yaxis = motionEvent.getAxisValue(MotionEvent.AXIS_HAT_Y);

            // Check if the AXIS_HAT_X value is -1 or 1, and set the D-pad
            // LEFT and RIGHT direction accordingly.
            if (Float.compare(xaxis, -1.0f) == 0) {
                Emulator.get.key_event(VirtualControl.KEY_CODE_DPAD_LEFT, true,VirtualControl.KEY_VALUE_UNUSED);
                Emulator.get.key_event(VirtualControl.KEY_CODE_DPAD_RIGHT, false,VirtualControl.KEY_VALUE_UNUSED);
                vibrator();
                pressed=true;
            } else if (Float.compare(xaxis, 1.0f) == 0) {
                Emulator.get.key_event( VirtualControl.KEY_CODE_DPAD_RIGHT, true,VirtualControl.KEY_VALUE_UNUSED);
                Emulator.get.key_event( VirtualControl.KEY_CODE_DPAD_LEFT, false,VirtualControl.KEY_VALUE_UNUSED);

                vibrator();
                pressed=true;
            }
            // Check if the AXIS_HAT_Y value is -1 or 1, and set the D-pad
            // UP and DOWN direction accordingly.
            if (Float.compare(yaxis, -1.0f) == 0) {
                Emulator.get.key_event(  VirtualControl.KEY_CODE_DPAD_UP, true,VirtualControl.KEY_VALUE_UNUSED);
                Emulator.get.key_event(VirtualControl.KEY_CODE_DPAD_DOWN, false,VirtualControl.KEY_VALUE_UNUSED);

                vibrator();
                pressed=true;
            } else if (Float.compare(yaxis, 1.0f) == 0) {
                Emulator.get.key_event(VirtualControl.KEY_CODE_DPAD_DOWN, true,VirtualControl.KEY_VALUE_UNUSED);
                Emulator.get.key_event(VirtualControl.KEY_CODE_DPAD_UP, false,VirtualControl.KEY_VALUE_UNUSED);

                vibrator();
                pressed=true;
            }
        }
        else if (event instanceof KeyEvent) {

            // Use the key code to find the D-pad direction.
            KeyEvent keyEvent = (KeyEvent) event;
            if (keyEvent.getKeyCode() == KeyEvent.KEYCODE_DPAD_LEFT) {
                Emulator.get.key_event(VirtualControl.KEY_CODE_DPAD_LEFT, true,VirtualControl.KEY_VALUE_UNUSED);
                Emulator.get.key_event(VirtualControl.KEY_CODE_DPAD_RIGHT, false,VirtualControl.KEY_VALUE_UNUSED);

                vibrator();
                pressed=true;

            } else if (keyEvent.getKeyCode() == KeyEvent.KEYCODE_DPAD_RIGHT) {
                Emulator.get.key_event(VirtualControl.KEY_CODE_DPAD_RIGHT, true,VirtualControl.KEY_VALUE_UNUSED);
                Emulator.get.key_event(VirtualControl.KEY_CODE_DPAD_LEFT, false,VirtualControl.KEY_VALUE_UNUSED);

                vibrator();
                pressed=true;

            } else if (keyEvent.getKeyCode() == KeyEvent.KEYCODE_DPAD_UP) {
                Emulator.get.key_event(VirtualControl.KEY_CODE_DPAD_UP, true,VirtualControl.KEY_VALUE_UNUSED);
                Emulator.get.key_event(VirtualControl.KEY_CODE_DPAD_DOWN, false,VirtualControl.KEY_VALUE_UNUSED);

                vibrator();
                pressed=true;

            } else if (keyEvent.getKeyCode() == KeyEvent.KEYCODE_DPAD_DOWN) {
                Emulator.get.key_event(VirtualControl.KEY_CODE_DPAD_DOWN, true,VirtualControl.KEY_VALUE_UNUSED);
                Emulator.get.key_event(VirtualControl.KEY_CODE_DPAD_UP, false,VirtualControl.KEY_VALUE_UNUSED);

                vibrator();
                pressed=true;

            }
        }

        if(pressed) return true;
        Emulator.get.key_event(VirtualControl.KEY_CODE_DPAD_LEFT, false,VirtualControl.KEY_VALUE_UNUSED);
        Emulator.get.key_event(VirtualControl.KEY_CODE_DPAD_UP, false,VirtualControl.KEY_VALUE_UNUSED);
        Emulator.get.key_event(VirtualControl.KEY_CODE_DPAD_RIGHT, false,VirtualControl.KEY_VALUE_UNUSED);
        Emulator.get.key_event(VirtualControl.KEY_CODE_DPAD_DOWN, false,VirtualControl.KEY_VALUE_UNUSED);
        return false;
    }


    private static boolean isDpadDevice(MotionEvent event) {
        // Check that input comes from a device with directional pads.
        if ((event.getSource() & InputDevice.SOURCE_DPAD)
                != InputDevice.SOURCE_DPAD) {
            return true;
        } else {
            return false;
        }
    }

    @Override
    public boolean onGenericMotion(View v, MotionEvent event) {

        if(isDpadDevice(event)&& handle_dpad(event)) return true;

        if ((event.getSource() & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK/*&&
			event.getAction() == MotionEvent.ACTION_MOVE*/) {
            float laxisX = event.getAxisValue(MotionEvent.AXIS_X);
            float laxisY = event.getAxisValue(MotionEvent.AXIS_Y);
            float raxisX = event.getAxisValue(MotionEvent.AXIS_Z);
            float raxisY = event.getAxisValue(MotionEvent.AXIS_RZ);

            final short _0=0;

            // Analog triggers. These were not handled at all, so on any pad that
            // reports triggers as AXES rather than as BUTTON_L2/R2 keycodes -
            // which is most of them, Xbox pads included - the triggers simply
            // did nothing no matter how they were mapped.
            //
            // Two axis pairs exist in the wild: LTRIGGER/RTRIGGER and
            // BRAKE/GAS. Pads report one or the other, so take whichever is
            // larger rather than guessing per device.
            {
                float lt = Math.max(event.getAxisValue(MotionEvent.AXIS_LTRIGGER),
                        event.getAxisValue(MotionEvent.AXIS_BRAKE));
                float rt = Math.max(event.getAxisValue(MotionEvent.AXIS_RTRIGGER),
                        event.getAxisValue(MotionEvent.AXIS_GAS));
                // Small deadzone: resting triggers often float a little above 0
                // and would otherwise hold a permanent light throttle.
                final float TRIGGER_DEADZONE = 0.06f;
                if (lt > TRIGGER_DEADZONE) {
                    Emulator.get.key_event(VirtualControl.KEY_CODE_TRIGGER_L, true,
                            (short) Math.min(255, (int) (lt * 255.0f)));
                } else {
                    Emulator.get.key_event(VirtualControl.KEY_CODE_TRIGGER_L, false, _0);
                }
                if (rt > TRIGGER_DEADZONE) {
                    Emulator.get.key_event(VirtualControl.KEY_CODE_TRIGGER_R, true,
                            (short) Math.min(255, (int) (rt * 255.0f)));
                } else {
                    Emulator.get.key_event(VirtualControl.KEY_CODE_TRIGGER_R, false, _0);
                }
            }

            //左摇杆
            {
                if(laxisX!=0){
                    if(laxisX<0){
                        Emulator.get.key_event(VirtualControl.KEY_CODE_LTHUMB_RIGHT,false,_0);
                        Emulator.get.key_event(VirtualControl.KEY_CODE_LTHUMB_LEFT,true,(short) (laxisX*32768.0f));
                    }
                    else{
                        Emulator.get.key_event(VirtualControl.KEY_CODE_LTHUMB_LEFT,false,_0);
                        Emulator.get.key_event(VirtualControl.KEY_CODE_LTHUMB_RIGHT,true,(short)(Math.abs(laxisX)*32767.0f));
                    }
                }
                else{
                    Emulator.get.key_event(VirtualControl.KEY_CODE_LTHUMB_RIGHT,false,_0);
                    Emulator.get.key_event(VirtualControl.KEY_CODE_LTHUMB_LEFT,false,_0);
                }

                //Joystick 左上角为-1.0,-1.0
                //X360左下角为 -32768,-32768
                if(laxisY!=0){
                    if(laxisY<0){
                        Emulator.get.key_event(VirtualControl.KEY_CODE_LTHUMB_DOWN,false,_0);
                        Emulator.get.key_event(VirtualControl.KEY_CODE_LTHUMB_UP,true,(short)(-laxisY*32767.0f));
                    }else{
                        Emulator.get.key_event(VirtualControl.KEY_CODE_LTHUMB_UP,false,_0);
                        Emulator.get.key_event(VirtualControl.KEY_CODE_LTHUMB_DOWN,true,(short)(-laxisY*32768.0f));
                    }
                }
                else{
                    Emulator.get.key_event(VirtualControl.KEY_CODE_LTHUMB_DOWN,false,_0);
                    Emulator.get.key_event(VirtualControl.KEY_CODE_LTHUMB_UP,false,_0);
                }
            }
            //右摇杆
            {
                if(raxisX!=0){
                    if(raxisX<0){
                        Emulator.get.key_event(VirtualControl.KEY_CODE_RTHUMB_RIGHT,false,_0);
                        Emulator.get.key_event(VirtualControl.KEY_CODE_RTHUMB_LEFT,true,(short)(raxisX*32768.f));
                    }else{
                        Emulator.get.key_event(VirtualControl.KEY_CODE_RTHUMB_LEFT,false,_0);
                        Emulator.get.key_event(VirtualControl.KEY_CODE_RTHUMB_RIGHT,true,(short)(raxisX*32767.0f));
                    }
                }
                else{
                    Emulator.get.key_event(VirtualControl.KEY_CODE_RTHUMB_RIGHT,false,_0);
                    Emulator.get.key_event(VirtualControl.KEY_CODE_RTHUMB_LEFT,false,_0);
                }

                //Joystick 左上角为-1.0,-1.0
                //X360左下角为 -32768,-32768
                if(raxisY!=0){
                    if(raxisY<0){
                        Emulator.get.key_event(VirtualControl.KEY_CODE_RTHUMB_DOWN,false,_0);
                        Emulator.get.key_event(VirtualControl.KEY_CODE_RTHUMB_UP,true,(short)(-raxisY*32767.0f));
                    }else{
                        Emulator.get.key_event(VirtualControl.KEY_CODE_RTHUMB_UP,false,_0);
                        Emulator.get.key_event(VirtualControl.KEY_CODE_RTHUMB_DOWN,true,(short)(-raxisY*32768.0f));
                    }
                }
                else{
                    Emulator.get.key_event(VirtualControl.KEY_CODE_RTHUMB_DOWN,false,_0);
                    Emulator.get.key_event(VirtualControl.KEY_CODE_RTHUMB_UP,false,_0);
                }
            }
            return true;
        }

        return super.onGenericMotionEvent(event);
    }

}
