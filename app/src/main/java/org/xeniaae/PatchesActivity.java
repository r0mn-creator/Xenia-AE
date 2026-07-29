package org.xeniaae;

import android.os.Bundle;
import android.text.Editable;
import android.text.TextWatcher;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.appcompat.app.AppCompatActivity;
import androidx.appcompat.widget.AppCompatCheckBox;
import androidx.appcompat.widget.SwitchCompat;
import androidx.recyclerview.widget.LinearLayoutManager;
import androidx.recyclerview.widget.RecyclerView;

import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;

/**
 * Game patches browser.
 *
 * Two levels:
 *  1. a searchable list of every installed .patch.toml (one per game / title
 *     update), showing how many of its patches are on;
 *  2. that game's individual patches, each a switch with the author's
 *     description underneath.
 *
 * Toggling writes is_enabled straight back into the .toml. The native patcher
 * re-reads the folder at title boot, so a change takes effect the next time the
 * game is started - no app restart needed.
 *
 * The app ships the whole community patch collection (~480 files, see
 * PatchManager.installBundledPatches), so this list is long: the game level is a
 * RecyclerView, and loading/parsing happens off the main thread. The patch level
 * stays a plain ScrollView - one game only ever has a handful of patches.
 */
public class PatchesActivity extends AppCompatActivity {

    private RecyclerView gameList;
    private GameAdapter gameAdapter;
    private ScrollView patchScroll;
    private LinearLayout patchContainer;
    private ProgressBar progress;
    private TextView emptyNote;

    private List<PatchManager.PatchFile> allFiles = new ArrayList<>();
    private final List<PatchManager.PatchFile> shownFiles = new ArrayList<>();
    private String query = "";

    private AppCompatCheckBox installedOnly;
    /** Title IDs the user actually owns; empty when none have been scanned yet. */
    private Set<String> installedIds = new HashSet<>();

    private static final String PREFS = "xenia_prefs";
    private static final String KEY_INSTALLED_ONLY = "patches_installed_only";

    @Override
    protected void onCreate(@Nullable Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setTitle(R.string.patches_title);

        final int pad = dp(16);

        final LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);

        // Search bar: typing filters the list down to matching games; the X
        // clears it and brings everything back. The X only appears while there
        // is something to clear, so it doesn't read as a dead control.
        final LinearLayout searchBar = new LinearLayout(this);
        searchBar.setOrientation(LinearLayout.HORIZONTAL);
        searchBar.setGravity(Gravity.CENTER_VERTICAL);

        final EditText search = new EditText(this);
        search.setHint(R.string.patches_search_hint);
        search.setSingleLine(true);
        search.setPadding(pad, pad, pad, pad);
        // In landscape the IME defaults to fullscreen "extract" mode, which
        // replaces the whole screen with a giant text box and hides the list we
        // are filtering - useless for a live search. Keep the normal UI visible.
        search.setImeOptions(android.view.inputmethod.EditorInfo.IME_FLAG_NO_EXTRACT_UI
                | android.view.inputmethod.EditorInfo.IME_ACTION_SEARCH);

        final TextView clear = new TextView(this);
        clear.setText("✕");            // ✕
        clear.setTextSize(20);
        clear.setPadding(pad, pad, pad, pad);
        clear.setClickable(true);
        clear.setVisibility(View.GONE);
        clear.setContentDescription(getString(R.string.patches_clear_search));
        clear.setOnClickListener(v -> search.setText(""));

        search.addTextChangedListener(new TextWatcher() {
            @Override public void beforeTextChanged(CharSequence s, int a, int b, int c) {}
            @Override public void onTextChanged(CharSequence s, int a, int b, int c) {}
            @Override public void afterTextChanged(Editable s) {
                query = s.toString().trim().toLowerCase(Locale.ROOT);
                clear.setVisibility(query.isEmpty() ? View.GONE : View.VISIBLE);
                applyFilter();
            }
        });

        searchBar.addView(search, new LinearLayout.LayoutParams(0,
                ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        searchBar.addView(clear, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));
        // Explicit WRAP_CONTENT height: without it the bar takes the LinearLayout
        // default and the EditText stretches to fill the screen, pushing the list
        // and the clear button out of view.
        root.addView(searchBar, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));

        // The app bundles patches for ~480 games, so by default show only the ones
        // the user actually owns - unchecking reveals the whole collection (useful
        // for enabling patches before adding the game, or just browsing).
        installedOnly = new AppCompatCheckBox(this);
        installedOnly.setText(R.string.patches_installed_only);
        installedOnly.setPadding(pad, 0, pad, dp(4));
        installedOnly.setChecked(getSharedPreferences(PREFS, MODE_PRIVATE)
                .getBoolean(KEY_INSTALLED_ONLY, true));
        installedOnly.setOnCheckedChangeListener((b, checked) -> {
            getSharedPreferences(PREFS, MODE_PRIVATE).edit()
                    .putBoolean(KEY_INSTALLED_ONLY, checked).apply();
            applyFilter();
        });
        root.addView(installedOnly, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));

        final FrameLayout body = new FrameLayout(this);

        gameAdapter = new GameAdapter();
        gameList = new RecyclerView(this);
        gameList.setLayoutManager(new LinearLayoutManager(this));
        gameList.setAdapter(gameAdapter);
        gameList.setPadding(pad, 0, pad, pad);
        gameList.setClipToPadding(false);
        body.addView(gameList, matchParent());

        patchContainer = new LinearLayout(this);
        patchContainer.setOrientation(LinearLayout.VERTICAL);
        patchContainer.setPadding(pad, 0, pad, pad);
        patchScroll = new ScrollView(this);
        patchScroll.addView(patchContainer, new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));
        patchScroll.setVisibility(View.GONE);
        body.addView(patchScroll, matchParent());

        emptyNote = new TextView(this);
        emptyNote.setTextSize(13);
        emptyNote.setPadding(pad, pad, pad, pad);
        emptyNote.setVisibility(View.GONE);
        body.addView(emptyNote, matchParent());

        progress = new ProgressBar(this);
        final FrameLayout.LayoutParams plp = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        plp.gravity = Gravity.CENTER;
        body.addView(progress, plp);

        root.addView(body, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));

        setContentView(root);
    }

    private static FrameLayout.LayoutParams matchParent() {
        return new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT);
    }

    @Override
    protected void onResume() {
        super.onResume();
        // Re-read from disk each time: the user may have copied new .patch.toml
        // files in via the file manager while this screen was backgrounded.
        // ~480 bundled files, so parse on a worker and publish on the UI thread.
        progress.setVisibility(View.VISIBLE);
        new Thread(() -> {
            PatchManager.installBundledPatches(this);   // no-op after first run
            final List<PatchManager.PatchFile> loaded = PatchManager.loadAll();
            // Re-read each time: the user may have added games to the library, or
            // GameScanner may have filled in a Title ID, since we were last shown.
            final Set<String> owned = MainActivity.installedTitleIds(this);
            runOnUiThread(() -> {
                if (isFinishing() || isDestroyed()) {
                    return;
                }
                progress.setVisibility(View.GONE);
                allFiles = loaded;
                installedIds = owned;
                showGameList();
            });
        }, "patch-load").start();
    }

    @Override
    public void onBackPressed() {
        // Inside a game's patch list, Back should return to the game list rather
        // than leaving the screen entirely.
        if (patchScroll.getVisibility() == View.VISIBLE) {
            showGameList();
            return;
        }
        super.onBackPressed();
    }

    // ------------------------------------------------------------ game list

    private void showGameList() {
        patchScroll.setVisibility(View.GONE);
        patchContainer.removeAllViews();
        gameList.setVisibility(View.VISIBLE);
        applyFilter();
    }

    private void applyFilter() {
        if (gameList.getVisibility() != View.VISIBLE) {
            return;
        }
        shownFiles.clear();
        for (PatchManager.PatchFile pf : allFiles) {
            if (matches(pf)) {
                shownFiles.add(pf);
            }
        }
        gameAdapter.notifyDataSetChanged();
        gameList.scrollToPosition(0);

        if (!shownFiles.isEmpty()) {
            emptyNote.setVisibility(View.GONE);
            return;
        }
        emptyNote.setVisibility(View.VISIBLE);
        if (allFiles.isEmpty()) {
            emptyNote.setText(getString(R.string.patches_none,
                    PatchManager.patchesDir().getAbsolutePath()));
        } else if (installedOnly.isChecked() && query.isEmpty()) {
            // Nothing owned matched. Distinguish "no patches exist for your games"
            // from "we don't know what you own yet" - the latter is what a library
            // whose Title IDs haven't been scanned looks like, and telling the user
            // to uncheck the box is the wrong advice for it.
            emptyNote.setText(installedIds.isEmpty()
                    ? getString(R.string.patches_no_title_ids)
                    : getString(R.string.patches_none_installed));
        } else {
            emptyNote.setText(getString(R.string.patches_no_match, query));
        }
    }

    private boolean matches(PatchManager.PatchFile pf) {
        if (installedOnly.isChecked() && !installedIds.contains(pf.titleId)) {
            return false;
        }
        if (query.isEmpty()) {
            return true;
        }
        // Match on the visible name, the title name and the Title ID, so users
        // can search either "halo" or "4D5307E6".
        return pf.displayName().toLowerCase(Locale.ROOT).contains(query)
                || pf.titleName.toLowerCase(Locale.ROOT).contains(query)
                || pf.titleId.toLowerCase(Locale.ROOT).contains(query);
    }

    private class GameAdapter extends RecyclerView.Adapter<GameAdapter.Holder> {

        @NonNull
        @Override
        public Holder onCreateViewHolder(@NonNull ViewGroup parent, int viewType) {
            final LinearLayout row = new LinearLayout(PatchesActivity.this);
            row.setOrientation(LinearLayout.VERTICAL);
            row.setPadding(0, dp(12), 0, dp(12));
            row.setClickable(true);
            row.setLayoutParams(new RecyclerView.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT));

            final TextView title = new TextView(PatchesActivity.this);
            title.setTextSize(16);
            row.addView(title);

            final TextView sub = new TextView(PatchesActivity.this);
            sub.setTextSize(12);
            row.addView(sub);

            return new Holder(row, title, sub);
        }

        @Override
        public void onBindViewHolder(@NonNull Holder h, int position) {
            final PatchManager.PatchFile pf = shownFiles.get(position);
            final int enabled = pf.enabledCount();
            h.title.setText(pf.displayName());
            // Bold the whole row when this game has something switched on, so an
            // active game is findable by eye in a 480-entry list.
            h.title.setTypeface(null, enabled > 0
                    ? android.graphics.Typeface.BOLD
                    : android.graphics.Typeface.NORMAL);
            h.sub.setText(getString(R.string.patches_game_subtitle,
                    pf.titleId, pf.patches.size(), enabled));
            h.itemView.setOnClickListener(v -> showPatchList(pf));
        }

        @Override
        public int getItemCount() {
            return shownFiles.size();
        }

        class Holder extends RecyclerView.ViewHolder {
            final TextView title;
            final TextView sub;

            Holder(View item, TextView title, TextView sub) {
                super(item);
                this.title = title;
                this.sub = sub;
            }
        }
    }

    // ----------------------------------------------------------- patch list

    private void showPatchList(PatchManager.PatchFile pf) {
        gameList.setVisibility(View.GONE);
        emptyNote.setVisibility(View.GONE);
        patchScroll.setVisibility(View.VISIBLE);
        patchContainer.removeAllViews();
        patchScroll.scrollTo(0, 0);

        addHeader(pf.titleName);
        addNote(getString(R.string.patches_apply_note));

        if (pf.patches.isEmpty()) {
            addNote(getString(R.string.patches_file_empty));
        }

        for (PatchManager.Patch p : pf.patches) {
            final StringBuilder sub = new StringBuilder();
            if (p.desc != null && !p.desc.isEmpty()) {
                sub.append(p.desc);
            }
            if (p.author != null && !p.author.isEmpty()) {
                if (sub.length() > 0) {
                    sub.append('\n');
                }
                sub.append(getString(R.string.patches_by_author, p.author));
            }
            if (p.enabledLine < 0) {
                // No is_enabled line to rewrite - show it but don't pretend it
                // can be toggled.
                addRow(p.name, getString(R.string.patches_not_toggleable), null);
                continue;
            }
            addSwitch(p.name, sub.toString(), p.enabled, checked -> {
                if (!PatchManager.setEnabled(pf, p, checked)) {
                    Toast.makeText(this, R.string.patches_write_failed,
                            Toast.LENGTH_LONG).show();
                }
            });
        }

        addRow(getString(R.string.patches_back), null, v -> showGameList());
    }

    // --------------------------------------------------------------- widgets

    private interface OnChecked { void onChanged(boolean checked); }

    private void addHeader(String text) {
        final TextView tv = new TextView(
                new android.view.ContextThemeWrapper(this, R.style.SettingsSectionHeader));
        tv.setText(text);
        patchContainer.addView(tv);
    }

    private void addNote(String text) {
        final TextView tv = new TextView(this);
        tv.setText(text);
        tv.setTextSize(12);
        tv.setPadding(0, dp(6), 0, dp(10));
        patchContainer.addView(tv);
    }

    private void addRow(String title, @Nullable String subtitle,
                        @Nullable View.OnClickListener onClick) {
        final LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.VERTICAL);
        row.setPadding(0, dp(12), 0, dp(12));
        if (onClick != null) {
            row.setClickable(true);
            row.setOnClickListener(onClick);
        }

        final TextView tv = new TextView(this);
        tv.setText(title);
        tv.setTextSize(16);
        row.addView(tv);

        if (subtitle != null && !subtitle.isEmpty()) {
            final TextView sub = new TextView(this);
            sub.setText(subtitle);
            sub.setTextSize(12);
            row.addView(sub);
        }
        patchContainer.addView(row);
    }

    private void addSwitch(String title, String subtitle, boolean checked, OnChecked cb) {
        final LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setPadding(0, dp(12), 0, dp(12));
        row.setGravity(Gravity.CENTER_VERTICAL);

        final LinearLayout texts = new LinearLayout(this);
        texts.setOrientation(LinearLayout.VERTICAL);

        final TextView tv = new TextView(this);
        tv.setText(title);
        tv.setTextSize(16);
        texts.addView(tv);

        if (subtitle != null && !subtitle.isEmpty()) {
            final TextView sub = new TextView(this);
            sub.setText(subtitle);
            sub.setTextSize(12);
            texts.addView(sub);
        }
        row.addView(texts, new LinearLayout.LayoutParams(0,
                ViewGroup.LayoutParams.WRAP_CONTENT, 1f));

        final SwitchCompat sw = new SwitchCompat(this);
        sw.setSaveEnabled(false);   // state comes from the file, not view restore
        sw.setChecked(checked);
        sw.setOnCheckedChangeListener((b, isChecked) -> cb.onChanged(isChecked));
        row.addView(sw);

        row.setOnClickListener(v -> sw.toggle());
        patchContainer.addView(row);
    }

    private int dp(int v) {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }
}
