# Breath of the Wild on the Vulkan backend: rendering findings (2026-11-16)

Found on an AYN Thor (Adreno 740, bundled Turnip), headless with `totk-native-vk`, the pass dump
(`TOTK_VK_PASS_DUMP`) and frame shots, compared with the Mac project's Metal backend (`nvn_mtl.m`), which renders
BotW correctly. Captures are not published (they are game images).

## Purple / pink / blue water: fixed (`73dae71`)

- **Cause.** The game bakes one colour per water type by drawing it into a 1x1 RGBA32F render target in a CPU-cached
  pool, then reads that pixel on the CPU two frames later with no invalidate. The backend's settled write-back copied
  GPU renderings back to guest memory only after a 60-submission quiet interval and in a budgeted sweep; while the
  world streamed in, the next water type's draw cancelled the pending copy. On the Great Plateau load 13 of 57 water
  types were lost, so ponds took whatever the pool held before: bright blue, pink, yellow or purple, different in
  each run (also with `TOTK_VK_SYNC=1`).
- **Metal's answer.** `botw_tiny_note` / `botw_tiny_commit` write such a pixel to memory as soon as the GPU finishes
  the submission, regardless of any budget.
- **Fix** (`games/totk/android/gfx/vk_res.c`, under `GAME_BOTW`): `wb_tiny()` recognises tiny targets (one level and
  layer, 1x1-block format, at most 256 bytes: Metal's bound); `vk_wb_record()` copies them out at the end of the
  submission that rendered them; a first loop in `wb_sweep_locked()` lands them as soon as the copy completes, ahead of
  the budgeted round, unless the CPU wrote the bytes since. `TOTK_BOTW_TINY_WB=0` restores the old path.
- **Result.** 57 of 57 water renders land before the game's read (49 the same frame, 8 one frame later); the pond is
  natural murky green and see-through in three separate runs, and pink again with the switch off. Title screen,
  outdoor view and the pause menu's blurred-scene background unchanged. Confirmed in the app by the owner.

## "Onion skinning" (a stale image blended over the scene): not reproduced, open

- **Where it shows.** Only around shrines in the owner's play: the Old Man's dialogue just outside Oman Au Shrine and
  the monk cutscene inside it. Near and far parts of the image (where depth of field applies) show a frozen older
  frame over the moving scene.
- **What was ruled out.** A full pass dump of the opening cutscene (Shrine of Resurrection, frame 2400): every texture
  read in that frame was rendered earlier in the same frame into the same Vulkan image; nothing came from an older
  frame. A "doubled" detail first flagged turned out to be the model's near and far legs, not a ghost.
- **Prime suspect.** The Vulkan backend lacks Metal's `rt_dup_of` / `BOTW_RT_DUP_*` handling: BotW's G-buffer passes
  attach the same image twice (c1 = c2, c3 = c4), which is undefined behaviour in Vulkan; Metal fixed a tile-memory bug
  caused by exactly this. A port is in [`botw-rt-dup.diff`](botw-rt-dup.diff) (switch `TOTK_BOTW_RT_DUP=0`; it also
  widens the pass dump's lines with image handles and serials). It made no visible difference in the opening cutscene,
  so it is not merged. Testing it needs a save taken just before the Old Man's dialogue or the monk cutscene.
- Metal's other BotW hooks: `BOTW_TEX_FOLLOW`, `BOTW_DEAD_KEEP`, the MetalFX hooks and `BOTW_T2B_NOTE` are
  Metal-specific; `BOTW_GPU_COPY_SUPERSEDES` already has an equivalent here (`vk_wb_gpu_wrote`).

## Also seen, not investigated

- With the prepared shader pack the outdoor scene looked darker/hazier than with shaders translated on the device.

## Earlier BotW findings (summarised; details in the commit messages)

- Crash in `nvnBufferMap` with a garbage pointer: sead spin locks use plain `ldxr`, so a job ran twice on fast cores;
  fixed by patching exclusive loads to acquire (`game_patches_botw.c`, `49b41e1`).
- Wrong start-screen background and water before the seed existed: skipped draws are not harmless in BotW (one-time
  draws), so draws wait for the on-device translator and for compiling pipelines (`961fea5`).
- The game never got past the tower-activation cutscene: movie pacing; off by default for BotW (`23801a5`).
- 17-28 s black screen at every start: the RomFS mod overlay was re-hashed each time; a verification record skips it
  (`d1be138`).
