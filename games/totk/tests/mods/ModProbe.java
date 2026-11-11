package com.aure.totk.setup;

import java.io.File;
import java.io.FileInputStream;
import java.io.FilterInputStream;
import java.io.IOException;
import java.io.InputStream;

/** Runs the production mod import (ModImport) on a host JVM.
 *   tree <folder> <private files> [interrupt]    zip <file> <private files> [interrupt]
 *   read <private files>      remove <private files>      recover <private files> */
public final class ModProbe {
    private static String line(ModImport.Summary s) {
        return s == null ? "none" : "files=" + s.files + " bytes=" + s.bytes + " patches=" + s.patches + " modules=" + String.join(",", s.modules)
                + " ignored=" + s.ignored + " source=" + s.source;
    }
    public static void main(String[] args) throws Exception {
        File files = new File(args[args[0].equals("tree") || args[0].equals("zip") ? 2 : 1]);
        if (args[0].equals("read")) { System.out.println(line(ModImport.read(files))); return; }
        if (args[0].equals("recover")) { ModImport.recover(files); System.out.println(line(ModImport.read(files))); return; }
        if (args[0].equals("remove")) { System.out.println(ModImport.remove(files) + " " + line(ModImport.read(files))); return; }
        File source = new File(args[1]);
        boolean interrupt = args.length > 3 && args[3].equals("interrupt");
        try (ModImport.Session session = new ModImport.Session(files, source.getName())) {
            if (args[0].equals("tree")) ModImport.addTree(session, source);
            else {
                InputStream in = new FileInputStream(source);
                if (interrupt) in = new FilterInputStream(in) {
                    long seen;
                    @Override public int read(byte[] b, int off, int len) throws IOException {
                        if (seen > 4096) throw new IOException("interrupted source");
                        int n = super.read(b, off, Math.min(len, 512)); if (n > 0) seen += n; return n;
                    }
                };
                ModImport.addZip(session, in);
            }
            if (interrupt && args[0].equals("tree")) throw new IOException("interrupted before commit");
            System.out.println("imported " + line(session.commit()));
        } catch (ModImport.Invalid invalid) {
            System.out.println("invalid " + invalid.code);
        } catch (IOException failed) {
            System.out.println("failed " + failed.getMessage());
        }
    }
}
