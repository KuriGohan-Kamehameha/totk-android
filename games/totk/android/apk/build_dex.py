#!/usr/bin/env python3
"""Compile the SDK-only companion: javac (Java 17) -> deterministic class jar -> d8.

    python3 build_dex.py --out classes.dex [--android-sdk ~/Android/Sdk]
No Gradle, AndroidX, Maven downloads, or native libraries are required.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import sys
import zipfile

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[3] / 'tools'))
import games  # noqa: E402  (games/games.toml: the per-game variables)
GAME_BUILD = Path('com/aure/totk/setup/GameBuild.java')


def version_key(path):
    return tuple(int(n) for n in re.findall(r'\d+', path.name))


def find_sdk(explicit=None):
    """Return a usable (root, android.jar, d8), newest installed stable tools/API first."""
    candidates = [explicit] if explicit else [os.environ.get('ANDROID_HOME'), os.environ.get('ANDROID_SDK_ROOT'), Path.home() / 'Android/Sdk']
    for candidate in candidates:
        if not candidate:
            continue
        root = Path(candidate).expanduser().resolve()
        jars = sorted((p / 'android.jar' for p in (root / 'platforms').glob('android-*')
                       if (p / 'android.jar').is_file() and version_key(p) >= (33,)), key=lambda p: version_key(p.parent), reverse=True)
        tools = sorted((p / ('d8.bat' if os.name == 'nt' else 'd8') for p in (root / 'build-tools').glob('*')
                        if (p / ('d8.bat' if os.name == 'nt' else 'd8')).is_file()), key=lambda p: version_key(p.parent), reverse=True)
        if jars and tools:
            return root, jars[0], tools[0]
    return None


def build(out, sdk=None, extra_sources=(), game=None):
    selected = find_sdk(sdk)
    if selected is None:
        raise RuntimeError('Android SDK with platform API >=33 and build-tools/d8 not found; use --android-sdk, ANDROID_HOME, or ANDROID_SDK_ROOT')
    _, android_jar, d8 = selected
    javac = shutil.which('javac')
    if not javac:
        raise RuntimeError('javac 17 or later not found on PATH')
    roots = [HERE.parent / 'java', *(Path(p).resolve() for p in extra_sources)]
    sources = sorted(p for root in roots for p in root.rglob('*.java'))
    # The game's variables (games/games.toml) as GameBuild.java, generated for this build in place of the committed copy
    # (Tears of the Kingdom's): the one per-game Java source.
    generated = games.java(game or 'totk')
    if not sources:
        raise RuntimeError('no Java sources found')
    out = Path(out).expanduser().resolve()
    out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='totk-dex-') as temp:
        temp = Path(temp)
        (temp / 'gen' / GAME_BUILD).parent.mkdir(parents=True)
        (temp / 'gen' / GAME_BUILD).write_text(generated)
        sources = sorted([p for p in sources if p != HERE.parent / 'java' / GAME_BUILD] + [temp / 'gen' / GAME_BUILD])
        classes, dex = temp / 'classes', temp / 'dex'
        classes.mkdir(); dex.mkdir()
        subprocess.run([javac, '--release', '17', '-encoding', 'UTF-8', '-g:none', '-classpath', str(android_jar),
                        '-d', str(classes), *map(str, sources)], check=True)
        jar = temp / 'classes.jar'
        with zipfile.ZipFile(jar, 'w', compression=zipfile.ZIP_STORED) as archive:
            for source in sorted(classes.rglob('*.class')):
                info = zipfile.ZipInfo(source.relative_to(classes).as_posix(), (1980, 1, 1, 0, 0, 0))
                archive.writestr(info, source.read_bytes())
        subprocess.run([str(d8), '--release', '--min-api', '33', '--lib', str(android_jar), '--output', str(dex), str(jar)], check=True)
        outputs = sorted(dex.glob('*.dex'))
        if len(outputs) != 1:
            raise RuntimeError(f'expected one classes.dex, got {len(outputs)}')
        shutil.copyfile(outputs[0], out)
    print(f'wrote {out} ({out.stat().st_size} bytes; {android_jar.parent.name}, {d8.parent.name})')
    return out


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--android-sdk', type=Path)
    parser.add_argument('--sources', type=Path, action='append', default=[], help='additional Java source root (preview only)')
    parser.add_argument('--game', default='totk', help='game (games/games.toml): totk or botw, or its version tag v143 / botw160')
    args = parser.parse_args(argv)
    try:
        build(args.out, args.android_sdk, args.sources, args.game)
    except (RuntimeError, subprocess.CalledProcessError, games.GameError) as error:
        parser.exit(1, f'build_dex: {error}\n')


if __name__ == '__main__':
    main()
