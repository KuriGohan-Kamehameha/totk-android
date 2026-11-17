# Maintaining the two apps

One branch, `common`, builds both Android apps: **TotK Native (fork)** (Tears of the Kingdom 1.4.3) and **BotW Native**
(Breath of the Wild 1.6.0). They share the runtime, the graphics layer, the app and the tools; what differs between them
is one list of variables, `games/games.toml`, below. Everything not on that list is common code: a fix or feature lands
once and ships to both apps in the same commit. A new difference is added to the list first (meaning and both values),
then read from it; code that still branches on the game itself is declared there file by file (`[branches]`), and
`tests/test_game_variables.py` fails when code and list disagree.

## The per-game variables

Generated from `games/games.toml` (`python3 tools/games.py table`; the test checks this copy):

| Variable | Meaning | totk | botw |
|---|---|---|---|
| `nso_version` | The game build's version tag: make VERSION=, the runtime's TOTK_GAME_DIR, the data folders under the game folder (extracted/, rewritten/, game-data/, shaders/), install.json 'version' and the mods folder mods/<tag> | `v143` | `botw160` |
| `game_id` | install.json 'game' (InstallState) and GameBuild.GAME | `totk` | `botw` |
| `title` | The game's name in the menu and About | `Tears of the Kingdom` | `Breath of the Wild` |
| `version_label` | The game version the app runs, as shown (Home chip, About, setup details) | `1.4.3` | `1.6.0` |
| `title_id` | The game's title id (saves export/import in SaveManager; android/Makefile hands it to duo/CMakeLists.txt as DUO_TITLE, which picks the title module; the runtime's own copies are under [branches]: fs_host.c, services.c, duo_port.c) | `0100F2C0115B6000` | `01007EF00011E000` |
| `save_slots` | The game's save layout under the save root (SaveManager: the slot list, import detection, export and backups): dir = a slot folder's name (regex, group 1 = the slot number), file = the file that makes the folder a slot, picture = the slot's picture (a JPEG, or a file with one inside), names = how the menu's texts name the slot folders. option.sav at the save root is common | `{"dir": "slot_(\\d\\d)", "file": "progress.sav", "picture": "caption.sav", "names": "slot_00 … slot_05"}` | `{"dir": "(\\d)", "file": "game_data.sav", "picture": "caption.jpg", "names": "0 … 5"}` |
| `main_build_id` | Build id of the supported main (mods: exefs/<build id>.ips is applied only when it names this main; the runtime reads it from main itself, this is for docs and mod authors). TotK's 1.4.3 files are pinned by sha256 instead | `pinned by games/totk/assets/expected-v1.4.3.json` | `8E9978D50BDD20B4C8395A106C27FFDE` |
| `romfs_size` | Size of the merged RomFS image extracted/<tag>/romfs/romfs.bin (InstallState's install check) | `17052625232` | `15131979952` |
| `ondevice_setup` | Whether the app's own setup can import this game from a dump (false: the game folder is filled by hand, BOTW.md) | `true` | `false` |
| `docs` | The game's own docs page (what differs, how to stage the game) | `games/totk/android/APP.md` | `games/totk/android/BOTW.md` |
| `c_defines` | Compiler defines that select the game's runtime code (Makefile VDEF); every #if on them is listed under [branches] | `["TOTK_GAME_V143=1"]` | `["GAME_BOTW=1", "GAME_BOTW160=1"]` |
| `game_patches` | Runtime sources only this game links (Makefile PATCHES): its game-code patches and game-only services | `["runtime/game_patches_v143.c"]` | `["runtime/game_patches_botw.c", "runtime/settings_service.c", "runtime/aoc_romfs.c"]` |
| `env_defaults` | Defaults of runtime switches that differ (totk.env overrides either way). Passed to the compiler as GAME_DEFAULT_<name> (Makefile); the C fallback, used by host tests, is TotK's value | `{"TOTK_XLATE_WAIT_MS": 0, "TOTK_VK_PIPE_SYNC": 0, "TOTK_MOVIE_PACE": 1}` | `{"TOTK_XLATE_WAIT_MS": 4000, "TOTK_VK_PIPE_SYNC": 1, "TOTK_MOVIE_PACE": 0}` |
| `runtime_code` | The game's addresses, services and behaviour inside the shared runtime, rewriter and graphics layer: C compiled with this game's c_defines, and scripts given its version tag. Each file with such branches is listed under [branches] | `#if TOTK_GAME_V143 / !GAME_BOTW branches` | `#if GAME_BOTW / GAME_BOTW160 branches` |
| `shader_seed` | The game's shared shader list and the script that prepares its Vulkan shader seed | `{"list": "games/totk/assets/shader_list.tsv", "script": "games/totk/scripts/prepare_shader_seed.py"}` | `{"list": "games/botw/assets/shader_list.tsv", "script": "games/totk/scripts/prepare_botw_seed.py"}` |
| `app_name` | The app's name inside its menus (GameBuild.APP); the launcher label is per flavour below | `TotK Native` | `BotW Native` |
| `accent` | Menu colours (GameBuild.SEL selection, ACCENT accent, SURFACE_HUE: hue the surfaces are tinted to, -1 = the design's own) | `{"sel": "0xffd4ea74", "accent": "0xff57dfbf", "surface_hue": -1}` | `{"sel": "0xffa6ebff", "accent": "0xff66d8fa", "surface_hue": 208}` |
| `launcher_art` | Launcher artwork and its committed density renders (apk/render_launcher.py --game writes the renders) | `{"art": "games/totk/android/app-assets/icon/totk-native-1024.png", "renders": "games/totk/android/apk/launcher"}` | `{"art": "games/totk/android/app-assets/icon/botw-native-1024.png", "renders": "games/totk/android/apk/launcher-botw"}` |
| `launcher_background` | Adaptive launcher icon background colour, RGB (build_apk.launcher_override, render_launcher.py) | `[11, 20, 22]` | `[7, 13, 24]` |
| `menu_emblem` | The emblem the menu draws (Widgets.Emblem; the launcher art is launcher_art): 'island' = the sky island above twin peaks, 'sheikah_eye' = the Sheikah eye (app-assets/icon/make_icon_botw.py drawn in Java) | `island` | `sheikah_eye` |
| `mod_manager` | The PC mod manager the Mods import texts name (its merge is what the import takes) | `TKMM` | `BCML` |
| `companion` | Second screen (CompanionContent): 'map' = TotK Native's own map and inventory (CompanionView, MIT, drawn from the art the setup generates; compiled into every app, shown by none at the moment), 'duo' = the Eden Duo page drawn by libduo_host.so (DuoSurfaceView). Both sit behind CompanionScreen. With 'duo' the setup does not offer or update the second-screen art (ArtPolicy.used) | `duo` | `duo` |
| `gpl_companion` | Whether the APK carries the GPL-3.0 Eden Duo companion (libduo_host.so, its package under assets/duo/, Eden licence texts), which makes the APK as a whole GPL-3.0-or-later (duo/NOTICE.txt; build_release.py then refuses a build from a commit with uncommitted changes). false: the build and audit_apk.py refuse all of it | `true` | `true` |
| `duo` | The Eden Duo companion of a gpl_companion game: package = the folder under duo/vendor/companions/packages that the APK carries as assets/duo/<package>/dualscreen (build_release.py; audit_apk.py compares it byte for byte), module = the title module's main source under duo/vendor/companions (android/Makefile passes it with title_id to duo/CMakeLists.txt, which adds the sources that module needs) | `{"package": "TearsOfTheKingdom", "module": "native/modules/0100F2C0115B6000.cpp"}` | `{"package": "BreathOfTheWild", "module": "native/modules/01007EF00011E000.cpp"}` |
| `tablet_can_be_missing` | Whether the game starts without its tablet (BotW's Sheikah Slate; the stick LEDs have a look for that) | `false` | `true` |
| `bifrost_looks` | Row of BifrostLooks.LOOKS the stick-LED bridge uses (colour table) | `TOTK` | `BOTW` |

Flavours (`build_release.py --game <game> --flavour <flavour>`):

| Game | Flavour | Package | Launcher label | Version | Code | Key | Repository | Release tag, asset |
|---|---|---|---|---|---|---|---|---|
| `totk` | `fork` | `io.github.kurigohankamehameha.totk` | TotK Native (fork) | `1.0.1-fork.2` | 10002 | `fork-release.p12` | KuriGohan-Kamehameha/totk-android | `v<version>`, `TotK-Native-<version>.apk` |
| `totk` | `dev` | `io.github.kurigohankamehameha.totk.dev` | TotK Native (dev) | `1.0.1-dev` | clock | `totk-dev.pem` | KuriGohan-Kamehameha/totk-android | `v<version>`, `TotK-Native-<version>.apk` |
| `botw` | `release` | `io.github.p1r4n351.botw` | BotW Native | `0.2.0-pre.1` | 200 | `botw-release.pem` | P1R4N351/botw-android | `botw-v<version>`, `BotW-Native-<version>.apk` |
| `botw` | `dev` | `io.github.p1r4n351.botw` | BotW Native | `0.2.0-dev` | clock | `botw-dev.pem` | P1R4N351/botw-android | `botw-v<version>`, `BotW-Native-<version>.apk` |

Code that branches on the game itself is listed under `[branches]` in `games/games.toml`: today the C runtime and
graphics layer (`#if GAME_BOTW` / `TOTK_GAME_V143`: the games' addresses, services and behaviour), the Makefile, a few
TotK-only scripts (`prepare_runtime.py`, `verify_rewrites.py`, the PC and iOS builders), the on-device setup (TotK only:
`ondevice_setup`), the shader-seed script, `CompanionContent` (`companion`) and `BifrostState` (`bifrost_looks`).
`GameBuild.java` is generated from the list (`tools/games.py java`; the committed copy is TotK's and `apk/build_dex.py
--game` generates the one each build compiles). There are no per-game file overrides (`java-<game>/` is gone).

## Branches

| Branch | What it is |
|---|---|
| `common` (P1R4N351/totk-android) | What both apps ship. Builds, releases and fixes start here. |
| `botw-android` | The BotW line before `common` (kept for history; `common` = it + the fork's work + the variables). |
| `fork-release` (KuriGohan-Kamehameha/totk-android) | The TotK line before `common`. |
| `botw-upstream` = AurelioB/totk-android#54 | Upstreamable subset: BotW support without the GPL Eden Duo companion and the Bifrost bridge. |
| upstream PR branches (#43 wind, #44 quick menu by Dorkington; #45 mods import; #46 LSFG import) | Upstreamable subsets of single features. Carried in `common` as cherry-picks with their authors. |

A change meant for upstream is made on a branch from `upstream/main` and opened as a PR there; once it is in
`common` too (cherry-pick, `-x`), both apps have it. GPL code (`games/totk/duo/`) and the Bifrost bridge stay out of
upstream PRs.

## Build and release

One entry point for both apps and every flavour; package, label, version, key and launcher art come from the list:

```
tools/build_app.sh <game> <flavour> [build_release.py options]     # in a checkout
/root/totk-work/tools/build-app.sh <game> <flavour> [ref]          # build machine: clean worktree of ref (default common)
python3 games/totk/android/build_release.py --game <game> --flavour <flavour> --ndk ... --turnip ...   # what both call
```

| App | Command | APK |
|---|---|---|
| TotK Native (fork), release | `build-app.sh totk fork` | `/dev/shm/app-totk-fork/TotK-Native-fork-<version>.apk` |
| TotK Native (dev), debuggable, installs beside the fork | `build-app.sh totk dev` | `/dev/shm/app-totk-dev/...` |
| BotW Native, pre-release | `build-app.sh botw release` | `/dev/shm/app-botw-release/BotW-Native-<version>.apk` |
| BotW Native, debuggable | `build-app.sh botw dev` | `/dev/shm/app-botw-dev/...` |

The keys stay in `/root/totk-work/keys` (`TOTK_KEYS_DIR`) on the build machine and never enter git; a release flavour
refuses to build without its key, and a signing key is never changed (users would have to reinstall and lose data).
To release, raise `version_name` / `version_code` of the flavour in `games/games.toml` (codes only grow), commit, build,
publish the APK and its sha256 in the flavour's `repository` as tag `<release_tag><version>` with asset
`<release_asset><version>.apk` (+ `.sha256`): About > Project links there, and "Check for updates" asks that repository's
latest release only (one HEAD, `UpdateRules`), for its own package, and only from an `x.y.z` version (a pre-release or dev
build never asks). Every APK is audited (`apk/audit_apk.py`): a game with `gpl_companion = false` must not carry
`libduo_host.so`, an Eden Duo package or its licence texts, and one with it (today both) carries only its own package
(`duo.package`), byte for byte the vendored one. Both APKs carry `libduo_host.so` and are therefore GPL-3.0-or-later as
a whole (`games/totk/duo/NOTICE.txt`): a release is built only from a commit without local changes, and that commit,
with the vendored companions' commit it names, must be public wherever the APK is published. One build at a time on
the 8 GB build machine (`JOBS=4`, `DUO_JOBS=2`).

## Tests

```
tools/check_all.sh            # unit tests, then per game: native runtime (make VERSION=<tag>) and the app's Java
```

It prints `PASS`/`FAIL` per step and game and `CHECK_ALL_EXIT=<failed steps>`. Some unit tests fail on the build
machine on a pristine tree too (they need macOS tools); compare with the previous result, not with zero. Use
`LC_ALL=C.UTF-8`.

## Syncing from upstream

```
git fetch upstream && git switch common && git merge upstream/main
python3 tools/games.py branches     # if tests/test_game_variables.py now fails: new game branches came in
tools/check_all.sh
```

Upstream adds TotK-version branches (`TOTK_GAME_V143`) from time to time; each one is code BotW does not get, so the
guard test stops the merge until it is looked at and declared in `[branches]` (or rewritten as a variable).
`UPSTREAM_MERGE.md` (games/totk/android) has the details of merging the graphics layer.
