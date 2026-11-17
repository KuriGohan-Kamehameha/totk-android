#!/usr/bin/env python3
"""Audit a built APK before it is published: every entry must be on the allowlist below (manifest, resources, dex, our
native libraries, our fonts and licence texts, the identifier-only pipeline key list), and nothing may look like data from
the game (images, text, fonts, shaders, tables, rewritten code, companion art, the old Control-NCA launcher icon) or like
key material. build_release.py runs it on every APK it makes and stops on any finding.

    audit_apk.py [--game botw] [--release] <apk> [<apk> ...]   exit 1 and one line per finding when something is wrong

--game <game> (build_release.py passes its own; default totk): a game whose gpl_companion is true in games/games.toml
(today both) may carry the Eden Duo companion host and its own package (games.toml duo.package), byte for byte the
vendored one; another game's package, or any of it (the library, the package, the Eden-Duo*.txt licence texts) in a
game without gpl_companion, is a finding. An APK that carries the library or the package must also carry its GPL
notice (Eden-Duo.txt, whose "Source of this APK" section names the build.txt commit, and Eden-Duo-third-party.txt) and
assets/build.txt.
--release (build_release.py passes it for every non-debuggable build): that notice must name a public repository and a
commit without uncommitted changes, as GPL-3.0 section 6 needs for an APK that is passed on.
"""
import re, sys, zipfile
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[4] / 'tools'))
import games  # noqa: E402

SHADER_LIST = Path(__file__).resolve().parents[2] / 'assets/shader_list.tsv'   # committed: shader hashes and bind states only
TURNIP_SHA256 = '9b87afeb3d8d30c6a0ad5aab3e0703bc1500e0321e9f17a922106fa556389ec6'   # the pinned Mesa build, MIT
NATIVE_LIBS = {
    'libtotk.so',          # the runtime (runtime/, android/gfx/)
    'libdl_android.so',    # android/apk/dl_android_stub.c
    'libtotk_lsfg.so',     # android/third_party/lsfg (MIT backend; the user's DLL is never packed)
    'libc++_shared.so',    # the NDK's C++ library (lsfg, Vulkan drivers)
    'libtotk_xlate.so',    # the shader translator (NativeAOT executable)
    'libtotk_setup.so',    # android/setup (on-device setup)
    'libhactool.so',       # hactool executable for the container route
    'libtotk_turnip.so',   # the pinned Mesa Turnip
}
ALLOWED = [re.compile(p) for p in (
    r'AndroidManifest\.xml', r'classes\.dex', r'resources\.arsc',
    r'META-INF/(MANIFEST\.MF|CERT\.SF|CERT\.RSA|CERT\.EC)',
    r'res/mipmap-(mdpi|hdpi|xhdpi|xxhdpi|xxxhdpi)/ic_launcher(_round|_background|_foreground)?\.png',
    r'res/mipmap-anydpi-v26/ic_launcher(_round)?\.xml',
    r'res/xml/(data_extraction_rules|backup_rules)\.xml',   # backup / device-transfer exclusions (build_apk.backup_rules)
    r'lib/arm64-v8a/(' + '|'.join(re.escape(n) for n in sorted(NATIVE_LIBS)) + ')',
    r'assets/fonts/NotoSans-(Regular|SemiBold)\.ttf', r'assets/fonts/NotoSans-OFL\.txt',   # the one UI font family + licence
    r'assets/pso_keys\.bin',
    r'assets/shader_list\.tsv',   # the committed shared shader list (shader hashes and bind states only; SeedGen's input)
    r'assets/build\.txt',         # the source commit shown in About (build_release.stage_assets)
    r'assets/licenses/[A-Za-z0-9][A-Za-z0-9_.+-]*\.txt',
)]
# Games with gpl_companion = true only (games/games.toml; audit(game=...), audit_apk.py --game ...): the Eden Duo companion
# host and the game's own package (games.toml duo.package), both vendored in games/totk/duo/vendor (GPL-3.0-or-later;
# assets/licenses/Eden-Duo.txt). The package is pages, code-drawn images and plain-text lists (Tears of the Kingdom: the
# map tiles and the font the module reads from the player's files), no game art, and every entry must be byte for byte
# the vendored file.
DUO_LIB = 'libduo_host.so'
DUO_NOTICES = ('assets/licenses/Eden-Duo.txt', 'assets/licenses/Eden-Duo-third-party.txt')
DUO_PACKAGES = Path(__file__).resolve().parents[2] / 'duo/vendor/companions/packages'


def duo_package(game):
    """(vendored dualscreen folder, its prefix in the APK) of a game's Eden Duo package."""
    name = games.values(game)['duo']['package']
    return DUO_PACKAGES / name / 'dualscreen', f'assets/duo/{name}/dualscreen/'


def source_of(notice):
    """(repository or None, commit) from the "Source of this APK" section build_release.source_section puts at the head
    of Eden-Duo.txt; None when the text has no such section."""
    lines = notice.splitlines()
    if not lines or lines[0] != 'Source of this APK':
        return None
    fields = dict(re.findall(r'^  (Repository|Commit):\s+(\S.*)$', notice[:2000], re.M))
    if 'Commit' not in fields or 'Repository' not in fields:
        return None
    repository = fields['Repository']
    return (repository if repository.startswith('https://') else None), fields['Commit']


def allowed_duo(prefix):
    return [re.compile(p) for p in (
        r'lib/arm64-v8a/' + re.escape(DUO_LIB),
        re.escape(prefix) + r'(manifest|ASSETS\.provenance|[0-9A-F]{16})\.json',
        re.escape(prefix) + r'[A-Za-z0-9][A-Za-z0-9_]*\.txt',
        re.escape(prefix) + r'assets/[A-Za-z0-9][A-Za-z0-9_.-]*\.png',
    )]
# Names and extensions of the game's own files and of what the build or the device setup derives from them.
DENIED_SUFFIXES = ('.bntx', '.bfttf', '.bfotf', '.zs', '.sarc', '.pack', '.msbt', '.msbp', '.bfres', '.byml', '.bgyml',
                   '.spv', '.vk', '.vv', '.vkpack', '.nso', '.nca', '.nsp', '.xci', '.nsz', '.npdm', '.keys', '.jpg', '.dat',
                   '.astc', '.webp')
DENIED_NAMES = {'runtime-tables.bin', 'romfs.bin', 'runtime-index.bin', 'romfs-config.txt', 'shared-fonts.txt', 'aoc-config.txt',
                'install.json', 'seed-id.txt', 'seed-manifest.json', 'shader-keys.json', 'control.nacp', 'main', 'sdk', 'subsdk0',
                'rtld', 'runtime_tables.h', 'romfs_config.h', 'shared_fonts_config.h'}
DENIED_PARTS = ('companion', 'romfs', 'exefs', 'rewritten', 'game-data', 'shaders', 'extracted')
# File starts of game formats (Nintendo containers, compressed game files) that must not appear in any asset.
DENIED_MAGIC = (b'BNTX', b'SARC', b'Yaz0', b'\x28\xb5\x2f\xfd', b'FFNT', b'MsgStdBn', b'FRES', b'BY\x00', b'YB\x00', b'PFS0',
                b'HEAD', b'NSO0', b'TOTKTAB1', b'TOTKRFS1', b'TOTKVKP1')
FONT_MAGIC = (b'\x00\x01\x00\x00', b'OTTO', b'true')   # a plain TrueType/OpenType font, not a BFTTF (obfuscated header)
ELF_MAGIC = b'\x7fELF'
# A host path in a shipped binary (build folders, a user's home): __FILE__ strings and DWARF must not carry one
# (build_release.py maps the sources with -ffile-prefix-map and strips the debug info).
HOST_PATH = re.compile(rb'/(home|Users)/[A-Za-z0-9_.-]+/')


def audit(apk, game='totk', release=False):
    """Findings for one APK (empty when it passes). game: the build it claims to be (id or version tag); only a game with
    gpl_companion may carry the Eden Duo companion, with its own package (allowed_duo), and then with its GPL notice and
    source commit (duo_notice). release: a build to be passed on (the notice must name published source)."""
    import hashlib
    problems = []
    duo_source, duo_assets = duo_package(game)
    gpl = games.values(game)['gpl_companion']
    allowed = ALLOWED + (allowed_duo(duo_assets) if gpl else [])
    with zipfile.ZipFile(apk) as z:
        names = z.namelist()
        if len(set(names)) != len(names):
            problems.append('duplicate zip entries')
        for name in names:
            path = Path(name)
            lower = name.lower()
            if name.endswith('/') or '..' in path.parts or name.startswith('/'):
                problems.append(f'{name}: not a plain file entry'); continue
            if not any(p.fullmatch(name) for p in allowed):
                problems.append(f'{name}: not on the allowlist')
            if not gpl and re.fullmatch(r'assets/licenses/Eden-Duo.*\.txt', name):
                problems.append(f'{name}: the Eden Duo notice in a game without gpl_companion (games.toml)')
            if lower.endswith(DENIED_SUFFIXES) or path.name.lower() in DENIED_NAMES or '.patched' in lower:
                problems.append(f'{name}: name of game data or of data derived from it')
            if any(part.lower() in DENIED_PARTS for part in path.parts[:-1]):
                problems.append(f'{name}: inside a game-data folder name')
            data = z.read(name)
            if name.startswith('lib/'):
                if not data.startswith(ELF_MAGIC):
                    problems.append(f'{name}: not an ELF file')
                if path.name == 'libtotk_turnip.so' and hashlib.sha256(data).hexdigest() != TURNIP_SHA256:
                    problems.append(f'{name}: not the pinned Turnip build (sha256 {TURNIP_SHA256})')
                if b'.debug_info' in data or b'.zdebug_info' in data:
                    problems.append(f'{name}: carries debug info (strip it with llvm-strip --strip-debug)')
            if name.startswith('lib/') or name == 'classes.dex':
                found = HOST_PATH.search(data)
                if found:
                    problems.append(f'{name}: names a host path ({found.group(0).decode(errors="replace")}...)')
            elif data.startswith(DENIED_MAGIC):
                problems.append(f'{name}: starts like a game file ({data[:8]!r})')
            if name == 'assets/shader_list.tsv' and SHADER_LIST.is_file() and data != SHADER_LIST.read_bytes():
                problems.append(f'{name}: differs from the committed games/totk/assets/shader_list.tsv')
            if name.startswith(duo_assets):
                source = duo_source / name[len(duo_assets):]
                if not source.is_file() or source.read_bytes() != data:
                    problems.append(f'{name}: differs from the vendored companion package ({source})')
            if name == 'assets/build.txt' and not re.fullmatch(rb'commit [0-9a-f]{7,40}(-dirty)?\n', data):
                problems.append(f'{name}: not one "commit <hash>" line')
            if lower.endswith(('.ttf', '.otf')) and not data.startswith(FONT_MAGIC):
                problems.append(f'{name}: not a plain TrueType/OpenType font')
            if b'-----BEGIN' in data and b'PRIVATE KEY-----' in data:
                problems.append(f'{name}: contains a private key')
        for required in ('AndroidManifest.xml', 'classes.dex', 'lib/arm64-v8a/libtotk.so'):
            if required not in names:
                problems.append(f'{required}: missing')
        if gpl and any(n == f'lib/arm64-v8a/{DUO_LIB}' or n.startswith('assets/duo/') for n in names):
            problems += duo_notice(z, names, release)
        info = {i.filename: i for i in z.infolist()}
        for name, i in info.items():
            if name.startswith('assets/') and name.lower().endswith(('.ttf', '.otf')) and i.compress_type != zipfile.ZIP_STORED:
                problems.append(f'{name}: fonts must be stored uncompressed (the app hands them to native code with AssetManager.openFd)')
            if name == 'assets/shader_list.tsv' and i.compress_type != zipfile.ZIP_DEFLATED:
                problems.append(f'{name}: must be compressed (79 MB stored; the app copies it out with AssetManager.open)')
    return problems


def duo_notice(z, names, release):
    """An APK with the Eden Duo companion is GPL-3.0-or-later as a whole (duo/NOTICE.txt): it must carry that notice and
    the source commit, the notice must name that commit (its "Source of this APK" section), and a release must name a
    public repository and a commit without uncommitted changes."""
    problems = [f'{n}: missing (the APK carries the GPL Eden Duo companion)' for n in (*DUO_NOTICES, 'assets/build.txt')
                if n not in names]
    if problems:
        return problems
    built = re.fullmatch(r'commit ([0-9a-f]{7,40}(?:-dirty)?)\n', z.read('assets/build.txt').decode(errors='replace'))
    source = source_of(z.read(DUO_NOTICES[0]).decode(errors='replace'))
    if not built:
        return problems   # reported above as not one "commit <hash>" line
    if not source:
        return [f'{DUO_NOTICES[0]}: no "Source of this APK" section (build_release.source_section)']
    repository, commit = source
    short, dirty = built.group(1).removesuffix('-dirty'), built.group(1).endswith('-dirty')
    if not re.fullmatch(r'[0-9a-f]{40}(-dirty)?', commit) or not commit.startswith(short) or commit.endswith('-dirty') != dirty:
        problems.append(f'{DUO_NOTICES[0]}: names commit {commit}, not the build.txt commit {built.group(1)}')
    if release and dirty:
        problems.append('assets/build.txt: a release built with uncommitted changes has no publishable source')
    if release and not repository:
        problems.append(f'{DUO_NOTICES[0]}: a release must name the repository its source is published in')
    return problems


def main(argv):
    if not argv:
        sys.exit(__doc__)
    game, release = 'totk', False
    while argv and argv[0] in ('--game', '--release'):
        if argv[0] == '--release':
            release, argv = True, argv[1:]
        else:
            game, argv = argv[1], argv[2:]
    failed = False
    for apk in argv:
        problems = audit(apk, game, release)
        for p in problems:
            print(f'{apk}: {p}')
        failed |= bool(problems)
        if not problems:
            print(f'{apk}: audit passed')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
