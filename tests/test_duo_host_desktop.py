"""The Eden Duo companion host on the desktop (games/totk/duo/test/README.md): libduo_host built with each title module
and run against the vendored packages. Opt-in (a build takes minutes): TOTK_DUO_DESKTOP=1. Tears of the Kingdom also
needs the player's files: TOTK_DUO_TEST_MAIN (the 1.4.3 main as loaded) and TOTK_DUO_TEST_ROMFS (a folder of RomFS
files); nothing of them is kept, and the pages written stay in a temporary folder that is removed."""
import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DUO = ROOT / 'games/totk/duo'
PACKAGES = DUO / 'vendor/companions/packages'
WANTED = os.environ.get('TOTK_DUO_DESKTOP') == '1'


def build(title, work):
    compilers = [f'-DCMAKE_{lang}_COMPILER={os.environ[var]}' for lang, var in (('C', 'DUO_CC'), ('CXX', 'DUO_CXX'))
                 if os.environ.get(var)]
    generator = ['-G', 'Ninja'] if shutil.which('ninja') else []
    subprocess.run(['cmake', '-S', str(DUO), '-B', str(work), *generator, '-DDUO_HOST_TEST=ON', f'-DDUO_TITLE={title}',
                    '-DCMAKE_BUILD_TYPE=Release', *compilers], check=True, capture_output=True, text=True)
    subprocess.run(['cmake', '--build', str(work), '-j', str(min(4, os.cpu_count() or 2)), '--target', 'duo_host_test'],
                   check=True, capture_output=True, text=True)
    return work / 'duo_host_test'


@unittest.skipUnless(WANTED and shutil.which('cmake'), 'opt-in: TOTK_DUO_DESKTOP=1 (and cmake)')
class DuoHostDesktop(unittest.TestCase):
    def test_breath_of_the_wild_page_without_game_files(self):
        with tempfile.TemporaryDirectory(prefix='duo-botw-') as work:
            test = build('01007EF00011E000', Path(work) / 'build')
            run = subprocess.run([str(test), str(PACKAGES), '/nonexistent', str(Path(work) / 'page.png'), '', '240'],
                                 capture_output=True, text=True, timeout=600)
            self.assertEqual(run.returncode, 0, run.stderr[-3000:])
            self.assertIn("duo: package 'Breath of the Wild companion'", run.stderr)
            self.assertIn('test: PASS', run.stderr)

    @unittest.skipUnless(os.path.isfile(os.environ.get('TOTK_DUO_TEST_MAIN', '')) and
                         os.path.isdir(os.environ.get('TOTK_DUO_TEST_ROMFS', '')),
                         'needs TOTK_DUO_TEST_MAIN and TOTK_DUO_TEST_ROMFS (the player\'s 1.4.3 files)')
    def test_tears_of_the_kingdom_pages_and_state(self):
        with tempfile.TemporaryDirectory(prefix='duo-totk-') as work:
            test = build('0100F2C0115B6000', Path(work) / 'build')
            for scene, status, menu, life in (('ready', 5, 0, '47/80'), ('menu', 5, 1, '47/80'), ('title', 3, 0, '-1/-1')):
                with self.subTest(scene=scene):
                    run = subprocess.run([str(test), str(PACKAGES), os.environ['TOTK_DUO_TEST_ROMFS'],
                                          os.environ['TOTK_DUO_TEST_MAIN'], str(Path(work) / 'page'), scene],
                                         capture_output=True, text=True, timeout=900)
                    self.assertEqual(run.returncode, 0, run.stderr[-3000:])
                    self.assertIn("duo: package 'Tears of the Kingdom companion'", run.stderr)
                    # The module's own font, handed over by port_module.cpp (DUO_MODULE_FONT).
                    self.assertRegex(run.stderr, r"font 'file:totk_font\.txt' has \d+ glyph\(s\).*\(via module\)")
                    state = re.search(r'last status (\d+) in_game \d+ menu (\d+) tablet 1 life (\S+)', run.stderr)
                    self.assertIsNotNone(state, run.stderr[-2000:])
                    self.assertEqual((int(state.group(1)), int(state.group(2)), state.group(3)), (status, menu, life))


if __name__ == '__main__':
    unittest.main()
