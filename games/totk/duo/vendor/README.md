# Vendored Eden Duo code

Everything below this directory is other projects' code, copied at the commits named here and compiled into
`libduo_host.so` for the Breath of the Wild and the Tears of the Kingdom builds (`../CMakeLists.txt`; each build links
its own game's title module). Paths are kept as upstream has them, so a file here can be compared with its upstream
copy directly.

| Directory | From | Commit | Licence |
|---|---|---|---|
| `emu/` | [Eden Duo](https://github.com/igawa6/eden-duo) | `9ebc508f2ccf938487217857918a82f42c9c0304` | GPL-3.0-or-later (`emu/LICENSE.txt`; per-file SPDX tags, texts in `emu/LICENSES/`) |
| `companions/` | [P1R4N351/eden-duo-companions](https://github.com/P1R4N351/eden-duo-companions) branch `totk-companion`, a fork of [Eden Duo Companions](https://github.com/igawa6/eden-duo-companions): upstream `2169928d2f301aa7bb3b13777ca8a94d0e73961d` plus the Tears of the Kingdom companion (4 commits) | `85b7ab81fcf8593448102c93890ae73c389535d7` | GPL-3.0 (`companions/LICENSE`); `native/modules/totk_reader/` MIT (`totk_reader/LICENSE.MIT`) |
| `third_party/nlohmann/` | [nlohmann/json](https://github.com/nlohmann/json) `v3.12.0` (`single_include/nlohmann`), the version Eden Duo pins in `cpmfile.json` | tag `v3.12.0` | MIT (`LICENSE.MIT`) |
| `third_party/ankerl/` | [unordered_dense](https://github.com/martinus/unordered_dense) (`include/ankerl`), the commit Eden Duo pins | `7b55cab841` | MIT (`LICENSE`) |
| `third_party/zstd/` | [zstd](https://github.com/facebook/zstd) `lib/`: `zstd.h`, `zstd_errors.h`, `common/`, `decompress/` (without `huf_decompress_amd64.S`: built with `ZSTD_DISABLE_ASM`), the commit Eden Duo pins in `cpmfile.json` (taken from its CPM cache) | `33618c8` | BSD / GPLv2 dual, BSD taken (`LICENSE`: the text of zstd v1.5.7, `f8745da`) |

## What was taken

`emu/`:
- `src/core/mods/`: the dual-screen runtime, all `*.h` and `*.cpp` **except** `mod_re_tools.cpp` and `mod_console.cpp`
  (developer tools, compiled out with `EDEN_DSMOD_BUILD_DEV_TOOLS=0`), `mod_module.cpp`, `mod_romfs_sources.cpp`,
  `mod_sysfont_source.cpp`, `mod_user_source.cpp`, `mod_load_plan.cpp` and `mod_launch_gate.cpp` (emulator-side loaders;
  `../port_module.cpp` and `../port_sources.cpp` stand in for the first four, the last two have no user here);
  `modules/dsmod_module_sdk.h` and its README.
- `src/video_core/dsmod/aux_routing.h`, `texture_readback.h`; `src/core/frontend/framebuffer_layout.h`;
  `src/video_core/textures/astc.{h,cpp}`; `src/common/cityhash.{h,cpp}`.
- Self-contained headers the above include: `src/common/{common_types,swap,common_funcs,scope_exit,math_util,alignment,
  atomic_ops,dsmod_dev_tools,device_power_state}.h`, `src/core/file_sys/vfs/vfs_types.h`.
- `externals/bc_decoder/` (MPL-2.0) and `externals/stb/` (public domain).
- `LICENSE.txt` and the licence texts its files name (`LICENSES/`).

Every other emulator header the runtime includes (`core/core.h`, `core/memory.h`, the kernel, the VFS, ...) is **not**
Eden's: `../shim/` holds the port's stand-ins at the same include paths.

`companions/`:
- `native/modules/01007EF00011E000.cpp`, `01007EF00011E000_actions.h` and every `botw_*.h` that is not a test fixture
  (the Breath of the Wild module; no `*_test.cpp`).
- `packages/BreathOfTheWild/`: `README.md` and `dualscreen/` whole (`manifest.json`, the two per-build data files,
  `ASSETS.provenance.json`, `assets/*.png`, `load-plans/`). The APK packs `dualscreen/` without `load-plans/`
  (`android/build_release.py`); the load plans are not used yet.
- The Tears of the Kingdom module: `native/modules/0100F2C0115B6000.cpp`, `totk_romfs.{h,cpp}`, `totk_reader/` whole
  (MIT: TotK Native's reader, `companion_snapshot.c`, `companion.h` and `companion_state.h` byte for byte the copies in
  `../../runtime/`, which `tests/test_companion_state.py` checks, plus `game_version.h`, `totk_gamedata.*`,
  `totk_reader_ro.*`, its README and licence), `wonder_assets.{h,cpp}`, `wonder_font.{h,cpp}` and
  `third_party/stb_truetype.h` (the font and texture decoders it shares with the Super Mario Bros. Wonder module).
- `native/modules/totk_native_actions.h`: the module's item-action table, which only this port fills
  (`../port_module.cpp` binds it to the runtime's DuoPort ABI 3 functions; Eden's loader never does).
- `packages/TearsOfTheKingdom/`: `README.md` and `dualscreen/` whole (`manifest.json`, `ASSETS.provenance.json`,
  `assets/*.png`, `map_tiles.txt` and `totk_font.txt`: names of the player's game files the module reads). The APK
  packs `dualscreen/`.
- `tools/totk/` (`gen_manifest.py`, `gen_map_tiles.py`): the generators of that package's `manifest.json` and
  `map_tiles.txt`, the form in which the package is edited. Not built, not shipped.

Which module a build links, and the sources it needs besides its own file, are set in `../CMakeLists.txt` by
`DUO_TITLE` (`games/games.toml` `title_id` and `duo.module`, passed by `android/Makefile`).

The commit above is on a fork that must be public, with that branch pushed, before any APK built from it is released:
the notice (`../NOTICE.txt`) names it as where the vendored companion code comes from.

## Local changes

Vendored files are unmodified except:

| File | Change | Why |
|---|---|---|
| `emu/src/core/mods/mod_assets.cpp` | Two `#if !DUO_PORT` ... `#endif // !DUO_PORT` pairs around `OpenPrivateRomFS`, `ModRuntime::RegisterAssetSources` and `ModRuntime::OpenGameRomFS`. No line inside them is changed. | Those three functions open the emulator's content (loader, NCA, PatchManager, system NAND). The port's versions, over its own RomFS reader, are in `../port_sources.cpp`. |
| `third_party/ankerl/unordered_dense.h` | Eden Duo's own patch `.patch/unordered-dense/0001-avoid-memset-when-clearing-an-empty-table.patch` applied. | To match what Eden Duo builds with. |

To update: copy the same file set from the new commits, re-apply the first change (the markers are greppable:
`DUO_PORT`), update the commit ids here and in `../NOTICE.txt`, and run the desktop host test for both titles
(`../test/README.md`). A change to the runtime's `companion_snapshot.c`, `companion.h` or `companion_state.h` goes to
the companions' `totk_reader/` too, and the vendored copy is then updated with it (the byte-identity test fails until
both agree).
