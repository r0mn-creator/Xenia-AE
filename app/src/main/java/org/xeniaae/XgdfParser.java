package org.xeniaae;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;

/**
 * Minimal parser for the Xbox Game Disc Format (GDFX/XDVDFS) filesystem.
 * Used to locate default.xex within an Xbox 360 disc image so that metadata
 * can be extracted from it without a full disc mount.
 */
class XgdfParser {

    private static final int SECTOR_SIZE = 2048;
    // Same candidate partition offsets Xenia tries when opening a disc image.
    private static final long[] GAME_OFFSETS = {
        0x00000000L, 0x0000FB20L, 0x00020600L, 0x02080000L, 0x0FD90000L
    };
    private static final byte[] MAGIC =
            "MICROSOFT*XBOX*MEDIA".getBytes(StandardCharsets.US_ASCII);

    /**
     * Returns the byte offset of default.xex within {@code channel}, or -1 if
     * the file is not a recognised Xbox 360 disc image or does not contain a
     * default.xex at the root.
     */
    static long findDefaultXex(FileChannel channel) throws IOException {
        for (long gameOffset : GAME_OFFSETS) {
            long descriptorAt = gameOffset + 32L * SECTOR_SIZE;
            if (!checkMagic(channel, descriptorAt)) continue;

            // Volume descriptor: root_sector at +20, root_size at +24 (both LE uint32)
            channel.position(descriptorAt + 20);
            ByteBuffer vd = ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN);
            if (channel.read(vd) < 8) continue;
            vd.flip();
            long rootSector = Integer.toUnsignedLong(vd.getInt(0));
            long rootSize   = Integer.toUnsignedLong(vd.getInt(4));

            if (rootSize < 14 || rootSize > 8 * 1024 * 1024) continue;

            long rootAt = gameOffset + rootSector * SECTOR_SIZE;
            if (rootAt + rootSize > channel.size()) continue;

            channel.position(rootAt);
            ByteBuffer dir = ByteBuffer.allocate((int) rootSize).order(ByteOrder.LITTLE_ENDIAN);
            int read = 0;
            while (read < (int) rootSize) {
                int r = channel.read(dir);
                if (r < 0) break;
                read += r;
            }
            dir.flip();

            long xexSector = findEntry(dir, 0, "default.xex", 0);
            if (xexSector >= 0) {
                return gameOffset + xexSector * SECTOR_SIZE;
            }
        }
        return -1;
    }

    private static boolean checkMagic(FileChannel channel, long offset) {
        try {
            if (offset + MAGIC.length > channel.size()) return false;
            channel.position(offset);
            ByteBuffer buf = ByteBuffer.allocate(MAGIC.length);
            int read = channel.read(buf);
            if (read < MAGIC.length) return false;
            byte[] arr = buf.array();
            for (int i = 0; i < MAGIC.length; i++) {
                if (arr[i] != MAGIC[i]) return false;
            }
            return true;
        } catch (IOException e) {
            return false;
        }
    }

    /**
     * BST search for {@code target} (case-insensitive) in the XDVDFS directory
     * block. Returns the first sector of the file, or -1 if not found.
     *
     * Directory entries are indexed by ordinal (entry_ordinal * 4 == byte offset
     * in the block). Left child ordinal 0 means "no left child"; same for right.
     * Root is always at ordinal 0 — it is never a child of anything in a valid image.
     */
    private static long findEntry(ByteBuffer dir, int ordinal, String target, int depth) {
        if (depth > 32) return -1; // guard against corrupt images

        int byteOff = ordinal * 4;
        if (byteOff + 14 > dir.limit()) return -1;

        int nodeL   = Short.toUnsignedInt(dir.getShort(byteOff));
        int nodeR   = Short.toUnsignedInt(dir.getShort(byteOff + 2));
        long sector = Integer.toUnsignedLong(dir.getInt(byteOff + 4));
        // size at +8, attributes at +12
        int nameLen = Byte.toUnsignedInt(dir.get(byteOff + 13));

        if (nameLen == 0 || byteOff + 14 + nameLen > dir.limit()) return -1;

        byte[] nb = new byte[nameLen];
        dir.position(byteOff + 14);
        dir.get(nb);
        String name = new String(nb, StandardCharsets.US_ASCII);

        int cmp = target.compareToIgnoreCase(name);
        if (cmp == 0) return sector;

        // BST: go left if target < current, right if target > current
        if (cmp < 0 && nodeL != 0) {
            long r = findEntry(dir, nodeL, target, depth + 1);
            if (r >= 0) return r;
        } else if (cmp > 0 && nodeR != 0) {
            long r = findEntry(dir, nodeR, target, depth + 1);
            if (r >= 0) return r;
        }
        return -1;
    }
}
