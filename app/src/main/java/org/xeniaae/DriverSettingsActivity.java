// SPDX-License-Identifier: WTFPL
package org.xeniaae;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.view.LayoutInflater;
import android.view.View;
import android.view.Window;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

import androidx.annotation.Nullable;
import androidx.appcompat.app.AlertDialog;
import androidx.appcompat.app.AppCompatActivity;
import androidx.appcompat.widget.Toolbar;

import java.io.File;

/**
 * Custom Vulkan driver manager: shows the detected GPU, lets the player pick
 * the active driver (system default or an installed one), import a new
 * driver from a .zip, and remove installed ones (long-press).
 */
public class DriverSettingsActivity extends AppCompatActivity {

    static final String CONFIG_KEY = "Vulkan|vulkan_lib_path";
    private static final int REQUEST_ADD_DRIVER = 7001;

    private LinearLayout container;

    @Override
    protected void onCreate(@Nullable Bundle savedInstanceState) {
        supportRequestWindowFeature(Window.FEATURE_NO_TITLE);
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_driver_settings);

        Toolbar toolbar = findViewById(R.id.toolbar);
        setSupportActionBar(toolbar);
        if (getSupportActionBar() != null) {
            getSupportActionBar().setTitle(R.string.settings_custom_driver);
        }
        toolbar.setNavigationOnClickListener(v -> onBackPressed());

        ((TextView) findViewById(R.id.detected_gpu_value)).setText(
                Application.gpu_device_name_vk != null
                        ? Application.gpu_device_name_vk
                        : getString(R.string.driver_gpu_unknown));

        container = findViewById(R.id.driver_list_container);

        View downloadRow = findViewById(R.id.row_download_driver);
        ((TextView) downloadRow.findViewById(R.id.row_title)).setText(getString(R.string.driver_download_section));
        downloadRow.findViewById(R.id.row_subtitle).setVisibility(View.GONE);
        downloadRow.setOnClickListener(v -> showDownloadDriverDialog());

        View addRow = findViewById(R.id.row_add_driver);
        ((TextView) addRow.findViewById(R.id.row_title)).setText(getString(R.string.driver_library_path_dialog_add_hint));
        addRow.findViewById(R.id.row_subtitle).setVisibility(View.GONE);
        addRow.setOnClickListener(v -> requestAddDriver());
    }

    // ---------------------------------------------------------------------
    // "Download a Driver" - suggestions fetched from the community
    // K11MCH1/AdrenoToolsDrivers GitHub repo (Turnip builds only), shown as a
    // popup list, newest release first (the order the GitHub API returns them in).
    // ---------------------------------------------------------------------

    private void showDownloadDriverDialog() {
        Toast.makeText(this, R.string.driver_download_checking, Toast.LENGTH_SHORT).show();
        DriverRepository.fetchTurnipDrivers((drivers, error) -> {
            if (error != null) {
                Toast.makeText(this, getString(R.string.driver_download_error, error), Toast.LENGTH_LONG).show();
                return;
            }
            if (drivers.isEmpty()) {
                Toast.makeText(this, R.string.driver_download_empty, Toast.LENGTH_SHORT).show();
                return;
            }
            String[] items = new String[drivers.size()];
            for (int i = 0; i < drivers.size(); i++) {
                DriverRepository.RemoteDriver d = drivers.get(i);
                items[i] = d.title + "  (" + getString(R.string.driver_download_size, d.sizeBytes / 1024.0 / 1024.0) + ")";
            }
            new AlertDialog.Builder(this)
                    .setTitle(R.string.driver_download_section)
                    .setItems(items, (dialog, which) -> downloadAndInstall(drivers.get(which)))
                    .setNegativeButton(android.R.string.cancel, null)
                    .show();
        });
    }

    private void downloadAndInstall(DriverRepository.RemoteDriver driver) {
        Toast.makeText(this, driver.title + ": " + getString(R.string.driver_download_in_progress), Toast.LENGTH_SHORT).show();
        File dest = new File(getCacheDir(), driver.assetName);
        new Thread(() -> {
            Exception error = null;
            try {
                DriverRepository.downloadTo(driver.downloadUrl, dest);
            } catch (Exception e) {
                error = e;
            }
            Exception finalError = error;
            runOnUiThread(() -> {
                if (finalError != null) {
                    Toast.makeText(this, getString(R.string.driver_download_failed, finalError.getMessage()),
                            Toast.LENGTH_LONG).show();
                    return;
                }
                String dirName = driver.assetName.substring(0, driver.assetName.lastIndexOf('.'));
                Utils.install_custom_driver_from_file(this, dirName, dest, path -> {
                    dest.delete();
                    writeCurrentPath(path);
                    refreshList();
                    Toast.makeText(this, getString(R.string.driver_download_installed, driver.title), Toast.LENGTH_SHORT).show();
                });
            });
        }).start();
    }

    @Override
    protected void onResume() {
        super.onResume();
        refreshList();
    }

    private String currentPath() {
        try {
            Emulator.Config cfg = Emulator.Config.open_config_file(
                    Application.get_global_config_file().getAbsolutePath());
            String val = cfg.load_config_entry(CONFIG_KEY);
            cfg.close_config_file();
            return val;
        } catch (Exception e) {
            return null;
        }
    }

    private void writeCurrentPath(String value) {
        try {
            Emulator.Config cfg = Emulator.Config.open_config_file(
                    Application.get_global_config_file().getAbsolutePath());
            cfg.save_config_entry(CONFIG_KEY, value);
            cfg.close_config_file();
        } catch (Exception ignored) {
        }
    }

    private void refreshList() {
        container.removeAllViews();
        String current = currentPath();
        boolean isDefault = current == null || current.isEmpty() || current.equals("default");

        addRow(getString(R.string._default), getString(R.string.driver_default_subtitle), isDefault, null);

        for (Utils.DriverInfo info : Utils.list_installed_drivers()) {
            boolean selected = !isDefault && info.libraryPath.equals(current);
            String subtitle = info.version != null
                    ? getString(R.string.driver_version_subtitle, info.version)
                    : (info.description != null ? info.description : info.libraryPath);
            addRow(info.name, subtitle, selected, info);
        }
    }

    private void addRow(String title, String subtitle, boolean selected, @Nullable Utils.DriverInfo info) {
        View row = LayoutInflater.from(this).inflate(R.layout.settings_row_click, container, false);
        ((TextView) row.findViewById(R.id.row_title)).setText(
                selected ? title + getString(R.string.driver_active_marker) : title);
        TextView subtitleView = row.findViewById(R.id.row_subtitle);
        subtitleView.setText(subtitle);
        subtitleView.setVisibility(subtitle == null ? View.GONE : View.VISIBLE);
        row.setOnClickListener(v -> select(info));
        if (info != null) {
            row.setOnLongClickListener(v -> {
                confirmDelete(info);
                return true;
            });
        }
        container.addView(row);
    }

    private void select(@Nullable Utils.DriverInfo info) {
        writeCurrentPath(info == null ? "default" : info.libraryPath);
        refreshList();
    }

    private void confirmDelete(Utils.DriverInfo info) {
        new AlertDialog.Builder(this)
                .setTitle(info.name)
                .setMessage(R.string.driver_delete_confirm)
                .setPositiveButton(android.R.string.ok, (dialog, which) -> {
                    boolean wasActive = info.libraryPath.equals(currentPath());
                    Utils.delete_driver(info);
                    if (wasActive) writeCurrentPath("default");
                    refreshList();
                })
                .setNegativeButton(android.R.string.cancel, null)
                .show();
    }

    private void requestAddDriver() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        startActivityForResult(intent, REQUEST_ADD_DRIVER);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, @Nullable Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != REQUEST_ADD_DRIVER || resultCode != Activity.RESULT_OK || data == null) return;
        Uri uri = data.getData();
        if (uri == null) return;
        String fileName = Utils.getFileNameFromUri(uri);
        if (fileName == null || !fileName.endsWith(".zip")) return;
        Utils.install_custom_driver_from_zip(this, uri, path -> {
            writeCurrentPath(path);
            refreshList();
        });
    }
}
