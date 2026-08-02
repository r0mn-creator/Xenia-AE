package org.xeniaae;

import android.content.ContentUris;
import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.MediaStore;
import android.util.Log;

import androidx.documentfile.provider.DocumentFile;

/**
 * Re-resolves a game's URI when the stored one has gone stale.
 *
 * <p>Why this is needed: the library scanner records a MediaStore URI built from
 * a row id ({@code content://media/external/downloads/1000068905}). That id is
 * <b>not stable</b> — MediaStore reassigns it whenever the file is re-indexed,
 * which happens after a reboot, a media scan, or moving the file. The ISO is
 * still sitting there, but the saved URI now points at nothing.
 *
 * <p>The symptom is bad: tapping the game did nothing at all. The open failed
 * deep in {@code Emulator.nc_open_uri_fd}, which logged a
 * {@code FileNotFoundException} and returned -1, so the emulator started against
 * no file and simply sat there. Nothing on screen said why.
 *
 * <p>This resolves by <b>identity that does not change</b> — the file's name —
 * rather than by a database row id, and prefers the document-tree URI, which is
 * backed by a persisted permission and survives re-indexing.
 */
public final class GameUriResolver {

    private static final String TAG = "XeniaAE";

    private GameUriResolver() {}

    /** True if the URI can actually be opened right now. */
    public static boolean canOpen(Context ctx, String uriString) {
        if (uriString == null || uriString.isEmpty()) {
            return false;
        }
        try (ParcelFileDescriptor pfd =
                     ctx.getContentResolver().openFileDescriptor(Uri.parse(uriString), "r")) {
            return pfd != null;
        } catch (Exception e) {
            return false;
        }
    }

    /**
     * Returns a URI that opens, or the original if nothing better is found.
     *
     * @param title the game's display name, e.g. "Need for Speed - Carbon" —
     *              used to find the file again by name.
     */
    public static String resolve(Context ctx, String uriString, String title) {
        if (canOpen(ctx, uriString)) {
            return uriString;
        }
        Log.w(TAG, "Stored game URI is stale, re-resolving: " + uriString);

        // 1. The saved folder tree. Preferred: a tree URI is backed by a
        //    persisted permission, so unlike a MediaStore row id it keeps
        //    working across re-indexing and reboots.
        String fromTree = findInTree(ctx, title);
        if (fromTree != null) {
            Log.i(TAG, "Re-resolved from folder tree: " + fromTree);
            return fromTree;
        }

        // 2. MediaStore, matched on display name rather than row id.
        String fromMedia = findInMediaStore(ctx, title);
        if (fromMedia != null) {
            Log.i(TAG, "Re-resolved from MediaStore: " + fromMedia);
            return fromMedia;
        }

        Log.e(TAG, "Could not re-resolve game URI for: " + title);
        return uriString;
    }

    /** Looks for the game inside the folder the user granted access to. */
    private static String findInTree(Context ctx, String title) {
        if (title == null || title.isEmpty()) {
            return null;
        }
        try {
            Uri treeUri = MainActivity.load_pref_game_dir(ctx);
            if (treeUri == null) {
                return null;
            }
            DocumentFile tree = DocumentFile.fromTreeUri(ctx, treeUri);
            if (tree == null) {
                return null;
            }
            for (DocumentFile f : tree.listFiles()) {
                String name = f.getName();
                if (name == null) {
                    continue;
                }
                // Directory entries are GOD/XBLA installs named by title;
                // files are "<title>.iso" / ".zar".
                if (normalize(name).equals(normalize(title))
                        || normalize(stripExtension(name)).equals(normalize(title))) {
                    if (f.isDirectory()) {
                        DocumentFile xex = LegacyGameScan.get_default_xex_file(f);
                        return xex != null ? xex.getUri().toString() : null;
                    }
                    return f.getUri().toString();
                }
            }
        } catch (Exception e) {
            Log.w(TAG, "Tree re-resolve failed: " + e);
        }
        return null;
    }

    /** Falls back to MediaStore, matching on the name instead of the row id. */
    private static String findInMediaStore(Context ctx, String title) {
        if (title == null || title.isEmpty()) {
            return null;
        }
        Uri collection = MediaStore.Downloads.EXTERNAL_CONTENT_URI;
        String[] projection = {MediaStore.Downloads._ID, MediaStore.Downloads.DISPLAY_NAME};
        // Cannot express the punctuation-insensitive match in SQL, so scan the
        // disc images and compare normalised names here.
        String selection = MediaStore.Downloads.DISPLAY_NAME + " LIKE ?";
        String[] args = {"%.iso"};
        final String want = normalize(title);
        try (Cursor c = ctx.getContentResolver()
                .query(collection, projection, selection, args, null)) {
            if (c != null) {
                int idCol = c.getColumnIndexOrThrow(MediaStore.Downloads._ID);
                int nameCol = c.getColumnIndexOrThrow(MediaStore.Downloads.DISPLAY_NAME);
                while (c.moveToNext()) {
                    String name = c.getString(nameCol);
                    if (name == null) continue;
                    if (normalize(stripExtension(name)).equals(want)) {
                        long id = c.getLong(idCol);
                        return ContentUris.withAppendedId(collection, id).toString();
                    }
                }
            }
        } catch (Exception e) {
            Log.w(TAG, "MediaStore re-resolve failed: " + e);
        }
        return null;
    }

    private static String stripExtension(String name) {
        int dot = name.lastIndexOf('.');
        return dot > 0 ? name.substring(0, dot) : name;
    }

    /**
     * Reduces a title or filename to something comparable.
     *
     * <p>The displayed title and the file on disk routinely disagree on
     * punctuation: the library shows "Need for Speed: Carbon" (the scanner
     * normalises to the canonical name) while the file is
     * "Need for Speed - Carbon.iso". A literal comparison therefore fails on
     * exactly the games that need re-resolving, which is what made the first
     * version of this class miss.
     *
     * <p>So: lower-case, turn every separator into a space, drop anything that
     * is not alphanumeric or space, and collapse runs of spaces. Both examples
     * above reduce to "need for speed carbon".
     */
    private static String normalize(String s) {
        if (s == null) return "";
        StringBuilder sb = new StringBuilder(s.length());
        boolean lastSpace = false;
        for (char c : s.toLowerCase().toCharArray()) {
            if (Character.isLetterOrDigit(c)) {
                sb.append(c);
                lastSpace = false;
            } else if (!lastSpace) {
                sb.append(' ');
                lastSpace = true;
            }
        }
        return sb.toString().trim();
    }
}
