package org.xeniaae;

import android.app.AlertDialog;
import android.content.Intent;
import android.os.Bundle;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import androidx.annotation.Nullable;
import androidx.appcompat.app.AppCompatActivity;
import androidx.appcompat.widget.SwitchCompat;

/**
 * One screen per settings category, reached from the top-level Settings list.
 *
 * The top level shows only category rows (Video, Audio, Input, Advanced, ...);
 * tapping one opens this activity with EXTRA_CATEGORY and it builds that
 * category's rows. Rows are inflated from the same settings_row_click /
 * settings_row_switch layouts the main screen uses, so the look stays identical
 * and there is a single place to change row styling.
 *
 * Adding a setting = add one line to the relevant build* method below.
 */
public class SettingsCategoryActivity extends AppCompatActivity {

    public static final String EXTRA_CATEGORY = "category";
    public static final String CAT_VIDEO = "video";
    public static final String CAT_AUDIO = "audio";
    public static final String CAT_INPUT = "input";
    public static final String CAT_ADVANCED = "advanced";
    public static final String CAT_COVERART = "coverart";

    private LinearLayout container;
    private LayoutInflater inflater;

    @Override
    protected void onCreate(@Nullable Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        final String category = getIntent().getStringExtra(EXTRA_CATEGORY);
        setTitle(titleFor(category));

        inflater = LayoutInflater.from(this);
        final ScrollView scroll = new ScrollView(this);
        container = new LinearLayout(this);
        container.setOrientation(LinearLayout.VERTICAL);
        scroll.addView(container, new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        setContentView(scroll);

        if (CAT_VIDEO.equals(category)) {
            buildVideo();
        } else if (CAT_AUDIO.equals(category)) {
            buildAudio();
        } else if (CAT_INPUT.equals(category)) {
            buildInput();
        } else if (CAT_ADVANCED.equals(category)) {
            buildAdvanced();
        } else if (CAT_COVERART.equals(category)) {
            buildCoverArt();
        }
    }

    private String titleFor(String category) {
        if (CAT_VIDEO.equals(category)) return getString(R.string.settings_section_video);
        if (CAT_AUDIO.equals(category)) return getString(R.string.settings_section_audio);
        if (CAT_INPUT.equals(category)) return getString(R.string.key_mappers);
        if (CAT_ADVANCED.equals(category)) return getString(R.string.settings_advanced);
        if (CAT_COVERART.equals(category)) return getString(R.string.settings_cover_art);
        return getString(R.string.settings);
    }

    // --------------------------------------------------------------- content

    private void buildVideo() {
        final View resolutionRow = addClickRow(
                getString(R.string.settings_resolution),
                SettingsStore.currentResolutionLabel(), null);
        resolutionRow.setOnClickListener(v -> new AlertDialog.Builder(this)
                .setTitle(R.string.settings_resolution)
                .setItems(SettingsStore.RESOLUTION_LABELS, (dialog, which) -> {
                    SettingsStore.writeString("Video|internal_display_resolution",
                            SettingsStore.RESOLUTION_VALUES[which]);
                    setSubtitle(resolutionRow, SettingsStore.RESOLUTION_LABELS[which]);
                })
                .show());

        addClickRow(getString(R.string.settings_custom_driver),
                SettingsStore.currentDriverLabel(this),
                v -> startActivity(new Intent(this, DriverSettingsActivity.class)));

        // Config stores vsync; the UI presents it as a 30/60 FPS choice, so the
        // switch is inverted relative to the stored value.
        final boolean vsyncOn = SettingsStore.readBool("GPU|vsync", true);
        addSwitchRow(getString(R.string.settings_frame_rate),
                vsyncOn ? getString(R.string.settings_frame_rate_30)
                        : getString(R.string.settings_frame_rate_60),
                !vsyncOn,
                checked -> SettingsStore.writeBool("GPU|vsync", !checked));

        addSwitchRow(getString(R.string.settings_overlay),
                getString(R.string.settings_overlay_subtitle),
                SettingsStore.getPrefBool(this, "show_status_overlay", false),
                checked -> SettingsStore.setPrefBool(this, "show_status_overlay", checked));

        // ---- Post-Processing -------------------------------------------------
        // Both settings are read once when the GPU/render-target cache is built,
        // so they need a game restart - same as Custom Driver. Both default to
        // OFF/neutral in the engine (draw_resolution_scale=1, anisotropic
        // _override=-1) and nothing is written unless the user picks a value.
        addSectionHeader(getString(R.string.settings_section_postprocess));

        addIntPickerRow(
                getString(R.string.settings_res_scale),
                getString(R.string.settings_res_scale),
                SettingsStore.currentResScaleLabel(),
                SettingsStore.RES_SCALE_LABELS,
                SettingsStore.RES_SCALE_VALUES,
                getString(R.string.settings_restart_required),
                SettingsStore::writeResScale);

        addIntPickerRow(
                getString(R.string.settings_af),
                getString(R.string.settings_af),
                SettingsStore.currentAfLabel(),
                SettingsStore.AF_LABELS,
                SettingsStore.AF_VALUES,
                getString(R.string.settings_restart_required),
                value -> SettingsStore.writeInt("GPU|anisotropic_override", value));
    }

    private void buildAudio() {
        // Config stores "mute"; the UI presents an enable switch, so it inverts.
        addSwitchRow(getString(R.string.settings_audio_driver), null,
                !SettingsStore.readBool("APU|mute", false),
                checked -> SettingsStore.writeBool("APU|mute", !checked));
    }

    private void buildInput() {
        addClickRow(getString(R.string.key_mappers), null,
                v -> startActivity(new Intent(this, KeyMapActivity.class)));
        addClickRow(getString(R.string.virtual_pad_edit), null,
                v -> startActivity(new Intent(this, VirtualControlEdit.class)));
    }

    private void buildAdvanced() {
        addClickRow(getString(R.string.open_file_manager), null,
                v -> MainActivity.open_file_manager(this));

        // Game patches live in <storage_root>/patches/*.patch.toml and are loaded
        // automatically at title boot by the native PatchDB (see patcher/), keyed
        // by title_id + xex hash. Users need to be able to drop files in there, so
        // give them a direct route rather than making them navigate to it.
        addClickRow(getString(R.string.patches_title),
                getString(R.string.patches_manage_subtitle),
                v -> startActivity(new Intent(this, PatchesActivity.class)));

        addClickRow(getString(R.string.settings_open_patches),
                getString(R.string.settings_open_patches_subtitle),
                v -> openPatchesFolder());
        addClickRow(getString(R.string.settings_advanced),
                getString(R.string.settings_advanced_subtitle),
                v -> startActivity(new Intent(this, EmulatorSettings.class)));
    }

    /**
     * Cover art sources, in the order BoxArtManager tries them:
     *   1. custom art the user set on a game (per-game, not configured here)
     *   2. Libretro thumbnails - free, no account needed
     *   3. TheGamesDB - richer coverage, needs a free API key
     * Only step 3 needs configuration, which is what this screen provides. The
     * key was already read from prefs by BoxArtManager but had no UI to set it.
     */
    private void buildCoverArt() {
        final View keyRow = addClickRow(getString(R.string.settings_gamesdb_key),
                apiKeyLabel(), null);
        keyRow.setOnClickListener(v -> showApiKeyDialog(keyRow));

        addClickRow(getString(R.string.settings_gamesdb_get_key),
                getString(R.string.settings_gamesdb_get_key_subtitle),
                v -> {
                    try {
                        startActivity(new Intent(Intent.ACTION_VIEW,
                                android.net.Uri.parse("https://thegamesdb.net/")));
                    } catch (Exception ignored) {
                    }
                });
    }

    private String currentApiKey() {
        return getSharedPreferences("xenia_prefs", MODE_PRIVATE)
                .getString("thegamesdb_api_key", "");
    }

    private String apiKeyLabel() {
        final String key = currentApiKey();
        if (key.isEmpty()) {
            return getString(R.string.settings_gamesdb_key_unset);
        }
        // Never show the whole key back to the user.
        return key.length() <= 4 ? "****"
                : "****" + key.substring(key.length() - 4);
    }

    private void showApiKeyDialog(View row) {
        final android.widget.EditText input = new android.widget.EditText(this);
        input.setSingleLine(true);
        input.setHint(getString(R.string.settings_gamesdb_key));
        input.setText(currentApiKey());
        final int pad = Math.round(16 * getResources().getDisplayMetrics().density);
        final LinearLayout wrap = new LinearLayout(this);
        wrap.setPadding(pad, pad, pad, 0);
        wrap.addView(input);

        new AlertDialog.Builder(this)
                .setTitle(R.string.settings_gamesdb_key)
                .setMessage(getString(R.string.settings_gamesdb_key_help))
                .setView(wrap)
                .setPositiveButton(android.R.string.ok, (d, w) -> {
                    getSharedPreferences("xenia_prefs", MODE_PRIVATE).edit()
                            .putString("thegamesdb_api_key", input.getText().toString().trim())
                            .apply();
                    setSubtitle(row, apiKeyLabel());
                })
                .setNeutralButton(R.string.settings_gamesdb_key_clear, (d, w) -> {
                    getSharedPreferences("xenia_prefs", MODE_PRIVATE).edit()
                            .remove("thegamesdb_api_key").apply();
                    setSubtitle(row, apiKeyLabel());
                })
                .setNegativeButton(android.R.string.cancel, null)
                .show();
    }

    /**
     * Opens the patches folder in the system file manager. The folder is created
     * first if missing, both so the browse target exists and so users have
     * somewhere obvious to copy .patch.toml files into.
     *
     * Uses this app's own DocumentsProvider (document IDs are absolute paths, see
     * DocumentsProvider.getDocIdForFile) to deep-link straight to the folder;
     * falls back to the generic file manager, and finally to showing the path, so
     * the user is never left with a dead button on devices lacking DocumentsUI.
     */
    private void openPatchesFolder() {
        final java.io.File patchesDir =
                new java.io.File(Application.get_app_data_dir(), "patches");
        if (!patchesDir.exists() && !patchesDir.mkdirs()) {
            android.widget.Toast.makeText(this,
                    getString(R.string.settings_open_patches_failed, patchesDir.getAbsolutePath()),
                    android.widget.Toast.LENGTH_LONG).show();
            return;
        }
        final String path = patchesDir.getAbsolutePath();

        // NOTE: deep-linking into a specific folder was tried and REMOVED.
        // DocumentsContract.buildDocumentUri + ACTION_VIEW does not throw, but
        // DocumentsUI ignores the URI and silently opens Downloads instead -
        // dropping the user in the wrong folder is worse than not trying, and it
        // swallowed the informative dialog below. Verified on Android 13/Odin 2.
        // Instead, show the path (copyable) and offer the file manager, where
        // this app appears as a source because it registers a DocumentsProvider.
        // Fallback: tell the user exactly where the folder is and let them copy
        // the path. A bare file-manager launch would drop them somewhere
        // unrelated (observed: it lands on Downloads), which is worse than
        // useless - so make the path itself the deliverable. The app also
        // registers a DocumentsProvider, so "Canary AE" appears as a source in
        // the file manager's side drawer.
        new AlertDialog.Builder(this)
                .setTitle(R.string.settings_open_patches)
                .setMessage(getString(R.string.settings_open_patches_help, path))
                .setPositiveButton(R.string.settings_copy_path, (d, w) -> {
                    final android.content.ClipboardManager cb =
                            (android.content.ClipboardManager)
                                    getSystemService(CLIPBOARD_SERVICE);
                    if (cb != null) {
                        cb.setPrimaryClip(
                                android.content.ClipData.newPlainText("patches path", path));
                        android.widget.Toast.makeText(this, R.string.settings_path_copied,
                                android.widget.Toast.LENGTH_SHORT).show();
                    }
                })
                .setNeutralButton(R.string.open_file_manager, (d, w) -> {
                    try {
                        MainActivity.open_file_manager(this);
                    } catch (Exception ignored2) {
                    }
                })
                .setNegativeButton(android.R.string.cancel, null)
                .show();
    }

    // --------------------------------------------------------------- helpers

    /** Section header, styled to match the top-level Settings screen. */
    private void addSectionHeader(String text) {
        final TextView tv = new TextView(
                new android.view.ContextThemeWrapper(this, R.style.SettingsSectionHeader));
        tv.setText(text);
        container.addView(tv);
    }

    private interface OnCheckedChanged { void onChanged(boolean checked); }

    private View addClickRow(String title, @Nullable String subtitle,
                             @Nullable View.OnClickListener onClick) {
        final View row = inflater.inflate(R.layout.settings_row_click, container, false);
        ((TextView) row.findViewById(R.id.row_title)).setText(title);
        setSubtitle(row, subtitle);
        if (onClick != null) {
            row.setOnClickListener(onClick);
        }
        container.addView(row);
        return row;
    }

    private View addSwitchRow(String title, @Nullable String subtitle, boolean checked,
                              OnCheckedChanged listener) {
        final View row = inflater.inflate(R.layout.settings_row_switch, container, false);
        ((TextView) row.findViewById(R.id.row_title)).setText(title);
        setSubtitle(row, subtitle);
        final SwitchCompat sw = row.findViewById(R.id.row_switch);
        // Same reasoning as SettingsFragment: every inflated row shares the literal
        // id row_switch, so Android's id-keyed state restore can put a stale value
        // on the wrong row. We always set state from the real source of truth.
        sw.setSaveEnabled(false);
        sw.setChecked(checked);
        sw.setOnCheckedChangeListener((buttonView, isChecked) -> listener.onChanged(isChecked));
        row.setOnClickListener(v -> sw.toggle());
        container.addView(row);
        return row;
    }

    /**
     * A row that opens a list picker and stores the chosen value as an int.
     * `values` maps 1:1 onto `labels`; the stored value is values[index], which
     * is NOT necessarily the number shown (anisotropic filtering stores an enum
     * index, e.g. label "16x" stores 5).
     */
    private void addIntPickerRow(String title, String dialogTitle, String currentLabel,
                                 String[] labels, int[] values, String subtitleSuffix,
                                 java.util.function.IntConsumer onPick) {
        final View row = addClickRow(title,
                currentLabel + (subtitleSuffix == null ? "" : "  \u00b7  " + subtitleSuffix),
                null);
        row.setOnClickListener(v -> new AlertDialog.Builder(this)
                .setTitle(dialogTitle)
                .setItems(labels, (dialog, which) -> {
                    onPick.accept(values[which]);
                    setSubtitle(row, labels[which]
                            + (subtitleSuffix == null ? "" : "  \u00b7  " + subtitleSuffix));
                })
                .show());
    }

    private void setSubtitle(View row, @Nullable String subtitle) {
        final TextView subtitleView = row.findViewById(R.id.row_subtitle);
        if (subtitle == null) {
            subtitleView.setVisibility(View.GONE);
        } else {
            subtitleView.setText(subtitle);
            subtitleView.setVisibility(View.VISIBLE);
        }
    }
}
