"""The import of the player's own Lossless.dll for the LSFG frame-generation engine (LsfgImport.java) on a host JVM:
where it lands, what is refused, and that a refused or interrupted import leaves the earlier file alone."""
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
GAME = ROOT / 'games/totk'
SOURCES = [GAME / 'android/java/com/aure/totk/setup/LsfgImport.java', GAME / 'tests/lsfg/LsfgProbe.java']


def dll(size=300_000, pe=0x80):
    """A file with a DOS header pointing at a PE signature, padded to size: what the import takes for a Windows DLL."""
    data = bytearray(b'MZ' + b'\0' * 58 + struct.pack('<I', pe))
    data += b'\0' * (pe - len(data)) + b'PE\0\0' + b'\x64\x86'
    return bytes(data) + bytes(range(256)) * ((size - len(data)) // 256 + 1)


class RequestCodes(unittest.TestCase):
    def test_the_pickers_and_the_updater_do_not_share_a_request_code(self):
        """onActivityResult tells its callers apart by request code alone: a picker that shares the updater's code is
        answered as the updater's and its file is dropped."""
        setup = GAME / 'android/java/com/aure/totk/setup'
        text = (setup / 'SetupActivity.java').read_text(encoding='utf-8') + (setup / 'UpdateInstaller.java').read_text(encoding='utf-8')
        codes = re.findall(r'\b(REQ_[A-Z_]+|REQUEST_SOURCES) = (\d+)', text)
        names = [name for name, _ in codes]
        self.assertIn('REQ_LSFG', names)
        self.assertIn('REQUEST_SOURCES', names)
        self.assertEqual(len(names), len(set(names)))
        fixed = sorted(int(value) for name, value in codes if name != 'REQ_PICK')   # REQ_PICK is the base of a range below 900
        self.assertEqual(len(fixed), len(set(fixed)), codes)


class LsfgImportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        javac, cls.java = shutil.which('javac'), shutil.which('java')
        if not javac or not cls.java:
            raise unittest.SkipTest('javac/java not found')
        cls.temp = tempfile.TemporaryDirectory(prefix='totk-lsfg-')
        cls.classes = Path(cls.temp.name) / 'classes'
        subprocess.run([javac, '--release', '17', '-encoding', 'UTF-8', '-d', str(cls.classes), *map(str, SOURCES)],
                       check=True, capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        self.files = Path(tempfile.mkdtemp(prefix='files-', dir=self.temp.name))
        self.target = self.files / 'lsfg/Lossless.dll'

    def pick(self, data, *failure):
        source = self.files / 'picked.bin'
        source.write_bytes(data)
        run = subprocess.run([self.java, '-cp', str(self.classes), 'com.aure.totk.setup.LsfgProbe', str(source), str(self.files), *failure],
                             capture_output=True, text=True, timeout=60, check=True)
        return run.stdout.strip()

    def leftovers(self):
        return sorted(p.name for p in (self.files / 'lsfg').iterdir()) if (self.files / 'lsfg').is_dir() else []

    def test_a_dll_lands_where_the_engine_reads_it(self):
        data = dll()
        self.assertEqual(self.pick(data), f'imported {len(data)} {self.target}')   # <private files>/lsfg/Lossless.dll
        self.assertEqual(self.target.read_bytes(), data)
        self.assertEqual(self.leftovers(), ['Lossless.dll'])
        newer = dll(500_000)
        self.assertEqual(self.pick(newer, 'short'), f'imported {len(newer)} {self.target}')   # replaced; short reads
        self.assertEqual(self.target.read_bytes(), newer)

    def test_other_files_are_refused_and_the_earlier_dll_stays(self):
        good = dll()
        self.pick(good)
        elf = b'\x7fELF' + b'\0' * 400
        no_pe = bytearray(dll()); no_pe[0x80:0x84] = b'NE\0\0'
        far = dll(pe=0x80)[:60] + struct.pack('<I', 0x7fffff00) + dll()[64:]
        for data, why in ((elf, 'no MZ header'), (b'MZ', 'no MZ header'), (b'', 'empty file'), (bytes(no_pe), 'no PE signature'),
                          (far, 'no PE signature'), (b'MZ' + b'\0' * 62, 'no PE signature')):
            self.assertEqual(self.pick(data), f'invalid {why}')
            self.assertEqual(self.target.read_bytes(), good)
            self.assertEqual(self.leftovers(), ['Lossless.dll'])

    def test_an_interrupted_copy_leaves_nothing_behind(self):
        self.assertEqual(self.pick(dll(), 'interrupt'), 'failed interrupted source')
        self.assertEqual(self.leftovers(), [])
        good = dll()
        self.pick(good)
        self.assertEqual(self.pick(dll(400_000), 'interrupt'), 'failed interrupted source')
        self.assertEqual(self.target.read_bytes(), good)
        self.assertEqual(self.leftovers(), ['Lossless.dll'])

    def test_the_engine_reads_the_same_path(self):
        source = (GAME / 'android/gfx/vk_framegen_lsfg.c').read_text()
        self.assertIn('"%s/lsfg/Lossless.dll"', source)
        java = SOURCES[0].read_text()
        self.assertIn('FOLDER = "lsfg", NAME = "Lossless.dll"', java)


if __name__ == '__main__':
    unittest.main()
