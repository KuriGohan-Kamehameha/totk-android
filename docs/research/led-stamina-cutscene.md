# LED features (cutscene hand-over, stamina ring): research, 2026-11-16

Research notes, published as found. Paths starting with `$S/` were local working copies (Bifrost sources, pulled driver modules,
disassembly scripts) and are not part of this repository; file:line references without `$S` are to this repository or to Bifrost.
Bifrost source: [Pollux-MoonBench/Bifrost](https://github.com/Pollux-MoonBench/Bifrost) (HEAD `1baddf1` = 1.3.1). The Thor runs **1.2.0-beta-7** (`dumpsys package`: versionCode 13),
which is commit `25f939f`; files from that commit are extracted to `$S/scratch_b7/`. The external API, LEDService
override code and LedController are unchanged between `25f939f` and HEAD (git diff shows only the enum `;`).

## 1a. LEDs per stick, order, how they are written

- **Hardware: 4 RGB LEDs per stick ring**, one HEROIC HTR3212 12-channel driver per stick (left on i2c0, right on i2c12).
  Evidence: stock vendor module `/vendor_dlkm/lib/modules/htr3212l.ko` (pulled to `$S/thor_ko/`): `set_rgb_group_brightness(group, r, g, b)`
  writes PWM registers `0x0D + 3*group .. +2` and is called for groups 0,1,2,3. ROCKNIX's mainline Thor DTS
  (`qcs8550-ayn-thor.dts`, commit `1cd4b07c`) lists the same: `l1..l4`, `r1..r4`, 12 channels per controller.
- **Physical positions/order: UNCONFIRMED.** Neither the DTS nor the stock driver names positions. ROCKNIX maps
  `l1->group 4, l2->1, l3->2, l4->3`, which suggests an order but proves nothing. It does not matter on stock firmware (below).
- **Stock Android write path (what Bifrost uses):** `echo "<ch>-<R>:<G>:<B>:<br>" > /sys/class/sn3112{l,r}/led/brightness`
  (`LedController.kt:85-100`), sent as a shell command through the AYN system binder `PServerBinder` (`LedController.kt:39-42, 213-219`).
  Disassembly of the driver's `set_brightness` (htr3212l.ko 0xa50; htr3212r.ko identical logic):
  parses `ch-R:G:B` with `strsep`/`kstrtoull`, **acts only when ch == 1**, then writes the SAME RGB to groups 0..3
  and latches (`Led_DataUpdate`). Any other `ch` (Bifrost's "2-", called "bottom") only `printk`s. The 4th field is
  never parsed (Bifrost's own comment agrees: "The LED kernel ignores the 4th wire field").
  `enable` = whole chip on/off; `breath` = whole-ring breathing. **No per-LED control exists in the stock kernel interface.**
- Sysfs node is mode 0666, `u:object_r:sysfs:s0`; no `/dev/i2c-*` nodes exist; `/system/bin/i2cset` exists but has no device to talk to.

## 1b. External broadcast API (beta-7)

Contract: `external/ExternalApi.kt`; parsing `ExternalCommandValidator.kt`; handling `LEDService.kt:1300-1616` (beta-7 numbering).
- Receiver `com.moonbench.bifrost/.external.ExternalApiReceiver`, permission `com.moonbench.bifrost.permission.CONTROL_LEDS`,
  user switch "Allow third-party LED control" (`ExternalApiGate`), commands dropped while LEDService is not running.
- `ACTION_DISPLAY` extras: `apiVersion`=1, `effect` (enum name), `color` (int ARGB, default 0), `colorRight` (int, default `color`),
  `intensity` 0..255 with optional `intensityScale`, `speed`/`smoothness`/`sensitivity` 0..1, `priority` 0..100 (default 50),
  one of `durationMs` (1..600000) | `until` (`NEXT_COMMAND`|`EXPLICIT_CLEAR`) | `indefinite`, `requestId` (<=64 chars),
  PIPBOY hints `phaseSeconds`, `flickering`, `burstWallMs`.
- **Left and right separately: YES** (`color` = left stick, `colorRight` = right stick; STATIC writes left zones with
  `color`, right zones with `colorRight`, `StaticAnimation.kt:59-72`; PIPBOY uses `setLedColorDual`, `PipBoyAnimation.kt:255-270`).
  **Single LEDs: NO** - no extra for it, and the hardware path cannot do it anyway (1a). Intensity is one value for both sticks.
- Rate limit: per caller UID, 8 burst, 4/s sustained (`ExternalApiReceiver.kt:182-205`); `ACTION_PULSE` is not rate limited.
- Effects AMBIENT, AUDIO_REACTIVE, AMBIAURORA are refused from outside (`EffectRequiresProjection` -> result -3).
  Note: `BifrostLooks.Feed.result` treats -3 as "PIPBOY missing, switch to classic", so never send those effects.
- **PIPBOY** (`PipBoyAnimation.kt`): Fallout 4 phosphor breath (0.5..1.0 envelope), optional downward CRT flicker,
  burst dip at `burstWallMs`, `ACTION_PULSE` PULSE/STATIC dips. Left = `color`, right = `colorRight`, each lerped
  0.2 per frame. Unset (black) colour becomes green. Same caller + same effect = keep-alive fast path: colours and
  brightness updated in place, no restart (`LEDService.kt:1344-1373`).
- With the Purah Pad / Sheikah Slate plugin installed, the PIPBOY **LivePolicy** applies (`out/plugins/*/manifest.json`):
  `brightnessCapPercent 30`, `brightnessSource BOTTOM_SCREEN` (overrides the feed's `intensity`, `LEDService.kt:256-275`),
  `colorStabilize true`, `duckSeverity 0.5`: an incoming colour that is a per-channel subset of the last accepted one AND
  below 60 % of its sum is treated as a dropout and replaced by the held hue dimmed to `1 - 0.5*(1 - lumaRatio)`
  (`LEDService.kt:301-325`). Consequence: a single jump to black only reaches ~50 %; steps of >= 60 % of the previous
  colour pass unchanged, so a stepped fade (x0.7 per command) works. Hue changes (green -> red) are never "subsets", so they pass.

## 1c. Reactive modes, override end, resume, fades

- Screen/sound-reactive types: **AMBIENT** (screen colours; MediaProjection or accessibility capture),
  **AUDIO_REACTIVE** (audio, needs MediaProjection), **AMBIAURORA** (screen + audio, needs MediaProjection).
  Status-reactive: BATTERY_INDICATOR, CPU_TEMPERATURE. None can be requested through the API.
- Override end: `ACTION_CLEAR` from the owning package; lease watchdog reverts if no command for **1500 ms**
  (`EXTERNAL_LEASE_TIMEOUT_MS`, checked every 400 ms, `LEDService.kt:83-84, 369-374`) whatever the terminator; `durationMs` expiry.
- Resume after clear (`revertExternalOverride` / `applyRevertResolution`, `LEDService.kt:1492-1566`): restores the
  pre-override snapshot, then re-resolves: charging indicator first; else, **if app-profile switching is on, the preset
  mapped to the foreground app** (the game, which the plugins map to the "Purah Pad"/"Sheikah Slate" PIPBOY preset);
  else the user's last manual preset. So "Bifrost's own reactive mode" during a cutscene only happens if the game's
  app mapping (or, with switching off, the manual preset) is a reactive preset. Owner decision.
- Fades: on revert Bifrost does its own dip-to-black crossfade (170 ms out, swap, 230 ms in; `LEDService.kt:85-89, 1575-1616`).
  On a fresh override there is no crossfade (hard start; a crossfade in progress is cancelled, `:1399-1406`).
  The only caller-side brightness control is `intensity` (ignored when the plugin's LivePolicy is installed) and the colour itself.

## 1d. Per-LED options (not possible through the public API)

1. **Nothing in Bifrost can help on stock firmware**: the kernel driver itself only sets all four LEDs of a ring to one
   colour (1a). An upstream Bifrost change, a new effect or a `.bfplugin` preset field cannot address single LEDs.
2. Per-LED would need root: a replacement `htr3212{l,r}.ko` in vendor_dlkm (module signing/AVB status not checked),
   or the mainline driver (ROCKNIX/Linux only). Not least-invasive; not recommended.
3. Least invasive realisable alternative: whole-ring gauge on the LEFT stick via `color` while `colorRight` keeps the
   tablet colour, in the same PIPBOY override (keep-alive path, no restart): hue/brightness by stamina fraction,
   orange/red when low or exhausted, `ACTION_PULSE` dip on exhaustion. Needs no Bifrost change.

## 2. Stamina

### Breath of the Wild 1.6.0 (build `8E9978D50BDD20B4C8395A106C27FFDE`)
- Reader exists: `games/totk/duo/vendor/companions/native/modules/botw_hud.h:134-157` (`botw::stamina`).
  Chain: `PlayerInfo* = *(main + 0x2CA1140)` (vtable `main+0x2B7BE80`); `actor = *(info+0x60)` (vtable `main+0x2B7BEA8`);
  **current stamina = float `info+0xA4`, maximum = float `info+0xA8`** (accepted 1000..3000, cross-checked with `actor+0x186C`).
  Offsets: `botw_inventory_offsets.h` `Inventory160`. Only valid in the plain World UI signature (`ui.flags == 0x1E09000000`).
- Published every tick by the module (`01007EF00011E000.cpp:1213-1215`): `botw.ready.stamina.valid`,
  `botw.ready.stamina.depleted` (= current < max, i.e. "not full", NOT exhaustion), `botw.ready.stamina.percent`
  (floor(current/max*100), -1 invalid). Raw current/max are not published.
- Not read anywhere: temporary extra (yellow) stamina, the exhausted state. UNCONFIRMED where they live.
- Port side: `duo/duo_state.h` keeps only state/heal values; add the three stamina names to `DUO_STATE_NAMES` to get them.

### Tears of the Kingdom 1.4.3
- Already read: `PlayerStatus.MaxStamina` and `PlayerStatus.ExtraStamina` (GameData floats, MurmurHash3 of the name,
  `companion_snapshot.c:295-297`); APP.md:1481 / 1732: 1000 units per wheel; "Current stamina ... not in GameData".
- Static RE of the 1.4.3 main (sha `e84a90d5...`, decompressed to `$S/totk_main/main.bin`, tools in `$S/rtools/`):
  the per-frame stamina update is `main+0x31d1e8` (the infinite-stamina cheat site `main+0x31d398`, `fmadd s0,s0,s8,s2`,
  is inside it; source: SuperSenad/Ultra-Instinct-Tears `tools/lifter/patches.py`, [UIT 0344]).
  On its object (`this`):
  - `+0x74` float **current stamina**, clamped at 0; overflow below 0 is taken from `+0x78`
  - `+0x78` float **extra (temporary) stamina**
  - `+0x5c/+0x60` copies after recovery; `+0x58` bit flags; `+0x54` rate.
  - At the end it **writes `+0x74` and `+0x78` into GameData floats by the hashes stored at `+0x64` and `+0x6c`**, and a bool
    (`+0x58` bit 0) by the hash at `+0x70` (calls `main+0x31d824` / `main+0x66de24` with `x0 = *(main+0x3ad66b0)`,
    i.e. GDM_SLOT, through GOT `main+0x395dff0`, the same GOT entry the companion's `ACTION_GOT` table checks).
  - The object is embedded at **`+0x1390` of its owner** (`main+0x318b10..0x318b2c`, `main+0x89f37c..0x89f3cc`; 14 sites use
    `+0x1390`; `main+0x1bd1160` adds stamina and then sets bit 23 of `*(main+0x3a82e70)+0x658`, the PLAYER_STATE flags).
  - UNCONFIRMED: the owner's class (no vtable/RTTI found for the callers) and the GameData flag names behind the hashes
    (`PlayerStatus.Stamina`, `PlayerStatus.CurrentStamina` etc. do not appear as immediates; the hashes are loaded at run time).
- Safe route: the GameData float that the game itself writes each frame (same table, same reader as Life). Identify its
  hash once at run time (a one-off log of the hashes at the stamina object `+0x64/+0x6c/+0x70`, or a diff of the GameData
  Float table while sprinting), then read it with `gd_float`-style lookup by hash. No actor pointer chasing needed.

### "Wheel visible / in use"
- Neither game's readers expose the UI wheel visibility. TotK strings exist (`IsDisplayExStaminaGauge`,
  `OneShotSetDisplayStaminaGauge`, `SeqExecuteSetStaminaGaugeVisibleOffTemporary`) but are unmapped.
- Derive it: shown while current < max (+ extra) and for a short hold after reaching full; exhausted = latched from 0 until
  full again (game behaviour, unconfirmed in code; TotK `+0x58` bit 0 / the `+0x70` bool is a candidate, unverified).

## 3. Cutscene / event state

| Source | Game | Covers | Evidence |
|---|---|---|---|
| `*(main+0x3ada558)+0x58 != 0` (event flow running; unreadable counts as event) | TotK 1.4.3 | any event flow: cutscenes, dialogue, item get, etc. Not split | `companion_snapshot.c:152, 576-580` (`CompanionView.event`), used by `companion_actions.c:205` |
| UIManager flags bit `0x800` at `*(main+0x2CBDB80)+0x64C30` ("event owns player/camera"); `dialogue` = HUD bit `0x1000000000` and Message/MessageSp/MessageGet screen shown; `cutscene = event && !dialogue` | BotW 1.6.0 | in-engine events, split into dialogue vs cutscene | `botw_states.h:10-16, 112-146`; published `botw.state.event/.cutscene/.dialogue` (`:151-152`) and the latch `botw.state.dim` (on after 200 ms, off after 400 ms) + `dim_kind` 1 dialogue / 2 cutscene (`:164-195`) |
| `movie_host_active()` (count of started host VP9 decoders) | both (`MOVIE_PACE_DEFAULT` has a GAME_BOTW branch; BotW romfs has `Movie/Demo*.webm`) | prerendered movies only | `runtime/movie_host.c:92-95`; already used by `hle_nvn.c:3568-3593` |
| fps60 scripted-camera stamp (`game_patches_v143.c:205-275`) | TotK, only with the fps60 hardening on | scripted camera animations only, "not a universal cinematic predicate" | the file's own comment |

Recommended: TotK `event || movie_host_active()`; BotW `botw.state.dim` (or `event`) `|| movie_host_active()`,
with a debounce like BotW's 200/400 ms latch for TotK too. Whether the TotK event pointer is also set during prerendered
movies is unconfirmed (OR-ing the movie flag covers it).

### The second screen's own trigger (owner's direction: reuse it)

- **BotW (Eden Duo module + manifest): `botw.state.dim`.** Every page of
  `duo/vendor/companions/packages/BreathOfTheWild/dualscreen/manifest.json` has a full-screen `ov_event_dim` rect
  (`#90000000`, `input_block`, `need_bind: botw.state.dim`, 180 ms fade). The module's `StateLatch`
  (`botw_states.h:164-195`) sets `dim` after UI flag bit `0x800` has held 200 ms and clears it after 400 ms clear;
  `dim_kind` 1 = dialogue, 2 = cutscene, latched at onset. Covers in-engine cutscenes, dialogue, warps and memory
  playback (flag signature `0x0E0902382C`, `botw_event_hold.h:6`); not title, loading or menus. During the event the
  page is held under the dim (`botw_event_hold.h`, `01007EF00011E000.cpp:334-339, 373-382`). Updated every 100 ms
  sample, reset on title/loading (`:1813`). Bridge access: not in `CompanionState` yet; add `"botw.state.dim"` (and
  `dim_kind`) to `DUO_STATE_NAMES` in `duo/duo_state.h` and map to a new `companion_state` field.
- **TotK companion: no dim/disable on events.** The page view only switches for title/loading/unsupported
  (`Motion.view`, `Motion.java:24-28`). The event appears only as the item-action gate: `companion_actions.c`
  `state_reason()` (`:197-208`) returns `"event"` when `CompanionView.event` (`*(main+0x3ada558)+0x58 != 0`, unreadable
  = event) is set, shown as "In a cutscene" (`CompanionView.java:803`). Precedence: not-ready / noPlayer / loading /
  saving come first, then event, then player-flag loading, then menu. After any blocked state the reason stays
  `"loading"` until the world has been OK for 1000 ms and 20 frames (`:395-403`). Computed on the frame-end tick;
  `"stale"` after 1000 ms without ticks. Covers cutscenes, dialogue and any event flow (not split).
  Bridge access: `companion_state()` (`companion_snapshot.c:729-747`) already calls
  `companion_action_state(&ok, &reason, &pause_menu, &aiming)` and drops `reason`; one line
  (`out->event = !strcmp(reason, "event")`) puts it into `CompanionState`.
- Neither covers prerendered movies explicitly in TotK (unconfirmed whether the event pointer is set during them);
  OR `movie_host_active()` in if needed.

## Implementation path

- `companion_state.h`: add `stamina`, `stamina_max` (incl. extra wheels or a separate `stamina_extra`), `stamina_valid`,
  `event` (and optionally `event_kind`); extend `companion_state_words` / `COMPANION_STATE_FIELDS` / `CompanionState.java`.
  TotK fills from the snapshot (event already in `CompanionView`; stamina after the hash is identified); BotW from
  `duo_state.h` by adding `botw.ready.stamina.{valid,percent}` and `botw.state.dim`/`dim_kind`; movie flag from `movie_host_active()` in both.
- Cutscene: on event (debounced) send colour steps x0.7 every ~330 ms (3 displays/s budget) down to ~10 %, then `ACTION_CLEAR`;
  Bifrost crossfades to its own resolution. On event end: first DISPLAY with a dim colour, then step up (rising steps
  always pass colorStabilize). Bifrost does not crossfade on takeover.
- Stamina: left-stick whole-ring gauge through `color` in the existing PIPBOY keep-alive (no restart, no extra rate
  budget beyond the 3/s already used); `colorRight` keeps the tablet look.

## Owner decisions needed
1. Per-LED radial pattern is impossible on the stock Thor kernel (not a Bifrost limitation): accept a whole-ring gauge
   (hue/brightness + pulse), or pursue a root/kernel route (not recommended). An upstream Bifrost PR would not help.
2. What "Bifrost's reactive mode" should be during cutscenes: the game's app-profile mapping currently points at the
   plugin's PIPBOY preset; for screen-reactive the owner must map the game to an AMBIENT (or other) preset or turn app-profile switching off.
3. TotK current stamina needs one on-device run to learn the GameData hash (or more static RE of the owner object).
