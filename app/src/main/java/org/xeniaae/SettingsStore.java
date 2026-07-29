package org.xeniaae;

import android.content.Context;

import androidx.preference.PreferenceManager;

/**
 * Shared access to the emulator's global .toml config and app preferences.
 *
 * Extracted from SettingsFragment so the top-level Settings screen and the
 * per-category submenus (SettingsCategoryActivity) read and write settings
 * through one implementation instead of duplicating it.
 *
 * The config file is opened and closed per call rather than held open, matching
 * the original behaviour - the native side also writes it, and a long-lived
 * handle risks clobbering those writes.
 */
public final class SettingsStore {

    private SettingsStore() {}

    public static String readString(String key) {
        try {
            final Emulator.Config config = Emulator.Config.open_config_file(
                    Application.get_global_config_file().getAbsolutePath());
            final String val = config.load_config_entry(key);
            config.close_config_file();
            return val;
        } catch (Exception e) {
            return null;
        }
    }

    public static void writeString(String key, String value) {
        try {
            final Emulator.Config config = Emulator.Config.open_config_file(
                    Application.get_global_config_file().getAbsolutePath());
            config.save_config_entry(key, value);
            config.close_config_file();
        } catch (Exception ignored) {
        }
    }

    public static boolean readBool(String key, boolean defaultValue) {
        final String val = readString(key);
        return val != null ? Boolean.parseBoolean(val) : defaultValue;
    }

    public static void writeBool(String key, boolean value) {
        writeString(key, Boolean.toString(value));
    }

    public static int readInt(String key, int defaultValue) {
        final String val = readString(key);
        if (val == null) {
            return defaultValue;
        }
        try {
            return Integer.parseInt(val.trim());
        } catch (NumberFormatException e) {
            return defaultValue;
        }
    }

    public static void writeInt(String key, int value) {
        writeString(key, Integer.toString(value));
    }

    // ---------------------------------------------------------------- labels

    public static final String[] RESOLUTION_LABELS =
            {"480p", "600p", "720p", "900p", "1080p"};
    public static final String[] RESOLUTION_VALUES =
            {"848x480", "1066x600", "1280x720", "1600x900", "1920x1080"};

    public static String currentResolutionLabel() {
        final String val = readString("Video|internal_display_resolution");
        for (int i = 0; i < RESOLUTION_VALUES.length; i++) {
            if (RESOLUTION_VALUES[i].equals(val)) {
                return RESOLUTION_LABELS[i];
            }
        }
        return RESOLUTION_LABELS[2];  // 720p default
    }

    public static String currentDriverLabel(Context ctx) {
        return Utils.driver_display_name_for_path(ctx, readString("Vulkan|vulkan_lib_path"));
    }

    // ------------------------------------------------- post-processing options

    /**
     * Supersampling: the game is rendered at N x resolution and downsampled,
     * which is Xenia's actual anti-aliasing mechanism. Backed by
     * GPU|draw_resolution_scale_x / _y (texture_cache.cc), whose own
     * documentation states only 1, 2 and 3 are broadly supported - support above
     * 1x depends on device properties (sparse binding / tiled resources), so it
     * can silently clamp on some GPUs.
     */
    public static final String[] RES_SCALE_LABELS =
            {"Off (1x)", "2x (4x pixels)", "3x (9x pixels)"};
    public static final int[] RES_SCALE_VALUES = {1, 2, 3};

    public static int currentResScale() {
        return readInt("GPU|draw_resolution_scale_x", 1);
    }

    public static String currentResScaleLabel() {
        final int v = currentResScale();
        for (int i = 0; i < RES_SCALE_VALUES.length; i++) {
            if (RES_SCALE_VALUES[i] == v) {
                return RES_SCALE_LABELS[i];
            }
        }
        return RES_SCALE_LABELS[0];
    }

    /** Both axes are written together - this is supersampling, not axis scaling. */
    public static void writeResScale(int scale) {
        writeInt("GPU|draw_resolution_scale_x", scale);
        writeInt("GPU|draw_resolution_scale_y", scale);
    }

    /**
     * Anisotropic filtering override. NOTE the stored value is an ENUM INDEX,
     * not the multiplier: -1 = no override, 0 = disable, 1 = 1x, 2 = 2x,
     * 3 = 4x, 4 = 8x, 5 = 16x (see anisotropic_override in gpu_flags.cc).
     */
    public static final String[] AF_LABELS =
            {"Auto (game default)", "Off", "1x", "2x", "4x", "8x", "16x"};
    public static final int[] AF_VALUES = {-1, 0, 1, 2, 3, 4, 5};

    public static String currentAfLabel() {
        final int v = readInt("GPU|anisotropic_override", -1);
        for (int i = 0; i < AF_VALUES.length; i++) {
            if (AF_VALUES[i] == v) {
                return AF_LABELS[i];
            }
        }
        return AF_LABELS[0];
    }

    // ------------------------------------------------------------- app prefs

    public static boolean getPrefBool(Context ctx, String key, boolean def) {
        return PreferenceManager.getDefaultSharedPreferences(ctx).getBoolean(key, def);
    }

    public static void setPrefBool(Context ctx, String key, boolean value) {
        PreferenceManager.getDefaultSharedPreferences(ctx)
                .edit().putBoolean(key, value).apply();
    }
}
