package com.aure.totk.setup;

import java.io.File;
import java.io.FileInputStream;
import java.io.FilterInputStream;
import java.io.IOException;
import java.io.InputStream;

/** Runs the production Lossless.dll import (LsfgImport) on a host JVM: args = source, private files folder, [failure]. */
public final class LsfgProbe {
    public static void main(String[] args) throws Exception {
        InputStream in = new FileInputStream(new File(args[0]));
        if (args.length > 2) {
            String failure = args[2];
            in = new FilterInputStream(in) {
                int reads;
                @Override public int read(byte[] b, int off, int len) throws IOException {
                    if (failure.equals("interrupt") && reads++ > 0) throw new IOException("interrupted source");
                    return super.read(b, off, Math.min(len, 100));   // short reads: the header must not depend on one read
                }
            };
        }
        File target = LsfgImport.target(new File(args[1]));
        try { System.out.println("imported " + LsfgImport.install(in, target) + " " + target.getPath()); }
        catch (LsfgImport.Invalid invalid) { System.out.println("invalid " + invalid.getMessage()); }
        catch (IOException failed) { System.out.println("failed " + failed.getMessage()); }
    }
}
