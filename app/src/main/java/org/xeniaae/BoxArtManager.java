package org.xeniaae;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.widget.ImageView;

import androidx.annotation.Nullable;

import com.bumptech.glide.Glide;
import com.bumptech.glide.load.engine.DiskCacheStrategy;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public class BoxArtManager {

    private static final String TAG = "BoxArtManager";

    // Libretro Xbox 360 thumbnail repo — no API key required
    private static final String LIBRETRO_BASE =
            "https://raw.githubusercontent.com/libretro-thumbnails/Microsoft_-_Xbox_360/master/Named_Boxarts/";

    private static final String THEGAMESDB_API = "https://api.thegamesdb.net/v1.1/Games/ByGameName";
    // TheGamesDB's platform id for Xbox 360 is 15. This was 61, which is a
    // valid id for a DIFFERENT platform, so every query filtered the real
    // results away and returned zero games - and because the API omits the
    // "include" block when there are no results, the code bailed and silently
    // fell back to a 64x64 icon. A correct API key looked completely broken.
    private static final int XBOX360_PLATFORM_ID = 15;

    private static final ExecutorService sExecutor = Executors.newFixedThreadPool(3);
    private static final Handler sMain = new Handler(Looper.getMainLooper());

    // Gated off while a game is running (see MainActivity.onStart/onStop) so this
    // launcher-only background work never competes with the emulator for CPU/network.
    private static final java.util.concurrent.atomic.AtomicBoolean sPaused =
            new java.util.concurrent.atomic.AtomicBoolean(false);

    static void pause() { sPaused.set(true); }
    static void resume() { sPaused.set(false); }

    static void load(Context context, MainActivity.GameEntry game, ImageView imageView) {
        if (sPaused.get()) return;
        // Custom art — load immediately, skip all scraping
        if (game.customArtUri != null) {
            Glide.with(imageView)
                    .load(Uri.parse(game.customArtUri))
                    .placeholder(R.mipmap.ic_launcher)
                    .diskCacheStrategy(DiskCacheStrategy.NONE)
                    .into(imageView);
            return;
        }

        // Disk cache hit - show it instantly, then decide whether to upgrade.
        final File cached = cachedFile(context, game);
        final int haveTier = cachedTier(context, game);
        if (cached.exists()) {
            Glide.with(imageView).load(cached).placeholder(R.mipmap.ic_launcher).into(imageView);
            if (haveTier >= TIER_GAMESDB) {
                return;  // already the best available - never re-query the API
            }
            // Below the top tier: try to improve it in the background, but keep
            // showing what we have so the grid never flashes a placeholder.
            imageView.setTag(game.uri);
            sExecutor.submit(() -> upgradeArt(context, game, imageView, haveTier));
            return;
        }

        imageView.setImageResource(R.mipmap.ic_launcher);
        imageView.setTag(game.uri);

        sExecutor.submit(() -> {
            if (sPaused.get()) return;
            Bitmap art = null;
            int firstFetchTier = TIER_NONE;

            // First fetch: walk the hierarchy worst-to-best and keep the best
            // that answers, recording its tier. Later loads only reach for tiers
            // ABOVE what is cached, so this is the only time a game can cost an
            // API request - and a game already at the top never costs another.

            // 1. XEX embedded thumbnail - instant and offline.
            try {
                art = XexThumbnailExtractor.extract(context, Uri.parse(game.uri));
                if (art != null) firstFetchTier = TIER_XEX;
            } catch (Exception e) {
                Log.d(TAG, "XEX extraction skipped: " + e.getMessage());
            }

            // 2. Xbox Live marketplace icon, by title id. 64x64, but it exists
            //    for effectively every title and needs no key.
            if (!sPaused.get()) {
                Bitmap mk = fetchFromMarketplace(game.titleId);
                if (mk != null) {
                    art = mk;
                    firstFetchTier = TIER_MARKETPLACE;
                }
            }

            // 3. TheGamesDB - real box art, if the user supplied a key.
            if (!sPaused.get()) {
                Bitmap db = fetchFromTheGamesDb(context, game.title);
                if (db != null) {
                    art = db;
                    firstFetchTier = TIER_GAMESDB;
                }
            }

            // 4. Libretro. Last: the Xbox 360 set has 12 box arts in total
            //    (measured 2026-08-02), so it almost never hits.
            if (art == null && !sPaused.get()) {
                art = fetchFromLibretro(game.title);
                if (art != null) firstFetchTier = TIER_MARKETPLACE;
            }

            if (art != null) {
                setCachedTier(context, game, firstFetchTier);
                saveToDiskCache(cached, art);
                final Bitmap finalArt = art;
                sMain.post(() -> {
                    if (imageView.getTag() != null && imageView.getTag().equals(game.uri)) {
                        imageView.setImageBitmap(finalArt);
                    }
                });
            }
        });
    }

    /**
     * Xbox Live marketplace icon, looked up by TITLE ID.
     *
     * <p>This is the only source that reliably has something for every game.
     * It is keyed by title id rather than by name, so it does not care how the
     * file was named or which region it is - the two things that make name
     * matching miss. Measured: it returned an image for every title tested.
     *
     * <p>It is only 64x64, so it is a FALLBACK, not a replacement for real box
     * art - but a correct small icon beats a generic placeholder, and the user
     * can always set custom art per game, which takes priority over everything.
     *
     * <p>HTTP only: the HTTPS endpoint does not respond, which is why
     * network_security_config.xml carries a scoped cleartext exception for this
     * one host.
     */
    @Nullable
    private static Bitmap fetchFromMarketplace(String titleId) {
        if (titleId == null || titleId.length() != 8) {
            return null;
        }
        final Bitmap result = downloadBitmap(
                "http://image.xboxlive.com/global/t." + titleId + "/icon/0/8000");
        if (result != null) Log.d(TAG, "Marketplace hit: " + titleId);
        return result;
    }

    @Nullable
    private static Bitmap fetchFromLibretro(String title) {
        try {
            final String sanitized = title
                    .replace("/", "_").replace("\\", "_")
                    .replace(":", "_").replace("*", "_")
                    .replace("?", "_").replace("\"", "_")
                    .replace("<", "_").replace(">", "_")
                    .replace("|", "_");
            final String encoded = java.net.URLEncoder.encode(sanitized, "UTF-8")
                    .replace("+", "%20");
            final Bitmap result = downloadBitmap(LIBRETRO_BASE + encoded + ".png");
            if (result != null) Log.d(TAG, "Libretro hit: " + title);
            return result;
        } catch (Exception e) {
            Log.d(TAG, "Libretro failed: " + e.getMessage());
            return null;
        }
    }

    @Nullable
    /**
     * Turns a filename-derived title into something a games database will match.
     *
     * <p>The library takes titles from filenames, which use conventions a
     * database does not: "Need for Speed - Carbon" for what is catalogued as
     * "Need for Speed: Carbon", and suffixes like "(Disc 1)" or "(USA)".
     * Searching the raw form returned <b>zero</b> results for every game in the
     * library, which is why a perfectly valid API key appeared to do nothing.
     *
     * <p>Returns several candidates, most specific first, because no single
     * transformation is right for every title - "Halo 3" needs nothing, while
     * "Metal Gear Rising - Revengeance" needs the separator swapped.
     */
    private static java.util.List<String> searchCandidates(String title) {
        java.util.List<String> out = new java.util.ArrayList<>();
        String t = title.trim();

        // Drop disc/region/version suffixes - a database indexes the work, not
        // the specific dump.
        String stripped = t
                .replaceAll("(?i)\\s*\\((Disc|CD|Disk)\\s*\\d+\\)", "")
                .replaceAll("(?i)\\s*\\((USA|Europe|Japan|World|En|Eng|NTSC|PAL)\\)", "")
                .replaceAll("\\s{2,}", " ")
                .trim();

        // Filenames cannot contain ':', so a subtitle is nearly always written
        // " - ". Swapping it back is the single highest-value transformation.
        String colonised = stripped.replaceAll("\\s+-\\s+", ": ");

        if (!colonised.isEmpty()) out.add(colonised);
        if (!stripped.equals(colonised) && !stripped.isEmpty()) out.add(stripped);
        if (!t.equals(stripped) && !t.isEmpty()) out.add(t);

        // Last resort: main title only, for entries catalogued without their
        // subtitle.
        int sep = colonised.indexOf(':');
        if (sep > 2) {
            String main = colonised.substring(0, sep).trim();
            if (!main.isEmpty() && !out.contains(main)) out.add(main);
        }
        return out;
    }

    @Nullable
    private static Bitmap fetchFromTheGamesDb(Context context, String title) {
        for (String candidate : searchCandidates(title)) {
            Bitmap b = fetchFromTheGamesDbExact(context, candidate);
            if (b != null) {
                Log.d(TAG, "TGDB hit: \"" + title + "\" via \"" + candidate + "\"");
                return b;
            }
        }
        return null;
    }

    private static Bitmap fetchFromTheGamesDbExact(Context context, String title) {
        final String apiKey = getApiKey(context);
        if (apiKey.isEmpty()) { Log.d(TAG, "TGDB: no api key"); return null; }

        try {
            final String encoded = java.net.URLEncoder.encode(title, "UTF-8");
            final String urlStr = THEGAMESDB_API + "?apikey=" + apiKey
                    + "&name=" + encoded
                    + "&fields=boxart&include=boxart"
                    + "&filter[platform]=" + XBOX360_PLATFORM_ID;

            // NEVER log urlStr or the response body: both contain the API key
            // (the response echoes the request URL back in "pages"). Log only
            // the search term.
            Log.d(TAG, "TGDB query: \"" + title + "\"");
            final HttpURLConnection conn = (HttpURLConnection) new URL(urlStr).openConnection();
            conn.setConnectTimeout(8000);
            conn.setReadTimeout(8000);
            int rc = conn.getResponseCode();
            if (rc != 200) { Log.d(TAG, "TGDB: http " + rc); return null; }

            final byte[] bytes = readStream(conn.getInputStream());
            final String body = new String(bytes, StandardCharsets.UTF_8);
            final JSONObject json = new JSONObject(body);
            final JSONObject include = json.optJSONObject("include");
            if (include == null) { Log.d(TAG, "TGDB: no include"); return null; }
            final JSONObject boxart = include.optJSONObject("boxart");
            if (boxart == null) { Log.d(TAG, "TGDB: no boxart"); return null; }
            final JSONObject baseUrls = boxart.optJSONObject("base_url");
            if (baseUrls == null) { Log.d(TAG, "TGDB: no base_url"); return null; }
            final String baseUrl = baseUrls.optString("large", baseUrls.optString("medium", ""));
            if (baseUrl.isEmpty()) { Log.d(TAG, "TGDB: empty base_url"); return null; }

            final JSONObject data = json.optJSONObject("data");
            if (data == null) { Log.d(TAG, "TGDB: no data"); return null; }
            final JSONArray games = data.optJSONArray("games");
            if (games == null || games.length() == 0) { Log.d(TAG, "TGDB: no games"); return null; }

            // Images live at include.boxart.DATA.<gameId>, not include.boxart.<gameId>.
            // Missing that level meant every lookup fetched fine, parsed the base
            // URLs, then returned null at the last step - so a valid API key
            // looked exactly like no key at all.
            final JSONObject boxartData = boxart.optJSONObject("data");
            if (boxartData == null) { Log.d(TAG, "TGDB: no boxart.data"); return null; }

            // Take the first game that actually HAS artwork, rather than assuming
            // the first result does. The API orders by relevance, not by whether
            // art exists, and several 360 entries (Classics/Collector's editions)
            // come back with none.
            JSONArray images = null;
            for (int gi = 0; gi < games.length() && images == null; gi++) {
                final int candidateId = games.getJSONObject(gi).optInt("id", -1);
                if (candidateId < 0) continue;
                final JSONArray candidate =
                        boxartData.optJSONArray(String.valueOf(candidateId));
                if (candidate != null && candidate.length() > 0) {
                    images = candidate;
                }
            }
            if (images == null || images.length() == 0) { Log.d(TAG, "TGDB: no images for any game"); return null; }

            String imagePath = null;
            for (int i = 0; i < images.length(); i++) {
                final JSONObject img = images.getJSONObject(i);
                if ("boxart".equals(img.optString("type")) && "front".equals(img.optString("side"))) {
                    imagePath = img.optString("filename");
                    break;
                }
            }
            if (imagePath == null) imagePath = images.getJSONObject(0).optString("filename");
            if (imagePath == null || imagePath.isEmpty()) return null;

            return downloadBitmap(baseUrl + imagePath);
        } catch (Exception e) {
            Log.d(TAG, "TheGamesDB failed: " + e.getMessage());
            return null;
        }
    }

    // Box art tiles render at ~150dp; no need to keep a full-resolution decode in memory.
    private static final int MAX_ART_DIMENSION_PX = 512;

    @Nullable
    private static Bitmap downloadBitmap(String urlStr) {
        try {
            final HttpURLConnection conn = (HttpURLConnection) new URL(urlStr).openConnection();
            conn.setConnectTimeout(10000);
            conn.setReadTimeout(10000);
            if (conn.getResponseCode() != 200) return null;
            final byte[] bytes = readStream(conn.getInputStream());
            return decodeSampledBitmap(bytes, MAX_ART_DIMENSION_PX);
        } catch (Exception e) {
            return null;
        }
    }

    private static Bitmap decodeSampledBitmap(byte[] bytes, int maxDimension) {
        final BitmapFactory.Options bounds = new BitmapFactory.Options();
        bounds.inJustDecodeBounds = true;
        BitmapFactory.decodeByteArray(bytes, 0, bytes.length, bounds);

        int sample = 1;
        while (bounds.outWidth / (sample * 2) >= maxDimension
                || bounds.outHeight / (sample * 2) >= maxDimension) {
            sample *= 2;
        }

        final BitmapFactory.Options opts = new BitmapFactory.Options();
        opts.inSampleSize = sample;
        return BitmapFactory.decodeByteArray(bytes, 0, bytes.length, opts);
    }

    private static void saveToDiskCache(File file, Bitmap bitmap) {
        try {
            file.getParentFile().mkdirs();
            try (FileOutputStream out = new FileOutputStream(file)) {
                bitmap.compress(Bitmap.CompressFormat.JPEG, 90, out);
            }
        } catch (Exception e) {
            Log.e(TAG, "Failed to save art: " + e.getMessage());
        }
    }

    /**
     * Deletes every cached cover so art is fetched again.
     *
     * <p>Needed because {@link #load} returns early on a cache hit and never
     * re-scrapes. That is right for normal use - it makes the grid instant - but
     * it means art can never IMPROVE: once a game has fallen back to the 64x64
     * marketplace icon, adding a TheGamesDB API key changes nothing, because the
     * icon is already on disk and the better source is never consulted.
     *
     * @return how many cached covers were removed.
     */
    /**
     * Tries to replace cached art with something better.
     *
     * <p>Only reaches for a tier ABOVE what is already cached, so a game showing
     * real box art never costs another API request, and one showing a 64x64 icon
     * gets one chance to improve per library load.
     */
    private static void upgradeArt(Context context, MainActivity.GameEntry game,
                                   ImageView imageView, int haveTier) {
        if (sPaused.get()) return;
        Bitmap better = null;
        int newTier = haveTier;

        if (haveTier < TIER_MARKETPLACE) {
            better = fetchFromMarketplace(game.titleId);
            if (better != null) newTier = TIER_MARKETPLACE;
        }
        if (!sPaused.get() && haveTier < TIER_GAMESDB) {
            Bitmap best = fetchFromTheGamesDb(context, game.title);
            if (best != null) {
                better = best;
                newTier = TIER_GAMESDB;
            }
        }
        if (better == null || newTier <= haveTier) {
            return;
        }
        saveToDiskCache(cachedFile(context, game), better);
        setCachedTier(context, game, newTier);
        final Bitmap shown = better;
        imageView.post(() -> {
            if (imageView.getTag() != null && imageView.getTag().equals(game.uri)) {
                imageView.setImageBitmap(shown);
            }
        });
        Log.d(TAG, "art upgraded to tier " + newTier + ": " + game.title);
    }

    /**
     * Clears cached covers so they are fetched again.
     *
     * @param keepBest when true, art already at the top tier is KEPT. Pull-to-
     *                 refresh uses this so a routine refresh does not spend the
     *                 monthly API allowance re-downloading art we already have
     *                 at full quality; only games still on a placeholder or a
     *                 64x64 icon are retried.
     */
    static int clearCache(Context context, boolean keepBest) {
        File dir = new File(context.getFilesDir(), "covers");
        File[] files = dir.listFiles();
        if (files == null) return 0;
        if (!keepBest) {
            context.getSharedPreferences(TIER_PREFS, Context.MODE_PRIVATE)
                    .edit().clear().apply();
            int n = 0;
            for (File f : files) if (f.delete()) n++;
            return n;
        }
        // Keep top-tier art: map cache files back to the games that own them.
        java.util.HashSet<String> keep = new java.util.HashSet<>();
        for (MainActivity.GameEntry g : MainActivity.sGames) {
            if (cachedTier(context, g) >= TIER_GAMESDB) {
                keep.add(cachedFile(context, g).getName());
            }
        }
        int n = 0;
        for (File f : files) {
            if (!keep.contains(f.getName()) && f.delete()) n++;
        }
        return n;
    }

    static int clearCache(Context context) {
        File dir = new File(context.getFilesDir(), "covers");
        File[] files = dir.listFiles();
        if (files == null) {
            return 0;
        }
        int n = 0;
        for (File f : files) {
            if (f.delete()) n++;
        }
        return n;
    }

    // Art quality tiers, worst to best. Cached art records the tier it came
    // from so a later run can UPGRADE it instead of settling for whatever was
    // found first.
    //
    // XEX is instant and offline but tiny; the marketplace icon is 64x64 but
    // exists for nearly every title; TheGamesDB is real box art but costs an API
    // request from a monthly allowance. So: show something immediately, replace
    // it when something better arrives, and never re-fetch a tier we already
    // have at the top.
    static final int TIER_NONE = 0;
    static final int TIER_XEX = 1;
    static final int TIER_MARKETPLACE = 2;
    static final int TIER_GAMESDB = 3;

    private static final String TIER_PREFS = "boxart_tiers";

    private static int cachedTier(Context ctx, MainActivity.GameEntry game) {
        return ctx.getSharedPreferences(TIER_PREFS, Context.MODE_PRIVATE)
                .getInt(cacheKey(game), TIER_NONE);
    }

    private static void setCachedTier(Context ctx, MainActivity.GameEntry game, int tier) {
        ctx.getSharedPreferences(TIER_PREFS, Context.MODE_PRIVATE)
                .edit().putInt(cacheKey(game), tier).apply();
    }

    private static String cacheKey(MainActivity.GameEntry game) {
        return game.titleId != null && !game.titleId.isEmpty()
                ? game.titleId : String.valueOf(game.uri);
    }

    static File cachedFile(Context context, MainActivity.GameEntry game) {
        // Key on the SAME identity the tier record uses, and prefer the title id
        // because it is stable.
        //
        // This used to hash game.uri. That is a MediaStore URI whose numeric id
        // Android reassigns whenever it re-indexes (a reboot, a media scan, a
        // file move) - the same instability that made games silently refuse to
        // launch. When the id changed, this computed a different filename, the
        // cached cover became invisible, and the art was re-downloaded from
        // scratch. To the user the covers simply "disappeared", and every
        // library visit quietly spent API requests re-fetching art that was
        // already on disk.
        final String hash = sha1(cacheKey(game));
        return new File(context.getFilesDir(), "covers/" + hash + ".jpg");
    }

    static void clearCache(Context context, MainActivity.GameEntry game) {
        cachedFile(context, game).delete();
    }

    private static String getApiKey(Context context) {
        return context.getSharedPreferences("xenia_prefs", Context.MODE_PRIVATE)
                .getString("thegamesdb_api_key", "");
    }

    private static byte[] readStream(InputStream is) throws Exception {
        final java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
        final byte[] buf = new byte[4096];
        int n;
        while ((n = is.read(buf)) != -1) out.write(buf, 0, n);
        return out.toByteArray();
    }

    private static String sha1(String input) {
        try {
            final MessageDigest md = MessageDigest.getInstance("SHA-1");
            final byte[] hash = md.digest(input.getBytes(StandardCharsets.UTF_8));
            final StringBuilder sb = new StringBuilder();
            for (byte b : hash) sb.append(String.format("%02x", b));
            return sb.toString();
        } catch (Exception e) {
            return String.valueOf(input.hashCode());
        }
    }
}
