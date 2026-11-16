# Breath of the Wild 1.6.0 on this port (experimental)

This branch adds a second game to the Android port: The Legend of Zelda: Breath of the Wild, version 1.6.0
(title 01007EF00011E000, build id 8E9978D50BDD20B4C8395A106C27FFDE). It reuses the Tears of the Kingdom runtime,
graphics layer and app. Nothing of the game is in the repository; you need your own dump.

It is a work in progress. It was developed and tested on one device, an AYN Thor (Snapdragon 8 Gen 2), and only
briefly played. The Tears of the Kingdom build is meant to be unchanged by everything here.

## What works

Tested on the AYN Thor unless noted.

- The three code modules rewrite for Android with the merged rewriter (`third_party/d2r`), and the result passes the
  structural and site-set checks against the MacNativeGames pins for 1.6.0.
- The game boots in the app, reaches the title screen, loads a save and plays at 30 fps with the bundled Turnip driver.
- Add-on content (DLC 3.0) mounts from extracted images.
- RomFS mods load through `TOTK_ROMFS_MODS` and a `manifest.tsv`, as for Tears of the Kingdom.
- A prepared Vulkan shader seed can be built from the game files (`scripts/prepare_botw_seed.py`).

## What is different from the Tears of the Kingdom build

| Area | Breath of the Wild |
|---|---|
| Build | `make -C games/totk/android VERSION=botw160 ...`; app: `build_release.py --game botw160 --dev-key --package <id> --label <name>` |
| Game patches | `runtime/game_patches_botw.c`: exclusive loads with acquire (a job-queue race on fast cores) and the real-time frame step, both ported from MacNativeGames (MIT) |
| Missing shaders | A draw waits for the on-device translator (`TOTK_XLATE_WAIT_MS`, default 4000) and for a compiling pipeline (`TOTK_VK_PIPE_SYNC`, default 1). The game draws some images once and keeps them, so a skipped draw stays wrong |
| App identity | `GameBuild.java` (per-game override in `android/java-botw/`): name, version label, install checks, accent colours |
| On-device setup | Not ported. The app's setup still imports Tears of the Kingdom only |

## Putting the game on a device (developer route)

The on-device setup does not know this game, so the game folder is filled by hand. Under the app's game folder
(`/sdcard/Android/data/<package>/files`):

```
rewritten/botw160/{main,subsdk0,sdk}.patched.bin      # patch_nso.py with DM_HOST_TP=tpidr_el0
game-data/botw160/runtime-tables.bin                  # scripts/prepare_runtime.py --version botw160
game-data/botw160/{romfs-config.txt,shared-fonts.txt,aoc-config.txt}
extracted/botw160/romfs/{romfs.bin,runtime-index.bin} # base + update merged, indexed
extracted/botw160/aoc/<1|2>/{romfs.bin,runtime-index.bin}   # optional DLC volumes
extracted/firmware/shared-fonts/
shaders/botw160/seed.vkpack                           # or loose .spv/.vk/.vv; never empty
install.json                                          # format 1, game "botw", version "botw160", playable true
```

`install.json` goes last. Files pushed with adb belong to the shell user: make them readable by the app.

## Not done, or not tested

- The on-device setup for this game, and every menu text that describes it.
- The Graphics menu's options were written for Tears of the Kingdom and are untested here.
- Saves: the menu's save manager does not read this game's saves.
- Other devices, other drivers, docked mode, 60 fps.
- The second-screen companion for this game is not part of this branch (see the fork's `botw-android` branch, which
  carries a GPL-3.0 integration of the Eden Duo companion).
- Long play. Only the opening, the Great Plateau and the menus were looked at.

## Credits

- MacNativeGames (MIT): the Breath of the Wild runtime differences, game patches, shader list and seed script this
  port takes over.
- The decompilation and research communities whose notes those build on.
