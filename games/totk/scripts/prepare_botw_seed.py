#!/usr/bin/env python3
"""Breath of the Wild 1.6.0 (VERSION=botw160): the prepared Vulkan shader seed for the Android runtime, in one run.

    python3 games/totk/scripts/prepare_botw_seed.py --romfs-tree <loose RomFS folder> --modules <rewritten/botw160> \\
        --translator <totk_xlate> --work <work folder> --output <out>/shaders/botw160 [--retain-vulkan-seed <folder>]

1. --romfs-tree: the folders that hold shaders (Pack/ Effect/ Event/ System/) of a loose RomFS tree are joined into
   <work>/romfs/romfs.bin with <work>/romfs/index.json, the layout prepare_shader_seed.py reads. The index names the
   SHA-256 of that image (of these four folders, not of the whole RomFS). --romfs <folder> takes such a folder as it is.
2. prepare_shader_seed.py --game botw --vulkan: every shader found is translated for the default bind state and for the
   bind states of games/botw/assets/shader_list.tsv by the translator started as the device starts it
   (totk_xlate --service --vulkan --chain totk-native-v5-unverified); it writes <variant>.spv, <variant>.vk and <key>.vv,
   the files android/gfx/vk_shader.c reads. --retain-vulkan-seed <folder>: the variants of an existing loose seed (the
   ones a device translated while playing, files/shaders/botw160) are translated again under their own names.
3. pack_shader_seed.py: <output>/seed.vkpack (the loose files stay), and <output>/seed-id.txt, the SHA-256 of the pack.

The translator must know --vulkan: build it from this tree (sh games/totk/tools/totk_xlate/publish_native.sh --out DIR;
.NET 10 SDK); the prebuilt/ ones of 2026-11 do not. The output is game-derived data: keep it out of the repository.
On the device: <app files>/shaders/botw160/seed.vkpack (frontend_android.c then selects the folder and the indexed seed
IO by itself); loose .spv/.vk/.vv files in that folder override the pack's entries of the same name.
"""
import argparse
import hashlib
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import prepare_shader_seed   # noqa: E402
from pack_shader_seed import pack_seed   # noqa: E402

VERSION = 'botw160'
FOLDERS = prepare_shader_seed.GAMES['botw']['folders']


def join_tree(tree, romfs):
    """<romfs>/romfs.bin and index.json from the shader folders of a loose RomFS tree; reused when the index names the
    same files (path and size)."""
    files = sorted(p for folder in FOLDERS for p in (tree/folder).rglob('*') if p.is_file())
    if not files:
        raise SystemExit(f'no {" ".join(FOLDERS)} files under {tree}')
    rows, at = [], 0
    for p in files:
        rows.append(dict(path=p.relative_to(tree).as_posix(), offset=at, size=p.stat().st_size)); at += rows[-1]['size']
    index = romfs/'index.json'
    if index.is_file() and (romfs/'romfs.bin').is_file() and (romfs/'romfs.bin').stat().st_size == at:
        old = json.loads(index.read_text())
        if [(r['path'], r['offset'], r['size']) for r in old.get('files', [])] == [(r['path'], r['offset'], r['size']) for r in rows]:
            return
    romfs.mkdir(parents=True, exist_ok=True)
    h = hashlib.sha256()
    with (romfs/'romfs.bin').open('wb') as image:
        for p in files:
            with p.open('rb') as f:
                while block := f.read(1 << 20):
                    image.write(block); h.update(block)
    index.write_text(json.dumps(dict(status='pass', romfs_sha256=h.hexdigest(), romfs_size=at, files=rows,
                                     integrity='joined from a loose RomFS tree (' + ' '.join(FOLDERS) + '); not an IVFC image')))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    source = p.add_mutually_exclusive_group(required=True)
    source.add_argument('--romfs-tree', type=Path, help='a loose RomFS folder (Pack/, Effect/, Event/, System/ are read)')
    source.add_argument('--romfs', type=Path, help='a folder with romfs.bin and index.json already')
    p.add_argument('--modules', type=Path, help='rewritten/botw160 (*.patched.bin): the game modules embed a few shaders')
    p.add_argument('--translator', type=Path, required=True, help='totk_xlate built from this tree (it must know --vulkan)')
    p.add_argument('--work', type=Path, help='work folder for the joined RomFS (required with --romfs-tree)')
    p.add_argument('--output', type=Path, required=True, help='the seed folder, e.g. <out>/shaders/botw160')
    p.add_argument('--shader-list', type=Path, help='default: games/botw/assets/shader_list.tsv')
    p.add_argument('--retain-vulkan-seed', type=Path, help='a loose Vulkan seed whose variants are translated again by name')
    p.add_argument('--no-pack', action='store_true', help='leave the loose files unpacked')
    a = p.parse_args()
    romfs = a.romfs
    if a.romfs_tree:
        if not a.work:
            p.error('--romfs-tree needs --work')
        romfs = a.work/'romfs'
        join_tree(a.romfs_tree.resolve(), romfs)
    output = a.output.resolve()
    report = prepare_shader_seed.prepare(romfs.resolve(), output, a.translator.resolve(), None, VERSION,
                                         a.shader_list.resolve() if a.shader_list else None,
                                         a.modules.resolve() if a.modules else None, 'botw',
                                         retain_vulkan_seed=a.retain_vulkan_seed.resolve() if a.retain_vulkan_seed else None,
                                         vulkan=True)
    if not a.no_pack:
        packed = pack_seed(output)
        h = hashlib.sha256()
        with packed.open('rb') as f:
            while block := f.read(1 << 20):
                h.update(block)
        (output/'seed-id.txt').write_text(h.hexdigest())   # 64 characters, no newline: vk_shader.c seed_init reads 64
        print(f'packed {packed} ({packed.stat().st_size} bytes, sha256 {h.hexdigest()})')
    print(f'{report["shaders"]} variants of {report["discovered"]} shaders, {len(report["skipped"])} not translated')


if __name__ == '__main__':
    main()
