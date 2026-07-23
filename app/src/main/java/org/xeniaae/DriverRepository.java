// SPDX-License-Identifier: WTFPL
package org.xeniaae;

import android.os.Handler;
import android.os.Looper;

import androidx.annotation.Nullable;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

/**
 * Looks up downloadable Mesa Turnip driver builds from the community
 * K11MCH1/AdrenoToolsDrivers GitHub repo, for the "download a driver" flow in
 * DriverSettingsActivity. The repo also hosts device-specific extracted
 * proprietary Qualcomm driver blobs (uncertain redistribution rights, narrow/
 * unverified compatibility) - those are deliberately excluded; only the
 * open-source, chip-agnostic Turnip builds are surfaced.
 */
public class DriverRepository {

    private static final String RELEASES_URL =
            "https://api.github.com/repos/K11MCH1/AdrenoToolsDrivers/releases?per_page=30";
    private static final int MAX_SUGGESTIONS = 5;

    public static class RemoteDriver {
        public final String title;
        public final String assetName;
        public final String downloadUrl;
        public final long sizeBytes;

        RemoteDriver(String title, String assetName, String downloadUrl, long sizeBytes) {
            this.title = title;
            this.assetName = assetName;
            this.downloadUrl = downloadUrl;
            this.sizeBytes = sizeBytes;
        }
    }

    public interface FetchCallback {
        void onResult(List<RemoteDriver> drivers, @Nullable String error);
    }

    /** Fetches off the main thread; delivers the result back on it. */
    public static void fetchTurnipDrivers(FetchCallback cb) {
        Handler main = new Handler(Looper.getMainLooper());
        new Thread(() -> {
            try {
                List<RemoteDriver> drivers = fetchTurnipDriversBlocking();
                main.post(() -> cb.onResult(drivers, null));
            } catch (Exception e) {
                main.post(() -> cb.onResult(new ArrayList<>(), e.getMessage()));
            }
        }).start();
    }

    private static List<RemoteDriver> fetchTurnipDriversBlocking() throws IOException, org.json.JSONException {
        JSONArray releases = new JSONArray(httpGet(RELEASES_URL));
        List<RemoteDriver> result = new ArrayList<>();

        for (int i = 0; i < releases.length() && result.size() < MAX_SUGGESTIONS; i++) {
            JSONObject release = releases.getJSONObject(i);
            JSONArray assets = release.getJSONArray("assets");

            JSONObject bestAsset = null;
            int bestScore = Integer.MAX_VALUE;
            for (int j = 0; j < assets.length(); j++) {
                JSONObject asset = assets.getJSONObject(j);
                String name = asset.getString("name");
                if (!name.toLowerCase().startsWith("turnip")) continue; // vendor-blob release, skip
                int score = variantScore(name.toLowerCase());
                if (score < bestScore) {
                    bestScore = score;
                    bestAsset = asset;
                }
            }
            if (bestAsset == null) continue;

            String releaseName = release.getString("name").trim();
            String title = releaseName.isEmpty() ? release.getString("tag_name") : releaseName;
            result.add(new RemoteDriver(
                    title,
                    bestAsset.getString("name"),
                    bestAsset.getString("browser_download_url"),
                    bestAsset.getLong("size")));
        }
        return result;
    }

    /** Lower = more "default"/preferred when a release ships several Turnip variants
     * (plain build vs. chip-specific / Gmem-Sysmem-Autotuner variants). */
    private static int variantScore(String nameLower) {
        if (nameLower.matches("turnip_v[\\d.]+_r\\d+\\.zip")) return 0;
        int score = 0;
        if (nameLower.contains("a8xx") || nameLower.contains("a7xx") || nameLower.contains("a6xx")) score += 10;
        if (nameLower.contains("gmem") || nameLower.contains("sysmem")) score += 2;
        if (nameLower.contains("auto") || nameLower.contains("profiled") || nameLower.contains("fix")) score += 1;
        return score;
    }

    private static String httpGet(String urlStr) throws IOException {
        HttpURLConnection conn = (HttpURLConnection) new URL(urlStr).openConnection();
        conn.setRequestProperty("Accept", "application/vnd.github+json");
        conn.setConnectTimeout(10000);
        conn.setReadTimeout(15000);
        try {
            int code = conn.getResponseCode();
            if (code != 200) throw new IOException("HTTP " + code);
            StringBuilder sb = new StringBuilder();
            try (BufferedReader reader = new BufferedReader(
                    new InputStreamReader(conn.getInputStream(), StandardCharsets.UTF_8))) {
                String line;
                while ((line = reader.readLine()) != null) sb.append(line);
            }
            return sb.toString();
        } finally {
            conn.disconnect();
        }
    }

    /** Downloads a driver zip to the given destination file. Blocking - call off the main thread. */
    public static void downloadTo(String url, File dest) throws IOException {
        HttpURLConnection conn = (HttpURLConnection) new URL(url).openConnection();
        conn.setInstanceFollowRedirects(true);
        conn.setConnectTimeout(10000);
        conn.setReadTimeout(30000);
        try {
            int code = conn.getResponseCode();
            if (code != 200) throw new IOException("HTTP " + code);
            try (InputStream in = conn.getInputStream();
                 FileOutputStream out = new FileOutputStream(dest)) {
                byte[] buffer = new byte[16384];
                int n;
                while ((n = in.read(buffer)) != -1) out.write(buffer, 0, n);
            }
        } finally {
            conn.disconnect();
        }
    }
}
