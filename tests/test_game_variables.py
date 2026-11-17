"""The per-game variables (games/games.toml) are the one list of what differs between the two apps.

Everything else is common code. These checks keep it that way: code that branches on the game must be declared in the
list ([branches]: file, number of branch points and the variable it implements), so a new difference cannot be added
without adding it to the list; the generated GameBuild.java, the Makefile's defines, the C fallbacks and MAINTENANCE.md
must agree with the list; and no whole-file per-game override (java-<game>/) may come back."""
import re
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import games  # noqa: E402

GAME_BUILD = ROOT / 'games/totk/android/java/com/aure/totk/setup/GameBuild.java'


class GameVariables(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.data = games.load()

    def test_every_variable_has_a_meaning_and_both_values(self):
        self.assertEqual(self.data['games'], ['totk', 'botw'])
        for name, spec in self.data['var'].items():
            self.assertTrue(spec['meaning'].strip(), name)
            for g in self.data['games']:
                self.assertIn(g, spec, f'{name}: no value for {g}')
        for required in ('nso_version', 'game_id', 'title', 'version_label', 'title_id', 'romfs_size', 'app_name', 'accent',
                         'launcher_art', 'launcher_background', 'companion', 'gpl_companion', 'duo', 'bifrost_looks', 'env_defaults',
                         'c_defines', 'game_patches', 'shader_seed', 'docs', 'mod_manager'):
            self.assertIn(required, self.data['var'])

    def test_version_tags_and_ids_resolve(self):
        self.assertEqual(games.game_id(self.data, 'v143'), 'totk')
        self.assertEqual(games.game_id(self.data, 'botw160'), 'botw')
        with self.assertRaises(games.GameError):
            games.game_id(self.data, 'v0x')

    def test_flavours_name_keys_outside_the_repository(self):
        for g, flavours in self.data['flavour'].items():
            for fl in flavours:
                spec = games.flavour(g, fl, self.data)
                self.assertNotIn('/', str(self.data['flavour'][g][fl]['key']), 'a key is a file name in TOTK_KEYS_DIR')
                self.assertFalse(str(spec['key']).startswith(str(ROOT)), f'{g}.{fl}: the key must not be in the repository')
                if not spec['debuggable']:
                    self.assertTrue(spec.get('version_code') or re.match(r'\d+\.\d+\.\d+', spec['version_name']))
        tracked = subprocess.run(['git', '-C', str(ROOT), 'ls-files'], capture_output=True, text=True, check=True).stdout.split('\n')
        self.assertFalse([f for f in tracked if f.endswith(('.p12', '.pfx', '.pass', '.jks', '.keystore'))])
        # Identities do not collide across games: each game's apps install side by side.
        packages = {g: {s['package'] for s in fl.values()} for g, fl in self.data['flavour'].items()}
        self.assertFalse(packages['totk'] & packages['botw'])

    def test_resources_named_by_variables_exist(self):
        for g in self.data['games']:
            v = games.values(g, self.data)
            for path in [v['shader_seed']['list'], v['shader_seed']['script'], v['launcher_art']['art'], v['launcher_art']['renders'],
                         v['docs']] + ['games/totk/' + p for p in v['game_patches']]:
                self.assertTrue((ROOT / path).exists(), f'{g}: {path}')
            self.assertEqual(len(v['launcher_background']), 3)

    def test_duo_names_a_vendored_module_and_package_the_build_knows(self):
        # duo.module / duo.package of every gpl_companion game: vendored files, a title duo/CMakeLists.txt builds, and
        # what android/Makefile and build_release.py / audit_apk.py read (tools/games.py get <game> duo.<key>).
        cmake = (ROOT / 'games/totk/duo/CMakeLists.txt').read_text()
        companions = ROOT / 'games/totk/duo/vendor/companions'
        make = (ROOT / 'games/totk/android/Makefile').read_text()
        self.assertIn('DUO_TITLE := $(shell $(GAMES_PY) get $(VERSION) title_id)', make)
        self.assertIn('DUO_MODULE := $(shell $(GAMES_PY) get $(VERSION) duo.module)', make)
        self.assertIn('-DDUO_TITLE=$(DUO_TITLE) -DDUO_MODULE=$(DUO_MODULE)', make)
        self.assertIn('runtime/duo_port.c', make.split('RUNTIME_C :=', 1)[1].split('\n', 1)[0])   # common runtime code
        for g in self.data['games']:
            v = games.values(g, self.data)
            with self.subTest(game=g):
                self.assertNotIn('runtime/duo_port.c', v['game_patches'])
                if not v['gpl_companion']:
                    continue
                self.assertEqual(v['companion'], 'duo')
                self.assertTrue((companions / v['duo']['module']).is_file(), v['duo']['module'])
                self.assertTrue((companions / 'packages' / v['duo']['package'] / 'dualscreen/manifest.json').is_file())
                self.assertEqual(v['duo']['module'], f'native/modules/{v["title_id"]}.cpp')
                self.assertIn(f'if (DUO_TITLE STREQUAL "{v["title_id"]}")', cmake.replace('elseif', 'if'))
                self.assertIn(f'set(DUO_MODULE_MAIN {v["duo"]["module"]})', cmake)
                import json
                manifest = json.loads((companions / 'packages' / v['duo']['package'] / 'dualscreen/manifest.json').read_text())
                self.assertEqual(manifest['title_id'].upper(), v['title_id'])
        out = subprocess.run([sys.executable, str(ROOT / 'tools/games.py'), 'get', 'v143', 'duo.package'],
                             capture_output=True, text=True, check=True).stdout.strip()
        self.assertEqual(out, 'TearsOfTheKingdom')
        with self.assertRaises(games.GameError):
            games.main(['get', 'v143', 'duo.nothing'])

    def test_committed_game_build_is_the_generated_totk_one(self):
        self.assertEqual(GAME_BUILD.read_text(), games.java('totk', self.data),
                         'GameBuild.java is generated: python3 tools/games.py java totk --out ' + str(GAME_BUILD.relative_to(ROOT)))
        botw = games.java('botw', self.data)
        for needle in ('VERSION = "botw160"', 'COMPANION = "duo"', 'TABLET_CAN_BE_MISSING = true', 'SURFACE_HUE = 208',
                       'TITLE_ID = "01007EF00011E000"', 'MOD_MANAGER = "BCML"', 'ROMFS_SIZE = 15131979952L'):
            self.assertIn(needle, botw)

    def test_no_per_game_file_overrides(self):
        self.assertEqual(games.overrides(), [], 'per-game whole-file overrides: declare the difference in games/games.toml instead')
        build_dex = (ROOT / 'games/totk/android/apk/build_dex.py').read_text()
        self.assertNotIn('java-', build_dex)

    def test_code_branches_on_the_game_only_where_declared(self):
        found = games.scan()
        declared = {f: spec['n'] for f, spec in self.data.get('branches', {}).items()}
        new = {f: n for f, n in found.items() if declared.get(f) != n}
        gone = {f: n for f, n in declared.items() if f not in found}
        self.assertEqual(new, {}, 'code branches on the game where games/games.toml [branches] does not declare it (or the count '
                                  'changed). Prefer a variable; else add the file, its count and the variable it implements')
        self.assertEqual(gone, {}, 'declared branches no longer in the code: update games/games.toml [branches]')

    def test_the_guard_sees_each_kind_of_branch(self):
        cases = [('c', '#ifdef GAME_BOTW'), ('c', '#if defined(TOTK_GAME_V143) && X'), ('c', 'if (!strcmp(version, "botw160"))'),
                 ('make', 'ifeq ($(VERSION),botw160)'), ('py', "if version == 'botw160':"), ('py', "if game != 'totk':"),
                 ('sh', 'make VERSION=botw160'), ('java', 'if (GameBuild.GAME.equals("botw")) x();'),
                 ('java', 'if ("botw".equals(GameBuild.GAME)) x();'), ('java', 'boolean d = GameBuild.COMPANION.equals("duo");')]
        for kind, line in cases:
            self.assertEqual(games.count_branches(line, kind), 1, line)
        for kind, line in [('c', '#include "game_version.h"'), ('py', "# version == 'botw160' in a comment"),
                           ('java', 'static final String VERSION = GameBuild.VERSION;'), ('c', 'ms = GAME_DEFAULT_XLATE_WAIT_MS;')]:
            self.assertEqual(games.count_branches(line, kind), 0, line)

    def test_makefile_takes_defines_and_sources_from_the_list(self):
        make = (ROOT / 'games/totk/android/Makefile').read_text()
        self.assertIn('VDEF := $(shell $(GAMES_PY) cflags $(VERSION))', make)
        self.assertIn('PATCHES := $(shell $(GAMES_PY) patches $(VERSION))', make)
        self.assertNotIn('-DGAME_BOTW=1', make)
        self.assertEqual(games.cflags('botw', self.data),
                         ['-DGAME_BOTW=1', '-DGAME_BOTW160=1', '-DGAME_DEFAULT_MOVIE_PACE=0', '-DGAME_DEFAULT_VK_PIPE_SYNC=1',
                          '-DGAME_DEFAULT_XLATE_WAIT_MS=4000'])

    def test_env_defaults_reach_the_code_and_fall_back_to_totk(self):
        sources = {p: p.read_text() for d in ('games/totk/runtime', 'games/totk/android/gfx') for p in (ROOT / d).glob('*.c')}
        totk = games.values('totk', self.data)['env_defaults']
        self.assertEqual(set(totk), set(games.values('botw', self.data)['env_defaults']))
        for name, value in totk.items():
            macro = games.env_macro(name)
            users = [p for p, text in sources.items() if f'getenv("{name}")' in text and macro in text]
            self.assertTrue(users, f'{name}: no C file reads it with the default {macro}')
            for p in users:
                fallback = re.search(rf'#ifndef {macro}\n#define {macro} (-?\d+)\n#endif', sources[p])
                self.assertTrue(fallback, f'{p.name}: {macro} has no fallback')
                self.assertEqual(int(fallback.group(1)), int(value), f'{p.name}: the fallback of {macro} is not TotK\'s value')

    def test_maintenance_lists_every_variable(self):
        text = (ROOT / 'MAINTENANCE.md').read_text()
        self.assertIn(games.table(self.data), text, 'MAINTENANCE.md: paste the output of python3 tools/games.py table')


if __name__ == '__main__':
    unittest.main()
