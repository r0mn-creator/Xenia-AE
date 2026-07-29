package org.xeniaae;

import android.content.Context;
import android.content.SharedPreferences;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * Reads and edits Xenia's {@code .patch.toml} game-patch files.
 *
 * Deliberately a LINE-BASED parser rather than a real TOML library:
 *  - these files are community-authored and full of comments explaining what
 *    each address does, and a parse/re-serialise round trip would destroy them;
 *  - toggling only ever needs to flip a single {@code is_enabled} line, so we
 *    record that line's index and rewrite exactly that one line;
 *  - it avoids adding a TOML dependency to the app for one small feature.
 *
 * The native side (xenia/patcher) does the real parsing at boot - it scans
 * {@code <storage_root>/patches/}, matches by title_id + xex hash, and applies
 * every entry whose {@code is_enabled} is true. So the only thing this class has
 * to get right is that boolean.
 */
public final class PatchManager {

    /** One [[patch]] block. */
    public static final class Patch {
        public final String name;
        public final String desc;      // may be null - not every patch has one
        public final String author;    // may be null
        public boolean enabled;
        final int enabledLine;         // index into PatchFile.lines, -1 if absent

        Patch(String name, String desc, String author, boolean enabled, int enabledLine) {
            this.name = name;
            this.desc = desc;
            this.author = author;
            this.enabled = enabled;
            this.enabledLine = enabledLine;
        }
    }

    /** One .patch.toml file = one game (sometimes one title update of a game). */
    public static final class PatchFile {
        public final File file;
        public final String titleId;
        public final String titleName;
        public final List<Patch> patches;
        final List<String> lines;

        PatchFile(File file, String titleId, String titleName,
                  List<Patch> patches, List<String> lines) {
            this.file = file;
            this.titleId = titleId;
            this.titleName = titleName;
            this.patches = patches;
            this.lines = lines;
        }

        public int enabledCount() {
            int n = 0;
            for (Patch p : patches) {
                if (p.enabled) {
                    n++;
                }
            }
            return n;
        }

        /** File name minus the .patch.toml suffix - distinguishes TU variants. */
        public String displayName() {
            String n = file.getName();
            final int i = n.indexOf(".patch.toml");
            if (i > 0) {
                n = n.substring(0, i);
            }
            return n;
        }
    }

    private PatchManager() {}

    public static File patchesDir() {
        return new File(Application.get_app_data_dir(), "patches");
    }

    // ------------------------------------------------------- bundled patches

    private static final String PREFS = "xenia_prefs";
    private static final String KEY_INSTALLED_VERSION = "bundled_patches_version";

    /**
     * Extracts the community patch collection shipped in {@code assets/patches}
     * into the patches folder, so every game has its patches available out of
     * the box with nothing for the user to download or copy.
     *
     * Existing files are never overwritten (Utils.extractAssetsDir skips them),
     * which is what keeps a user's is_enabled choices - and any patch file they
     * added by hand - intact across app updates.
     *
     * ⚠️ The files MUST be written by the app itself. Pushing them in with
     * `adb push` leaves them owned by `shell` with the SELinux context
     * media_rw_data_file:s0 instead of the app's own category-labelled context,
     * and the app can then read but NOT write them - toggling a patch silently
     * fails. (Same trap as hand-editing the config file; see
     * EmulatorSettings' self-heal.)
     *
     * Cheap after the first run: a version stamp short-circuits it, and it is
     * re-run after an app update so newly shipped patches get added.
     *
     * Safe to call from any thread; call it OFF the main thread the first time,
     * as it writes ~2 MB.
     */
    public static void installBundledPatches(Context ctx) {
        final SharedPreferences prefs =
                ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        final int version = appVersionCode(ctx);
        if (prefs.getInt(KEY_INSTALLED_VERSION, -1) == version) {
            return;
        }
        Utils.extractAssetsDir(ctx, "patches", patchesDir());
        prefs.edit().putInt(KEY_INSTALLED_VERSION, version).apply();
    }

    private static int appVersionCode(Context ctx) {
        try {
            return ctx.getPackageManager()
                    .getPackageInfo(ctx.getPackageName(), 0).versionCode;
        } catch (Exception e) {
            return 0;   // forces a re-extract; harmless, existing files survive
        }
    }

    /** All patch files currently installed, sorted by display name. */
    public static List<PatchFile> loadAll() {
        final List<PatchFile> out = new ArrayList<>();
        final File dir = patchesDir();
        final File[] files = dir.listFiles();
        if (files == null) {
            return out;
        }
        for (File f : files) {
            if (!f.isFile() || !f.getName().endsWith(".patch.toml")) {
                continue;
            }
            final PatchFile pf = parse(f);
            if (pf != null) {
                out.add(pf);
            }
        }
        Collections.sort(out, (a, b) -> a.displayName().compareToIgnoreCase(b.displayName()));
        return out;
    }

    public static PatchFile parse(File f) {
        final List<String> lines;
        try {
            lines = Files.readAllLines(f.toPath(), StandardCharsets.UTF_8);
        } catch (Exception e) {
            return null;
        }

        String titleId = null;
        String titleName = null;
        final List<Patch> patches = new ArrayList<>();

        // Fields of the [[patch]] block currently being read.
        boolean inPatch = false;
        String name = null, desc = null, author = null;
        boolean enabled = false;
        int enabledLine = -1;

        for (int i = 0; i < lines.size(); i++) {
            final String raw = lines.get(i);
            final String line = stripComment(raw).trim();
            if (line.isEmpty()) {
                continue;
            }

            if (line.startsWith("[[patch]]")) {
                if (inPatch && name != null) {
                    patches.add(new Patch(name, desc, author, enabled, enabledLine));
                }
                inPatch = true;
                name = null; desc = null; author = null;
                enabled = false; enabledLine = -1;
                continue;
            }

            // Nested [[patch.be32]] / [[patch.be16]] blocks hold the actual
            // address/value pairs - we don't need them, but must not treat
            // their fields as belonging to the patch header.
            if (line.startsWith("[[patch.") || line.startsWith("[")) {
                continue;
            }

            if (!inPatch) {
                if (titleId == null && line.startsWith("title_id")) {
                    titleId = unquote(valueOf(line));
                } else if (titleName == null && line.startsWith("title_name")) {
                    titleName = unquote(valueOf(line));
                }
                continue;
            }

            if (line.startsWith("name")) {
                name = unquote(valueOf(line));
            } else if (line.startsWith("desc")) {
                desc = unquote(valueOf(line));
            } else if (line.startsWith("author")) {
                author = unquote(valueOf(line));
            } else if (line.startsWith("is_enabled")) {
                enabled = valueOf(line).trim().startsWith("true");
                enabledLine = i;
            }
        }
        if (inPatch && name != null) {
            patches.add(new Patch(name, desc, author, enabled, enabledLine));
        }

        if (titleId == null) {
            return null;  // not a valid patch file
        }
        if (titleName == null) {
            titleName = titleId;
        }
        return new PatchFile(f, titleId.toUpperCase(), titleName, patches, lines);
    }

    /**
     * Flips one patch's is_enabled and rewrites the file, touching only that
     * line so surrounding comments/formatting are preserved verbatim.
     *
     * @return true on success
     */
    public static boolean setEnabled(PatchFile pf, Patch patch, boolean enabled) {
        if (patch.enabledLine < 0 || patch.enabledLine >= pf.lines.size()) {
            return false;
        }
        final String old = pf.lines.get(patch.enabledLine);
        // Keep leading indentation and any trailing comment.
        final String indent = old.substring(0, old.length() - old.stripLeading().length());
        String trailing = "";
        final int hash = old.indexOf('#');
        if (hash >= 0) {
            trailing = " " + old.substring(hash);
        }
        pf.lines.set(patch.enabledLine,
                indent + "is_enabled = " + (enabled ? "true" : "false") + trailing);
        try {
            Files.write(pf.file.toPath(), pf.lines, StandardCharsets.UTF_8);
        } catch (Exception e) {
            pf.lines.set(patch.enabledLine, old);  // roll back in-memory state
            return false;
        }
        patch.enabled = enabled;
        return true;
    }

    // ---------------------------------------------------------------- helpers

    /** Everything after '=' on a `key = value` line. */
    private static String valueOf(String line) {
        final int i = line.indexOf('=');
        return i < 0 ? "" : line.substring(i + 1).trim();
    }

    private static String unquote(String s) {
        s = s.trim();
        if (s.length() >= 2 && (s.startsWith("\"") && s.endsWith("\""))) {
            return s.substring(1, s.length() - 1);
        }
        return s;
    }

    /**
     * Strips a trailing comment, but only when the '#' is outside quotes - patch
     * files routinely put explanatory comments after values, e.g.
     * {@code value = 0x60000000 # Replace with NOP}.
     */
    private static String stripComment(String line) {
        boolean inQuotes = false;
        for (int i = 0; i < line.length(); i++) {
            final char c = line.charAt(i);
            if (c == '"') {
                inQuotes = !inQuotes;
            } else if (c == '#' && !inQuotes) {
                return line.substring(0, i);
            }
        }
        return line;
    }
}
