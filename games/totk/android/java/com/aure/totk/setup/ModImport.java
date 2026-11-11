package com.aure.totk.setup;

import java.io.BufferedInputStream;
import java.io.Closeable;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/** Android-free import of mods into <private files>/mods/<version>, where the runtime looks when the game folder has no
 * mods of its own (runtime/frontend_android.c). What is imported is a mod manager's merge as it is: a romfs/ folder,
 * which the runtime scans at start (runtime/romfs.c mods_load), and beside it an exefs/ folder, of which the IPS
 * patches of main are applied (runtime/mod_ips.h) and code modules are not loaded. Private storage keeps the mods
 * where saves are: a move or a deletion of the game data leaves them alone, and nothing else on the device can
 * change them between the import and the game.
 *
 * The picked folder or zip may hold romfs/ at its top or below one or more folders (TKMM's Merged, a mod's own
 * folder); everything outside romfs/ and exefs/ is left out, and so are desktop leftovers and a manifest.tsv (it
 * would bind the files to hashes made elsewhere). Archive paths are checked component by component and are never
 * output paths as given. The earlier mods are replaced only after the whole import was copied (two renames; the
 * earlier mods are restored if the process dies between them, and they are not kept once the new ones are in place). */
final class ModImport {
    static final String VERSION = "v143", RECORD = "import.txt";
    static final int MAX_FILES = 200_000, MAX_DEPTH = 32, MAX_PATH = 4095;
    static final long MAX_BYTES = 64L << 30, MIN_FREE = 256L << 20;

    static final class Invalid extends IOException {
        final String code;
        Invalid(String code, String detail) { super(detail == null ? code : code + ": " + detail); this.code = code; }
    }

    /** What an import holds, and what the menu shows for the mods that are installed. */
    static final class Summary {
        int files, patches, ignored;
        long bytes;
        final List<String> modules = new ArrayList<>();   // exefs files that are neither an IPS patch nor main.npdm: not loaded
        String source = "";
    }

    static File folder(File privateFiles) { return new File(privateFiles, "mods/" + VERSION); }

    /** What a desktop leaves behind: the runtime's scan skips the same names (runtime/romfs_scan.h). */
    static boolean junk(String name) {
        return name.startsWith(".") || name.equalsIgnoreCase("desktop.ini") || name.equalsIgnoreCase("Thumbs.db");
    }

    /** One import. Entries are added under their path inside the picked folder or archive; commit() puts the result in
     * place of the earlier mods, and close() without it leaves those as they were. */
    static final class Session implements Closeable {
        private final File target, incoming, old;
        private final Summary summary = new Summary();
        private final Set<String> seen = new HashSet<>();
        private String prefix;
        private boolean committed;
        private long entries, skipped;

        Session(File privateFiles, String source) throws IOException {
            target = folder(privateFiles);
            incoming = new File(target.getPath() + ".incoming");
            old = new File(target.getPath() + ".old");
            recover(privateFiles);
            if (old.exists() && !target.exists()) throw new IOException("cannot restore the earlier mods");
            delete(old);        // a swap that was cut short after the new mods were in place
            delete(incoming);
            if (!incoming.mkdirs()) throw new IOException("cannot create " + incoming);
            summary.source = source == null ? "" : source.replace('\n', ' ').replace('\r', ' ');
        }

        /** Where an entry goes ("romfs/..." or "exefs/<name>"), or null when it is left out. Only an entry that is taken
         * decides which folder the import comes from, so a leftover beside the merge cannot make a good merge "several". */
        private String place(String path) throws Invalid {
            String[] parts = path.replace('\\', '/').split("/");   // some Windows archivers write backslashes
            int at = -1;
            for (int i = 0; i < parts.length && at < 0; i++) {
                if (parts[i].equalsIgnoreCase("romfs") || parts[i].equalsIgnoreCase("exefs")) at = i;
                else if (junk(parts[i]) || parts[i].equals("__MACOSX")) return null;      // below a leftover folder
            }
            if (at < 0 || at == parts.length - 1) return null;
            String kind = parts[at].toLowerCase(Locale.ROOT);
            StringBuilder out = new StringBuilder(kind);
            for (int i = at + 1; i < parts.length; i++) {
                String part = parts[i];
                if (part.isEmpty() || part.equals(".") || part.equals("..") || part.indexOf('\0') >= 0) throw new Invalid("bad_path", path);
                if (junk(part)) return null;
                out.append('/').append(part);
            }
            int depth = parts.length - at - 2;
            if (depth > MAX_DEPTH || out.length() > MAX_PATH) throw new Invalid("bad_path", path);
            String name = parts[parts.length - 1];
            if (kind.equals("exefs") && (depth > 0 || !name.matches("[A-Za-z0-9._-]{1,96}"))) return null;   // flat, plain names (they go into the record)
            if (kind.equals("romfs") && depth == 0 && name.equalsIgnoreCase("manifest.tsv")) return null;
            String before = String.join("/", Arrays.copyOf(parts, at));
            if (prefix == null) prefix = before;
            else if (!prefix.equals(before)) throw new Invalid("several", before.isEmpty() ? prefix : before);
            return out.toString();
        }

        private void budget() throws Invalid {
            if (++entries > 4L * MAX_FILES) throw new Invalid("too_many", "more than " + 4L * MAX_FILES + " entries");
            if (summary.bytes + skipped > MAX_BYTES) throw new Invalid("too_big", "more than " + (MAX_BYTES >> 30) + " GB");
        }

        /** Copy one entry (the stream is read to its end, not closed). True when it was taken; false when it is left
         * out, and then nothing was read from the stream. */
        boolean add(String path, InputStream in) throws IOException {
            budget();
            String rel = place(path);
            if (rel == null) { summary.ignored++; return false; }
            if (!seen.add(rel)) throw new Invalid("duplicate", rel);
            if (seen.size() > MAX_FILES) throw new Invalid("too_many", "more than " + MAX_FILES + " files");
            File file = new File(incoming, rel);
            File parent = file.getParentFile();
            if (!parent.isDirectory() && !parent.mkdirs()) throw new IOException("cannot create " + parent);
            if (incoming.getUsableSpace() < MIN_FREE) throw new Invalid("no_space", null);
            long size = 0;
            // CREATE_NEW: two names the filesystem takes for one (case, normalization) are refused, not written over each other
            try (OutputStream out = Files.newOutputStream(file.toPath(), java.nio.file.StandardOpenOption.CREATE_NEW, java.nio.file.StandardOpenOption.WRITE)) {
                byte[] block = new byte[1 << 16];
                for (int n; (n = in.read(block)) > 0; size += n) {
                    if (summary.bytes + skipped + size + n > MAX_BYTES) throw new Invalid("too_big", "more than " + (MAX_BYTES >> 30) + " GB");
                    if ((size + n) >> 26 != size >> 26 && incoming.getUsableSpace() < MIN_FREE) throw new Invalid("no_space", null);   // every 64 MB
                    out.write(block, 0, n);
                }
            } catch (java.nio.file.FileAlreadyExistsException alias) {
                throw new Invalid("duplicate", rel);
            }
            summary.bytes += size;
            if (rel.startsWith("romfs/")) summary.files++;
            else {
                String name = rel.substring(6);
                if (name.toLowerCase(Locale.ROOT).endsWith(".ips")) summary.patches++;
                else if (!name.equals("main.npdm")) summary.modules.add(name);
            }
            return true;
        }

        /** Read an entry that was left out to its end (a zip stream has to inflate it anyway), against the same size budget. */
        void skip(InputStream in) throws IOException {
            byte[] block = new byte[1 << 16];
            for (int n; (n = in.read(block)) > 0; ) {
                skipped += n;
                if (summary.bytes + skipped > MAX_BYTES) throw new Invalid("too_big", "more than " + (MAX_BYTES >> 30) + " GB");
            }
        }

        /** Put the import in place of the earlier mods and return what it holds. */
        Summary commit() throws IOException {
            if (summary.files == 0 && summary.patches == 0) throw new Invalid(summary.modules.isEmpty() ? "no_romfs" : "only_modules", null);
            java.util.Collections.sort(summary.modules);
            StringBuilder record = new StringBuilder();
            record.append("files=").append(summary.files).append("\nbytes=").append(summary.bytes).append("\npatches=").append(summary.patches)
                  .append("\nmodules=").append(String.join(",", summary.modules)).append("\nignored=").append(summary.ignored)
                  .append("\nsource=").append(summary.source).append('\n');
            Files.write(new File(incoming, RECORD).toPath(), record.toString().getBytes(StandardCharsets.UTF_8));
            if (target.exists() && !target.renameTo(old)) throw new IOException("cannot set the earlier mods aside");
            if (!incoming.renameTo(target)) {
                if (old.exists() && !old.renameTo(target)) throw new IOException("cannot restore the earlier mods");
                throw new IOException("cannot move the import into place");
            }
            committed = true;
            delete(old);
            return summary;
        }

        @Override public void close() { if (!committed) delete(incoming); }
    }

    /** Feed a folder (reachable as files: All files access, or a test) to a session; links are not followed. */
    static void addTree(Session session, File root) throws IOException {
        if (!root.isDirectory()) throw new Invalid("no_romfs", root.getName());
        walk(session, root, "", 0);
    }
    private static void walk(Session session, File dir, String path, int depth) throws IOException {
        if (depth > MAX_DEPTH + 8) throw new Invalid("bad_path", path);
        File[] children = dir.listFiles();
        if (children == null) throw new IOException("cannot read " + dir);
        Arrays.sort(children);
        for (File child : children) {
            String at = path.isEmpty() ? child.getName() : path + "/" + child.getName();
            if (Files.isSymbolicLink(child.toPath())) continue;
            if (child.isDirectory()) walk(session, child, at, depth + 1);
            else if (child.isFile()) try (InputStream in = new BufferedInputStream(new FileInputStream(child))) { session.add(at, in); }
        }
    }

    /** Feed a zip archive to a session, as a stream: nothing of it is kept outside the import. */
    static void addZip(Session session, InputStream archive) throws IOException {
        long entries = 0;
        try (ZipInputStream zip = new ZipInputStream(new BufferedInputStream(archive))) {
            for (ZipEntry entry; (entry = zip.getNextEntry()) != null; ) {
                if (++entries > 8L * MAX_FILES) throw new Invalid("too_many", "more than " + 8L * MAX_FILES + " entries");
                if (!entry.isDirectory() && !session.add(entry.getName(), zip)) session.skip(zip);
            }
        } catch (java.util.zip.ZipException | IllegalArgumentException bad) {
            throw new Invalid("bad_zip", bad.getMessage());
        }
        if (entries == 0) throw new Invalid("bad_zip", "no entries");
    }

    /** The mods that are installed, from the record the import wrote; null when there are none. */
    static Summary read(File privateFiles) {
        File record = new File(folder(privateFiles), RECORD);
        if (!record.isFile()) return null;
        if (record.length() > 1 << 16) return new Summary();
        Summary s = new Summary();
        try {
            for (String line : new String(Files.readAllBytes(record.toPath()), StandardCharsets.UTF_8).split("\n")) {
                int eq = line.indexOf('=');
                if (eq < 0) continue;
                String key = line.substring(0, eq), value = line.substring(eq + 1);
                switch (key) {
                    case "files": s.files = Integer.parseInt(value); break;
                    case "bytes": s.bytes = Long.parseLong(value); break;
                    case "patches": s.patches = Integer.parseInt(value); break;
                    case "ignored": s.ignored = Integer.parseInt(value); break;
                    case "modules": if (!value.isEmpty()) s.modules.addAll(Arrays.asList(value.split(","))); break;
                    case "source": s.source = value; break;
                    default: break;
                }
            }
        } catch (IOException | NumberFormatException unreadable) { return new Summary(); }
        return s;
    }

    /** After an import that died between its two renames only <version>.old is left: put those earlier mods back.
     * Call it with the game lock held or while no import can run (the menu does, before it reads the record). */
    static void recover(File privateFiles) {
        File target = folder(privateFiles), old = new File(target.getPath() + ".old");
        if (old.exists() && !target.exists()) old.renameTo(target);
    }

    /** Remove the imported mods (and what an interrupted import left). False when something could not be deleted. */
    static boolean remove(File privateFiles) {
        File target = folder(privateFiles), incoming = new File(target.getPath() + ".incoming"), old = new File(target.getPath() + ".old");
        delete(target); delete(incoming); delete(old);
        return !target.exists() && !incoming.exists() && !old.exists();
    }

    private static void delete(File file) {
        if (!Files.isSymbolicLink(file.toPath())) {
            File[] children = file.listFiles();
            if (children != null) for (File child : children) delete(child);
        }
        file.delete();
    }
}
