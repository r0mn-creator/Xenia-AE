package org.xeniaae;

import android.content.Context;
import android.content.SharedPreferences;

import java.io.File;

/**
 * Per-game GPU driver overrides, keyed by Title ID.
 *
 * WHY THIS IS NOT STORED IN THE PER-GAME CONFIG TOML
 * --------------------------------------------------
 * Xenia does support per-game config files (config/<TITLEID>.config.toml, loaded
 * by config::LoadGameConfig from emulator.cc), but the GPU driver CANNOT live
 * there: `vulkan_lib_path` is consumed when the Vulkan instance is created during
 * Emulator::Setup (emulator.cc:325), whereas the per-game config isn't read until
 * CompleteLaunch (emulator.cc:1688) - a whole phase later. The driver is already
 * loaded by then, so a per-game TOML entry would be silently ignored.
 *
 * Instead the choice is kept app-side and injected as a LAUNCH ARGUMENT
 * (--vulkan_lib_path=...) in EmulatorActivity, before the emulator starts.
 * Xenia's cvar precedence (base/cvar.h ConfigVar::UpdateValue) is:
 *     commandline_value > game_config_value > config_value
 * so a launch arg overrides both config files, and the global config is never
 * mutated - the Settings UI keeps showing the user's real global default.
 */
public final class GameDriverStore {

    private static final String PREFS = "xenia_prefs";
    private static final String KEY_PREFIX = "game_driver_";

    /** Stored for "use whatever the global setting is" - i.e. no override. */
    public static final String USE_GLOBAL = "";
    /** Stored to force the device's built-in driver for this game specifically. */
    public static final String FORCE_DEFAULT = "default";

    private GameDriverStore() {}

    private static SharedPreferences prefs(Context ctx) {
        return ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    private static String key(String titleId) {
        return KEY_PREFIX + titleId.toUpperCase();
    }

    /**
     * @return the override for this title: "" = follow global, "default" = force
     *         the built-in driver, otherwise an absolute path to a driver .so.
     */
    public static String get(Context ctx, String titleId) {
        if (titleId == null) {
            return USE_GLOBAL;
        }
        return prefs(ctx).getString(key(titleId), USE_GLOBAL);
    }

    public static void set(Context ctx, String titleId, String value) {
        if (titleId == null) {
            return;
        }
        prefs(ctx).edit().putString(key(titleId), value).apply();
    }

    public static boolean hasOverride(Context ctx, String titleId) {
        return !USE_GLOBAL.equals(get(ctx, titleId));
    }

    /**
     * The value to pass as --vulkan_lib_path, or null when this title should just
     * follow the global setting (in which case no launch arg is added at all).
     */
    public static String launchArgValue(Context ctx, String titleId) {
        final String v = get(ctx, titleId);
        if (USE_GLOBAL.equals(v)) {
            return null;
        }
        if (FORCE_DEFAULT.equals(v)) {
            return "default";
        }
        // Stale path (driver deleted since it was chosen) - fall back to global
        // rather than handing the emulator a path that no longer exists.
        return new File(v).exists() ? v : null;
    }

    /** Human-readable label for the current override, for menus/subtitles. */
    public static String label(Context ctx, String titleId) {
        final String v = get(ctx, titleId);
        if (USE_GLOBAL.equals(v)) {
            return ctx.getString(R.string.game_driver_use_global);
        }
        if (FORCE_DEFAULT.equals(v)) {
            return ctx.getString(R.string.game_driver_force_default);
        }
        return Utils.driver_display_name_for_path(ctx, v);
    }
}
