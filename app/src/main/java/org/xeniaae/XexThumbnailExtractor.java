package org.xeniaae;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.net.Uri;
import android.os.ParcelFileDescriptor;

import androidx.annotation.Nullable;

import java.io.FileInputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.channels.FileChannel;

public class XexThumbnailExtractor {

    private static final int XEX2_MAGIC = 0x58455832;
    private static final int HEADER_THUMBNAIL_SMALL = 0x00005007;
    private static final int HEADER_THUMBNAIL_LARGE = 0x00009007;

    @Nullable
    public static Bitmap extract(Context context, Uri uri) {
        try (ParcelFileDescriptor pfd = context.getContentResolver()
                .openFileDescriptor(uri, "r")) {
            if (pfd == null) return null;

            try (FileInputStream fis = new FileInputStream(pfd.getFileDescriptor());
                 FileChannel channel = fis.getChannel()) {

                // Every 360 title embeds its own artwork in the XEX header, so
                // the game file itself is the most reliable source - no network,
                // no API key, and it matches the exact release the user owns.
                //
                // A bare default.xex (GOD / XBLA folder installs) starts with
                // the XEX2 magic. A DISC IMAGE does not: the XEX sits inside the
                // GDF filesystem, so reading offset 0 finds no magic. This used
                // to return null right there, which is why every .iso in the
                // library showed no art while folder installs did.
                android.util.Log.d("BoxArtManager", "XEX extract start: " + uri);
                long xexBase = 0;
                ByteBuffer magic = ByteBuffer.allocate(4).order(ByteOrder.BIG_ENDIAN);
                channel.position(0);
                if (channel.read(magic) < 4) return null;
                magic.flip();
                if (magic.getInt(0) != XEX2_MAGIC) {
                    // Not a raw XEX - locate default.xex inside the disc image.
                    xexBase = XgdfParser.findDefaultXex(channel);
                    android.util.Log.d("BoxArtManager",
                            "not a raw XEX; default.xex in image at " + xexBase);
                    if (xexBase < 0) return null;
                }

                channel.position(xexBase);
                ByteBuffer header = ByteBuffer.allocate(24).order(ByteOrder.BIG_ENDIAN);
                if (channel.read(header) < 24) return null;
                header.flip();

                if (header.getInt(0) != XEX2_MAGIC) return null;

                int optCount = header.getInt(20);
                if (optCount <= 0 || optCount > 256) return null;

                ByteBuffer optHeaders = ByteBuffer.allocate(optCount * 8).order(ByteOrder.BIG_ENDIAN);
                if (channel.read(optHeaders) < optCount * 8) return null;
                optHeaders.flip();

                for (int i = 0; i < optCount; i++) {
                    int key = optHeaders.getInt(i * 8);
                    int dataOffset = optHeaders.getInt(i * 8 + 4);

                    if (key == HEADER_THUMBNAIL_LARGE || key == HEADER_THUMBNAIL_SMALL) {
                        android.util.Log.d("BoxArtManager",
                                "thumbnail header found, key=0x" + Integer.toHexString(key));
                        // Offsets in the XEX header are relative to the XEX, so
                        // inside a disc image they must be biased by its base.
                        return readImageAt(channel, xexBase + dataOffset);
                    }
                }
                android.util.Log.d("BoxArtManager",
                        "no thumbnail header among " + optCount + " opt headers");
            }
        } catch (Exception e) {
            android.util.Log.d("BoxArtManager", "XEX extract failed: " + e);
        }
        return null;
    }

    @Nullable
    private static Bitmap readImageAt(FileChannel channel, long offset) {
        try {
            channel.position(offset);
            ByteBuffer sizeBuf = ByteBuffer.allocate(4).order(ByteOrder.BIG_ENDIAN);
            if (channel.read(sizeBuf) < 4) return null;
            sizeBuf.flip();

            // Size field includes itself
            int dataSize = sizeBuf.getInt() - 4;
            if (dataSize <= 0 || dataSize > 2 * 1024 * 1024) return null;

            ByteBuffer data = ByteBuffer.allocate(dataSize);
            int read = 0;
            while (read < dataSize) {
                int r = channel.read(data);
                if (r < 0) break;
                read += r;
            }

            byte[] bytes = data.array();
            return BitmapFactory.decodeByteArray(bytes, 0, read);
        } catch (Exception ignored) {
            return null;
        }
    }
}
