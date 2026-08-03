package org.xeniaae;

import android.os.Bundle;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.TextView;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.core.content.ContextCompat;
import androidx.fragment.app.Fragment;
import androidx.recyclerview.widget.GridLayoutManager;
import androidx.recyclerview.widget.RecyclerView;
import androidx.swiperefreshlayout.widget.SwipeRefreshLayout;

public class GameGridFragment extends Fragment {

    private GameAdapter mAdapter;
    private TextView mEmptyText;

    @Nullable
    @Override
    public View onCreateView(@NonNull LayoutInflater inflater,
                             @Nullable ViewGroup container,
                             @Nullable Bundle savedInstanceState) {
        return inflater.inflate(R.layout.fragment_game_grid, container, false);
    }

    @Override
    public void onViewCreated(@NonNull View view, @Nullable Bundle savedInstanceState) {
        final RecyclerView grid = view.findViewById(R.id.grid_games);
        mEmptyText = view.findViewById(R.id.text_empty);

        final int columns = Math.max(2, (int) (getResources().getDisplayMetrics().widthPixels
                / getResources().getDisplayMetrics().density / 120));
        grid.setLayoutManager(new GridLayoutManager(requireContext(), columns));

        mAdapter = new GameAdapter();
        grid.setAdapter(mAdapter);

        final SwipeRefreshLayout swipe = view.findViewById(R.id.swipe_refresh);
        swipe.setColorSchemeColors(ContextCompat.getColor(requireContext(), R.color.xenia_green));
        // Pull-to-refresh means "everything", because that is what a user
        // expects from the gesture and because the two things that go stale do
        // so invisibly:
        //   * the game list holds MediaStore ids, which Android reassigns on
        //     re-index, so a game silently stops launching
        //   * box art is cached and never re-fetched, so it can never improve
        //     (adding an API key changed nothing until the cache was cleared)
        // Previously this only called notifyDataSetChanged(), i.e. it redrew the
        // same cached data - so the gesture appeared to do nothing.
        swipe.setOnRefreshListener(() -> {
            // keepBest: routine refreshes must not re-spend the API allowance
            // on art we already have at full quality.
            BoxArtManager.clearCache(requireContext(), true);
            final androidx.fragment.app.FragmentActivity host = requireActivity();
            if (host instanceof MainActivity) {
                ((MainActivity) host).refreshGameList();
            }
            mAdapter.notifyDataSetChanged();
            updateEmpty();
            swipe.setRefreshing(false);
            android.widget.Toast.makeText(host,
                    "Refreshed library and box art", android.widget.Toast.LENGTH_SHORT)
                    .show();
        });

        updateEmpty();
    }

    public void refresh() {
        if (mAdapter != null) mAdapter.notifyDataSetChanged();
        updateEmpty();
    }

    private void updateEmpty() {
        if (mEmptyText == null) return;
        final boolean empty = MainActivity.sGames.isEmpty();
        mEmptyText.setVisibility(empty ? View.VISIBLE : View.GONE);
    }

    private class GameAdapter extends RecyclerView.Adapter<GameAdapter.ViewHolder> {

        @NonNull
        @Override
        public ViewHolder onCreateViewHolder(@NonNull ViewGroup parent, int viewType) {
            final View v = LayoutInflater.from(parent.getContext())
                    .inflate(R.layout.card_game, parent, false);
            return new ViewHolder(v);
        }

        @Override
        public void onBindViewHolder(@NonNull ViewHolder holder, int position) {
            final MainActivity.GameEntry game = MainActivity.sGames.get(position);
            holder.title.setText(game.title);
            holder.region.setText(game.region);
            // Tap = A = launch; long press = Dolphin-style properties menu
            holder.itemView.setOnClickListener(v ->
                    requireActivity().startActivity(
                            EmulatorActivity.createInternalIntent(
                                    requireContext(), game.uri, game.title, game.titleId)));
            holder.itemView.setOnLongClickListener(v -> {
                GamePropertiesDialog.newInstance(position)
                        .show(requireActivity().getSupportFragmentManager(), GamePropertiesDialog.TAG);
                return true;
            });

            // Load box art: embedded XEX → libretro thumbnails → TheGamesDB → placeholder
            BoxArtManager.load(requireContext(), game, holder.art);
        }

        @Override
        public int getItemCount() { return MainActivity.sGames.size(); }

        class ViewHolder extends RecyclerView.ViewHolder {
            final TextView title;
            final TextView region;
            final android.widget.ImageView art;
            ViewHolder(View v) {
                super(v);
                title = v.findViewById(R.id.text_game_title);
                region = v.findViewById(R.id.text_game_region);
                art = v.findViewById(R.id.image_game_art);
            }
        }
    }
}
