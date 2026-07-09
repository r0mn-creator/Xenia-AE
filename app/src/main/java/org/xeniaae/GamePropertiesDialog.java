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

        final String[] options = {
                "Game Details",
                "Launch Game",
                getString(R.string.precache_shaders),
                "Set Custom Box Art",
                "Clear Box Art",
                "Create Shortcut",
                "Game Settings",
                "Remove from Library"
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

            case 7: // Remove
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
