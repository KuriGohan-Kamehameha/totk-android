"""The in-app mod import (ModImport.java) on a host JVM: which files of a picked folder or zip are taken and where they
land, what is refused, and that a refused or interrupted import leaves the earlier mods as they were. The layout it
writes is the one the runtime's scan reads (runtime/romfs.c mods_load, tests/test_romfs_scan.py)."""
import io
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
GAME = ROOT / 'games/totk'
sys.path.insert(0, str(ROOT / 'tools'))
import games  # noqa: E402
SOURCES = [GAME / 'android/java/com/aure/totk/setup/ModImport.java', GAME / 'tests/mods/ModProbe.java',
           GAME / 'android/java/com/aure/totk/setup/GameBuild.java']   # Tears of the Kingdom's (mods/v143)
MERGE = {'romfs/Pack/Actor/Player.pack.zs': b'p' * 5000, 'romfs/RSDB/Tag.Product.143.rstbl.byml.zs': b'tag',
         'romfs/UltraCam/maxlastbreath.ini': b'[Console]\n', 'romfs/Model/desktop.ini': b'junk', 'romfs/.DS_Store': b'junk',
         'romfs/manifest.tsv': b'# from elsewhere\n', 'romfs/Pack/manifest.tsv': b'a file of that name deeper down is a game file',
         'exefs/277178B7DBA1B6D4949A84778D4ABC58B31B34F5.ips': b'IPS32EEOF', 'exefs/main.npdm': b'META', 'exefs/subsdk1': b'NSO0',
         'exefs/subsdk2': b'NSO0', 'exefs/sub/nested.bin': b'x', 'exefs/odd,name\nx': b'x', 'cheats/0123456789ABCDEF.txt': b'cheat',
         'readme.txt': b'hello'}
TAKEN = {'romfs/Pack/Actor/Player.pack.zs', 'romfs/RSDB/Tag.Product.143.rstbl.byml.zs', 'romfs/UltraCam/maxlastbreath.ini',
         'romfs/Pack/manifest.tsv', 'exefs/277178B7DBA1B6D4949A84778D4ABC58B31B34F5.ips', 'exefs/main.npdm', 'exefs/subsdk1',
         'exefs/subsdk2'}
SUMMARY = 'files=4 bytes={} patches=1 modules=subsdk1,subsdk2 ignored=7'.format(sum(len(MERGE[n]) for n in TAKEN))


def archive(entries):
    out = io.BytesIO()
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
        for name, data in entries.items():
            z.writestr(name, data)
    return out.getvalue()


class ModImportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        javac, cls.java = shutil.which('javac'), shutil.which('java')
        if not javac or not cls.java:
            raise unittest.SkipTest('javac/java not found')
        cls.temp = tempfile.TemporaryDirectory(prefix='totk-mods-')
        cls.classes = Path(cls.temp.name) / 'classes'
        subprocess.run([javac, '--release', '17', '-encoding', 'UTF-8', '-d', str(cls.classes), *map(str, SOURCES)],
                       check=True, capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        self.case = Path(tempfile.mkdtemp(prefix='case-', dir=self.temp.name))
        self.files = self.case / 'files'
        self.files.mkdir()
        self.mods = self.files / 'mods/v143'

    def probe(self, *args):
        run = subprocess.run([self.java, '-cp', str(self.classes), 'com.aure.totk.setup.ModProbe', *map(str, args)],
                             capture_output=True, text=True, timeout=120, check=True)
        return run.stdout.strip()

    def tree(self, entries, name='Merged'):
        root = self.case / name
        for path, data in entries.items():
            (root / path).parent.mkdir(parents=True, exist_ok=True)
            (root / path).write_bytes(data)
        return root

    def zipped(self, entries, name='mod.zip'):
        path = self.case / name
        path.write_bytes(archive(entries))
        return path

    def installed(self):
        return {p.relative_to(self.mods).as_posix() for p in self.mods.rglob('*') if p.is_file()} if self.mods.is_dir() else set()

    def leftovers(self):
        return sorted(p.name for p in (self.files / 'mods').iterdir()) if (self.files / 'mods').is_dir() else []

    def test_a_merge_folder_is_imported_as_the_runtime_reads_it(self):
        self.assertEqual(self.probe('tree', self.tree(MERGE), self.files), f'imported {SUMMARY} source=Merged')
        self.assertEqual(self.installed(), TAKEN | {'import.txt'})
        self.assertEqual((self.mods / 'romfs/Pack/Actor/Player.pack.zs').read_bytes(), MERGE['romfs/Pack/Actor/Player.pack.zs'])
        self.assertEqual(self.probe('read', self.files), f'{SUMMARY} source=Merged')
        self.assertEqual(self.leftovers(), ['v143'])
        self.assertFalse((self.mods / 'manifest.tsv').exists() or (self.mods / 'romfs/manifest.tsv').exists())   # so the runtime scans

    def test_the_merge_may_sit_below_other_folders_and_in_a_zip(self):
        for prefix in ('', 'Merged/', 'Downloads/My Mod v2/', 'RomFS-pack/'):
            with self.subTest(prefix=prefix):
                entries = {prefix + name: data for name, data in MERGE.items()}
                self.assertEqual(self.probe('zip', self.zipped(entries), self.files), f'imported {SUMMARY} source=mod.zip')
                self.assertEqual(self.installed(), TAKEN | {'import.txt'})
        backslashes = {name.replace('/', '\\'): data for name, data in MERGE.items()}      # some Windows archivers
        self.assertEqual(self.probe('zip', self.zipped(backslashes), self.files), f'imported {SUMMARY} source=mod.zip')
        upper = {name.replace('romfs/', 'RomFS/', 1).replace('exefs/', 'ExeFS/', 1): data for name, data in MERGE.items()}
        self.assertEqual(self.probe('zip', self.zipped(upper), self.files), f'imported {SUMMARY} source=mod.zip')
        self.assertEqual(self.installed(), TAKEN | {'import.txt'})                           # stored as romfs/ and exefs/

    def test_a_new_import_replaces_the_earlier_one_whole(self):
        self.probe('tree', self.tree(MERGE), self.files)
        small = {'romfs/Event/EventFlow/A.bfevfl.zs': b'e' * 10}
        self.assertEqual(self.probe('zip', self.zipped(small), self.files), 'imported files=1 bytes=10 patches=0 modules= ignored=0 source=mod.zip')
        self.assertEqual(self.installed(), {'romfs/Event/EventFlow/A.bfevfl.zs', 'import.txt'})
        self.assertEqual(self.leftovers(), ['v143'])
        self.assertEqual(self.probe('remove', self.files), 'true none')
        self.assertEqual(self.leftovers(), [])

    def test_what_is_refused_leaves_the_earlier_mods(self):
        self.probe('tree', self.tree(MERGE), self.files)
        before = self.installed()
        cases = (({'Pack/Actor/Player.pack.zs': b'x', 'readme.txt': b'x'}, 'no_romfs'),                 # the romfs folder itself was zipped
                 ({'ModA/romfs/a.bin': b'a', 'ModB/romfs/b.bin': b'b'}, 'several'),                      # two mods: merge them first
                 ({'romfs/a.bin': b'a', 'Other/exefs/x.ips': b'IPS32EEOF'}, 'several'),
                 ({'romfs/../../escape.bin': b'x'}, 'bad_path'), ({'romfs/a/./b.bin': b'x'}, 'bad_path'),
                 ({'romfs//double.bin': b'x'}, 'bad_path'), ({'romfs/' + '/'.join(['d'] * 40) + '/deep.bin': b'x'}, 'bad_path'),
                 ({'romfs/a.bin': b'1', 'romfs\\a.bin': b'2'}, 'duplicate'),
                 ({'exefs/subsdk1': b'NSO0', 'exefs/main.npdm': b'META', 'readme.txt': b'x'}, 'only_modules'))            # nothing this port uses
        for entries, code in cases:
            with self.subTest(code=code, first=next(iter(entries))):
                self.assertEqual(self.probe('zip', self.zipped(entries), self.files), f'invalid {code}')
                self.assertEqual(self.installed(), before)
                self.assertEqual(self.leftovers(), ['v143'])
                self.assertFalse((self.case / 'escape.bin').exists() or (self.files / 'escape.bin').exists())
        not_zip = self.case / 'not.zip'
        not_zip.write_bytes(b'this is not an archive')
        self.assertEqual(self.probe('zip', not_zip, self.files), 'invalid bad_zip')
        self.assertEqual(self.probe('tree', self.tree({'Pack/a.bin': b'x'}, 'romfs-only'), self.files), 'invalid no_romfs')
        self.assertEqual(self.installed(), before)

    def test_leftovers_beside_the_merge_do_not_count_as_a_second_mod(self):
        entries = {'__MACOSX/Merged/romfs/._a.bin': b'fork', '__MACOSX/Other/romfs/Pack/b.bin': b'fork', '.Trash/Old/romfs/c.bin': b'old',
                   'Notes/exefs/sub/x.bin': b'nested exefs', 'Other/romfs/desktop.ini': b'junk', 'Merged/romfs/a.bin': b'a',
                   'Merged/exefs/277178B7DBA1B6D4949A84778D4ABC58B31B34F5.ips': b'IPS32EEOF'}
        self.assertEqual(self.probe('zip', self.zipped(entries), self.files), 'imported files=1 bytes=10 patches=1 modules= ignored=5 source=mod.zip')
        self.assertEqual(self.installed(), {'romfs/a.bin', 'exefs/277178B7DBA1B6D4949A84778D4ABC58B31B34F5.ips', 'import.txt'})
        spelt = {'romfs/Manifest.TSV': b'another spelling of the manifest', 'romfs/a.bin': b'a'}      # left out however it is spelt
        self.assertEqual(self.probe('zip', self.zipped(spelt), self.files), 'imported files=1 bytes=1 patches=0 modules= ignored=1 source=mod.zip')
        self.assertEqual(self.probe('zip', self.zipped({'exefs/277178B7DBA1B6D4949A84778D4ABC58B31B34F5.ips': b'IPS32EEOF'}), self.files),
                         'imported files=0 bytes=9 patches=1 modules= ignored=0 source=mod.zip')                      # patches alone are a mod

    def test_an_interrupted_import_leaves_the_earlier_mods(self):
        big = dict(MERGE, **{f'romfs/Tex/{i}.bin': os.urandom(3000) for i in range(8)})
        self.assertEqual(self.probe('zip', self.zipped(big), self.files, 'interrupt'), 'failed interrupted source')
        self.assertEqual((self.installed(), self.leftovers()), (set(), []))
        self.probe('tree', self.tree(MERGE), self.files)
        before = self.installed()
        self.assertEqual(self.probe('zip', self.zipped(big), self.files, 'interrupt'), 'failed interrupted source')
        self.assertEqual(self.probe('tree', self.tree(big, 'Bigger'), self.files, 'interrupt'), 'failed interrupted before commit')
        self.assertEqual((self.installed(), self.leftovers()), (before, ['v143']))
        # a swap cut short between its two renames: the menu (recover) and the next import put the earlier mods back
        (self.files / 'mods/v143').rename(self.files / 'mods/v143.old')
        self.assertEqual(self.probe('read', self.files), 'none')
        self.assertEqual(self.probe('recover', self.files), f'{SUMMARY} source=Merged')
        self.assertEqual((self.installed(), self.leftovers()), (before, ['v143']))
        (self.files / 'mods/v143').rename(self.files / 'mods/v143.old')
        self.assertEqual(self.probe('zip', self.zipped({'x/y.bin': b'x'}), self.files), 'invalid no_romfs')
        self.assertEqual((self.installed(), self.leftovers()), (before, ['v143']))
        self.assertEqual(self.probe('recover', self.files), f'{SUMMARY} source=Merged')   # nothing to do: unchanged

    def test_links_in_a_folder_are_not_followed(self):
        root = self.tree({'romfs/a.bin': b'a'})
        outside = self.tree({'secret.bin': b's' * 50}, 'outside')
        os.symlink(outside, root / 'romfs/linked')
        os.symlink(outside / 'secret.bin', root / 'romfs/link.bin')
        self.assertEqual(self.probe('tree', root, self.files), 'imported files=1 bytes=1 patches=0 modules= ignored=0 source=Merged')
        self.assertEqual(self.installed(), {'romfs/a.bin', 'import.txt'})

    def test_the_runtime_looks_where_the_import_writes(self):
        frontend = (GAME / 'runtime/frontend_android.c').read_text()
        self.assertIn('a->internalDataPath', frontend[frontend.index('Player mods (runtime/romfs.c'):][:2500])
        self.assertIn('"mods/" + VERSION', SOURCES[0].read_text())
        self.assertIn('VERSION = GameBuild.VERSION', SOURCES[0].read_text())   # mods/<the game's tag>: v143, botw160


class ActiveModsTests(unittest.TestCase):
    """What the menu reports (ModImport.active) is what the runtime will load, for both games: TOTK_ROMFS_MODS in the
    game folder's totk.env, else the first mods/<tag> of the game folder, the private import and the shared folder
    (runtime/frontend_android.c); in it a manifest.tsv, romfs/manifest.tsv or the romfs/ tree (runtime/romfs.c mods_load)."""
    @classmethod
    def setUpClass(cls):
        javac, cls.java = shutil.which('javac'), shutil.which('java')
        if not javac or not cls.java:
            raise unittest.SkipTest('javac/java not found')
        cls.temp = tempfile.TemporaryDirectory(prefix='totk-active-mods-')
        cls.classes = {}
        for g in ('totk', 'botw'):
            gen = Path(cls.temp.name) / f'gen-{g}/GameBuild.java'
            gen.parent.mkdir(parents=True)
            gen.write_text(games.java(g))
            cls.classes[g] = Path(cls.temp.name) / f'classes-{g}'
            subprocess.run([javac, '--release', '17', '-encoding', 'UTF-8', '-d', str(cls.classes[g]), str(SOURCES[0]), str(SOURCES[1]), str(gen)],
                           check=True, capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        self.case = Path(tempfile.mkdtemp(prefix='case-', dir=self.temp.name))
        self.game, self.private, self.shared = self.case / 'game', self.case / 'files', self.case / 'shared'
        for d in (self.game, self.private, self.shared):
            d.mkdir()

    def active(self, g):
        run = subprocess.run([self.java, '-cp', str(self.classes[g]), 'com.aure.totk.setup.ModProbe', 'active', self.game, self.private, self.shared],
                             capture_output=True, text=True, timeout=60, check=True)
        return dict(item.split('=', 1) for item in run.stdout.split() if '=' in item)   # 'none': no summary

    def put(self, base, files):
        for path, data in files.items():
            (base / path).parent.mkdir(parents=True, exist_ok=True)
            (base / path).write_bytes(data)
        return base

    def manifest(self, base, files):
        """A manifest.tsv like the owner's Second Wind merge: name, size, sha256, original sha256 or '-'."""
        self.put(base, files)
        (base / 'manifest.tsv').write_text('# merged\n' + ''.join(f'{n}\t{len(d)}\t{"a" * 64}\t-\n' for n, d in files.items()))
        return base

    def test_totk_env_names_the_folder_as_on_the_owners_thor(self):
        for g, tag in (('botw', 'botw160'), ('totk', 'v143')):
            with self.subTest(game=g):
                romfs = self.manifest(self.game / f'mods/{tag}/romfs', {'Actor/ActorInfo.product.sbyml': b'a' * 1000, 'Pack/Bootup.pack': b'b' * 24})
                self.put(self.game / f'mods/{tag}', {'exefs/8E9978D50BDD20B4C8395A106C27FFDE.ips': b'IPS32EEOF', 'exefs/subsdk9': b'NSO0'})
                (self.game / 'totk.env').write_text(f'TOTK_ROMFS_MODS={romfs}\nTOTK_MOVIE_PACE=0\n')
                a = self.active(g)
                self.assertEqual((a['from'], a['folder'], a['layout'], a['files'], a['bytes'], a['patches'], a['modules'], a['any']),
                                 ('env', str(romfs), 'manifest', '2', '1024', '1', 'subsdk9', 'true'))   # exefs beside a folder named romfs
                shutil.rmtree(self.game / 'mods')

    def test_search_order_without_totk_env(self):
        g, tag = 'botw', games.values('botw')['nso_version']
        self.assertEqual(self.active(g)['from'], '')
        self.assertEqual(self.active(g)['any'], 'false')
        self.put(self.shared / f'mods/{tag}', {'romfs/Pack/a.pack': b'x' * 7})
        self.assertEqual((self.active(g)['from'], self.active(g)['files']), ('shared', '1'))
        imported = self.private / f'mods/{tag}'
        self.put(imported, {'romfs/Pack/b.pack': b'y' * 3, 'romfs/UI/c.bfres': b'z' * 5})
        (imported / 'import.txt').write_text('files=2\nbytes=8\npatches=0\nmodules=\nignored=0\nsource=Merged.zip\n')
        a = self.active(g)
        self.assertEqual((a['from'], a['files'], a['bytes'], a['importUnused']), ('private', '2', '8', 'false'))
        # by hand in the game folder: scanned (desktop leftovers and links left out, as runtime/romfs_scan.h), first
        self.put(self.game / f'mods/{tag}', {'romfs/Model/x.bfres': b'm' * 11, 'romfs/Model/.DS_Store': b'junk', 'romfs/desktop.ini': b'junk'})
        os.symlink(self.case, self.game / f'mods/{tag}/romfs/loop')
        a = self.active(g)
        self.assertEqual((a['from'], a['layout'], a['files'], a['bytes'], a['importUnused']), ('game', 'merge', '1', '11', 'true'))
        # romfs/manifest.tsv decides over the files beside it
        self.manifest(self.game / f'mods/{tag}/romfs', {'Model/x.bfres': b'm' * 11, 'Model/y.bfres': b'n' * 2})
        self.assertEqual((self.active(g)['layout'], self.active(g)['files']), ('manifest', '2'))
        # totk.env wins over all of them; 0 turns the mods off
        (self.game / 'totk.env').write_text('# mine\nexport TOTK_ROMFS_MODS="0"\n')
        a = self.active(g)
        self.assertEqual((a['from'], a['off'], a['any'], a['importUnused']), ('env', 'true', 'false', 'true'))
        (self.game / 'totk.env').write_text(f'TOTK_ROMFS_MODS={imported}\n')
        a = self.active(g)
        self.assertEqual((a['from'], a['files'], a['importUnused']), ('env', '2', 'false'))

    def test_each_game_looks_for_its_own_tag(self):
        self.put(self.game / 'mods/v143', {'romfs/a.bin': b'a'})
        self.assertEqual(self.active('totk')['from'], 'game')
        self.assertEqual(self.active('botw')['from'], '')
        self.put(self.game / 'mods/botw160', {'romfs/a.bin': b'a', 'romfs/b.bin': b'b'})
        self.assertEqual((self.active('botw')['from'], self.active('botw')['files']), ('game', '2'))

    def test_a_folder_the_runtime_refuses_is_reported(self):
        (self.game / 'mods/v143').mkdir(parents=True)
        (self.game / 'mods/v143/romfs').write_bytes(b'not a folder')
        a = self.active('totk')
        self.assertEqual((a['from'], a['problem'], a['any']), ('game', 'romfs_not_folder', 'false'))
        (self.game / 'mods/v143/romfs').unlink()
        a = self.active('totk')
        self.assertEqual((a['from'], a['problem'], a['any'], a['layout']), ('game', '', 'false', ''))   # an empty folder: no mods

    def test_the_search_matches_the_runtime(self):
        frontend = (GAME / 'runtime/frontend_android.c').read_text()
        self.assertIn('const char *const roots[] = {g_data, a->internalDataPath, a->externalDataPath};', frontend)
        self.assertIn('snprintf(p, sizeof p, "%s/totk.env", g_data); env_file(p);', frontend)


if __name__ == '__main__':
    unittest.main()
