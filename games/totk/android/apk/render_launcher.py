#!/usr/bin/env python3
"""Render the launcher icon's density PNGs from the app's own artwork into android/apk/launcher/, which is committed so
that building the APK needs no Pillow. Rerun after changing a game's artwork (needs Pillow):

    python3 games/totk/android/apk/render_launcher.py [--game totk|botw] [--icon PNG] [--out DIR]

--game takes the artwork, the output folder and the adaptive background from games/games.toml (launcher_art,
launcher_background); --icon / --out override the first two.

Writes res/mipmap-<density>/ic_launcher{,_round,_background,_foreground}.png (build_apk.launcher_override, the same
images build_apk.py --launcher-icon makes), launcher-512.png (store icon) and source.sha256 (the artwork they came from).
"""
import argparse, hashlib, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import build_apk  # noqa: E402

ICON = HERE.parents[0] / 'app-assets/icon/totk-native-1024.png'
OUT = HERE / 'launcher'


def render(icon=ICON, out=OUT, game='totk'):
    images, store = build_apk.launcher_override(icon, build_apk.launcher_background(game))
    out = Path(out)
    for _, path, data in images:
        (out / path).parent.mkdir(parents=True, exist_ok=True)
        (out / path).write_bytes(data)
    (out / 'launcher-512.png').write_bytes(store)
    (out / 'source.sha256').write_text(hashlib.sha256(Path(icon).read_bytes()).hexdigest() + '  ' + Path(icon).name + '\n')
    return out


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--game', default='totk')
    ap.add_argument('--icon', type=Path)
    ap.add_argument('--out', type=Path)
    a = ap.parse_args()
    art = build_apk.games.values(a.game)['launcher_art']
    root = HERE.parents[3]
    print(f'rendered {render(a.icon or root / art["art"], a.out or root / art["renders"], a.game)}')
