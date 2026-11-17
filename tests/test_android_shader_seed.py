"""The menu's shader count (setup/ShaderSeed.java, InstallState.shaderVariants) on a host JVM: what the runtime's seed
reader (gfx/vk_seed_io.c inventory_build) will find in <game>/shaders/<version>, i.e. the *.spv entries of seed.vkpack
(made here by the real packer, scripts/pack_shader_seed.py) plus the loose modules beside it, a loose file of a packed
name counted once, and only the loose files when the runtime would refuse the pack. For both games: the folder is
shaders/<nso_version> (v143, botw160); BotW's pack is prepared by hand (scripts/prepare_botw_seed.py) and its
install.json count is not the seed's."""
from pathlib import Path
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
JAVA = ROOT / 'games/totk/android/java/com/aure/totk/setup'
sys.path.insert(0, str(ROOT / 'games/totk/scripts'))
import pack_shader_seed  # noqa: E402

PROBE = r'''
package com.aure.totk.setup;
public class SeedProbe {
    public static void main(String[] args) {
        for (String dir : args) {
            ShaderSeed.Count c = ShaderSeed.count(new java.io.File(dir));
            System.out.println(c.variants + " " + c.packed + " " + c.loose + " " + c.pack);
        }
    }
}
'''


def module(i, variant='5bf108d693d4'):
    return f'{i:064x}-{variant}'


class ShaderSeedTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        javac, cls.java = shutil.which('javac'), shutil.which('java')
        if not javac or not cls.java:
            raise unittest.SkipTest('javac/java not found')
        cls.temp = tempfile.TemporaryDirectory(prefix='totk-seed-')
        cls.addClassCleanup(cls.temp.cleanup)
        work = Path(cls.temp.name)
        (work / 'SeedProbe.java').write_text(PROBE)
        cls.classes = work / 'classes'
        subprocess.run([javac, '--release', '17', '-encoding', 'UTF-8', '-d', str(cls.classes), str(JAVA / 'ShaderSeed.java'),
                        str(work / 'SeedProbe.java')], check=True, capture_output=True, text=True)

    def setUp(self):
        self.dir = Path(tempfile.mkdtemp(prefix='case-', dir=self.temp.name))

    def count(self, *dirs):
        run = subprocess.run([self.java, '-cp', str(self.classes), 'com.aure.totk.setup.SeedProbe', *map(str, dirs)],
                             capture_output=True, text=True, timeout=60)
        self.assertEqual(run.returncode, 0, run.stderr)
        return run.stdout.split('\n')[:len(dirs)]

    def seed(self, tag, packed, loose=(), extra=()):
        """shaders/<tag>: a pack of `packed` modules (each with .vk and a .vv per key, as prepare_*_seed.py writes),
        then the loose modules and other loose files."""
        folder = self.dir / 'shaders' / tag
        source = self.dir / f'loose-{tag}'
        source.mkdir(parents=True)
        for i in packed:
            (source / f'{module(i)}.spv').write_bytes(b'\x03\x02\x23\x07' + bytes([i % 256]) * 37)
            (source / f'{module(i)}.vk').write_bytes(b'vk')
            (source / f'{i:064x}.vv').write_text('variant\tdeps\n')
        folder.mkdir(parents=True)
        if packed:
            pack_shader_seed.pack_seed(source, folder / 'seed.vkpack')
        for i in loose:
            (folder / f'{module(i, "c0caede3b8ab")}.spv').write_bytes(b'\x03\x02\x23\x07')
            (folder / f'{module(i, "c0caede3b8ab")}.vk').write_bytes(b'vk')
        for name in extra:
            (folder / name).write_bytes(b'x')
        return folder

    def test_pack_and_loose_modules_for_both_games(self):
        # TotK's folder (v143) and BotW's (botw160): the count is the pack's modules plus the loose ones beside it
        totk = self.seed('v143', packed=range(40), loose=range(100, 103))
        botw = self.seed('botw160', packed=range(17, 317), loose=range(1000, 1107), extra=('seed-id.txt', 'notes.spv.bak'))
        self.assertEqual(self.count(totk, botw), ['43 40 3 true', '407 300 107 true'])

    def test_a_loose_file_of_a_packed_name_counts_once(self):
        folder = self.seed('botw160', packed=range(10))
        (folder / f'{module(3)}.spv').write_bytes(b'newer translation')   # the runtime's loose overlay wins; one variant
        (folder / f'{module(3)}.vk').write_bytes(b'vk')
        self.assertEqual(self.count(folder), ['10 10 0 true'])

    def test_a_pack_the_runtime_refuses_leaves_the_loose_files(self):
        folder = self.seed('v143', packed=range(5), loose=range(50, 52))
        pack = folder / 'seed.vkpack'
        data = bytearray(pack.read_bytes())
        intact = self.count(folder)
        data[24:32] = struct.pack('<Q', len(data) + 1)            # total size disagrees with the file (vk_seed_pack_open)
        pack.write_bytes(bytes(data))
        os.utime(pack, (1_700_000_000, 1_700_000_000))
        truncated = self.dir / 'trunc/v143'
        truncated.mkdir(parents=True)
        (truncated / 'seed.vkpack').write_bytes(b'TOTKVKP1' + b'\0' * 10)
        empty = self.dir / 'empty/v143'
        empty.mkdir(parents=True)
        self.assertEqual(intact, ['7 5 2 true'])
        self.assertEqual(self.count(folder, truncated, empty, self.dir / 'missing'), ['2 0 2 false', '0 0 0 false', '0 0 0 false', '0 0 0 false'])

    def test_unsorted_names_are_refused_like_the_runtime(self):
        folder = self.seed('botw160', packed=range(3))
        pack = folder / 'seed.vkpack'
        data = bytearray(pack.read_bytes())
        first, second = data[32:32 + 84], data[32 + 104:32 + 104 + 84]
        data[32:32 + 84], data[32 + 104:32 + 104 + 84] = second, first
        pack.write_bytes(bytes(data))
        self.assertEqual(self.count(folder), ['0 0 0 false'])

    def test_the_menu_counts_from_the_folder(self):
        state = (JAVA / 'InstallState.java').read_text()
        self.assertIn('ShaderSeed.variants(new File(game, "shaders/" + GameBuild.VERSION))', state)


if __name__ == '__main__':
    unittest.main()
