#!/usr/bin/env python3
"""The per-game variables (games/games.toml): load, check and hand them to the build.

    python3 tools/games.py list                         the games, one per line (id and version tag)
    python3 tools/games.py get <game> <var>[.<key>...]  one variable's value (JSON for tables and lists), or one key of a
                                                        table variable (get v143 duo.package)
    python3 tools/games.py cflags <game>                compiler defines for make (c_defines + GAME_DEFAULT_*)
    python3 tools/games.py patches <game>               the game's own runtime sources (Makefile PATCHES)
    python3 tools/games.py flavour <game> <flavour> <field>   a flavour's package / label / key / ... (paths resolved)
    python3 tools/games.py java <game> [--out FILE]     GameBuild.java for that game
    python3 tools/games.py table                        the Markdown table of every variable (MAINTENANCE.md)
    python3 tools/games.py branches                     branch points on the game, counted per file, as TOML

<game> is a game id (totk, botw) or its version tag (v143, botw160). Needs Python 3.11+ (tomllib).
"""
import json, os, re, subprocess, sys, tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DESCRIPTOR = ROOT / 'games/games.toml'
SCHEMA = 1
KEYS_DIR = Path(os.environ.get('TOTK_KEYS_DIR', '/root/totk-work/keys'))
FLAVOUR_FIELDS = {'package', 'label', 'version_name', 'version_code', 'key', 'key_pass_file', 'debuggable', 'repository', 'release_tag',
                  'release_asset'}
RELEASE_FIELDS = ('repository', 'release_tag', 'release_asset')
REPOSITORY = re.compile(r'https://github\.com/[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+')
RELEASE_NAME = re.compile(r'[A-Za-z0-9_.-]*')


class GameError(ValueError):
    pass


def load(path=DESCRIPTOR):
    data = tomllib.loads(Path(path).read_text())
    if data.get('schema') != SCHEMA:
        raise GameError(f'{path}: schema {data.get("schema")!r}, this tools/games.py reads {SCHEMA}')
    games = data.get('games')
    if not games or len(set(games)) != len(games):
        raise GameError(f'{path}: games must list each game once')
    for name, var in data.get('var', {}).items():
        if not isinstance(var.get('meaning'), str) or not var['meaning']:
            raise GameError(f'{path}: var.{name} has no meaning')
        missing = [g for g in games if g not in var]
        extra = [k for k in var if k not in games and k != 'meaning']
        if missing or extra:
            raise GameError(f'{path}: var.{name}: values missing for {missing}, unknown keys {extra}')
    tags = [data['var']['nso_version'][g] for g in games]
    if len(set(tags)) != len(tags):
        raise GameError(f'{path}: two games share a version tag')
    for game, flavours in data.get('flavour', {}).items():
        if game not in games:
            raise GameError(f'{path}: flavour.{game}: no such game')
        for fl, spec in flavours.items():
            unknown = set(spec) - FLAVOUR_FIELDS
            if unknown or not {'package', 'label', 'version_name', 'key', 'debuggable', *RELEASE_FIELDS} <= set(spec):
                raise GameError(f'{path}: flavour.{game}.{fl}: unknown {sorted(unknown)} or missing fields')
            if not REPOSITORY.fullmatch(spec['repository']) or not all(RELEASE_NAME.fullmatch(spec[f]) for f in RELEASE_FIELDS[1:]):
                raise GameError(f'{path}: flavour.{game}.{fl}: repository must be https://github.com/<owner>/<name>, '
                                'release_tag / release_asset plain file-name characters')
        # The app finds its releases by its package (GameBuild.RELEASES): flavours that share one must agree on them.
        for fl, spec in flavours.items():
            for other, o in flavours.items():
                if o['package'] == spec['package'] and any(o[f] != spec[f] for f in RELEASE_FIELDS):
                    raise GameError(f'{path}: flavour.{game}.{fl} and .{other} share a package but not their releases')
    for file, spec in data.get('branches', {}).items():
        bad = [v for v in spec.get('vars', []) if v not in data['var']]
        if bad or not spec.get('vars') or not isinstance(spec.get('n'), int):
            raise GameError(f'{path}: branches."{file}": needs n and vars naming declared variables (unknown: {bad})')
    return data


def game_id(data, name):
    """totk / botw from an id or a version tag."""
    if name in data['games']:
        return name
    for g in data['games']:
        if data['var']['nso_version'][g] == name:
            return g
    raise GameError(f'unknown game {name!r}: one of ' + ', '.join(f'{g} ({data["var"]["nso_version"][g]})' for g in data['games']))


def values(name, data=None):
    data = data or load()
    g = game_id(data, name)
    return {var: spec[g] for var, spec in data['var'].items()}


def flavour(name, fl, data=None):
    """A flavour's fields, with key / key_pass_file as paths in TOTK_KEYS_DIR."""
    data = data or load()
    g = game_id(data, name)
    flavours = data.get('flavour', {}).get(g, {})
    if fl not in flavours:
        raise GameError(f'{g} has no flavour {fl!r}: ' + ', '.join(sorted(flavours)))
    spec = dict(flavours[fl])
    spec['key'] = KEYS_DIR / spec['key']
    if spec.get('key_pass_file'):
        spec['key_pass_file'] = KEYS_DIR / spec['key_pass_file']
    return spec


def env_macro(name):
    """TOTK_XLATE_WAIT_MS -> GAME_DEFAULT_XLATE_WAIT_MS."""
    return 'GAME_DEFAULT_' + re.sub(r'^TOTK_', '', name)


def cflags(name, data=None):
    v = values(name, data)
    return [f'-D{d}' for d in v['c_defines']] + [f'-D{env_macro(k)}={int(x)}' for k, x in sorted(v['env_defaults'].items())]


def releases(name, data=None):
    """[package, repository, release_tag, release_asset] of each of the game's flavours, one row per package."""
    data = data or load()
    rows = {}
    for spec in data.get('flavour', {}).get(game_id(data, name), {}).values():
        rows.setdefault(spec['package'], [spec['package'], *(spec[f] for f in RELEASE_FIELDS)])
    return list(rows.values())


def java_hex(x):
    return f'0x{x & 0xffffffff:08x}' if x >= 0 else str(x)


def java(name, data=None):
    """GameBuild.java for this game: the variables the app reads, as constants."""
    data = data or load()
    v = values(name, data)
    a, slots = v['accent'], v['save_slots']
    s = lambda x: json.dumps(x, ensure_ascii=False)
    rows = ',\n        '.join('{' + ', '.join(s(x) for x in row) + '}' for row in releases(name, data))
    return f'''package com.aure.totk.setup;

// GENERATED by tools/games.py java {v['game_id']} from games/games.toml: edit that file, not this one. The committed copy is
// Tears of the Kingdom's; apk/build_dex.py --game generates the one each build compiles (tests/test_game_variables.py).

/** Which game this app build runs: what install.json must name and where its files are under the game folder, what
 *  the menu calls the app and the game, the menu's accent colours and which second screen it has. */
public final class GameBuild {{
    private GameBuild() {{ }}
    public static final String GAME = {s(v['game_id'])}, VERSION = {s(v['nso_version'])};
    public static final long ROMFS_SIZE = {v['romfs_size']}L;
    public static final String APP = {s(v['app_name'])}, TITLE = {s(v['title'])}, VERSION_LABEL = {s(v['version_label'])};
    public static final String TITLE_ID = {s(v['title_id'])};
    /** The save layout (SaveManager): a slot is a folder whose name matches SAVE_SLOT_DIR (group 1 = its number) holding
     *  SAVE_SLOT_FILE; SAVE_PICTURE is its picture; SAVE_SLOT_NAMES is how the texts name the slot folders. */
    public static final String SAVE_SLOT_DIR = {s(slots['dir'])}, SAVE_SLOT_FILE = {s(slots['file'])},
            SAVE_PICTURE = {s(slots['picture'])}, SAVE_SLOT_NAMES = {s(slots['names'])};
    /** The app flavours of this game by package (games.toml [flavour]): {{package, repository, release tag prefix,
     *  release asset prefix}}. The repository is About > Project and the one the update check asks (UpdateRules.Release). */
    public static final String[][] RELEASES = {{
        {rows}
    }};
    /** Whether the app's own setup can import this game (false: the game folder is filled by hand). */
    public static final boolean ONDEVICE_SETUP = {str(v['ondevice_setup']).lower()};
    /** Theme.SEL (selection) and Theme.ZONAI (accent), and the hue Theme.tint gives the surfaces (-1: unchanged). */
    public static final int SEL = {java_hex(a['sel'])}, ACCENT = {java_hex(a['accent'])}, SURFACE_HUE = {a['surface_hue']};
    /** The emblem the menu draws: "island" or "sheikah_eye" (Widgets.Emblem). */
    public static final String EMBLEM = {s(v['menu_emblem'])};
    /** The PC mod manager whose merge the Mods import takes. */
    public static final String MOD_MANAGER = {s(v['mod_manager'])};
    /** Second screen: "map" (CompanionView) or "duo" (DuoSurfaceView, libduo_host.so). */
    public static final String COMPANION = {s(v['companion'])};
    /** Whether the game starts without its tablet (the stick LEDs then have a look for that). */
    public static final boolean TABLET_CAN_BE_MISSING = {str(v['tablet_can_be_missing']).lower()};
    /** BifrostLooks row name: "TOTK" or "BOTW". */
    public static final String BIFROST_LOOKS = {s(v['bifrost_looks'])};
}}
'''


def table(data=None):
    data = data or load()
    games = data['games']
    def colours(x):   # ARGB colours in hex, as in the code
        if isinstance(x, dict):
            return {k: colours(v) for k, v in x.items()}
        return f'0x{x:08x}' if isinstance(x, int) and not isinstance(x, bool) and x > 0xffffff and x <= 0xffffffff else x
    fmt = lambda x: '`' + (json.dumps(colours(x), ensure_ascii=False) if not isinstance(x, str) else x).replace('|', '\\|') + '`'
    head = '| Variable | Meaning | ' + ' | '.join(games) + ' |\n|---|---|' + '---|' * len(games) + '\n'
    rows = [f'| `{name}` | {spec["meaning"].replace("|", "/")} | ' + ' | '.join(fmt(spec[g]) for g in games) + ' |'
            for name, spec in data['var'].items()]
    flav = []
    for g in games:
        for fl, spec in data.get('flavour', {}).get(g, {}).items():
            flav.append(f'| `{g}` | `{fl}` | `{spec["package"]}` | {spec["label"]} | `{spec["version_name"]}` | '
                        f'{spec.get("version_code", "from name" if not spec["debuggable"] else "clock")} | `{spec["key"]}` | '
                        f'{spec["repository"].removeprefix("https://github.com/")} | `{spec["release_tag"]}<version>`, '
                        f'`{spec["release_asset"]}<version>.apk` |')
    return (head + '\n'.join(rows) + '\n\nFlavours (`build_release.py --game <game> --flavour <flavour>`):\n\n'
            '| Game | Flavour | Package | Launcher label | Version | Code | Key | Repository | Release tag, asset |\n'
            '|---|---|---|---|---|---|---|---|---|\n' + '\n'.join(flav) + '\n')


# ---------------------------------------------------------------- branch points on the game in code

TAGS = r'(?:v143|botw160|botw|totk)'
C_LIKE = ('.c', '.h', '.m', '.mm', '.cpp', '.cc', '.hpp', '.S', '.s')
PATTERNS = {
    # preprocessor conditions on a game's define, and runtime string compares on a version tag
    'c': [re.compile(r'^\s*#\s*(?:if|ifdef|ifndef|elif)\b.*\b(?:GAME_BOTW\w*|TOTK_GAME_V143)\b'),
          re.compile(r'str(?:n?case)?cmp\([^;]*"' + TAGS + r'"')],
    'make': [re.compile(r'^\s*(?:else\s+)?if(?:n?eq|n?def)\b.*\b(?:' + TAGS + r'|GAME_BOTW\w*|TOTK_GAME_V143)\b'),
             re.compile(r'-D(?:GAME_BOTW\w*|TOTK_GAME_V143)\b')],
    'py': [re.compile(r'(?:==|!=|\bin\b)\s*\(?\s*[\'"]' + TAGS + r'[\'"]'), re.compile(r'[\'"]' + TAGS + r'[\'"]\s*(?:==|!=)')],
    'sh': [re.compile(r'\b(?:botw160|v143)\b')],
    'java': [re.compile(r'"(?:' + TAGS[3:-1] + r'|map|duo|TOTK|BOTW)"\s*\.equals\('),
             re.compile(r'\.equals\(\s*"(?:' + TAGS[3:-1] + r'|map|duo|TOTK|BOTW)"'), re.compile(r'GameBuild\.(?:GAME|VERSION)\s*[!=]=')],
}
EXCLUDED = (re.compile(r'^(?:tests/|games/totk/tests/|games/totk/duo/vendor/|games/games\.toml$|tools/games\.py$)'),
            re.compile(r'\.md$'))


def kind(path):
    name = path.rsplit('/', 1)[-1]
    if name == 'Makefile' or name.endswith('.mk') or name == 'CMakeLists.txt':
        return 'make'
    if name.endswith(C_LIKE):
        return 'c'
    for ext, k in (('.py', 'py'), ('.sh', 'sh'), ('.java', 'java')):
        if name.endswith(ext):
            return k
    return None


def count_branches(text, k):
    n = 0
    for line in text.splitlines():
        stripped = line.strip()
        if k in ('py', 'sh', 'make') and stripped.startswith('#'):
            continue
        if k == 'java' and stripped.startswith(('//', '*', '/*')):
            continue
        n += any(p.search(line) for p in PATTERNS[k])
    return n


def scan(root=ROOT):
    """{path: branch points} for every tracked file that branches on the game."""
    files = subprocess.run(['git', '-C', str(root), 'ls-files'], capture_output=True, text=True, check=True).stdout.split('\n')
    found = {}
    for f in files:
        k = kind(f) if f else None
        if not k or any(p.search(f) for p in EXCLUDED):
            continue
        try:
            text = (Path(root) / f).read_text(errors='replace')
        except OSError:
            continue
        n = count_branches(text, k)
        if n:
            found[f] = n
    return found


def overrides(root=ROOT):
    """Whole-file per-game source overrides (java-<game>/ and the like), which the variables replace."""
    return sorted(str(p.relative_to(root)) for p in (Path(root) / 'games/totk/android').glob('java-*'))


def main(argv):
    if not argv or argv[0] in ('-h', '--help'):
        sys.exit(__doc__)
    cmd, args = argv[0], argv[1:]
    data = load()
    if cmd == 'list':
        for g in data['games']:
            print(g, data['var']['nso_version'][g])
    elif cmd == 'get':
        name, *keys = args[1].split('.')
        x = values(args[0], data)[name]
        for key in keys:
            if not isinstance(x, dict) or key not in x:
                raise GameError(f'{args[1]}: {name} has no key {key!r}')
            x = x[key]
        print(x if isinstance(x, (str, int)) and not isinstance(x, bool) else json.dumps(x))
    elif cmd == 'cflags':
        print(' '.join(cflags(args[0], data)))
    elif cmd == 'patches':
        print(' '.join(values(args[0], data)['game_patches']))
    elif cmd == 'flavour':
        x = flavour(args[0], args[1], data).get(args[2], '')
        print(str(x).lower() if isinstance(x, bool) else x)
    elif cmd == 'java':
        text = java(args[0], data)
        if '--out' in args:
            out = Path(args[args.index('--out') + 1])
            out.parent.mkdir(parents=True, exist_ok=True)
            out.write_text(text)
        else:
            sys.stdout.write(text)
    elif cmd == 'table':
        sys.stdout.write(table(data))
    elif cmd == 'branches':
        declared = data.get('branches', {})
        for f, n in sorted(scan().items()):
            vars_ = declared.get(f, {}).get('vars', ['runtime_code'])
            print(f'"{f}" = {{ n = {n}, vars = {json.dumps(vars_)} }}')
    else:
        sys.exit(f'unknown command {cmd!r}\n\n{__doc__}')


if __name__ == '__main__':
    try:
        main(sys.argv[1:])
    except GameError as e:
        sys.exit(f'games.py: {e}')
