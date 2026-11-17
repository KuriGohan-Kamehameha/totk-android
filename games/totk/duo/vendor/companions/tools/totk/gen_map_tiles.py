#!/usr/bin/env python3
"""Writes packages/TearsOfTheKingdom/dualscreen/map_tiles.txt: for every MainField map tile
(layer G/S/U, column 0-11, row 0-9, LOD 01 and 02) the romfs path of the variant the companion
draws. A tile exists in variants named by reveal flags (T/F per area) and event suffixes (A/B);
the pick is TotK Native's (android/setup/companion/stage_companion.c map_jobs): most T flags
first, then the longest A/B chain, then the larger chain, then the larger flag string.

The output holds file names only, no game data. Input: a romfs folder, or a text file listing
romfs paths one per line (e.g. a dump's file list).

    gen_map_tiles.py <romfs dir | list.txt> [out]
"""
import os
import re
import sys

TILE = re.compile(r'^UI/Map/MainField/([GSU])_(\d\d)-(\d\d)_0([12])(?:_([TF]+))?((?:_[AB])*)\.bntx\.zs$')


def paths(source):
    if os.path.isdir(source):
        base = os.path.join(source, 'UI/Map/MainField')
        for name in os.listdir(base):
            yield 'UI/Map/MainField/' + name
    else:
        with open(source) as f:
            for line in f:
                yield line.strip()


def main():
    source = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
        os.path.dirname(__file__), '../../packages/TearsOfTheKingdom/dualscreen/map_tiles.txt')
    best = {}
    for p in paths(source):
        m = TILE.match(p)
        if not m:
            continue
        layer, col, row, lod, variant, event = m.group(1), int(m.group(2)), int(m.group(3)), int(m.group(4)), \
            m.group(5) or '', (m.group(6) or '').replace('_', '', 1)
        if col >= 12 or row >= 10:
            sys.exit(f'unexpected tile {p}')
        key = (layer, col, row, lod)
        rank = (variant.count('T'), len(event), event, variant)
        if key not in best or rank > best[key][0]:
            best[key] = (rank, p)
    missing = [k for k in ((l, c, r, d) for l in 'GSU' for c in range(12) for r in range(10) for d in (1, 2))
               if k not in best]
    if missing:
        sys.exit(f'{len(missing)} tiles missing, e.g. {missing[0]}')
    with open(out, 'w') as f:
        f.write(''.join(best[k][1] + '\n' for k in sorted(best)))
    print(f'{len(best)} tiles -> {out}')


if __name__ == '__main__':
    main()
