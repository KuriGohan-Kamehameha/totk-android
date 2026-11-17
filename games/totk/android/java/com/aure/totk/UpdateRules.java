package com.aure.totk;

import java.io.File;
import java.io.IOException;
import java.math.BigInteger;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.Arrays;
import java.util.Locale;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/** Update policy and private-file state, shared by the game and :setup and exercised on the host JVM. */
public final class UpdateRules {
    private static final Pattern VERSION = Pattern.compile("(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)");
    private static final Pattern REPOSITORY = Pattern.compile("https://github\\.com(/[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+)");
    private static final Pattern NAME = Pattern.compile("[A-Za-z0-9_.-]*");
    private UpdateRules() { }
    /** Where one app's releases are (games/games.toml [flavour], GameBuild.RELEASES): only the app with this package
     *  asks this repository, for tags <tag><version> carrying <asset><version>.apk. Each app has its own, so no app is
     *  offered another app's (or upstream's) release. */
    public static final class Release {
        public final String pkg, repository, project, tag, asset;
        public Release(String pkg, String repository, String tag, String asset) {
            Matcher m = REPOSITORY.matcher(repository == null ? "" : repository);
            if (pkg == null || pkg.isEmpty() || !m.matches() || tag == null || !NAME.matcher(tag).matches()
                    || asset == null || asset.isEmpty() || !NAME.matcher(asset).matches()) throw new IllegalArgumentException("Invalid release source");
            this.pkg = pkg; this.repository = repository; this.project = m.group(1); this.tag = tag; this.asset = asset;
        }
    }
    /** This package's row of table ({package, repository, tag, asset} rows), or null: an app that is none of the
     *  flavours (a build without --flavour) has no releases to check. */
    public static Release release(String pkg, String[][] table) {
        if (pkg != null && table != null)
            for (String[] row : table)
                if (row != null && row.length == 4 && pkg.equals(row[0])) {
                    try { return new Release(row[0], row[1], row[2], row[3]); } catch (IllegalArgumentException invalid) { return null; }
                }
        return null;
    }
    public static boolean version(String value) { return value != null && VERSION.matcher(value).matches(); }
    public static int compare(String left, String right) {
        if (!version(left) || !version(right)) throw new IllegalArgumentException("Invalid release version");
        String[] a = left.split("\\."), b = right.split("\\.");
        for (int i = 0; i < 3; i++) {
            int c = new BigInteger(a[i]).compareTo(new BigInteger(b[i]));
            if (c != 0) return c;
        }
        return 0;
    }
    public static String location(String value, URI source, boolean override, Release release) {
        if (release == null) return null;
        try {
            URI uri = new URI(value);
            if (uri.getRawUserInfo() != null || uri.getRawQuery() != null || uri.getRawFragment() != null) return null;
            if (!override && (!"https".equals(uri.getScheme()) || !"github.com".equals(uri.getHost()) || uri.getPort() != -1)) return null;
            if (override && !sameOrigin(uri, source)) return null;
            String prefix = release.project + "/releases/tag/" + release.tag;
            String path = uri.getRawPath();
            return path != null && path.startsWith(prefix) && version(path.substring(prefix.length())) ? path.substring(prefix.length()) : null;
        } catch (Exception malformed) { return null; }
    }
    public static boolean sameOrigin(URI a, URI b) {
        return a.getScheme() != null && a.getScheme().equals(b.getScheme()) && a.getHost() != null
                && a.getHost().equals(b.getHost()) && a.getPort() == b.getPort();
    }
    public static boolean allowedDownload(URI uri, URI source, boolean override) {
        if (uri.getRawUserInfo() != null || uri.getRawFragment() != null) return false;
        if (override) return sameOrigin(uri, source);
        String host = uri.getHost();
        return "https".equals(uri.getScheme()) && uri.getPort() == -1 && host != null
                && (host.equals("github.com") || host.endsWith(".githubusercontent.com"));
    }
    public static String asset(Release release, String version) {
        if (!version(version)) throw new IllegalArgumentException("Invalid release version");
        return release.asset + version + ".apk";
    }
    public static String sha256(String text, String filename) throws IOException {
        Matcher m = Pattern.compile("([0-9a-fA-F]{64}) [ *]" + Pattern.quote(filename) + "\\r?\\n?").matcher(text);
        if (!m.matches()) throw new IOException("The update checksum file is invalid.");
        return m.group(1).toLowerCase(Locale.ROOT);
    }
    public static String verify(String ownPackage, long ownCode, byte[][] ownCerts,
                                String archivePackage, long archiveCode, byte[][] archiveCerts) {
        if (!ownPackage.equals(archivePackage)) return "The update is for a different app.";
        if (archiveCode <= ownCode) return "The update is not newer than the installed app.";
        if (ownCerts == null || archiveCerts == null || ownCerts.length == 0 || ownCerts.length != archiveCerts.length)
            return "The update signing certificate does not match this app.";
        for (byte[] cert : ownCerts) {
            boolean found = false;
            for (byte[] other : archiveCerts) if (Arrays.equals(cert, other)) found = true;
            if (!found) return "The update signing certificate does not match this app.";
        }
        return null;
    }
    public interface Head { String location() throws IOException; }
    /** The newer version to offer, or null. Only a release build asks (current is x.y.z: a pre-release or dev build,
     *  e.g. 0.2.0-pre.1, never does), only for its own package's repository (release), and only about a newer x.y.z. */
    public static String check(boolean enabled, boolean online, String current, String pkg, boolean override, URI source,
                               Release release, Head head) {
        if (!enabled || !online || !version(current) || release == null || (!release.pkg.equals(pkg) && !override)) return null;
        try {
            String latest = location(head.location(), source, override, release);
            return latest != null && compare(latest, current) > 0 ? latest : null;
        } catch (IOException | RuntimeException failed) { return null; }
    }
    /** An offer is actionable only after :setup can recover it, including after the game process stops. */
    public static String checkAndStore(File files, boolean enabled, boolean online, String current, String pkg,
                                       boolean override, URI source, Release release, Head head) {
        String version = check(enabled, online, current, pkg, override, source, release, head);
        if (version == null) return null;
        try { setPending(files, version); return version; }
        catch (IOException | RuntimeException failed) { return null; }
    }
    public static boolean enabled(File files) { return !"off".equals(read(files, "update-checks.txt")); }
    public static void enabled(File files, boolean on) throws IOException { store(files, "update-checks.txt", on ? "on" : "off"); }
    public static void setPending(File files, String value) throws IOException {
        if (!version(value)) throw new IOException("Invalid pending update");
        store(files, "pending-update.txt", value);
    }
    public static String pending(File files, String current) {
        String value = read(files, "pending-update.txt");
        if (!version(value) || !version(current)) return null;
        if (compare(value, current) > 0) return value;
        try { clearPending(files); } catch (IOException ignored) { }
        return null;
    }
    public static void clearPending(File files) throws IOException { Files.deleteIfExists(new File(files, "pending-update.txt").toPath()); }
    public static String read(File files, String name) {
        try { return new String(Files.readAllBytes(new File(files, name).toPath()), StandardCharsets.UTF_8).trim(); }
        catch (IOException failed) { return ""; }
    }
    private static void store(File files, String name, String value) throws IOException {
        File tmp = File.createTempFile(name, ".tmp", files);
        try {
            Files.write(tmp.toPath(), value.getBytes(StandardCharsets.UTF_8));
            Files.move(tmp.toPath(), new File(files, name).toPath(), StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
        } finally { Files.deleteIfExists(tmp.toPath()); }
    }
}
