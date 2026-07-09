package org.xeniaae;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.util.Log;

import androidx.annotation.Nullable;

import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * Runs once when a game is added to the library.
 *
 * Steps:
 *   1. Open the file (ISO or bare XEX) and locate default.xex.
 *   2. Extract the Title ID from the XEX execution-info header.
 *   3. If the XEX has an embedded thumbnail, cache it immediately.
 *   4. Otherwise, query the Wikipedia page-summary REST API (free, no key)
 *      using the game title as a search term. Wikipedia's redirect system
 *      maps common abbreviations (e.g. "nfs_carbon" → "Need for Speed: Carbon")
 *      and returns the canonical title + box-art image in one call.
 *      The canonical title is written back to {@code entry.title} so the grid
 *      card shows the proper game name.
 */
class GameScanner {

    private static final String TAG = "GameScanner";
    private static final int XEX2_MAGIC     = 0x58455832; // 'XEX2'
    private static final int THUMB_KEY_LARGE = 0x00009007;
    private static final int THUMB_KEY_SMALL = 0x00005007;

    // Wikipedia REST API — free, no API key required.
    // Replaces underscores/spaces with underscores so filenames work directly.
    private static final String WIKI_SUMMARY =
            "https://en.wikipedia.org/api/rest_v1/page/summary/";

    private static final ExecutorService sExecutor = Executors.newSingleThreadExecutor();
    private static final Handler sMain = new Handler(Looper.getMainLooper());

    // Gated off while a game is running (see MainActivity.onStart/onStop) so this
    // launcher-only background work never competes with the emulator for CPU/network.
    private static final java.util.concurrent.atomic.AtomicBoolean sPaused =
            new java.util.concurrent.atomic.AtomicBoolean(false);

    static void pause() { sPaused.set(true); }
    static void resume() { sPaused.set(false); }

    /** Submits a background scan for {@code entry} and calls {@code onComplete} when done. */
    static void scan(Context context, MainActivity.GameEntry entry, Runnable onComplete) {
        sExecutor.submit(() -> {
            if (!sPaused.get()) {
                try {
                    doScan(context, entry);
                } catch (Exception e) {
                    Log.w(TAG, "Scan failed for " + entry.uri + ": " + e.getMessage());
                }
            }
            sMain.post(onComplete);
        });
    }

    // ---------------------------------------------------------------------------

    /**
     * Synchronously reads just the Title ID from a game file's XEX header — the
     * ROM's real, stable identity, unlike its content:// URI (MediaStore row IDs
     * are NOT stable across a file rename/move — the exact same physical file can
     * get reassigned a new URI, which would otherwise look like a "new" game to
     * any URI-based duplicate check). Cheap: reads only a small header, not the
     * whole (multi-GB) disc image. Returns null if the file isn't a readable
     * XEX2/GDFX image or has no Title ID.
     */
    @Nullable
    static String peekTitleId(Context context, Uri uri) {
        try (ParcelFileDescriptor pfd =
                     context.getContentResolver().openFileDescriptor(uri, "r");
             FileInputStream fis = new FileInputStream(pfd.getFileDescriptor());
             FileChannel channel = fis.getChannel()) {

            channel.position(0);
            ByteBuffer magicBuf = ByteBuffer.allocate(4).order(ByteOrder.BIG_ENDIAN);
            if (channel.read(magicBuf) < 4) return null;
            magicBuf.flip();
            int magic = magicBuf.getInt();

            long xexOffset;
            if (magic == XEX2_MAGIC) {
                xexOffset = 0;
            } else {
                xexOffset = XgdfParser.findDefaultXex(channel);
                if (xexOffset < 0) return null;
            }

            int titleId = XexMetaReader.readTitleId(channel, xexOffset);
            return titleId != 0 ? String.format("%08X", titleId) : null;
        } catch (Exception e) {
            return null;
        }
    }

    // STFS package magic values (first 4 bytes): "CON ", "LIVE", "PIRS".
    private static final int STFS_MAGIC_CON  = 0x434F4E20;
    private static final int STFS_MAGIC_LIVE = 0x4C495645;
    private static final int STFS_MAGIC_PIRS = 0x50495253;
    // Well-known STFS metadata file offsets (used by every STFS tool).
    private static final long STFS_TITLE_ID_OFFSET     = 0x360;
    private static final long STFS_DISPLAY_NAME_OFFSET  = 0x411;
    private static final int  STFS_DISPLAY_NAME_MAX_CHARS = 128; // UTF-16BE units

    /** Result of {@link #peekStfs}: the identity of a folder-based (XBLA/GOD)
     *  content package. {@code displayName} may be null if unreadable. */
    static class StfsInfo {
        final String titleId;     // 8-hex, may be null
        final String displayName; // human title, may be null
        StfsInfo(String titleId, String displayName) {
            this.titleId = titleId;
            this.displayName = displayName;
        }
    }

    /**
     * If {@code uri} points at an STFS content package (an XBLA/Games-on-Demand
     * install, e.g. {@code <titleId>/000D0000/<hash>}), reads its Title ID and
     * display name from the STFS header. Returns null for anything that isn't an
     * STFS package (checked cheaply via the 4-byte magic before reading further).
     */
    @Nullable
    static StfsInfo peekStfs(Context context, Uri uri) {
        try (ParcelFileDescriptor pfd =
                     context.getContentResolver().openFileDescriptor(uri, "r");
             FileInputStream fis = new FileInputStream(pfd.getFileDescriptor());
             FileChannel channel = fis.getChannel()) {

            channel.position(0);
            ByteBuffer magicBuf = ByteBuffer.allocate(4).order(ByteOrder.BIG_ENDIAN);
            if (channel.read(magicBuf) < 4) return null;
            magicBuf.flip();
            int magic = magicBuf.getInt();
            if (magic != STFS_MAGIC_CON && magic != STFS_MAGIC_LIVE
                    && magic != STFS_MAGIC_PIRS) {
                return null;
            }

            // Title ID (big-endian u32).
            String titleId = null;
            ByteBuffer tidBuf = ByteBuffer.allocate(4).order(ByteOrder.BIG_ENDIAN);
            channel.position(STFS_TITLE_ID_OFFSET);
            if (channel.read(tidBuf) == 4) {
                tidBuf.flip();
                int tid = tidBuf.getInt();
                if (tid != 0) titleId = String.format("%08X", tid);
            }

            // Display name (UTF-16BE, null-terminated). Sanitized + validated, so
            // a bad read simply yields null rather than garbage.
            String displayName = null;
            ByteBuffer nameBuf = ByteBuffer.allocate(STFS_DISPLAY_NAME_MAX_CHARS * 2)
                    .order(ByteOrder.BIG_ENDIAN);
            channel.position(STFS_DISPLAY_NAME_OFFSET);
            if (channel.read(nameBuf) > 0) {
                nameBuf.flip();
                StringBuilder sb = new StringBuilder();
                while (nameBuf.remaining() >= 2) {
                    char ch = nameBuf.getChar();
                    if (ch == 0) break;             // null terminator
                    if (ch >= 0x20 && ch != 0xFFFF) sb.append(ch); // printable only
                }
                String cleaned = sb.toString().trim();
                if (!cleaned.isEmpty()) displayName = cleaned;
            }

            return new StfsInfo(titleId, displayName);
        } catch (Exception e) {
            return null;
        }
    }

    private static void doScan(Context context, MainActivity.GameEntry entry)
            throws Exception {

        // ---- Phase 1: read XEX data from file ------------------------------------
        Uri uri = Uri.parse(entry.uri);
        boolean embeddedArtFound = false;

        try (ParcelFileDescriptor pfd =
                     context.getContentResolver().openFileDescriptor(uri, "r");
             FileInputStream fis = new FileInputStream(pfd.getFileDescriptor());
             FileChannel channel = fis.getChannel()) {

            channel.position(0);
            ByteBuffer magicBuf = ByteBuffer.allocate(4).order(ByteOrder.BIG_ENDIAN);
            if (channel.read(magicBuf) < 4) return;
            magicBuf.flip();
            int magic = magicBuf.getInt();

            long xexOffset;
            if (magic == XEX2_MAGIC) {
                xexOffset = 0;
            } else {
                xexOffset = XgdfParser.findDefaultXex(channel);
                if (xexOffset < 0) {
                    Log.d(TAG, "No GDFX filesystem found in " + entry.uri);
                    return;
                }
            }

            // Extract the Title ID.
            int titleId = XexMetaReader.readTitleId(channel, xexOffset);
            if (titleId != 0) {
                entry.titleId = String.format("%08X", titleId);
                Log.d(TAG, "Title ID: " + entry.titleId + " for " + entry.title);
            }

            // Try embedded thumbnail.
            File cacheFile = BoxArtManager.cachedFile(context, entry);
            if (!cacheFile.exists()) {
                Bitmap thumb = extractThumbnail(channel, xexOffset);
                if (thumb != null) {
                    saveBitmap(cacheFile, thumb);
                    embeddedArtFound = true;
                    Log.d(TAG, "Embedded art cached for " + entry.title);
                }
            } else {
                embeddedArtFound = true; // already cached from a previous add
            }
        }

        // ---- Phase 2: Wikipedia lookup (runs outside the file descriptor) --------
        // Skip if we already have art from the XEX itself.
        if (!embeddedArtFound) {
            fetchFromWikipedia(context, entry);
        }
    }

    /**
     * Queries the Wikipedia page-summary API using the game title as the page
     * slug. Wikipedia's redirect system resolves common abbreviations, allowing
     * filenames like "nfs_carbon" or "halo_3" to find the correct article.
     *
     * On success, updates {@code entry.title} with the canonical Wikipedia title
     * and caches the article's box-art image to the BoxArtManager disk cache.
     */
    private static void fetchFromWikipedia(Context context, MainActivity.GameEntry entry) {
        try {
            // Build the Wikipedia page slug from the game title:
            // spaces → underscores (Wikipedia URL convention).
            String slug = entry.title.replace(' ', '_');
            String urlStr = WIKI_SUMMARY + java.net.URLEncoder.encode(slug, "UTF-8")
                    .replace("+", "%20");

            HttpURLConnection conn = (HttpURLConnection) new URL(urlStr).openConnection();
            conn.setConnectTimeout(8000);
            conn.setReadTimeout(8000);
            conn.setRequestProperty("User-Agent", "XeniaAE/1.0");

            if (conn.getResponseCode() != 200) {
                Log.d(TAG, "Wikipedia: no page for '" + slug + "' (" + conn.getResponseCode() + ")");
                return;
            }

            byte[] body = readStream(conn.getInputStream());
            JSONObject json = new JSONObject(new String(body, StandardCharsets.UTF_8));

            // Skip disambiguation pages and missing images.
            if ("disambiguation".equals(json.optString("type"))) return;

            JSONObject imgObj = json.optJSONObject("originalimage");
            if (imgObj == null) imgObj = json.optJSONObject("thumbnail");
            if (imgObj == null) return;

            String imageUrl = imgObj.optString("source", "");
            if (imageUrl.isEmpty()) return;

            // Use the canonical Wikipedia title as the game's display name.
            String canonicalTitle = json.optString("title", "");
            if (!canonicalTitle.isEmpty()) {
                entry.title = canonicalTitle;
                Log.d(TAG, "Canonical title: " + canonicalTitle);
            }

            // Download and cache the box-art image.
            Bitmap art = downloadBitmap(imageUrl);
            if (art != null) {
                File cacheFile = BoxArtManager.cachedFile(context, entry);
                if (!cacheFile.exists()) {
                    saveBitmap(cacheFile, art);
                    Log.d(TAG, "Wikipedia art cached: " + imageUrl);
                }
            }
        } catch (Exception e) {
            Log.d(TAG, "Wikipedia fetch failed: " + e.getMessage());
        }
    }

    // ---------------------------------------------------------------------------
    // XEX embedded thumbnail extraction
    // ---------------------------------------------------------------------------

    private static Bitmap extractThumbnail(FileChannel channel, long xexOffset)
            throws Exception {
        channel.position(xexOffset);
        ByteBuffer hdr = ByteBuffer.allocate(24).order(ByteOrder.BIG_ENDIAN);
        if (channel.read(hdr) < 24) return null;
        hdr.flip();

        if (hdr.getInt(0) != XEX2_MAGIC) return null;
        int optCount = hdr.getInt(20);
        if (optCount <= 0 || optCount > 256) return null;

        ByteBuffer opts = ByteBuffer.allocate(optCount * 8).order(ByteOrder.BIG_ENDIAN);
        if (channel.read(opts) < optCount * 8) return null;
        opts.flip();

        for (int i = 0; i < optCount; i++) {
            int key    = opts.getInt(i * 8);
            int offset = opts.getInt(i * 8 + 4);
            if (key == THUMB_KEY_LARGE || key == THUMB_KEY_SMALL) {
                return readImageAt(channel, xexOffset + Integer.toUnsignedLong(offset));
            }
        }
        return null;
    }

    private static Bitmap readImageAt(FileChannel channel, long offset) throws Exception {
        channel.position(offset);
        ByteBuffer sizeBuf = ByteBuffer.allocate(4).order(ByteOrder.BIG_ENDIAN);
        if (channel.read(sizeBuf) < 4) return null;
        sizeBuf.flip();

        int dataSize = sizeBuf.getInt() - 4;
        if (dataSize <= 0 || dataSize > 2 * 1024 * 1024) return null;

        ByteBuffer data = ByteBuffer.allocate(dataSize);
        int read = 0;
        while (read < dataSize) {
            int r = channel.read(data);
            if (r < 0) break;
            read += r;
        }
        return BitmapFactory.decodeByteArray(data.array(), 0, read);
    }

    // ---------------------------------------------------------------------------
    // Utility helpers
    // ---------------------------------------------------------------------------

    private static Bitmap downloadBitmap(String urlStr) throws Exception {
        HttpURLConnection conn = (HttpURLConnection) new URL(urlStr).openConnection();
        conn.setConnectTimeout(10000);
        conn.setReadTimeout(10000);
        conn.setRequestProperty("User-Agent", "XeniaAE/1.0");
        if (conn.getResponseCode() != 200) return null;
        return BitmapFactory.decodeStream(conn.getInputStream());
    }

    private static byte[] readStream(InputStream is) throws Exception {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] buf = new byte[4096];
        int n;
        while ((n = is.read(buf)) != -1) out.write(buf, 0, n);
        return out.toByteArray();
    }

    private static void saveBitmap(File file, Bitmap bitmap) {
        try {
            file.getParentFile().mkdirs();
            try (FileOutputStream out = new FileOutputStream(file)) {
                bitmap.compress(Bitmap.CompressFormat.JPEG, 90, out);
            }
        } catch (Exception e) {
            Log.e(TAG, "Failed to save bitmap: " + e.getMessage());
        }
    }
}
