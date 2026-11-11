# How the Android port works

TotK Native is not an emulator. The game's own ARM64 code is rewritten once, on your device, so that it runs directly
on the phone's CPU; the Switch system services are answered by a small host, and the game's NVN graphics are drawn
through Vulkan on a bundled Mesa Turnip driver. One APK serves every device and contains no game files: everything
game-specific is built on the device from the player's own dump.

## The pieces

| Part | Where | What it does |
|---|---|---|
| Code rewriter | `games/totk/third_party/d2r` (Python reference), `games/totk/android/setup/` (the on-device C port) | Rewrites the few instructions Android does not allow a user process (the platform register x18, the thread-pointer registers, `svc` system calls, counter reads) into short jumps to host stubs. The C port in the setup library produces output byte-identical to the reference rewriter. |
| Loader and Horizon host | `games/totk/runtime/` | Maps and relocates the rewritten modules, runs the game's threads and answers the Switch OS services: filesystem (the RomFS image and its index), time, audio (AAudio), input, saves. |
| Graphics | `games/totk/third_party/d2r_gfx/gfx/nvn`, `games/totk/android/gfx/` | The NVN API layer, drawn with Vulkan. Game shaders are translated to SPIR-V by Ryujinx's shader translator (`games/totk/tools/totk_xlate`, `vendor/ryujinx`). |
| Driver | `games/totk/android/turnip/` | The pinned Mesa Turnip build (Vulkan for Adreno) packed in the APK. |
| App | `games/totk/android/java/com/aure/totk/` | The setup flow, the pre-game menu (`setup/`), the game activity and the second-screen companion (`companion/`). |

The Linux / Android differences of the runtime (thread slots through `tpidr_el0`, no red zone, guest memory as shared
anonymous memory below 2^39, Linux counterparts of the Darwin services) are described in
[games/totk/android/README.md](../games/totk/android/README.md#how-the-linux--android-differences-are-handled); the
Mac design it shares, in [how-it-works.md](how-it-works.md).

## Why Adreno only

The game draws through the bundled Turnip driver, which supports Adreno GPUs only. The devices' own Qualcomm drivers
lack the memory-sharing extensions the game needs (`VK_EXT_external_memory_host` / dma-buf), so they cannot run it.
`TOTK_VK_DRIVER=system` exists for testing ([developer-settings.md](developer-settings.md)), but no current Qualcomm
system driver passes the game's startup check.

## Setup on the device

Setup reads the player's files (decrypted RomFS + ExeFS, or base XCI/NSP + 1.4.3 update NSP + `prod.keys`, merged on
the device) and checks every input against the pinned 1.4.3 hashes (`games/totk/assets/expected-v1.4.3.json`) before
anything is written. It then, each step with its own progress:

1. rewrites the game code for Android (the C port of the rewriter) and verifies it;
2. copies and verifies the RomFS (reading, hashing and writing overlap, so the copy runs at the storage's speed);
3. builds the data index;
4. writes the shared fonts from the bundled Noto Sans;
5. translates the Vulkan **shader seed** from the shared [shader list](shader-list.md) and the player's own game data;
6. draws the **second-screen art** from the player's RomFS (optional; [companion.md](companion.md)).

Setup can be cancelled and resumed from the last finished step. On the container route, the keys are only held in a
private temporary file (app-only, excluded from backups) while a step needs them, and deleted afterwards. Details and
measured times: [android-install.md](android-install.md).

## Device profiles

The first start picks the tested profile of the chip from Android's exact `SOC_MODEL` (the whole part number, never
`board.platform`, the manufacturer or a substring), in `Settings.profile()`; each Dragonwing part shares the profile of
its Snapdragon counterpart. The runtime itself never switches behaviour by device; the only device-specific runtime
default is the Adreno 722 `TU_DEBUG=sysmem` correctness guard. Profiles and options: [README](../README.md#settings);
the settings each one writes: [developer-settings.md](developer-settings.md).

## Further reading

- [developer-settings.md](developer-settings.md): `settings.env`, `totk.env` and the developer switches.
- [building.md](building.md): building the APK and running the tests.
- [companion.md](companion.md): the dual-screen companion.
- [frame-interpolation.md](frame-interpolation.md): frame generation.
- [shader-list.md](shader-list.md): the shared shader list.
- [audio-troubleshooting.md](audio-troubleshooting.md): streamed-audio diagnosis.
- [v1.4.3-port.md](v1.4.3-port.md), [v1.2.1-port.md](v1.2.1-port.md): how each game version was ported.
- [benchmarks/](benchmarks/): the performance baseline and ledger.
- [games/totk/android/APP.md](../games/totk/android/APP.md), [VULKAN.md](../games/totk/android/VULKAN.md),
  [DEBUGGING.md](../games/totk/android/DEBUGGING.md): the full developer notes.
- [mods.md](mods.md): RomFS mods, including a TKMM merge on Android.
