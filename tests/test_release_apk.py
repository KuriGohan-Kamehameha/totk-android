"""The public APK's content audit (games/totk/android/apk/audit_apk.py, run by build_release.py on every APK) and the
release build's input checks, on synthetic APKs; no game files, NDK or SDK.

TOTK_RELEASE_APK=<apk> additionally audits a real build_release.py output."""
import hashlib
import importlib.util
import os
from pathlib import Path
import sys
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / 'games/totk/android/apk')]
import audit_apk  # noqa: E402

spec = importlib.util.spec_from_file_location('release_build', ROOT / 'games/totk/android/build_release.py')
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)

ELF = b'\x7fELF' + bytes(60)
TTF = b'\x00\x01\x00\x00' + bytes(60)


def apk(path, entries, stored=()):
    with zipfile.ZipFile(path, 'w') as z:
        for name, data in entries.items():
            z.writestr(zipfile.ZipInfo(name), data, zipfile.ZIP_STORED if name in stored or name.endswith('.ttf') else zipfile.ZIP_DEFLATED)
    return path


GOOD = {'AndroidManifest.xml': b'\x03\x00', 'classes.dex': b'dex\n035', 'resources.arsc': b'\x02\x00',
        'META-INF/MANIFEST.MF': b'Manifest-Version: 1.0', 'META-INF/CERT.SF': b'sf', 'META-INF/CERT.RSA': b'rsa',
        'res/mipmap-xxxhdpi/ic_launcher.png': b'\x89PNG', 'res/mipmap-anydpi-v26/ic_launcher.xml': b'\x03\x00',
        'lib/arm64-v8a/libtotk.so': ELF, 'lib/arm64-v8a/libtotk_setup.so': ELF, 'lib/arm64-v8a/libhactool.so': ELF,
        'lib/arm64-v8a/libtotk_xlate.so': ELF, 'assets/pso_keys.bin': b'TOTKPS01', 'assets/fonts/NotoSans-Regular.ttf': TTF,
        'assets/fonts/NotoSans-OFL.txt': b'OFL', 'assets/licenses/Mesa-Turnip.txt': b'MIT'}


class AuditTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.dir = Path(self.temp.name)

    def audit(self, entries, **kw):
        return audit_apk.audit(apk(self.dir / f'{len(os.listdir(self.dir))}.apk', entries, **kw))

    def test_clean_apk_passes(self):
        self.assertEqual(self.audit(GOOD), [])

    def test_build_info_is_one_commit_line(self):
        self.assertEqual(self.audit({**GOOD, 'assets/build.txt': b'commit 85f12832ab12\n'}), [])
        self.assertEqual(self.audit({**GOOD, 'assets/build.txt': b'commit 85f12832ab12-dirty\n'}), [])
        for data in (b'commit 85f12832ab12', b'commit xyz\n', b'commit 85f12832ab12\nextra\n', b'anything else\n'):
            with self.subTest(data=data):
                self.assertNotEqual(self.audit({**GOOD, 'assets/build.txt': data}), [])

    def test_game_derived_names_and_unknown_entries_fail(self):
        for name, data in (('assets/companion/index.json', b'{}'), ('assets/Font.bfttf', b'x'), ('assets/x.bntx', b'x'),
                           ('assets/Layout.Product.sarc.zs', b'x'), ('assets/msg.msbt', b'x'), ('assets/runtime-tables.bin', b'x'),
                           ('assets/a.spv', b'x'), ('assets/seed.vkpack', b'x'), ('res/mipmap-xxxhdpi/ic_launcher.jpg', b'\xff\xd8'),
                           ('lib/arm64-v8a/main.patched.bin', ELF), ('assets/extra.txt', b'x'), ('lib/arm64-v8a/libother.so', ELF),
                           ('assets/licenses/k.txt', b'-----BEGIN PRIVATE KEY-----'), ('assets/fonts/Std.ttf', b'BFTTF..'),
                           ('assets/fonts/Cinzel-Regular.ttf', TTF)):
            with self.subTest(name=name):
                self.assertNotEqual(self.audit({**GOOD, name: data}), [])

    def test_game_file_magic_fails_even_under_an_allowed_name(self):
        self.assertTrue(any('starts like a game file' in p for p in self.audit({**GOOD, 'assets/pso_keys.bin': b'SARC' + bytes(8)})))

    def test_fonts_stored_shader_list_compressed_and_committed(self):
        with zipfile.ZipFile(self.dir / 'f.apk', 'w') as z:
            for name, data in GOOD.items():
                z.writestr(zipfile.ZipInfo(name), data, zipfile.ZIP_DEFLATED)
        problems = audit_apk.audit(self.dir / 'f.apk')
        self.assertTrue(any('stored uncompressed' in p for p in problems))
        committed = (ROOT / 'games/totk/assets/shader_list.tsv').read_bytes()
        self.assertEqual(self.audit({**GOOD, 'assets/shader_list.tsv': committed}), [])
        self.assertNotEqual(self.audit({**GOOD, 'assets/shader_list.tsv': committed + b'V\t' + b'0' * 64 + b'\t{}\n'}), [])
        stored = self.audit({**GOOD, 'assets/shader_list.tsv': committed}, stored=('assets/shader_list.tsv',))
        self.assertTrue(any('must be compressed' in p for p in stored))

    def duo_entries(self, game):
        """libduo_host.so and a few files of the game's own package (games.toml duo.package), as build_release packs them."""
        package, prefix = audit_apk.duo_package(game)
        names = {'botw160': ['manifest.json', 'ASSETS.provenance.json', '8E9978D50BDD20B4.json', 'assets/pov-cone.png'],
                 'v143': ['manifest.json', 'ASSETS.provenance.json', 'map_tiles.txt', 'totk_font.txt', 'assets/cmdbar-228x112-normal.png']}[game]
        return {'lib/arm64-v8a/libduo_host.so': ELF, **{prefix + n: (package / n).read_bytes() for n in names}}, prefix

    def test_eden_duo_companion_carries_the_games_own_package(self):
        """Each gpl_companion game (games.toml: both) may carry libduo_host.so and its own package (games/totk/duo/vendor,
        duo.package), byte for byte the vendored one; another game's package is not on its allowlist."""
        self.assertEqual(audit_apk.duo_package('v143')[1], 'assets/duo/TearsOfTheKingdom/dualscreen/')
        self.assertEqual(audit_apk.duo_package('botw')[1], 'assets/duo/BreathOfTheWild/dualscreen/')
        for game, other in (('botw160', 'v143'), ('v143', 'botw160')):
            with self.subTest(game=game):
                run = lambda entries: audit_apk.audit(apk(self.dir / f'{len(os.listdir(self.dir))}.apk', entries), game=game)
                duo, prefix = self.duo_entries(game)
                self.assertEqual(run({**GOOD, **duo}), [])
                self.assertEqual(run(GOOD), [])   # the companion is optional: the app starts without the library
                foreign = {k: v for k, v in self.duo_entries(other)[0].items() if not k.startswith('lib/')}
                for name, data in foreign.items():
                    self.assertTrue(any('not on the allowlist' in p for p in run({**GOOD, **duo, name: data})), name)
                png = next(n for n in duo if n.endswith('.png'))
                self.assertTrue(any('differs from the vendored companion package' in p
                                    for p in run({**GOOD, **duo, png: b'\x89PNG other'})))
                for name in ('load-plans/8E9978D50BDD20B4C8395A106C27FFDE00000000000000000000000000000000.json',
                             'assets/map.jpg', 'x.so', 'assets/notes.txt', 'map-tiles.txt', 'manifest.json.zs'):
                    self.assertNotEqual(run({**GOOD, **duo, prefix + name: b'{}'}), [], name)
        # A changed TotK file list is refused like a changed image.
        duo, prefix = self.duo_entries('v143')
        changed = audit_apk.audit(apk(self.dir / 'tiles.apk', {**GOOD, **duo, prefix + 'map_tiles.txt': b'UI/Map/other\n'}), game='v143')
        self.assertTrue(any('differs from the vendored companion package' in p for p in changed))

    def test_eden_duo_companion_is_refused_without_gpl_companion(self):
        """A game whose gpl_companion is false may carry none of it (games.toml; the variable stays)."""
        from unittest import mock
        real = audit_apk.games.values
        def values(game):
            v = dict(real(game))
            v['gpl_companion'] = False
            return v
        with mock.patch.object(audit_apk.games, 'values', values):
            for game in ('v143', 'botw160'):
                duo, _ = self.duo_entries(game)
                for name, data in duo.items():
                    with self.subTest(game=game, name=name):
                        found = audit_apk.audit(apk(self.dir / f'{len(os.listdir(self.dir))}.apk', {**GOOD, name: data}), game=game)
                        self.assertTrue(any('not on the allowlist' in p for p in found))

    def test_release_stages_the_games_own_package(self):
        """build_release.stage_assets packs games.toml duo.package minus load plans; what it stages passes the audit."""
        saved = release.GAME
        self.addCleanup(setattr, release, 'GAME', saved)
        for game, name in (('totk', 'TearsOfTheKingdom'), ('botw', 'BreathOfTheWild')):
            with self.subTest(game=game):
                release.GAME = release.games.values(game)
                stage = release.stage_assets(self.dir / f'stage-{game}', {'TotK-Native.txt': 'MIT\n'}, commit='85f12832ab12')
                packages = sorted(p.name for p in (stage / 'duo').iterdir())
                self.assertEqual(packages, [name])
                staged = sorted(str(p.relative_to(stage)) for p in (stage / 'duo').rglob('*') if p.is_file())
                self.assertIn(f'duo/{name}/dualscreen/manifest.json', staged)
                self.assertFalse(any('load-plans' in s for s in staged))
                entries = {f'assets/{s}': (stage / s).read_bytes() for s in staged}
                found = audit_apk.audit(apk(self.dir / f'staged-{game}.apk', {**GOOD, **entries}), game=game)
                self.assertEqual(found, [])

    def test_gpl_release_refuses_uncommitted_changes(self):
        """The APK is GPL-3.0-or-later as a whole: a release names a commit that exists as built (build.txt). A dirty tree
        stops a release before anything is built; a developer build only warns."""
        from unittest import mock
        saved = release.GAME
        self.addCleanup(setattr, release, 'GAME', saved)
        release.GAME = release.games.values('totk')
        self.assertTrue(release.GAME['gpl_companion'])

        class Built(Exception):
            pass

        def build_native(*a, **k):
            raise Built()
        args = dict(work=self.dir / 'work', ndk=self.dir, logs=self.dir, out=self.dir / 'x.apk', key=self.dir / 'k')
        with mock.patch.object(release, 'source_commit', lambda: '85f12832ab12-dirty'), \
                mock.patch.object(release, 'build_native', build_native), mock.patch.object(release, 'say', lambda *a: None):
            with self.assertRaisesRegex(release.BuildError, 'commit the changes first'):
                release.app(release=True, **args)
            with self.assertRaises(Built):
                release.app(release=False, make_key=True, **args)
        with mock.patch.object(release, 'source_commit', lambda: '85f12832ab12'), mock.patch.object(release, 'build_native', build_native):
            with self.assertRaises(Built):
                release.app(release=True, **args)

    def test_turnip_must_be_the_pinned_build(self):
        self.assertTrue(any('pinned Turnip' in p for p in self.audit({**GOOD, 'lib/arm64-v8a/libtotk_turnip.so': ELF})))

    def test_debug_info_and_host_paths_fail(self):
        debug = ELF + b'\x00.debug_info\x00.debug_line\x00'
        self.assertTrue(any('debug info' in p for p in self.audit({**GOOD, 'lib/arm64-v8a/libtotk.so': debug})))
        for path in (b'/home/builder/src/opus/celt/bands.c', b'/Users/builder/totk/runtime/loader.c'):
            with self.subTest(path=path):
                problems = self.audit({**GOOD, 'lib/arm64-v8a/libtotk_setup.so': ELF + path})
                self.assertTrue(any('host path' in p for p in problems), problems)
        self.assertTrue(any('host path' in p for p in self.audit({**GOOD, 'classes.dex': b'dex\n035/home/builder/x'})))
        # Mapped paths (-ffile-prefix-map) and stripped libraries pass.
        self.assertEqual(self.audit({**GOOD, 'lib/arm64-v8a/libtotk.so': ELF + b'opus/celt/bands.c\x00.gnu_debuglink\x00'}), [])

    def test_required_entries(self):
        entries = dict(GOOD); del entries['classes.dex']
        self.assertIn('classes.dex: missing', self.audit(entries))

    @unittest.skipUnless(os.environ.get('TOTK_RELEASE_APK'), 'set TOTK_RELEASE_APK to audit a real release APK')
    def test_release_apk(self):
        self.assertEqual(audit_apk.audit(os.environ['TOTK_RELEASE_APK']), [])


class ReleaseInputTests(unittest.TestCase):
    def test_turnip_pin(self):
        with tempfile.TemporaryDirectory() as tmp:
            fake = Path(tmp) / 'driver.so'
            fake.write_bytes(b'not the pinned driver')
            with self.assertRaisesRegex(release.BuildError, 'not the pinned Turnip'):
                release.check_turnip(fake)
            with self.assertRaisesRegex(release.BuildError, 'no Turnip'):
                release.check_turnip(Path(tmp) / 'missing.so')
        self.assertEqual(release.TURNIP_SHA256, '9b87afeb3d8d30c6a0ad5aab3e0703bc1500e0321e9f17a922106fa556389ec6')

    def test_release_version(self):
        # versionName is the app's semver (the game version is shown separately); versionCode grows with it.
        release.semver_code(release.APP_VERSION)   # the release version itself must be a valid MAJOR.MINOR.PATCH
        self.assertEqual(release.semver_code('1.0.0'), 10000)
        self.assertEqual([release.semver_code(v) for v in ('1.0.1', '1.1.0', '1.99.99', '2.0.0', '2.0.0-rc1')],
                         [10001, 10100, 19999, 20000, 20000])
        for bad in ('1.0', '1.100.0', '1.0.100', 'x.y.z', '1.0.0.0'):
            with self.subTest(version=bad), self.assertRaises(release.BuildError):
                release.semver_code(bad)

    def test_release_requires_inputs_and_never_makes_a_key(self):
        with self.assertRaises(SystemExit):
            release.parse_args(['--ndk', '/ndk'])
        source = (ROOT / 'games/totk/android/build_release.py').read_text()
        main = source[source.index('def main('):]
        self.assertNotIn('make_key=True', main)

    def test_committed_prebuilt_licences_and_notices_cover_shipped_components(self):
        licences = ROOT / 'games/totk/android/apk/licenses'
        self.assertIn('MIT', (licences / 'Mesa-Turnip.txt').read_text())
        self.assertIn('.NET Foundation', (licences / 'dotnet.txt').read_text())
        notices = (ROOT / 'THIRD_PARTY_NOTICES.md').read_text()
        for component in ('Turnip', 'Capstone', 'hactool', 'mbedTLS', 'zstd', 'Noto Sans', 'lsfg-vk-android', 'libopus',
                          'libvpx', '.NET', 'Ryujinx', 'libc++'):
            self.assertIn(component, notices, component)
        # The UI uses Noto Sans only: no other font ships, so none is listed.
        self.assertNotIn('Cinzel', notices)
        fonts = sorted(p.name for p in (ROOT / 'games/totk/android/app-assets/fonts').iterdir())
        self.assertEqual(fonts, ['NotoSans-OFL.txt', 'NotoSans-Regular.ttf', 'NotoSans-SemiBold.ttf'])


if __name__ == '__main__':
    unittest.main()
