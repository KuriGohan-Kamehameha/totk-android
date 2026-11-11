package com.aure.totk.setup;

import java.io.BufferedInputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

/** Android-free import of the player's own Lossless.dll (Lossless Scaling) for the LSFG frame-generation engine
 * (android/gfx/vk_framegen_lsfg.c reads <private files>/lsfg/Lossless.dll). The file is copied as it is: nothing is
 * derived from it here, and the app ships neither it nor anything made from it. */
final class LsfgImport {
    static final String FOLDER = "lsfg", NAME = "Lossless.dll";
    static final long MAX = 64L << 20;   // Lossless.dll 3.2 is a few MB; this only bounds a wrong pick

    static final class Invalid extends IOException {
        Invalid(String why) { super(why); }
    }

    /** Where the engine looks: <private files>/lsfg/Lossless.dll. */
    static File target(File privateFiles) { return new File(new File(privateFiles, FOLDER), NAME); }

    /** Owns/closes source. The target changes only after the whole file was copied and looks like a Windows DLL (MZ
     * header pointing at a PE signature inside the file); otherwise it stays as it was. Returns the size copied. */
    static long install(InputStream source, File target) throws IOException {
        File folder = target.getParentFile(), temp = new File(target.getPath() + ".tmp");
        if (!folder.isDirectory() && !folder.mkdirs()) throw new IOException("cannot create " + folder);
        try {
            long size = 0;
            int pe = -1;
            byte[] head = new byte[0];
            try (InputStream in = new BufferedInputStream(source); OutputStream out = new FileOutputStream(temp)) {
                byte[] block = new byte[1 << 16];
                for (int n; (n = in.readNBytes(block, 0, block.length)) > 0; size += n) {
                    if (size + n > MAX) throw new Invalid("larger than 64 MB");
                    if (size == 0) {
                        head = java.util.Arrays.copyOf(block, n);
                        if (n < 64 || block[0] != 'M' || block[1] != 'Z') throw new Invalid("no MZ header");
                        pe = (block[60] & 0xff) | (block[61] & 0xff) << 8 | (block[62] & 0xff) << 16 | (block[63] & 0xff) << 24;
                    }
                    out.write(block, 0, n);
                }
            }
            if (size == 0) throw new Invalid("empty file");
            if (pe < 64 || pe + 4 > head.length || head[pe] != 'P' || head[pe + 1] != 'E' || head[pe + 2] != 0 || head[pe + 3] != 0)
                throw new Invalid("no PE signature");
            if (!temp.renameTo(target)) throw new IOException("rename failed");
            return size;
        } finally {
            temp.delete();
        }
    }
}
