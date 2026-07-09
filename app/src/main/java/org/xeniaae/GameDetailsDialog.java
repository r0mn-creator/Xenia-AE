package org.xeniaae;

import android.app.Dialog;
import android.os.Bundle;
import android.view.LayoutInflater;
import android.view.View;
import android.widget.TextView;
import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.appcompat.app.AlertDialog;
import androidx.fragment.app.DialogFragment;

public class GameDetailsDialog extends DialogFragment {

    private static final String ARG_INDEX = "game_index";

    public static GameDetailsDialog newInstance(int index) {
        final GameDetailsDialog d = new GameDetailsDialog();
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

        final View view = LayoutInflater.from(requireContext())
                .inflate(R.layout.dialog_game_details, null);

        ((TextView) view.findViewById(R.id.text_game_title)).setText(game.title);
        ((TextView) view.findViewById(R.id.text_game_publisher)).setText(game.region);
        ((TextView) view.findViewById(R.id.text_game_path)).setText(game.uri);
        ((TextView) view.findViewById(R.id.text_title_id)).setText(
                game.titleId != null ? game.titleId : "Scanning…");
        ((TextView) view.findViewById(R.id.text_region)).setText(game.region);

        final String ext = game.uri.contains(".")
                ? game.uri.substring(game.uri.lastIndexOf('.') + 1).toUpperCase() : "Unknown";
        ((TextView) view.findViewById(R.id.text_format)).setText(ext);

        view.findViewById(R.id.button_launch).setOnClickListener(v -> {
            startActivity(EmulatorActivity.createInternalIntent(
                    requireContext(), game.uri, game.title, game.titleId));
            dismiss();
        });

        view.findViewById(R.id.button_remove).setOnClickListener(v -> {
            MainActivity.sGames.remove(index);
            MainActivity.saveLibrary(requireContext());
            if (getActivity() instanceof MainActivity) {
                ((MainActivity) getActivity()).getSupportFragmentManager()
                        .getFragments().forEach(f -> {
                            if (f instanceof GameGridFragment) ((GameGridFragment) f).refresh();
                        });
            }
            dismiss();
        });

        return new AlertDialog.Builder(requireContext())
                .setView(view)
                .create();
    }
}
