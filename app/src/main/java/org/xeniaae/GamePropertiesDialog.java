package org.xeniaae;

import android.app.Dialog;
import android.content.Intent;
import android.content.pm.ShortcutInfo;
import android.content.pm.ShortcutManager;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.drawable.Icon;
import android.net.Uri;
import android.os.Bundle;
import android.widget.Toast;

import android.text.SpannableString;
import android.text.Spanned;
import android.text.style.ForegroundColorSpan;
import android.text.style.StyleSpan;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.fragment.app.DialogFragment;

import com.google.android.material.dialog.MaterialAlertDialogBuilder;

import java.io.File;

public class GamePropertiesDialog extends DialogFragment {

    private static final String ARG_INDEX = "index";
    public static final String TAG = "GamePropertiesDialog";

    // Caller must set this before showing the dialog to handle image picker result
    interface Listener {
        void onSetCustomArt(int gameIndex);
    }

    public static GamePropertiesDialog newInstance(int index) {
        final GamePropertiesDialog d = new GamePropertiesDialog();
        final Bundle args = new Bundle();
        args.putInt(ARG_INDEX, index);
        d.setArguments(args);
        return d;
    }

    @NonNull
    @Override
    public Dialog onCreateDialog(@Nullable Bundle savedInstanceState) {
        final int index = requireArguments().getInt(ARG_INDEX);
        final MainActivity.GameEntry game = MainActivity.sGames.get(index);

        // "Remove from Library" is styled bold red as a destructive-action cue.
        // setItems takes CharSequence[], so a SpannableString renders correctly
        // here without needing a custom adapter.
        final SpannableString remove = new SpannableString("Remove from Library");
        remove.setSpan(new ForegroundColorSpan(0xFFD32F2F), 0, remove.length(),
                Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
        remove.setSpan(new StyleSpan(android.graphics.Typeface.BOLD), 0, remove.length(),
                Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);

        final CharSequence[] options = {
                "Game Details",
                "Launch Game",
                getString(R.string.precache_shaders),
                "Set Custom Box Art",
                "Clear Box Art",
                "Create Shortcut",
                "Game Settings",
                "Game GPU Driver",
                remove
        };

        return new MaterialAlertDialogBuilder(requireContext())
                .setTitle(game.title)
                .setItems(options, (dialog, which) -> handleOption(which, index, game))
                .create();
    }

    private void handleOption(int which, int index, MainActivity.GameEntry game) {
        // The outer list dialog (this DialogFragment) auto-dismisses and detaches
        // the instant an item is tapped — before any of these branches run. Cases
        // that defer work into a SECOND dialog's callback (e.g. Remove's confirm
        // button) would then call requireContext()/getActivity() on an already-
        // detached fragment and crash. Capture the Activity once up front, while
        // still attached, and use that everywhere instead.
        final MainActivity activity = (MainActivity) requireActivity();

        switch (which) {
            case 0: // Game Details
                GameDetailsDialog.newInstance(index)
                        .show(activity.getSupportFragmentManager(), "game_details");
                break;

            case 1: // Launch
                activity.startActivity(EmulatorActivity.createInternalIntent(
                        activity, game.uri, game.title, game.titleId));
                break;

            case 2: // Pre-cache Shaders
                activity.startActivity(EmulatorActivity.createPrecacheIntent(
                        activity, game.uri, game.title));
                break;

            case 3: // Set Custom Box Art
                if (activity instanceof Listener) {
                    ((Listener) activity).onSetCustomArt(index);
                }
                break;

            case 4: // Clear Box Art
                game.customArtUri = null;
                BoxArtManager.clearCache(activity, game);
                notifyGrids(activity);
                Toast.makeText(activity, "Box art cleared", Toast.LENGTH_SHORT).show();
                break;

            case 5: // Create Shortcut
                createShortcut(activity, game);
                break;

            case 6: // Game Settings - per-game overrides, layered on top of the global config.
                String title_id = game.titleId != null
                        ? game.titleId : GameScanner.peekTitleId(activity, Uri.parse(game.uri));
                if (title_id == null) {
                    Toast.makeText(activity, "Couldn't identify this game yet - try again shortly.",
                            Toast.LENGTH_SHORT).show();
                    break;
                }
                File config_file = Application.ensure_game_config_file(title_id);
                Intent settings_intent = new Intent(activity, EmulatorSettings.class);
                settings_intent.putExtra(EmulatorSettings.EXTRA_CONFIG_PATH, config_file.getAbsolutePath());
                settings_intent.putExtra(EmulatorSettings.EXTRA_GAME_TITLE, game.title);
                activity.startActivity(settings_intent);
                break;

            case 7: { // Game GPU Driver - per-title override of the global driver.
                // Deliberately NOT stored in the per-game config TOML: the driver
                // is loaded during Emulator::Setup, before the per-game config is
                // read, so it must be a launch argument instead (applied in
                // EmulatorActivity). See GameDriverStore for the full reasoning.
                String drv_title_id = game.titleId != null
                        ? game.titleId : GameScanner.peekTitleId(activity, Uri.parse(game.uri));
                if (drv_title_id == null) {
                    Toast.makeText(activity, "Couldn't identify this game yet - try again shortly.",
                            Toast.LENGTH_SHORT).show();
                    break;
                }
                showDriverPicker(activity, drv_title_id, game.title);
                break;
            }

            case 8: // Remove
                new MaterialAlertDialogBuilder(activity)
                        .setTitle("Remove Game")
                        .setMessage("Remove \"" + game.title + "\" from your library?")
                        .setPositiveButton("Remove", (d, w) -> {
                            MainActivity.sGames.remove(index);
                            MainActivity.saveLibrary(activity);
                            notifyGrids(activity);
                        })
                        .setNegativeButton("Cancel", null)
                        .show();
                break;
        }
    }

    /**
     * Pins a specific GPU driver to one game. Options: follow the global setting
     * (default), force the device's built-in driver, or any driver installed via
     * Settings > Video > Custom Driver.
     *
     * Different titles need different drivers on Adreno - e.g. Halo 3's menu
     * vista requires Turnip (the stock driver mis-compiles the resolve shader),
     * while other titles can regress on it. This lets both be right at once.
     */
    private void showDriverPicker(MainActivity activity, String titleId, String gameTitle) {
        final java.util.List<String> labels = new java.util.ArrayList<>();
        final java.util.List<String> values = new java.util.ArrayList<>();

        labels.add(activity.getString(R.string.game_driver_use_global));
        values.add(GameDriverStore.USE_GLOBAL);
        labels.add(activity.getString(R.string.game_driver_force_default));
        values.add(GameDriverStore.FORCE_DEFAULT);

        for (Utils.DriverInfo d : Utils.list_installed_drivers()) {
            labels.add(d.name);
            values.add(d.libraryPath);
        }

        final String current = GameDriverStore.get(activity, titleId);
        int checked = values.indexOf(current);
        if (checked < 0) {
            checked = 0;
        }

        new MaterialAlertDialogBuilder(activity)
                .setTitle(gameTitle)
                .setSingleChoiceItems(labels.toArray(new String[0]), checked, (d, which) -> {
                    GameDriverStore.set(activity, titleId, values.get(which));
                    Toast.makeText(activity,
                            activity.getString(R.string.game_driver_set, labels.get(which)),
                            Toast.LENGTH_LONG).show();
                    d.dismiss();
                })
                .setNegativeButton(android.R.string.cancel, null)
                .show();
    }

    private void createShortcut(MainActivity activity, MainActivity.GameEntry game) {
        final ShortcutManager shortcutManager = activity.getSystemService(ShortcutManager.class);
        final File art = BoxArtManager.cachedFile(activity, game);
        Bitmap icon = art.exists() ? BitmapFactory.decodeFile(art.getAbsolutePath()) : null;
        if (icon == null) {
            icon = BitmapFactory.decodeResource(activity.getResources(), R.drawable.app_icon);
        }

        final Intent intent = EmulatorActivity.createInternalIntent(
                activity, game.uri, game.title, game.titleId);
        intent.setAction(Intent.ACTION_VIEW);

        shortcutManager.requestPinShortcut(new ShortcutInfo.Builder(activity, game.uri)
                .setShortLabel(game.title)
                .setIcon(Icon.createWithBitmap(icon))
                .setIntent(intent)
                .build(), null);
    }

    private void notifyGrids(MainActivity activity) {
        activity.getSupportFragmentManager().getFragments().forEach(f -> {
            if (f instanceof GameGridFragment) ((GameGridFragment) f).refresh();
        });
    }
}
