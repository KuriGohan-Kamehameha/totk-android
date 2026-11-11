"""A mod's IPS patches of main (runtime/mod_ips.h): the file format, the build-id match, which records are refused and
why, and the loader as the game patches call it. Runs against a made-up module (tests/test_mod_ips.c): no game files."""
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BUILD_ID = '277178B7DBA1B6D4949A84778D4ABC58B31B34F5'   # the made-up module's note (test_mod_ips.c)
MOV = struct.pack('<I', 0x52804008)                     # mov w8, #0x200
X18 = struct.pack('<I', 0xd2800032)                     # mov x18, #1


def ips32(*records):
    """IPS32 bytes from (main address, bytes) pairs; offsets count the 0x100-byte NSO header."""
    out = b'IPS32'
    for address, data in records:
        out += struct.pack('>IH', address + 0x100, len(data)) + data
    return out + b'EEOF'


class ModIps(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('cc') or shutil.which('clang') or shutil.which('gcc')
        if not compiler:
            raise unittest.SkipTest('A C compiler is required for the native IPS regression')
        cls.temp = tempfile.TemporaryDirectory(prefix='mod-ips-')
        cls.exe = Path(cls.temp.name) / 'test-mod-ips'
        build = subprocess.run([compiler, '-std=c11', '-D_DEFAULT_SOURCE', '-O1', '-Wall', '-Wextra', '-Werror',
                                str(ROOT / 'tests/test_mod_ips.c'), '-o', str(cls.exe)], capture_output=True, text=True)
        if build.returncode:
            raise AssertionError(build.stdout + build.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def mods(self, files, layout='inside'):
        """A mods folder with exefs/ holding `files`; returns the TOTK_ROMFS_MODS value for the layout."""
        root = Path(tempfile.mkdtemp(prefix='mods-', dir=self.temp.name))
        (root / 'exefs').mkdir()
        (root / 'romfs').mkdir()
        for name, data in files.items():
            (root / 'exefs' / name).write_bytes(data)
        return root if layout == 'inside' else root / 'romfs'

    def load(self, mods, **env):
        run = subprocess.run([str(self.exe), 'load'], capture_output=True, text=True,
                             env={**os.environ, 'TOTK_ROMFS_MODS': str(mods), **env})
        return run.returncode, run.stdout, run.stderr

    def test_parser_names_instructions_and_record_checks(self):
        run = subprocess.run([str(self.exe)], capture_output=True, text=True)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertRegex(run.stdout, r'^409 mod IPS cases, 0 failed$')

    def test_a_file_for_this_main_is_applied_whole(self):
        patch = ips32((0x100, MOV), (0x2100, b'\x7f'), (0x3200, b'\x55\x66'))
        for layout in ('inside', 'beside'):     # <mods>/exefs, and <merge>/exefs when the mods folder is <merge>/romfs
            code, out, err = self.load(self.mods({BUILD_ID + '.ips': patch}, layout))
            self.assertEqual((code, out), (0, 'LOADED\t52804008\t7f\t55\n'), err)
            self.assertIn('applied, 3 records, 7 bytes in main', err)
            self.assertIn('main+0x100, 4 bytes of code', err)
            self.assertIn('main+0x2100, 1 bytes of read-only data', err)
            self.assertIn('main+0x3200, 2 bytes of data', err)

    def test_a_refused_record_stops_the_start_and_nothing_is_written(self):
        for patch, reason, where in ((ips32((0x3200, b'\x55'), (0x100, X18)), 'names x18', 'main+0x100'),
                                     (ips32((0x3200, b'\x55'), (0x40, MOV)), 'this port rewrote', 'main+0x40'),
                                     (ips32((0x3200, b'\x55'), (0x3104, b'\x01')), 'relocated', 'main+0x3104'),
                                     (ips32((0x3200, b'\x55'))[:-1], 'ends inside a record', None)):
            code, out, err = self.load(self.mods({BUILD_ID + '.ips': patch}))
            self.assertEqual(code, 3, err)
            self.assertIn('FAIL\tmod IPS patch refused', out)
            self.assertNotIn('LOADED', out)
            self.assertIn(reason, err)
            if where:
                self.assertIn(f'refused at {where}:', err)
            self.assertNotIn(' applied, ', err)

    def test_files_for_another_module_are_left_alone(self):
        other = 'AA' + BUILD_ID[2:]
        files = {other + '.ips': ips32((0x100, X18)), 'notes.ips': b'junk', 'subsdk1': b'NSO0', 'main.npdm': b'META',
                 BUILD_ID[:32] + '.ips': ips32((0x100, MOV)),          # a prefix of the id is another id
                 BUILD_ID + '00AA.ips': ips32((0x100, MOV))}           # and so is the id with more behind it
        code, out, err = self.load(self.mods(files))
        self.assertEqual((code, out), (0, 'LOADED\td503201f\t00\t00\n'), err)
        self.assertEqual(err.count("is not named after this main's build id"), 4)
        self.assertNotIn(' applied, ', err)     # the line a file that was applied gets

    def test_two_files_for_main_are_refused(self):
        patch = ips32((0x100, MOV))
        code, out, err = self.load(self.mods({BUILD_ID + '.ips': patch, BUILD_ID.lower() + '0' * 24 + '.ips': patch}))
        self.assertEqual(code, 3, err)
        self.assertIn('FAIL\tmod IPS: two patch files for main', out)
        self.assertIn("are both named after main's build id", err)
        self.assertNotIn('LOADED', out)

    def test_switch_off_no_mods_folder_and_no_exefs(self):
        patch = ips32((0x100, MOV))
        code, out, err = self.load(self.mods({BUILD_ID + '.ips': patch}), TOTK_MOD_IPS='0')
        self.assertEqual((code, out), (0, 'LOADED\td503201f\t00\t00\n'))
        self.assertIn('not read (TOTK_MOD_IPS=0)', err)
        for mods in ('0', '', str(Path(self.temp.name) / 'absent'), self.temp.name):
            code, out, err = self.load(mods)
            self.assertEqual((code, out, err), (0, 'LOADED\td503201f\t00\t00\n', ''))

    def test_exefs_folder_of_a_mods_folder(self):
        root = self.mods({})
        for mods, want in ((root, root / 'exefs'), (root / 'romfs', root / 'exefs'), (root / 'exefs', None), (Path(self.temp.name), None)):
            out = subprocess.run([str(self.exe), 'exefs', str(mods)], capture_output=True, text=True, check=True).stdout
            self.assertEqual(out, f'EXEFS\t{want if want else "-"}\n')


if __name__ == '__main__':
    unittest.main()
