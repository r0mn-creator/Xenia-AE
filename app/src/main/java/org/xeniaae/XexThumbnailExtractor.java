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
                        return readImageAt(channel, dataOffset);
                    }
                }
            }
        } catch (Exception ignored) {
        }
        return null;
    }

    @Nullable
    private static Bitmap readImageAt(FileChannel channel, int offset) {
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
