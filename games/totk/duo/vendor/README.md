# Vendored Eden Duo code

Everything below this directory is other projects' code, copied at the commits named here and compiled into
`libduo_host.so` for the Breath of the Wild build (`../CMakeLists.txt`). Paths are kept as upstream has them, so a file
here can be compared with its upstream copy directly.

| Directory | From | Commit | Licence |
|---|---|---|---|
| `emu/` | [Eden Duo](https://github.com/igawa6/eden-duo) | `9ebc508f2ccf938487217857918a82f42c9c0304` | GPL-3.0-or-later (`emu/LICENSE.txt`; per-file SPDX tags, texts in `emu/LICENSES/`) |
| `companions/` | [Eden Duo Companions](https://github.com/igawa6/eden-duo-companions) | `2169928d2f301aa7bb3b13777ca8a94d0e73961d` | GPL-3.0 (`companions/LICENSE`) |
| `third_party/nlohmann/` | [nlohmann/json](https://github.com/nlohmann/json) `v3.12.0` (`single_include/nlohmann`), the version Eden Duo pins in `cpmfile.json` | tag `v3.12.0` | MIT (`LICENSE.MIT`) |
| `third_party/ankerl/` | [unordered_dense](https://github.com/martinus/unordered_dense) (`include/ankerl`), the commit Eden Duo pins | `7b55cab841` | MIT (`LICENSE`) |

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

## Local changes

Vendored files are unmodified except:

| File | Change | Why |
|---|---|---|
| `emu/src/core/mods/mod_assets.cpp` | Two `#if !DUO_PORT` ... `#endif // !DUO_PORT` pairs around `OpenPrivateRomFS`, `ModRuntime::RegisterAssetSources` and `ModRuntime::OpenGameRomFS`. No line inside them is changed. | Those three functions open the emulator's content (loader, NCA, PatchManager, system NAND). The port's versions, over its own RomFS reader, are in `../port_sources.cpp`. |
| `third_party/ankerl/unordered_dense.h` | Eden Duo's own patch `.patch/unordered-dense/0001-avoid-memset-when-clearing-an-empty-table.patch` applied. | To match what Eden Duo builds with. |

To update: copy the same file set from the new commits, re-apply the first change (the markers are greppable:
`DUO_PORT`), update the commit ids here and in `../NOTICE.txt`, and run the desktop host test (`../test/`).
