package com.aure.totk.setup;

import java.io.BufferedInputStream;
import java.io.DataInputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.HashSet;
import java.util.Set;

/** How many shader variants the runtime's seed reader will find in <game>/shaders/<version> (gfx/vk_seed_io.c
 * inventory_build): the entries of seed.vkpack (when the pack is valid, gfx/vk_seed_pack.h) and the loose files beside
 * it, a loose file of the same name as a packed one taking its place (counted once). A variant is one translated SPIR-V
 * module, a *.spv name (setup.c run_shaders counts the same way). Plain Java: tests/test_android_shader_seed.py runs it
 * on a host JVM. The count is kept per folder until the pack or the folder changes, so the menu reads the pack's index
 * (104 bytes per entry, ~6 MB for 63,000 entries) once. */
final class ShaderSeed {
    private ShaderSeed() { }
    static final String PACK = "seed.vkpack";
    private static final int HEADER = 32, RECORD = 104, NAME = 84;

    /** variants: distinct *.spv names; packed: of them in a valid seed.vkpack; loose: loose *.spv not in the pack. */
    static final class Count {
        final int variants, packed, loose; final boolean pack;
        Count(int packed, int loose, boolean pack) { this.packed = packed; this.loose = loose; this.variants = packed + loose; this.pack = pack; }
    }

    private static String cachedKey;
    private static Count cached;

    static synchronized Count count(File dir) {
        File pack = new File(dir, PACK);
        String key = dir.getPath() + '|' + pack.length() + '|' + pack.lastModified() + '|' + dir.lastModified();
        if (key.equals(cachedKey)) return cached;
        Count c = scan(dir);
        cachedKey = key; cached = c;
        return c;
    }
    static int variants(File dir) { return count(dir).variants; }

    static Count scan(File dir) {
        Set<String> packed = packedModules(new File(dir, PACK));
        int loose = 0;
        String[] names = dir.list();
        if (names != null) for (String name : names)
            if (module(name) && !(packed != null && packed.contains(name)) && new File(dir, name).isFile()) loose++;
        return new Count(packed == null ? 0 : packed.size(), loose, packed != null);
    }

    /** A name the seed reader takes (vk_seed_pack_valid_name) that is a module: [A-Za-z0-9_.-]{5,83} ending in .spv. */
    static boolean module(String name) { return seedName(name, ".spv"); }
    /** [A-Za-z0-9_.-]{1,83} with more than suffix. */
    private static boolean seedName(String name, String suffix) {
        int n = name.length();
        if (n <= suffix.length() || n > NAME - 1 || !name.endsWith(suffix)) return false;
        for (int i = 0; i < n; i++) {
            char c = name.charAt(i);
            if (!(c >= 'A' && c <= 'Z' || c >= 'a' && c <= 'z' || c >= '0' && c <= '9' || c == '_' || c == '-' || c == '.')) return false;
        }
        return true;
    }

    /** The *.spv names of a TOTKVKP1 pack (<8sIIQQ> header: magic, version 1, record size 104, entries, file size; then
     * <84sIQQ> records sorted by name), or null when the pack is missing or the runtime would refuse it (the checks of
     * vk_seed_pack_open, except that payload ranges are not compared for overlap): it then reads the loose files only. */
    static Set<String> packedModules(File pack) {
        long size = pack.length();
        if (!pack.isFile() || size < HEADER) return null;
        try (DataInputStream in = new DataInputStream(new BufferedInputStream(new FileInputStream(pack), 1 << 16))) {
            byte[] header = new byte[HEADER];
            in.readFully(header);
            ByteBuffer h = ByteBuffer.wrap(header).order(ByteOrder.LITTLE_ENDIAN);
            if (!new String(header, 0, 8, StandardCharsets.US_ASCII).equals("TOTKVKP1") || h.getInt(8) != 1 || h.getInt(12) != RECORD
                    || h.getLong(24) != size) return null;
            long count = h.getLong(16);
            if (count < 0 || count > (size - HEADER) / RECORD) return null;
            long indexEnd = HEADER + count * RECORD;
            Set<String> out = new HashSet<>();
            byte[] record = new byte[RECORD];
            ByteBuffer r = ByteBuffer.wrap(record).order(ByteOrder.LITTLE_ENDIAN);
            String previous = null;
            for (long i = 0; i < count; i++) {
                in.readFully(record);
                int n = 0;
                while (n < NAME && record[n] != 0) n++;
                if (n == 0 || n > NAME - 1) return null;
                for (int k = n + 1; k < NAME; k++) if (record[k] != 0) return null;
                String name = new String(record, 0, n, StandardCharsets.ISO_8859_1);
                if (!module(name) && !seedName(name, ".vk") && !seedName(name, ".vv")) return null;
                if (previous != null && previous.compareTo(name) >= 0) return null;   // bytewise: the names are ASCII
                previous = name;
                long offset = r.getLong(88), length = r.getLong(96);
                if (r.getInt(84) != 0 || (offset & 7) != 0 || offset < indexEnd || offset > size || length < 0 || length > size - offset) return null;
                if (name.endsWith(".spv")) out.add(name);
            }
            return out;
        } catch (IOException unreadable) { return null; }
    }
}
