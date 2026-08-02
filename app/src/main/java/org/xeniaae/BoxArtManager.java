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
    private static final int XBOX360_PLATFORM_ID = 61;

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

        // Disk cache hit — load instantly
        final File cached = cachedFile(context, game);
        if (cached.exists()) {
            Glide.with(imageView).load(cached).placeholder(R.mipmap.ic_launcher).into(imageView);
            return;
        }

        imageView.setImageResource(R.mipmap.ic_launcher);
        imageView.setTag(game.uri);

        sExecutor.submit(() -> {
            if (sPaused.get()) return;
            Bitmap art = null;

            // 1. XEX embedded thumbnail (offline, fastest)
            try {
                art = XexThumbnailExtractor.extract(context, Uri.parse(game.uri));
            } catch (Exception e) {
                Log.d(TAG, "XEX extraction skipped: " + e.getMessage());
            }

            // 2. TheGamesDB — real box art, but only if the user added an API
            //    key in settings. Tried before the small sources so anyone who
            //    has set a key gets proper artwork rather than a 64x64 icon.
            if (art == null && !sPaused.get()) {
                art = fetchFromTheGamesDb(context, game.title);
            }

            // 3. Xbox Live marketplace icon, by title id. Small, but it has
            //    something for effectively every title and needs no key, so it
            //    is what stops the grid being a wall of placeholders.
            if (art == null && !sPaused.get()) {
                art = fetchFromMarketplace(game.titleId);
            }

            // 4. Libretro thumbnails. Last: the Xbox 360 set contains only 12
            //    box arts in total (measured 2026-08-02), so it almost never
            //    hits and is not worth a round-trip ahead of the others.
            if (art == null && !sPaused.get()) {
                art = fetchFromLibretro(game.title);
            }

            if (art != null) {
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
    private static Bitmap fetchFromTheGamesDb(Context context, String title) {
        final String apiKey = getApiKey(context);
        if (apiKey.isEmpty()) return null;

        try {
            final String encoded = java.net.URLEncoder.encode(title, "UTF-8");
            final String urlStr = THEGAMESDB_API + "?apikey=" + apiKey
                    + "&name=" + encoded
                    + "&fields=boxart&include=boxart"
                    + "&filter[platform]=" + XBOX360_PLATFORM_ID;

            final HttpURLConnection conn = (HttpURLConnection) new URL(urlStr).openConnection();
            conn.setConnectTimeout(8000);
            conn.setReadTimeout(8000);
            if (conn.getResponseCode() != 200) return null;

            final byte[] bytes = readStream(conn.getInputStream());
            final JSONObject json = new JSONObject(new String(bytes, StandardCharsets.UTF_8));
            final JSONObject include = json.optJSONObject("include");
            if (include == null) return null;
            final JSONObject boxart = include.optJSONObject("boxart");
            if (boxart == null) return null;
            final JSONObject baseUrls = boxart.optJSONObject("base_url");
            if (baseUrls == null) return null;
            final String baseUrl = baseUrls.optString("large", baseUrls.optString("medium", ""));
            if (baseUrl.isEmpty()) return null;

            final JSONObject data = json.optJSONObject("data");
            if (data == null) return null;
            final JSONArray games = data.optJSONArray("games");
            if (games == null || games.length() == 0) return null;

            final int gameId = games.getJSONObject(0).getInt("id");
            final JSONArray images = boxart.optJSONArray(String.valueOf(gameId));
            if (images == null || images.length() == 0) return null;

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

    static File cachedFile(Context context, MainActivity.GameEntry game) {
        final String hash = sha1(game.uri);
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
