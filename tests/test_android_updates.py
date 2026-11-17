"""Production update URL/version/checksum/trust policy and cross-process private-file persistence on a host JVM.

Each app asks its own repository (games/games.toml [flavour]: repository, release_tag, release_asset, by package): the
fork's and BotW's releases, never each other's or upstream's."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'games/totk/android/java/com/aure/totk/UpdateRules.java'
sys.path.insert(0, str(ROOT / 'tools'))
import games  # noqa: E402
PROBE = r'''
import com.aure.totk.UpdateRules;
import java.net.URI;
import java.io.*;
import java.nio.file.*;
public class UpdateProbe {
    static URI github = URI.create("https://github.com");
    // The test's own release source (the real ones are checked in "flavours")
    static UpdateRules.Release own = new UpdateRules.Release("com.aure.totk", "https://github.com/AurelioB/totk-android", "v", "TotK-Native-");
    static String tag(String version) { return "https://github.com/AurelioB/totk-android/releases/tag/v" + version; }
    static void equal(Object a, Object b) { if (!java.util.Objects.equals(a, b)) throw new AssertionError(a + " != " + b); }
    public static void main(String[] args) throws Exception {
        switch (args[0]) {
        case "location":
            equal(UpdateRules.location(tag("1.0.2"), github, false, own), "1.0.2");
            for (String bad : new String[]{"", "/AurelioB/totk-android/releases/tag/v1.0.2", tag("1.0.2-rc1"), tag("1.0.2+build"),
                tag("01.0.2"), tag("1.2"), tag("1.0.2/"), tag("1.0.2?x=1"), tag("1.0.2#x"), tag("%31.0.2"),
                tag("1.0.2").replace("github.com", "github.com.evil.test"), tag("1.0.2").replace("https:", "http:"),
                tag("1.0.2").replace("github.com", "user@github.com"), tag("1.0.2").replace("github.com", "github.com:443"),
                tag("1.0.2").replace("AurelioB", "Other")}) equal(UpdateRules.location(bad, github, false, own), null);
            URI fake = URI.create("http://192.168.8.2:8042");
            equal(UpdateRules.location("http://192.168.8.2:8042/AurelioB/totk-android/releases/tag/v1.0.2", fake, true, own), "1.0.2");
            equal(UpdateRules.location("http://192.168.8.3:8042/AurelioB/totk-android/releases/tag/v1.0.2", fake, true, own), null);
            equal(UpdateRules.location(tag("1.0.2"), github, false, null), null);
            break;
        case "flavours": {
            // args: package repository tag asset, per flavour row; then the expectations below use them
            String[][] table = new String[(args.length - 1) / 4][];
            for (int i = 0; i < table.length; i++) table[i] = java.util.Arrays.copyOfRange(args, 1 + 4 * i, 5 + 4 * i);
            for (String[] row : table) {
                UpdateRules.Release r = UpdateRules.release(row[0], table);
                equal(r.repository, row[1]);
                String own = row[1] + "/releases/tag/" + row[2] + "2.0.0";
                equal(UpdateRules.location(own, github, false, r), "2.0.0");
                equal(UpdateRules.asset(r, "2.0.0"), row[3] + "2.0.0.apk");
                equal(UpdateRules.check(true, true, "1.9.9", row[0], false, github, r, () -> own), "2.0.0");
                // a release build of the current version, or a pre-release / dev build, is not offered anything (no nag)
                equal(UpdateRules.check(true, true, "2.0.0", row[0], false, github, r, () -> own), null);
                equal(UpdateRules.check(true, true, "1.9.9-pre.1", row[0], false, github, r, () -> own), null);
                // every other app's releases, and upstream's, are not this app's
                for (String[] other : table) {
                    if (other[1].equals(row[1]) && other[2].equals(row[2])) continue;
                    String theirs = other[1] + "/releases/tag/" + other[2] + "2.0.0";
                    equal(UpdateRules.location(theirs, github, false, r), null);
                    equal(UpdateRules.check(true, true, "1.9.9", row[0], false, github, r, () -> theirs), null);
                }
                equal(UpdateRules.location(tag("2.0.0"), github, false, r), null);
                equal(UpdateRules.check(true, true, "1.9.9", row[0], false, github, r, () -> tag("2.0.0")), null);
                // another package with this table's row: not asked
                equal(UpdateRules.check(true, true, "1.9.9", row[0] + ".other", false, github, r, () -> own), null);
            }
            equal(UpdateRules.release("com.aure.totk", table), null);   // no flavour: no update source
            equal(UpdateRules.release("x", new String[][]{{"x", "https://evil.test/a/b", "v", "A-"}}), null);
            equal(UpdateRules.release("x", new String[][]{{"x", "https://github.com/a/b", "../", "A-"}}), null);
            break;
        }
        case "semver":
            equal(Integer.signum(UpdateRules.compare("1.10.0", "1.9.999")), 1);
            equal(Integer.signum(UpdateRules.compare("2.0.0", "1.999.999")), 1);
            equal(Integer.signum(UpdateRules.compare("1.0.0", "1.0.1")), -1);
            equal(UpdateRules.compare("1.0.1", "1.0.1"), 0);
            equal(Integer.signum(UpdateRules.compare("999999999999999999999999.0.0", "2.0.0")), 1);
            for (String bad : new String[]{"1.0.1-dev", "v1.0.1", "1.0.01", "1.0", "1.0.1\n"}) equal(UpdateRules.version(bad), false);
            break;
        case "checksum":
            String hash = "aB".repeat(32), asset = UpdateRules.asset(own, "1.0.2");
            equal(UpdateRules.sha256(hash + "  " + asset + "\n", asset), hash.toLowerCase(java.util.Locale.ROOT));
            equal(UpdateRules.sha256(hash + " *" + asset + "\r\n", asset), hash.toLowerCase(java.util.Locale.ROOT));
            for (String bad : new String[]{hash, hash + "  other.apk", hash + "  ../" + asset, hash + "  " + asset + "\nextra", "g".repeat(64) + "  " + asset}) {
                try { UpdateRules.sha256(bad, asset); throw new AssertionError("accepted " + bad); } catch (IOException expected) { }
            }
            break;
        case "decision":
            int[] calls = {0};
            UpdateRules.Head head = () -> { calls[0]++; return tag("1.0.2"); };
            equal(UpdateRules.check(true, true, "1.0.1", "com.aure.totk", false, github, own, head), "1.0.2");
            equal(calls[0], 1);
            equal(UpdateRules.check(false, true, "1.0.1", "com.aure.totk", false, github, own, head), null);
            equal(UpdateRules.check(true, false, "1.0.1", "com.aure.totk", false, github, own, head), null);
            equal(UpdateRules.check(true, true, "1.0.1-dev", "com.aure.totk", false, github, own, head), null);
            equal(UpdateRules.check(true, true, "1.0.1", "com.aure.totk.test", false, github, own, head), null);
            equal(calls[0], 1);
            equal(UpdateRules.check(true, true, "1.0.2", "com.aure.totk", false, github, own, head), null);
            equal(UpdateRules.check(true, true, "1.0.3", "com.aure.totk", false, github, own, head), null);
            equal(UpdateRules.check(true, true, "1.0.1", "com.aure.totk", false, github, own, () -> { throw new java.net.SocketTimeoutException(); }), null);
            equal(UpdateRules.check(true, true, "1.0.1", "com.aure.totk", false, github, own, () -> "malformed"), null);
            equal(UpdateRules.check(true, true, "1.0.1", "com.aure.totk.test", true, github, own, head), "1.0.2");
            break;
        case "trust":
            byte[][] cert = {new byte[]{1, 2, 3}}, wrong = {new byte[]{4, 5, 6}};
            equal(UpdateRules.verify("com.aure.totk", 10001, cert, "com.aure.totk", 10002, cert), null);
            for (String result : new String[]{
                UpdateRules.verify("com.aure.totk", 10001, cert, "other", 10002, cert),
                UpdateRules.verify("com.aure.totk", 10001, cert, "com.aure.totk", 10001, cert),
                UpdateRules.verify("com.aure.totk", 10001, cert, "com.aure.totk", 10000, cert),
                UpdateRules.verify("com.aure.totk", 10001, cert, "com.aure.totk", 10002, wrong),
                UpdateRules.verify("com.aure.totk", 10001, null, "com.aure.totk", 10002, cert),
                UpdateRules.verify("com.aure.totk", 10001, cert, "com.aure.totk", 10002, null)}) if (result == null) throw new AssertionError("accepted untrusted APK");
            equal(UpdateRules.allowedDownload(URI.create("https://release-assets.githubusercontent.com/a?token=b"), github, false), true);
            for (String bad : new String[]{"http://github.com/a", "https://github.com.evil/a", "https://evilgithubusercontent.com/a", "https://user@github.com/a", "https://github.com:8080/a"})
                equal(UpdateRules.allowedDownload(URI.create(bad), github, false), false);
            break;
        case "persistence":
            File files = new File(args[1]);
            equal(UpdateRules.enabled(files), true);
            UpdateRules.enabled(files, false);
            equal(UpdateRules.enabled(new File(args[1])), false);
            UpdateRules.setPending(files, "1.0.2");
            equal(UpdateRules.pending(new File(args[1]), "1.0.1"), "1.0.2");
            equal(UpdateRules.pending(files, "1.0.2"), null);
            equal(new File(files, "pending-update.txt").exists(), false);
            UpdateRules.enabled(files, true);
            equal(UpdateRules.enabled(files), true);
            Files.writeString(new File(files, "pending-update.txt").toPath(), "1.0.2-rc1");
            equal(UpdateRules.pending(files, "1.0.1"), null);
            equal(UpdateRules.checkAndStore(files, true, true, "1.0.1", "com.aure.totk", false, github, own, () -> tag("1.0.2")), "1.0.2");
            equal(UpdateRules.pending(files, "1.0.1"), "1.0.2");
            File blocked = new File(files, "not-a-directory");
            Files.write(blocked.toPath(), new byte[]{1});
            int[] attempted = {0};
            equal(UpdateRules.checkAndStore(blocked, true, true, "1.0.1", "com.aure.totk", false, github, own,
                    () -> { attempted[0]++; return tag("1.0.2"); }), null);
            equal(attempted[0], 1);
            break;
        default: throw new AssertionError(args[0]);
        }
    }
}
'''


class AndroidUpdateTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which('javac') or not shutil.which('java'):
            raise unittest.SkipTest('javac/java not found')
        cls.temp = tempfile.TemporaryDirectory(prefix='totk-updates-')
        cls.work = Path(cls.temp.name)
        probe = cls.work / 'UpdateProbe.java'
        probe.write_text(PROBE)
        subprocess.run(['javac', '--release', '17', '-d', str(cls.work), str(SOURCE), str(probe)], check=True, capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def probe(self, name, *args):
        with tempfile.TemporaryDirectory(prefix='totk-update-state-') as state:
            run = subprocess.run(['java', '-cp', str(self.work), 'UpdateProbe', name, *(args or (state,))], capture_output=True, text=True, timeout=10)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def test_location(self): self.probe('location')
    def test_semver(self): self.probe('semver')

    def test_each_app_asks_only_its_own_repository(self):
        data = games.load()
        rows = [row for g in data['games'] for row in games.releases(g, data)]
        repos = {row[0]: row[1] for row in rows}
        self.assertEqual(repos['io.github.p1r4n351.botw'], 'https://github.com/P1R4N351/botw-android')
        self.assertEqual(repos['io.github.kurigohankamehameha.totk'], 'https://github.com/KuriGohan-Kamehameha/totk-android')
        self.assertNotIn('AurelioB', ' '.join(repos.values()))
        self.probe('flavours', *(x for row in rows for x in row))
        for g in data['games']:   # GameBuild carries the game's rows
            for row in games.releases(g, data):
                self.assertIn('{' + ', '.join(f'"{x}"' for x in row) + '}', games.java(g, data))
    def test_checksum(self): self.probe('checksum')
    def test_decision(self): self.probe('decision')
    def test_signature_package_version(self): self.probe('trust')
    def test_pending_persistence_and_default_on(self): self.probe('persistence')
