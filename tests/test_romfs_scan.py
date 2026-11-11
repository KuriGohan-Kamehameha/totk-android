"""The walk of a mod folder that has no manifest (runtime/romfs_scan.h; runtime/romfs.c serves a mod manager's merged
romfs/ with it): which entries are visited under which RomFS paths, and which are left out. No game files or keys."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class RomfsScan(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('cc') or shutil.which('clang') or shutil.which('gcc')
        if not compiler:
            raise unittest.SkipTest('A C compiler is required for the native scan regression')
        cls.temp = tempfile.TemporaryDirectory(prefix='romfs-scan-')
        cls.exe = Path(cls.temp.name) / 'test-romfs-scan'
        build = subprocess.run([compiler, '-std=c11', '-D_DEFAULT_SOURCE', '-Wall', '-Wextra', '-Werror',
                                str(ROOT / 'tests/test_romfs_scan.c'), '-o', str(cls.exe)], capture_output=True, text=True)
        if build.returncode:
            raise AssertionError(build.stdout + build.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def scan(self, root, *args):
        """(visited {RomFS path: size}, (rc, files, junk, special, invalid, error))."""
        out = subprocess.run([str(self.exe), str(root), *args], capture_output=True, text=True, check=True).stdout
        files, counts = {}, None
        for line in out.splitlines():
            kind, *rest = line.split('\t')
            if kind == 'F':
                self.assertNotIn(rest[0], files)
                files[rest[0]] = int(rest[1])
            else:
                counts = (int(rest[0]), *(int(v) for v in rest[1:5]), rest[5])
        return files, counts

    def tree(self, files):
        root = Path(tempfile.mkdtemp(prefix='tree-', dir=self.temp.name))
        for name, size in files.items():
            (root / name).parent.mkdir(parents=True, exist_ok=True)
            (root / name).write_bytes(b'x' * size)
        return root

    def test_merged_output_is_visited_under_its_romfs_paths(self):
        wanted = {'Pack/Actor/Player.pack.zs': 5, 'RSDB/Tag.Product.143.rstbl.byml.zs': 7, 'AI/DriftRestore.module.ainb': 0,
                  'System/Resource/ResourceSizeTable.Product.143.Nin_NX_NVN.rsizetable.zs': 11, 'top.bin': 1,
                  'Sound/Resource/Stream/a b (1).bwav': 3}
        root = self.tree(wanted)
        files, counts = self.scan(root)
        self.assertEqual(files, wanted)
        self.assertEqual(counts, (0, len(wanted), 0, 0, 0, '-'))

    def test_desktop_leftovers_links_and_impossible_names_are_left_out(self):
        root = self.tree({'Model/a.bfres.mc': 4, 'Model/desktop.ini': 242, 'Model/Thumbs.db': 9, 'Model/.DS_Store': 6,
                          'Model/._a.bfres.mc': 2, '.git/config': 3, 'Banc/DESKTOP.INI': 1, 'Banc/b.bcett.byml.zs': 8,
                          'Banc/back\\slash.bin': 5})
        outside = self.tree({'secret.bin': 99})
        os.symlink(outside / 'secret.bin', root / 'Model/link.bin')
        os.symlink(outside, root / 'Linked')
        os.mkfifo(root / 'Banc/pipe')
        files, counts = self.scan(root)
        self.assertEqual(files, {'Model/a.bfres.mc': 4, 'Banc/b.bcett.byml.zs': 8})
        # junk: desktop.ini, Thumbs.db, .DS_Store, ._a, .git (the folder, not its file), DESKTOP.INI; special: 2 links + pipe
        self.assertEqual(counts, (0, 2, 6, 3, 1, '-'))

    def test_empty_missing_and_linked_roots(self):
        files, counts = self.scan(self.tree({}))
        self.assertEqual((files, counts), ({}, (0, 0, 0, 0, 0, '-')))
        files, counts = self.scan(Path(self.temp.name) / 'absent')
        self.assertEqual((files, counts[0]), ({}, -1))
        self.assertIn('No such file', counts[5])
        real = self.tree({'a.bin': 1})
        link = Path(self.temp.name) / 'linked-root'
        os.symlink(real, link)
        files, counts = self.scan(link)                    # the root is opened without following a link
        self.assertEqual((files, counts[0]), ({}, -1))
        files, counts = self.scan(real / 'a.bin')          # a file is not a root either
        self.assertEqual((files, counts[0]), ({}, -1))

    def nested(self, folders):
        """A tree of nested folders with f.bin in the last, made one level at a time (the whole path may pass PATH_MAX)."""
        root = Path(tempfile.mkdtemp(prefix='tree-', dir=self.temp.name))
        fd = os.open(root, os.O_RDONLY)
        try:
            for name in folders:
                os.mkdir(name, dir_fd=fd)
                below = os.open(name, os.O_RDONLY, dir_fd=fd)
                os.close(fd)
                fd = below
            os.close(os.open('f.bin', os.O_WRONLY | os.O_CREAT, 0o644, dir_fd=fd))
        finally:
            os.close(fd)
        return root

    def test_nesting_and_path_length_limits_stop_the_walk(self):
        deep = ['d'] * 32                                  # 32 folders below the root: the limit
        files, counts = self.scan(self.nested(deep))
        self.assertEqual((files, counts[0]), ({'/'.join(deep) + '/f.bin': 0}, 0))
        files, counts = self.scan(self.nested(deep + ['d']))
        self.assertEqual((files, counts[0]), ({}, -1))
        files, counts = self.scan(self.nested(['n' * 200] * 20 + ['m' * 80]))   # 4,100 bytes of relative path
        self.assertEqual((files, counts[0]), ({}, -1))
        self.assertIn('too long', counts[5])

    def test_visitor_refusal_stops_the_walk_with_its_code(self):
        files, counts = self.scan(self.tree({'a.bin': 1, 'b.bin': 1, 'c/d.bin': 1}), 'stop')
        self.assertEqual((len(files), counts[0]), (1, 7))


if __name__ == '__main__':
    unittest.main()
