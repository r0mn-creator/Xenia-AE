package org.xeniaae;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.channels.FileChannel;

/**
 * Reads metadata fields from a XEX2 (Xbox 360 executable) file.
 * Works at an arbitrary offset within a FileChannel so it can read XEX data
 * that is embedded inside a disc image without extracting it first.
 */
class XexMetaReader {

    private static final int XEX2_MAGIC              = 0x58455832; // 'XEX2'
    private static final int XEX_HEADER_EXECUTION_INFO = 0x00040006;

    /**
     * Reads the Title ID from the XEX2 file starting at {@code xexOffset} in
     * {@code channel}. Returns 0 if the file is not a valid XEX2 or does not
     * contain an execution-info optional header.
     *
     * <p>The Title ID is the 32-bit big-endian value at offset 0x0C inside the
     * {@code xex2_opt_execution_info} struct (media_id + version + base_version
     * precede it).</p>
     */
    static int readTitleId(FileChannel channel, long xexOffset) {
        try {
            // Main header: magic(4) module_flags(4) header_size(4) reserved(4)
            //              security_offset(4) header_count(4) = 24 bytes
            channel.position(xexOffset);
            ByteBuffer hdr = ByteBuffer.allocate(24).order(ByteOrder.BIG_ENDIAN);
            if (channel.read(hdr) < 24) return 0;
            hdr.flip();

            if (hdr.getInt(0) != XEX2_MAGIC) return 0;

            int optCount = hdr.getInt(20); // header_count at offset 0x14
            if (optCount <= 0 || optCount > 256) return 0;

            // Optional header table: each entry is (key: 4 bytes, offset: 4 bytes)
            ByteBuffer opts = ByteBuffer.allocate(optCount * 8).order(ByteOrder.BIG_ENDIAN);
            if (channel.read(opts) < optCount * 8) return 0;
            opts.flip();

            for (int i = 0; i < optCount; i++) {
                int key    = opts.getInt(i * 8);
                int offset = opts.getInt(i * 8 + 4);

                if (key != XEX_HEADER_EXECUTION_INFO) continue;

                // offset is relative to start of the XEX file (not end of header).
                // The execution-info struct has NO size prefix — 24 bytes of raw data.
                long structAt = xexOffset + Integer.toUnsignedLong(offset);
                channel.position(structAt);
                ByteBuffer exec = ByteBuffer.allocate(24).order(ByteOrder.BIG_ENDIAN);
                if (channel.read(exec) < 24) return 0;
                exec.flip();
                // xex2_opt_execution_info layout (all BE):
                //   0x00 media_id(4)  0x04 version(4)  0x08 base_version(4)
                //   0x0C title_id(4)  0x10 platform(1) ...
                return exec.getInt(12);
            }
        } catch (IOException ignored) {}
        return 0;
    }
}
