package org.xeniaae;

import android.app.AlertDialog;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.preference.PreferenceManager;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.TextView;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.appcompat.widget.SwitchCompat;
import androidx.fragment.app.Fragment;

import java.io.File;

/**
 * The curated Settings tab: Dark Mode, Refresh Game List, Video (Resolution,
 * Custom Driver, Frame Rate, Overlay), Audio, plus the pre-existing Xenia-AE
 * screens (Key Mappers, Virtual Pad Edit, Open File Manager, Advanced
 * Settings, About) folded in under "More".
 */
public class SettingsFragment extends Fragment {

    private static final String[] RESOLUTION_LABELS = {"360p", "480p", "720p", "1080p"};
    // Indices into es_arr_v_video_internal_display_resolution / es_arr_video_internal_display_resolution
    // (640x480, 848x480, 1280x720, 1920x1080 — closest matches Xenia actually offers).
    private static final String[] RESOLUTION_VALUES = {"0", "5", "8", "16"};

    @Nullable
    @Override
    public View onCreateView(@NonNull LayoutInflater inflater,
                             @Nullable ViewGroup container,
                             @Nullable Bundle savedInstanceState) {
        return inflater.inflate(R.layout.fragment_settings, container, false);
    }

    @Override
    public void onViewCreated(@NonNull View view, @Nullable Bundle savedInstanceState) {
        setupSwitchRow(view, R.id.row_dark_mode, getString(R.string.dark_mode), null,
                Application.is_dark_mode_enabled(requireContext()),
                checked -> Application.set_dark_mode_enabled(requireContext(), checked));

        setupClickRow(view, R.id.row_refresh_list, getString(R.string.settings_refresh_game_list), null,
                v -> ((MainActivity) requireActivity()).refreshGameList());

        setupClickRow(view, R.id.row_resolution, getString(R.string.settings_resolution),
                currentResolutionLabel(), v -> showResolutionPicker(view));

        setupClickRow(view, R.id.row_custom_driver, getString(R.string.settings_custom_driver),
                currentDriverLabel(), v -> showDriverPicker(view));

        final boolean vsyncOn = readGlobalBool("GPU|vsync", true);
        setupSwitchRow(view, R.id.row_frame_rate, getString(R.string.settings_frame_rate),
                vsyncOn ? getString(R.string.settings_frame_rate_30) : getString(R.string.settings_frame_rate_60),
                !vsyncOn,
                checked -> writeGlobalBool("GPU|vsync", !checked));

        final SharedPrefsHelper prefs = new SharedPrefsHelper(requireContext());
        setupSwitchRow(view, R.id.row_overlay, getString(R.string.settings_overlay),
                getString(R.string.settings_overlay_subtitle),
                prefs.getBoolean("show_status_overlay", false),
                prefs::putBoolean_showStatusOverlay);

        setupSwitchRow(view, R.id.row_audio, getString(R.string.settings_audio_driver), null,
                !readGlobalBool("APU|mute", false),
                checked -> writeGlobalBool("APU|mute", !checked));

        setupClickRow(view, R.id.row_key_mappers, getString(R.string.key_mappers), null,
                v -> startActivity(new Intent(requireContext(), KeyMapActivity.class)));

        setupClickRow(view, R.id.row_virtual_pad_edit, getString(R.string.virtual_pad_edit), null,
                v -> startActivity(new Intent(requireContext(), VirtualControlEdit.class)));

        setupClickRow(view, R.id.row_open_file_mgr, getString(R.string.open_file_manager), null,
                v -> MainActivity.open_file_manager(requireActivity()));

        setupClickRow(view, R.id.row_advanced_settings, getString(R.string.settings_advanced),
                getString(R.string.settings_advanced_subtitle),
                v -> startActivity(new Intent(requireContext(), EmulatorSettings.class)));

        setupClickRow(view, R.id.row_about, getString(R.string.about), null,
                v -> startActivity(new Intent(requireContext(), AboutActivity.class)));
    }

    // ---------------------------------------------------------------------
    // Row helpers
    // ---------------------------------------------------------------------

    private interface OnCheckedChanged { void onChanged(boolean checked); }
    private interface OnRowClick { void onClick(View v); }

    private void setupSwitchRow(View root, int includeId, String title, @Nullable String subtitle,
                                 boolean checked, OnCheckedChanged listener) {
        final View row = root.findViewById(includeId);
        ((TextView) row.findViewById(R.id.row_title)).setText(title);
        setSubtitleView(row, subtitle);
        final SwitchCompat sw = row.findViewById(R.id.row_switch);
        // Each switch row is inflated from the same settings_row_switch.xml include,
        // so every instance shares the literal id row_switch. Android's automatic
        // view-instance-state save/restore is keyed by id, which can restore a stale
        // checked value onto the wrong row (or clobber a value we just set) across an
        // activity relaunch. We already fully manage checked state from the real
        // source of truth (prefs/config) on every call, so disable that mechanism.
        sw.setSaveEnabled(false);
        sw.setChecked(checked);
        sw.setOnCheckedChangeListener((buttonView, isChecked) -> listener.onChanged(isChecked));
        row.setOnClickListener(v -> sw.toggle());
    }

    private void setupClickRow(View root, int includeId, String title, @Nullable String subtitle,
                                OnRowClick listener) {
        final View row = root.findViewById(includeId);
        ((TextView) row.findViewById(R.id.row_title)).setText(title);
        setSubtitleView(row, subtitle);
        row.setOnClickListener(listener::onClick);
    }

    private void setRowSubtitle(View root, int includeId, String subtitle) {
        setSubtitleView(root.findViewById(includeId), subtitle);
    }

    private void setSubtitleView(View row, @Nullable String subtitle) {
        final TextView subtitleView = row.findViewById(R.id.row_subtitle);
        if (subtitle == null) {
            subtitleView.setVisibility(View.GONE);
        } else {
            subtitleView.setText(subtitle);
            subtitleView.setVisibility(View.VISIBLE);
        }
    }

    // ---------------------------------------------------------------------
    // Resolution
    // ---------------------------------------------------------------------

    private String currentResolutionLabel() {
        final String val = readGlobalString("Video|internal_display_resolution");
        for (int i = 0; i < RESOLUTION_VALUES.length; i++) {
            if (RESOLUTION_VALUES[i].equals(val)) return RESOLUTION_LABELS[i];
        }
        return RESOLUTION_LABELS[2]; // 720p default
    }

    private void showResolutionPicker(View root) {
        new AlertDialog.Builder(requireContext())
                .setTitle(R.string.settings_resolution)
                .setItems(RESOLUTION_LABELS, (dialog, which) -> {
                    writeGlobalString("Video|internal_display_resolution", RESOLUTION_VALUES[which]);
                    setRowSubtitle(root, R.id.row_resolution, RESOLUTION_LABELS[which]);
                })
                .show();
    }

    // ---------------------------------------------------------------------
    // Custom driver (from file) — "from net" is a larger, separate feature
    // (community driver repo integration) and is not implemented here.
    // ---------------------------------------------------------------------

    private static final int REQUEST_CUSTOM_DRIVER = 9001;

    private String currentDriverLabel() {
        final String val = readGlobalString("Vulkan|vulkan_lib_path");
        if (val == null || val.isEmpty() || val.equals("default")) return getString(R.string._default);
        final File f = new File(val);
        return f.getName();
    }

    private void showDriverPicker(View root) {
        final File[] files = Application.get_custom_driver_dir().listFiles();
        final int installedCount = files == null ? 0 : files.length;
        final String[] items = new String[installedCount + 2];
        items[0] = getString(R.string._default);
        for (int i = 0; i < installedCount; i++) items[i + 1] = files[i].getName();
        items[installedCount + 1] = getString(R.string.driver_library_path_dialog_add_hint);

        new AlertDialog.Builder(requireContext())
                .setTitle(R.string.settings_custom_driver)
                .setItems(items, (dialog, which) -> {
                    if (which == 0) {
                        writeGlobalString("Vulkan|vulkan_lib_path", "default");
                        setRowSubtitle(root, R.id.row_custom_driver, getString(R.string._default));
                    } else if (which == installedCount + 1) {
                        requestSelectCustomDriverFile();
                    } else {
                        final File f = files[which - 1];
                        writeGlobalString("Vulkan|vulkan_lib_path", f.getAbsolutePath());
                        setRowSubtitle(root, R.id.row_custom_driver, f.getName());
                    }
                })
                .show();
    }

    private void requestSelectCustomDriverFile() {
        final Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        startActivityForResult(intent, REQUEST_CUSTOM_DRIVER);
    }

    @Override
    public void onActivityResult(int requestCode, int resultCode, @Nullable Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != REQUEST_CUSTOM_DRIVER || resultCode != android.app.Activity.RESULT_OK || data == null) return;
        final Uri uri = data.getData();
        if (uri == null) return;
        final String fileName = Utils.getFileNameFromUri(uri);
        if (fileName != null && fileName.endsWith(".zip")) {
            Utils.install_custom_driver_from_zip(requireActivity(), uri, path -> {
                writeGlobalString("Vulkan|vulkan_lib_path", path);
                setRowSubtitle(requireView(), R.id.row_custom_driver, new File(path).getName());
            });
        }
    }

    // ---------------------------------------------------------------------
    // Global (.toml) config access — opened/closed per call, not held open.
    // ---------------------------------------------------------------------

    private String readGlobalString(String key) {
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

    private void writeGlobalString(String key, String value) {
        try {
            final Emulator.Config config = Emulator.Config.open_config_file(
                    Application.get_global_config_file().getAbsolutePath());
            config.save_config_entry(key, value);
            config.close_config_file();
        } catch (Exception ignored) {
        }
    }

    private boolean readGlobalBool(String key, boolean defaultValue) {
        final String val = readGlobalString(key);
        return val != null ? Boolean.parseBoolean(val) : defaultValue;
    }

    private void writeGlobalBool(String key, boolean value) {
        writeGlobalString(key, Boolean.toString(value));
    }

    /** Small wrapper so a method reference can target a specific SharedPreferences key. */
    private static class SharedPrefsHelper {
        private final android.content.SharedPreferences prefs;
        SharedPrefsHelper(android.content.Context ctx) {
            prefs = PreferenceManager.getDefaultSharedPreferences(ctx);
        }
        boolean getBoolean(String key, boolean def) { return prefs.getBoolean(key, def); }
        void putBoolean_showStatusOverlay(boolean value) {
            prefs.edit().putBoolean("show_status_overlay", value).apply();
        }
    }
}
