package org.xeniaae;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.Dialog;
import android.content.ClipData;
import android.content.Context;
import android.content.DialogInterface;
import android.content.Intent;
import android.content.SharedPreferences;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.os.Message;
import android.preference.PreferenceManager;
import android.provider.DocumentsContract;
import android.provider.MediaStore;
import android.provider.OpenableColumns;
import android.provider.Settings;
import android.util.Log;
import android.view.KeyEvent;
import android.view.View;
import android.view.Window;

import androidx.annotation.NonNull;
import androidx.appcompat.app.AppCompatActivity;
import androidx.documentfile.provider.DocumentFile;
import androidx.fragment.app.Fragment;
import androidx.fragment.app.FragmentActivity;
import androidx.viewpager2.adapter.FragmentStateAdapter;
import androidx.viewpager2.widget.ViewPager2;

import com.google.android.material.appbar.MaterialToolbar;
import com.google.android.material.floatingactionbutton.FloatingActionButton;
import com.google.android.material.snackbar.Snackbar;
import com.google.android.material.tabs.TabLayout;
import com.google.android.material.tabs.TabLayoutMediator;

import org.json.JSONArray;
import org.json.JSONObject;

import java.util.ArrayList;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;

public class MainActivity extends AppCompatActivity implements GamePropertiesDialog.Listener {

    private static final int REQUEST_OPEN_GAME   = 1;
    private static final int REQUEST_PICK_ART    = 3;
    private static final int REQUEST_OPEN_FOLDER = 4;

    static final int TAB_PROFILES = 0;
    static final int TAB_GAMES    = 1;
    static final int TAB_SETTINGS = 2;

    private static final String TAG = "MainActivity";

    /** SharedPreferences key for the set of folder tree URIs the user has imported. */
    static final String PREF_WATCHED_FOLDERS = "watched_folders";
    /** SharedPreferences key for the serialized game library (JSON array). */
    private static final String PREF_GAME_LIBRARY = "game_library";
    /** Legacy pref key: single folder tree the old file-list UI auto-scanned. */
    private static final String PREF_GAME_DIR = "game_dir";

    private int mPendingArtIndex = -1;

    static final List<GameEntry> sGames = new ArrayList<>();

    static final int DELAY_ON_CREATE = 0xaeae0000;
    Dialog delay_dialog = null;
    final Handler delay_on_create = new Handler(new Handler.Callback() {
        @Override
        public boolean handleMessage(@NonNull Message msg) {
            if (msg.what != DELAY_ON_CREATE) return false;
            if (delay_dialog != null) {
                delay_dialog.dismiss();
                delay_dialog = null;
            }
            on_create();
            return true;
        }
    });

    void show_device_unsupport_vulkan_dialog(){
        AlertDialog.Builder ab=new AlertDialog.Builder(this);
        ab.setPositiveButton(R.string.quit, new DialogInterface.OnClickListener(){
            @Override
            public void onClick(DialogInterface p1, int p2)
            {
                p1.cancel();
                finish();
            }
        });
        Dialog d=ab.create();
        d.setCanceledOnTouchOutside(false);
        d.setOnKeyListener(new DialogInterface.OnKeyListener(){
            @Override
            public boolean onKey(DialogInterface p1, int p2, KeyEvent p3){
                return true;
            }
        });
        d.show();
    }

    boolean storage_permission_pending = false;

    private boolean ensure_all_files_permission() {
        if (Build.VERSION.SDK_INT < 30) return true; // pre-Android 11: legacy storage is fine
        return Environment.isExternalStorageManager();
    }

    private void request_all_files_permission() {
        storage_permission_pending = true;
        Intent intent = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                Uri.parse("package:" + getPackageName()));
        startActivity(intent);
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (storage_permission_pending) {
            storage_permission_pending = false;
            on_create();
            return;
        }
        refreshAllGridFragments();
    }

    // The launcher's own background work (box art fetching, game scanning) has no
    // reason to run once a game is launched — EmulatorActivity runs in a separate
    // process and owns the device from here. Pause it while backgrounded so it
    // never competes with the emulator for CPU/network; resume when back in view.
    @Override
    protected void onStart() {
        super.onStart();
        BoxArtManager.resume();
        GameScanner.resume();
    }

    @Override
    protected void onStop() {
        super.onStop();
        BoxArtManager.pause();
        GameScanner.pause();
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        supportRequestWindowFeature(Window.FEATURE_NO_TITLE);
        super.onCreate(savedInstanceState);

        if(!Application.device_support_vulkan()){
            show_device_unsupport_vulkan_dialog();
            return;
        }

        if (!ensure_all_files_permission()) {
            request_all_files_permission();
            return;
        }

        if(!Application.should_delay_load()){
            on_create();
            return;
        }

        delay_dialog=ProgressTask.create_progress_dialog( this,getString(R.string.loading));
        delay_dialog.show();
        new Thread(){
            @Override
            public void run() {
                try {
                    Thread.sleep(500);
                    Emulator.load_library();
                    Thread.sleep(100);
                    delay_on_create.sendEmptyMessage(DELAY_ON_CREATE);
                } catch (InterruptedException e) {
                    throw new RuntimeException(e);
                }
            }
        }.start();
    }

    void on_create(){
        _on_create();
    }

    void _on_create(){
        setContentView(R.layout.activity_main);

        if (sGames.isEmpty()) {
            loadLibrary(this);
        }
        if (sGames.isEmpty()) {
            migrateLegacyGameList();
        }

        final MaterialToolbar toolbar = findViewById(R.id.toolbar);
        setSupportActionBar(toolbar);

        final ViewPager2 pager = findViewById(R.id.pager);
        final TabLayout tabs = findViewById(R.id.tabs);

        pager.setAdapter(new PagerAdapter(this));
        new TabLayoutMediator(tabs, pager, (tab, position) -> {
            switch (position) {
                case 0: tab.setText(R.string.tab_profiles); break;
                case 1: tab.setText(R.string.tab_games); break;
                case 2: tab.setText(R.string.tab_settings); break;
            }
        }).attach();
        pager.setCurrentItem(TAB_GAMES, false);

        final FloatingActionButton fab = findViewById(R.id.fab_add_games);
        fab.setOnClickListener(v -> showAddDialog());
        fab.setVisibility(View.VISIBLE);

        pager.registerOnPageChangeCallback(new ViewPager2.OnPageChangeCallback() {
            @Override
            public void onPageSelected(int position) {
                fab.setVisibility(position == TAB_GAMES ? View.VISIBLE : View.GONE);
            }
        });
    }

    /** Persists a folder tree URI so future launches remember it was imported. */
    static void saveWatchedFolder(Context context, Uri treeUri) {
        final SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(context);
        final HashSet<String> folders = new HashSet<>(
                prefs.getStringSet(PREF_WATCHED_FOLDERS, Collections.emptySet()));
        folders.add(treeUri.toString());
        prefs.edit().putStringSet(PREF_WATCHED_FOLDERS, folders).apply();
    }

    /** Serializes sGames to SharedPreferences as a JSON array. */
    static void saveLibrary(Context context) {
        try {
            final JSONArray array = new JSONArray();
            for (final GameEntry e : sGames) {
                final JSONObject obj = new JSONObject();
                obj.put("title", e.title);
                obj.put("uri", e.uri);
                obj.put("region", e.region);
                if (e.titleId != null)     obj.put("titleId", e.titleId);
                if (e.customArtUri != null) obj.put("customArtUri", e.customArtUri);
                array.put(obj);
            }
            PreferenceManager.getDefaultSharedPreferences(context)
                    .edit().putString(PREF_GAME_LIBRARY, array.toString()).apply();
        } catch (Exception e) {
            Log.w(TAG, "saveLibrary failed: " + e.getMessage());
        }
    }

    /** Restores sGames from SharedPreferences. Call once on a fresh launch. */
    private static void loadLibrary(Context context) {
        final String json = PreferenceManager.getDefaultSharedPreferences(context)
                .getString(PREF_GAME_LIBRARY, null);
        if (json == null) return;
        try {
            final JSONArray array = new JSONArray(json);
            sGames.clear();
            for (int i = 0; i < array.length(); i++) {
                final JSONObject obj = array.getJSONObject(i);
                final GameEntry e = new GameEntry(
                        obj.getString("title"),
                        obj.getString("uri"),
                        obj.optString("region", "Xbox 360"));
                e.titleId      = obj.has("titleId")     ? obj.getString("titleId")     : null;
                e.customArtUri = obj.has("customArtUri") ? obj.getString("customArtUri") : null;
                sGames.add(e);
            }
        } catch (Exception e) {
            Log.w(TAG, "loadLibrary failed: " + e.getMessage());
        }
    }

    /**
     * First-run migration: the previous file-list UI auto-scanned a configured
     * folder (or Downloads for *.iso) instead of maintaining an explicit
     * library. If the persisted library is empty, run that same scan once so
     * existing users don't lose games they already had detected.
     */
    private void migrateLegacyGameList() {
        final ArrayList<Emulator.GameInfo> metas = LegacyGameScan.scan(this, load_pref_game_dir(this));
        for (Emulator.GameInfo meta : metas) {
            final GameEntry entry = new GameEntry(meta.name, meta.uri, "Xbox 360");
            sGames.add(entry);
            GameScanner.scan(this, entry, () -> {
                saveLibrary(this);
                refreshAllGridFragments();
            });
        }
        if (!metas.isEmpty()) saveLibrary(this);
    }

    /**
     * Re-scans every watched folder (added via "Select a folder") plus the legacy
     * auto-scan source (configured game dir, or Downloads for *.iso — the source
     * most existing libraries were originally populated from) for game files not
     * already in the library.
     *
     * MediaStore row IDs (the numeric id embedded in a content:// Downloads URI)
     * are NOT stable — renaming or moving a file can silently reassign it a new
     * id, orphaning the old one. A URI-only "already known?" check would then see
     * the same physical ROM as a brand new file and add a duplicate entry. So
     * candidates are also checked against every existing entry's Title ID (read
     * synchronously from the XEX header — the ROM's real, stable identity).
     */
    void refreshGameList() {
        final SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(this);
        for (String folderUri : prefs.getStringSet(PREF_WATCHED_FOLDERS, Collections.emptySet())) {
            addGamesFromTree(Uri.parse(folderUri));
        }

        // Scanning + Title ID reads touch disk — do that off the main thread.
        // sGames itself is only ever read/mutated back on the UI thread below.
        new Thread(() -> {
            final ArrayList<Emulator.GameInfo> candidates =
                    LegacyGameScan.scan(this, load_pref_game_dir(this));
            for (Emulator.GameInfo meta : candidates) {
                final String titleId = GameScanner.peekTitleId(this, Uri.parse(meta.uri));
                runOnUiThread(() -> addLegacyCandidateIfNew(meta, titleId));
            }
        }).start();
    }

    private void addLegacyCandidateIfNew(Emulator.GameInfo meta, String titleId) {
        if (isAlreadyInLibrary(meta.uri) || isDuplicateByTitleId(titleId)) return;
        final GameEntry entry = new GameEntry(meta.name, meta.uri, "Xbox 360");
        entry.titleId = titleId;
        sGames.add(entry);
        saveLibrary(this);
        refreshAllGridFragments();
        GameScanner.scan(this, entry, () -> {
            saveLibrary(this);
            refreshAllGridFragments();
        });
    }

    private boolean isAlreadyInLibrary(String uri) {
        for (GameEntry e : sGames) if (e.uri.equals(uri)) return true;
        return false;
    }

    private boolean isDuplicateByTitleId(String titleId) {
        if (titleId == null) return false;
        for (GameEntry e : sGames) if (titleId.equals(e.titleId)) return true;
        return false;
    }

    static void save_pref_game_dir(Context ctx,Uri uri){
        try{
            ctx.getContentResolver().takePersistableUriPermission(uri,Intent.FLAG_GRANT_READ_URI_PERMISSION);
            SharedPreferences.Editor editor= PreferenceManager.getDefaultSharedPreferences(ctx).edit();
            editor.putString(PREF_GAME_DIR,uri.toString());
            editor.apply();
        }
        catch(Exception e){
            e.printStackTrace();
        }
    }

    static Uri load_pref_game_dir(Context ctx){
        try{
            String uri_str=PreferenceManager.getDefaultSharedPreferences(ctx).getString(PREF_GAME_DIR,null);
            if(uri_str==null)
                return null;
            Uri uri= Uri.parse(uri_str);
            ctx.getContentResolver().takePersistableUriPermission(uri,Intent.FLAG_GRANT_READ_URI_PERMISSION);
            return uri;
        }
        catch(Exception e){
            e.printStackTrace();
            return null;
        }
    }

    static Intent get_file_manager_intent(String pkg_name)
    {
        Intent it=new Intent(Intent.ACTION_VIEW);
        it.setClassName(pkg_name, "com.android.documentsui.files.FilesActivity");
        it.setFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        return it;
    }

    static void open_file_manager(Activity activity)
    {
        try{
            activity.startActivity(get_file_manager_intent("com.android.documentsui"));
            return;
        }
        catch(Exception e){
        }
        try{
            activity.startActivity(get_file_manager_intent("com.google.android.documentsui"));
            return;
        }
        catch(Exception e){
        }
    }

    @Override
    public boolean onKeyDown(int keyCode,KeyEvent event){
        if(keyCode==KeyEvent.KEYCODE_BACK){
            finish();
            return true;
        }
        return super.onKeyDown(keyCode,event);
    }

    @Override
    public void onSetCustomArt(int gameIndex) {
        mPendingArtIndex = gameIndex;
        final Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("image/*");
        startActivityForResult(intent, REQUEST_PICK_ART);
    }

    private void showAddDialog() {
        new AlertDialog.Builder(this)
                .setTitle(R.string.add_games)
                .setItems(new String[]{"Select game files", "Select a folder"},
                        (d, which) -> { if (which == 0) openFilePicker(); else openFolderPicker(); })
                .show();
    }

    /** Opens the file picker with multi-select so several games can be picked at once. */
    private void openFilePicker() {
        final Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        intent.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
        startActivityForResult(intent, REQUEST_OPEN_GAME);
    }

    /** Opens the folder/tree picker; every game file found inside is added at once. */
    private void openFolderPicker() {
        startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), REQUEST_OPEN_FOLDER);
    }

    @Override
    protected void onActivityResult(final int requestCode, final int resultCode, final Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (resultCode != Activity.RESULT_OK || data == null) return;

        if (requestCode == REQUEST_OPEN_GAME) {
            // With EXTRA_ALLOW_MULTIPLE the system puts everything into ClipData.
            final ClipData clip = data.getClipData();
            if (clip != null && clip.getItemCount() > 0) {
                final int count = clip.getItemCount();
                for (int i = 0; i < count; i++) {
                    addGameFromUri(clip.getItemAt(i).getUri(), null);
                }
                showScanningSnackbar(count);
            } else if (data.getData() != null) {
                // Single file (some pickers skip ClipData for a single selection)
                addGameFromUri(data.getData(), null);
            }

        } else if (requestCode == REQUEST_OPEN_FOLDER) {
            final Uri treeUri = data.getData();
            if (treeUri == null) return;
            try {
                getContentResolver().takePersistableUriPermission(
                        treeUri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
            } catch (SecurityException ignored) {}
            saveWatchedFolder(this, treeUri);
            addGamesFromTree(treeUri);

        } else if (requestCode == REQUEST_PICK_ART
                && mPendingArtIndex >= 0 && mPendingArtIndex < sGames.size()) {
            final Uri uri = data.getData();
            if (uri == null) return;
            getContentResolver().takePersistableUriPermission(
                    uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
            sGames.get(mPendingArtIndex).customArtUri = uri.toString();
            BoxArtManager.clearCache(this, sGames.get(mPendingArtIndex));
            saveLibrary(this);
            refreshAllGridFragments();
            mPendingArtIndex = -1;
        }
    }

    /** Persists read permission, creates a GameEntry, shows its card, and queues a background scan. */
    private void addGameFromUri(Uri uri, String displayName) {
        try {
            getContentResolver().takePersistableUriPermission(
                    uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (SecurityException ignored) {}

        final String title = (displayName != null)
                ? displayName.replaceFirst("(?i)\\.(iso|xex|zar|xbla)$", "")
                : resolveTitle(uri);

        final GameEntry entry = new GameEntry(title, uri.toString(), "Xbox 360");
        sGames.add(entry);
        saveLibrary(this);
        refreshAllGridFragments();
        GameScanner.scan(this, entry, () -> {
            saveLibrary(this);  // capture updated title / titleId from scanner
            refreshAllGridFragments();
        });
    }

    /**
     * Lists all game files directly inside a folder tree and adds each one.
     */
    private void addGamesFromTree(Uri treeUri) {
        final Uri childrenUri = DocumentsContract.buildChildDocumentsUriUsingTree(
                treeUri, DocumentsContract.getTreeDocumentId(treeUri));

        int added = 0;
        try (Cursor c = getContentResolver().query(childrenUri,
                new String[]{
                        DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                        DocumentsContract.Document.COLUMN_DISPLAY_NAME
                }, null, null, null)) {
            while (c != null && c.moveToNext()) {
                final String docId = c.getString(0);
                final String name  = c.getString(1);
                if (!isGameFileName(name)) continue;
                final Uri fileUri = DocumentsContract.buildDocumentUriUsingTree(treeUri, docId);
                if (isAlreadyInLibrary(fileUri.toString())) continue;
                addGameFromUri(fileUri, name);
                added++;
            }
        } catch (Exception e) {
            Log.w(TAG, "Folder scan failed: " + e.getMessage());
        }

        if (added == 0) {
            Snackbar.make(findViewById(android.R.id.content),
                    getString(R.string.no_game_files_found), Snackbar.LENGTH_LONG).show();
        } else {
            showScanningSnackbar(added);
        }
    }

    private static boolean isGameFileName(String name) {
        if (name == null) return false;
        final String lower = name.toLowerCase();
        return lower.endsWith(".iso") || lower.endsWith(".xex")
                || lower.endsWith(".zar") || lower.endsWith(".xbla");
    }

    private void showScanningSnackbar(int count) {
        final String msg = count == 1
                ? getString(R.string.scanning_game)
                : getString(R.string.scanning_games, count);
        Snackbar.make(findViewById(android.R.id.content), msg, Snackbar.LENGTH_LONG).show();
    }

    /** Returns a clean display title for a picked game URI, using the file's actual name. */
    private String resolveTitle(Uri uri) {
        try (Cursor c = getContentResolver().query(
                uri, new String[]{OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst()) {
                final String name = c.getString(0);
                if (name != null && !name.isEmpty())
                    return name.replaceFirst("(?i)\\.(iso|xex|zar|xbla)$", "");
            }
        } catch (Exception ignored) {}
        final String seg = uri.getLastPathSegment();
        return seg != null ? seg.replaceFirst("(?i)\\.(iso|xex|zar|xbla)$", "") : "Unknown Game";
    }

    void refreshAllGridFragments() {
        getSupportFragmentManager().getFragments().forEach(f -> {
            if (f instanceof GameGridFragment) ((GameGridFragment) f).refresh();
        });
    }

    static class GameEntry {
        String title;   // may be updated by GameScanner to the canonical Wikipedia title
        final String uri;
        final String region;
        String customArtUri = null;
        String titleId = null;  // 8-char hex e.g. "454107EC", filled in by GameScanner

        GameEntry(String title, String uri, String region) {
            this.title = title;
            this.uri = uri;
            this.region = region;
        }
    }

    private static class PagerAdapter extends FragmentStateAdapter {
        PagerAdapter(FragmentActivity fa) { super(fa); }

        @NonNull
        @Override
        public Fragment createFragment(int position) {
            switch (position) {
                case TAB_PROFILES: return new ProfilesFragment();
                case TAB_SETTINGS: return new SettingsFragment();
                default: return new GameGridFragment();
            }
        }

        @Override
        public int getItemCount() { return 3; }
    }
}
