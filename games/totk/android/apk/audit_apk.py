#!/usr/bin/env python3
"""Audit a built APK before it is published: every entry must be on the allowlist below (manifest, resources, dex, our
native libraries, our fonts and licence texts, the identifier-only pipeline key list), and nothing may look like data from
the game (images, text, fonts, shaders, tables, rewritten code, companion art, the old Control-NCA launcher icon) or like
key material. build_release.py runs it on every APK it makes and stops on any finding.

    audit_apk.py [--game botw] <apk> [<apk> ...]   exit 1 and one line per finding when something is wrong

--game <game> (build_release.py passes its own): a game whose gpl_companion is true in games/games.toml (botw, botw160)
also may carry the Eden Duo companion host and its package; the default audits a Tears of the Kingdom APK, where they are
findings.
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
# Games with gpl_companion = true only (games/games.toml: Breath of the Wild; audit(game='botw'), audit_apk.py --game botw): the Eden Duo companion host and its
# package, both vendored in games/totk/duo/vendor (GPL-3.0-or-later; assets/licenses/Eden-Duo.txt). The package is pages
# and code-drawn images, no game art, and every entry must be byte for byte the vendored file.
DUO_LIB = 'libduo_host.so'
DUO_PACKAGE = Path(__file__).resolve().parents[2] / 'duo/vendor/companions/packages/BreathOfTheWild/dualscreen'
DUO_ASSETS = 'assets/duo/BreathOfTheWild/dualscreen/'
ALLOWED_BOTW = [re.compile(p) for p in (
    r'lib/arm64-v8a/' + re.escape(DUO_LIB),
    re.escape(DUO_ASSETS) + r'(manifest|ASSETS\.provenance|[0-9A-F]{16})\.json',
    re.escape(DUO_ASSETS) + r'assets/[A-Za-z0-9][A-Za-z0-9_.-]*\.png',
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


def audit(apk, game='totk'):
    """Findings for one APK (empty when it passes). game: the build it claims to be (id or version tag); only a game with
    gpl_companion may carry the Eden Duo companion (ALLOWED_BOTW)."""
    import hashlib
    problems = []
    allowed = ALLOWED + (ALLOWED_BOTW if games.values(game)['gpl_companion'] else [])
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
            if name.startswith(DUO_ASSETS):
                source = DUO_PACKAGE / name[len(DUO_ASSETS):]
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
        info = {i.filename: i for i in z.infolist()}
        for name, i in info.items():
            if name.startswith('assets/') and name.lower().endswith(('.ttf', '.otf')) and i.compress_type != zipfile.ZIP_STORED:
                problems.append(f'{name}: fonts must be stored uncompressed (the app hands them to native code with AssetManager.openFd)')
            if name == 'assets/shader_list.tsv' and i.compress_type != zipfile.ZIP_DEFLATED:
                problems.append(f'{name}: must be compressed (79 MB stored; the app copies it out with AssetManager.open)')
    return problems


def main(argv):
    if not argv:
        sys.exit(__doc__)
    game = 'totk'
    if argv[0] == '--game':
        game, argv = argv[1], argv[2:]
    failed = False
    for apk in argv:
        problems = audit(apk, game)
        for p in problems:
            print(f'{apk}: {p}')
        failed |= bool(problems)
        if not problems:
            print(f'{apk}: audit passed')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
