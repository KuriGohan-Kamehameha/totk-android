# The second-screen waist: one interface for both companions

This is an analysis and design document; it changes no behaviour. It maps every endpoint of the two second-screen
companions and records, endpoint by endpoint, how each would be handled through a common **waist**. The waist is one
narrow, MIT-licensed interface owned by this repository. Both games feed it, every consumer reads it, and the per-game
code stays below it.

- **Tears of the Kingdom 1.4.3** uses its own reader and Canvas UI (MIT).
- **Breath of the Wild 1.6.0** uses the vendored Eden Duo host, module and package (GPL-3.0). Only the BotW APK
  carries them.

The waist already exists in part: `runtime/companion_state.h` holds 8 state words. Section 3 lists what is in it today
and what is proposed. `tests/test_companion_waist.py` keeps that table and the header from drifting apart.

Path abbreviations used throughout:

| Abbreviation | Path |
|---|---|
| `R/` | `games/totk/runtime/` |
| `D/` | `games/totk/duo/` |
| `M/` | `games/totk/duo/vendor/companions/native/modules/` (`MOD` = `M/01007EF00011E000.cpp`) |
| `P/` | `games/totk/duo/vendor/companions/packages/BreathOfTheWild/dualscreen/` |
| `J/` | `games/totk/android/java/com/aure/totk/companion/` |
| `A/` | `games/totk/android/java/com/aure/totk/` |
| `NVN` | `games/totk/third_party/d2r_gfx/gfx/nvn/hle_nvn.c` |

## Headline

- **62 endpoints** are in the ledger (section 3). Each one merges the per-game rows of section 2 that serve the same
  purpose.
  - **46 are common:** 40 that both games have, plus 6 game-agnostic runtime services both can use. 14 of them already
    cross the waist or a shared Java class (state words, shell, LED bridge, display).
  - **16 are single-game:** 5 TotK-only and 11 BotW-only.
  - **19 rows should stay game-specific,** and each says why. That includes 7 common ones whose semantics or mechanism
    differ: gear, effects, abilities, inventory grids, map marks, game calls and the art pipeline.
- **Top five by value for effort:**
  1. **L13 `dim`** (S), with L11 `event` (S) under it. One latched "event or cutscene" word, filled from BotW's
     existing `botw.state.dim` and TotK's existing event pointer. It unlocks the LED cutscene hand-off and a TotK
     page dim.
  2. **L09 stamina on BotW** (S). The two published names are mapped through `duo_state.h`; the raw values are a
     one-line local change. TotK's half is M and needs one device run.
  3. **L10 `stamina_wheel`** (S once L09 lands). It is derived once in common code, so both games show the same
     "wheel visible" rule.
  4. **L08 TotK `LOADING` status** (S). The loading bit is already read (`R/companion_snapshot.c:581-584`) but never
     reaches the state, so a loading screen reports as the title.
  5. **L41 action gate reason** (S). It promotes TotK's reason vocabulary (`menu`, `event`, `loading`, `saving`,
     `stale`) to the waist, so any screen and any consumer can grey out controls the same way.

## 1. What the waist is

```
 consumers   Java CompanionState ─ shell (PresentationController) ─ Bifrost LED bridge ─ (future overlay, common pages)
                                 ▲ one getter: CompanionNative.state(long[])  (R/companion_jni.c:167-181)
 ══ WAIST (MIT, this repo) ════════════════════════════════════════════════════════════════════════════════════
   W-S  state words      R/companion_state.h          numbers only, fixed order, age attached, -1 = unknown
   W-A  action verbs     (proposed) companion_verbs.h  request(verb, item token) → seq; result(seq) → state+reason
   W-R  runtime services (proposed) companion_host.h   read, frame tick, clock, log, save-busy, romfs, place-thread
 ══════════════════════════════════════════════════════════════════════════════════════════════════════════════
 adapters    TotK: R/companion_snapshot.c, R/companion_actions.c   BotW: D/duo_state.h (MIT name map) ← duo_host.cpp
 game side   GameData by hash, actor fields, game calls             Eden Duo module: UIManager/actor reads, mailbox
```

Four rules decide what crosses the waist:

1. **Something crosses only when a consumer other than the game's own page needs it:** the shell, the LEDs, an
   overlay or a shared page. Page content that only one renderer draws stays below. That includes inventory grids,
   quest lists and map markers.
2. **Fields are numbers with one unit and one "unknown" value (-1), plus the sample's age.** Derived signals
   (latches, holds) are computed once, in common code above the adapters, so two games cannot drift apart.
3. **Nothing GPL is needed to build or read the waist.**
   - The headers are MIT and live in `R/` or as port-owned files in `D/`, as `duo_port.h`, `duo_state.h` and
     `companion_state.h` already do.
   - The BotW adapter maps *published value names* only (`D/duo_state.h:15-21`). It contains no Eden code, and the
     TotK build never compiles `D/` (`games.toml` `game_patches`, `gpl_companion = false`, `audit_apk.py`).
4. **A new word is appended after the existing ones.** Word indices never move, so the Java constants
   (`J/CompanionState.java:13-14`) only grow.

## 2. Endpoint inventory

### 2a. TotK (our reader and Canvas UI)

Polling rates for this section:

- **Snapshot JSON:** live every 125 ms, full every 1000 ms (`J/SnapshotPoller.java:43,61`).
- **State getter:** the shell every 250 ms, Bifrost every 125 ms.
- **Item actions:** run on the game's frame-end thread while a request is pending (`NVN:721`, armed by
  `runtime_companion_pending`, `R/world_survey.c:17-18`).

| ID | Endpoint | Evidence | Mechanism | Frequency |
|---|---|---|---|---|
| T-R01 | status: starting / unsupported / title / ready | `R/companion_snapshot.c:637-671` (gate), `:729-747` (companion_state) | SHA-256 of 8 instruction words, then `GameDataMgr *(main+0x3ad66b0)`, a `MaxLife` sanity check, and a scene-resident actor named `Player` | state getter |
| T-R02 | pause menu open | `R/companion_snapshot.c:149,575-576`; `R/companion_actions.c:392-421,275-281` | byte `PouchMgr+0x1fcc0`, refreshed on the frame-end tick | frame end while watched (3 s watch) |
| T-R03 | life, max life | `R/companion_snapshot.c:290-294,737-738` | GameData int `PlayerStatus.Life` / `MaxLife` (MurmurHash3 of the name) | state, snapshot |
| T-R04 | extra / break / temporary life | `R/companion_snapshot.c:290-293` | GameData ints | snapshot |
| T-R05 | stamina **capacity** (max, extra) | `R/companion_snapshot.c:295-297` | GameData floats `PlayerStatus.MaxStamina` / `ExtraStamina`; current stamina is **not read** | snapshot |
| T-R06 | battery capacity, current energy | `R/companion_snapshot.c:295-297`; `R/companion_actions.c:97-106,410-416` | GameData floats; the battery component of the player actor through game calls | snapshot / frame end |
| T-R07 | rupees, poes | `R/companion_snapshot.c:292-293` | GameData ints | snapshot |
| T-R08 | special power (ability) | `R/companion_snapshot.c:299-300` | GameData enum | snapshot |
| T-R09 | cook buff | `R/companion_snapshot.c:301-305` | GameData ints and float | snapshot |
| T-R10 | position, heading, map layer | `R/companion_snapshot.c:306-329` | actor position and rotation fields (fallback `World_PlayerPos`); `MapData.CurrentLayer` corrected by height | snapshot |
| T-R11 | pouch, 9 categories, with the actions per item | `R/companion_snapshot.c:335-345,363-446` | GameData arrays, 2 partitions; partition at `*(main+0x3a81678)+0x188` | full snapshot |
| T-R12 | equipped gear | `R/companion_snapshot.c:359-362,427-428` | `EquipIndex` / `Armor.EquipIndexes` | full snapshot |
| T-R13 | sages | `R/companion_snapshot.c:459-479` | GameData bools, enums, ints and floats | full snapshot |
| T-R14 | map pins and stamps | `R/companion_snapshot.c:481-516` | GameData vec and enum arrays | full snapshot |
| T-R15 | event flow (cutscene, dialogue, any event) | `R/companion_snapshot.c:152,577`; used at `R/companion_actions.c:205` | `*(main+0x3ada558)+0x58 != 0`; unreadable counts as an event | frame end |
| T-R16 | loading pause | `R/companion_snapshot.c:581-584` | bit 1 of `+0x78` in scene component 29 | frame end |
| T-R17 | player flags (load, weapon change) | `R/companion_snapshot.c:151`; `R/companion_actions.c:207,405` | `*(main+0x3a82e70)+0x658` | frame end |
| T-R18 | aiming | `R/companion_actions.c:78,406` | game call `main+0x711000` | frame end |
| T-R19 | pouch identity and generation | `R/companion_snapshot.c:518-553`; `R/companion_actions.c:283-289` | FNV over the partition's columns | full snapshot |
| T-R20 | Zonai hold unlocked | `R/companion_snapshot.c:588-589` | GameData bool hash `0x1c96382f` | frame end |
| T-W01 | item actions: equip, unequip, eat, drink, use, takeOut, fuseArrow | `R/companion.h:53-66`; `R/companion_actions.c:69-93,297-345,199-210,392-403` | the game's own pouch functions, each prologue SHA-checked, one request at a time, outcome read back | on tap; frame end |
| T-W02 | battery energy writes | `R/companion_jni.c:97-102`; `R/companion_actions.c:442+` | `CompanionEnv.wr`, only the fields the pause menu's energy branch writes | inside an action |
| T-W03 | button shortcut (Plus, Minus) | `R/companion_jni.c:183-216`; `J/CompanionView.java:431-432` | `runtime_input_key_synthetic`: press, then release after 120 ms; status must be ready | on tap |
| T-S01 | guest read | `R/companion_jni.c:88`; `R/companion.h:12-13` | `host_safe_read` (fault-free) | every read |
| T-S02 | game call | `R/companion_jni.c:93-96` | direct native call on the frame-end thread | inside actions |
| T-S03 | frame tick | `NVN:721`; `R/companion_jni.c:105-110` | frame-end hook while pending | per frame (when pending) |
| T-S04 | save busy | `R/companion_jni.c:107`; `R/companion_actions.c:204` | `runtime_fs_save_busy` | frame end |
| T-S05 | clock | `R/companion_jni.c:103` | `runtime_now_ns` | — |
| T-S06 | log | `R/companion_jni.c:69-86,134-146`; `R/companion_actions.c:291-296` | stderr, which goes to totk.log | on events |
| T-S07 | settings | `R/companion_jni.c:55-68,138` | `TOTK_COMPANION`, `_COVER`, `_LOG` | at start |
| T-S08 | art, text, font | `J/AssetStore.java:36,90-155`; `games/totk/android/setup/companion/stage_companion.c` | generated **at setup** from the player's RomFS into `<game>/companion/`; no RomFS reads at run time | — |
| T-S09 | display and touch | `J/PresentationController.java`; `J/CompanionView.java:920-962` | common shell; gestures in Java | — |
| T-S10 | game paused (Android) | `R/companion_jni.c:115,200` | `runtime_android_game_paused` | per request |
| T-X01 | snapshot JSON for the Canvas | `R/companion_jni.c:148-159`; `J/Snapshot.java` | JSON schema 1 | 125 / 1000 ms |
| T-X02 | state words | `R/companion_jni.c:167-181` | `companion_state()`, run on the caller's thread | 125–250 ms |
| T-X03 | page | `R/companion_jni.c:162-166` | Java writes it, the native getter reports it | on change |

### 2b. BotW (vendored Eden Duo module, port host)

The module samples on every host tick. A tick comes once per presented frame or every 16 ms, whichever is first, and
drops to 4 Hz after 2 s without frames (`D/duo_host.cpp:357-427`, `:378-381`). The port is read-only: the shim refuses
guest writes and no mailbox is given (`D/duo_host.cpp:17,169,194-200`). Most readers sample twice and require the UI
signature to stay the same between the two samples.

| ID | Endpoint | Evidence | Mechanism | Published as |
|---|---|---|---|---|
| B-R01 | valid / title / loading | `M/botw_states.h:112-146`; republished at `MOD:1206-1208` | UIManager flags `*(main+0x2CBDB80)+0x64C30` and ScreenMgr slots; sampled twice | `botw.state.{valid,title,loading}` → `D/duo_state.h:27-32` |
| B-R02 | menu open | `M/botw_states.h:95-110,162` | `menu_screen` is non-empty (pause menu, AppHome tabs) | `botw.state.menu_open` → `D/duo_state.h:34` |
| B-R03 | Sheikah Slate owned | `M/botw_states.h:17,139` | GameData bool `IsGet_Obj_DRStone_Get` | `botw.state.slate_obtained` → `D/duo_state.h:35` |
| B-R04 | life, max life | `M/botw_heal_advisor.h:26-45,279-290` | actor `+0x848` (yellow hearts included), PlayerInfo `+0xA0` | `botw.ready.heal.{valid,life,max}` → `D/duo_state.h:36-38` |
| B-R05 | stamina | `M/botw_hud.h:134-157`; `MOD:1213-1215` | PlayerInfo `*(main+0x2CA1140)`: `+0xA4` current, `+0xA8` max (1000..3000), checked against actor `+0x186C`; World UI only | `botw.ready.stamina.{valid,depleted,percent}`. **Percent only; raw values are not published** |
| B-R06 | event, cutscene, dialogue | `M/botw_states.h:122-125,151-152` | UI flag bit `0x800`; dialogue = HUD bit plus a Message screen | `botw.state.{event,cutscene,dialogue,event_name}` |
| B-R07 | dim latch, page | `M/botw_states.h:164-195` | on after 200 ms of event, off after 400 ms clear; `dim_kind` 1 = dialogue, 2 = cutscene | `botw.state.{dim,dim_kind,page}` |
| B-R08 | rupees | `M/botw_hud.h:27-71` | GameData int hash `0x23149BF8` | `botw.ready.rupees(.text,.valid)` |
| B-R09 | time of day | `M/botw_hud.h:92-133`; `MOD:1209-1210` | GameData float hash `0x9C79A188` | `botw.ready.clock.{valid,minutes,text}` |
| B-R10 | temperature, weather forecast | `M/botw_environment.h:12-49` | world manager objects | `botw.ready.temperature.*`, `botw.ready.weather.0..4` |
| B-R11 | position, heading | `M/botw_map.h:127-150` | PlayerInfo `+0x88`; actor matrix | `botw.map.{x,y,heading,…}` |
| B-R12 | area name | `M/botw_map_area.h:6-37` | map manager player marker | `botw.ready.area(.key,.valid)` |
| B-R13 | map type, shrine | `M/botw_states.h:41-51,90-94,153-158` | FixedSafeString at main offsets; GameData bools | `botw.state.map_*`, `botw.state.shrine.*` |
| B-R14 | pouch and inventory | `M/botw_pouch.h:29-94` | PouchMgr `main+0x2CA6D50` `+0x68` list | `botw.item.N`, `botw.inventory.*` |
| B-R15 | equipped gear and durability | `MOD:1701-1718,1733-1736` | pouch items flagged equipped | `botw.ready.{weapon,bow,shield,arrow}.*` |
| B-R16 | rune | `M/botw_hud.h:72-91` | rune actor `+0x240` | `botw.ready.rune.*` |
| B-R17 | noise, sensor | `M/botw_hud.h:18-26`; `M/botw_sensor.h:23-82` | UIManager / map manager fields | `botw.ready.noise.*`, `.sensor.*` |
| B-R18 | food and elixir effects | `M/botw_effects.h:10-55` | Player `+0x1FF0` / `+0x1ED8` | `botw.ready.effect.N.*` |
| B-R19 | quests | `M/botw_quest_offsets.h:14`; `MOD:1234-1249,1362-1430` | quest manager `main+0x2CD1700` | `botw.quest.*` |
| B-R20 | trackers (Koroks, shrines, towers) | `M/botw_tracker_flags.h:5-30` | bounded GameData flag banks | `botw.tracker.*` |
| B-R21 | map markers, tiles | `M/botw_map*.h`; tiles at `M/botw_map.h:162` | map manager; `romfs:/UI/MapTex/...` | `botw.map.*` (about 112 names) |
| B-W01 | action bridge: inventory equip, consume, hold, sort; warp; map pins; quests; recipes; quick picker; HUD suppression | `M/01007EF00011E000_actions.h`; `M/botw_bridge.h:91-160,347-640`; `P/load-plans/8E99…json` | guest mailbox (`load_u32` / `store_u32` extensions) served by a guest helper that the load plan's 2 `bl` patches install. **Off in the port** (no write, no mailbox, no load plan: `R/duo_port.c:14`) | `on_action` names |
| B-S01 | guest read | `D/duo_port.h:25-26`; `R/duo_port.c:49`; `D/duo_host.cpp:161-167` | `DuoPort.safe_read` = `host_safe_read` | every read |
| B-S02 | RomFS / AOC read | `D/duo_port.h:27-32`; `R/duo_port.c:142`; `D/port_sources.cpp:20-44` | stateless relative-path readers | MSBT, font, layout and map tiles at run time |
| B-S03 | log | `D/duo_port.h:33-34`; `R/duo_port.c:50` | levels 1–4 | — |
| B-S04 | frame tick | `R/game_patches_botw.c:159`; `R/duo_port.c:160-165`; `D/duo_port.h:56-57` | `runtime_duo_frame` posts a wake per presented frame | per frame |
| B-S05 | thread placement | `D/duo_port.h:42-44`; `R/duo_port.c:89-106` | spare cores, nice 4 | per thread |
| B-S06 | package and data folders | `D/duo_port.h:35-38,50-51`; `R/duo_port.c:134-152`; `J/DuoSurfaceView.java:88-130` | APK `assets/duo/` unpacked to `files/duo/`, `.ready` written last | at start |
| B-S07 | font, text, layout | `M/botw_msbt_reflow.h:494-512`; `P/manifest.json` | `romfs:` sources (`Font_US.sbfarc`, `Msg_USen`) | at run time |
| B-S08 | surface | `D/duo_host.cpp:510-588` | `nativeSurface` → presenter thread, CPU blit | per new page frame |
| B-S09 | touch | `D/duo_host.cpp:590-630` | `nativeTouch` → `AuxRouting`; **off unless `TOTK_DUO_TOUCH=1`** | per MotionEvent |
| B-S10 | settings | `R/duo_port.c:125-128,92,111,144` | `TOTK_DUO`, `TOTK_COMPANION`, `TOTK_DUO_AFFINITY`, `_LIB`, `_DIR` | at start |
| B-S11 | build identity | `R/duo_port.c:137-141`; `D/port_module.cpp:84-117` | title id plus NSO build id, checked by the manifest and the module | at start |
| B-S12 | host queries | `M/botw_bridge.h:97-145` | `get_text("__page")`, `get_i64("__aux_present")`, `__mailbox_epoch` (Eden runtime) | per tick |
| B-X01 | common state | `D/port_module.cpp:44-59`; `D/duo_host.cpp:431-451`; `R/duo_port.c:53-81` | `LedTap` filters published names, `LedCommit` → `DuoPort.state` → seqlock | per complete sample |
| B-X02 | published value table | `D/duo_host.cpp:173-181,258-285` | about 439 literal names (495 manifest patterns) | per tick, internal |
| B-X03 | page pixels | `D/duo_host.cpp:486-560` | ARGB frame → Surface | per new frame |

### 2c. Common today (shared code, both games)

- **State and its consumers:** `R/companion_state.h` (8 words), `J/CompanionState.java`, `J/CompanionScreen.java`,
  `J/PresentationController.java` (display choice, cover, focus reclaim, touch pulse), `A/BifrostState.java:21-24`,
  and `A/BifrostLooks.java:98-107` (`fromState` uses `inGame`, `menuOpen`, `tabletOwned`, `life` and `maxLife`).
- **Declared per-game differences:** `games/games.toml` (`companion`, `gpl_companion`, `tablet_can_be_missing`,
  `bifrost_looks`; `[branches]` for `companion_jni.c`, `companion_snapshot.c` and `duo_port.c`), enforced by
  `tests/test_game_variables.py`.

### 2d. Triggers that dim, disable or replace the screen

| Trigger | TotK | BotW | Waist today |
|---|---|---|---|
| Not in a save (title, starting, unsupported) | full-panel views (`J/Motion.java:24-27`) | `page_binds` on `botw.state.page` 1/2 → mainmenu / loading pages (`P/manifest.json`) | `status` |
| Loading screen | **reported as title** (the loading bit T-R16 is not used for the state) | `botw.state.loading` → loading page | `status` LOADING (BotW only) |
| Event, cutscene or dialogue | **none on screen**; actions show "In a cutscene" (`R/companion_actions.c:205`) | `ov_event_dim` full-screen rect (`#90000000`, `input_block`) on `botw.state.dim`, on every page | **missing** (L11–L13) |
| Pause menu open | actions disabled, reason `menu` | about 42 manifest references to `menu_open` | `menu_open` |
| Stale sample | last good page held as "loading" for up to 30 s (`J/SnapshotPoller.java:52-54`); actions report `stale` after 1 s without ticks | host 4 Hz idle clock | `age_ms`, rejected above 2000 ms (`J/CompanionState.java:16,37`) |
| Companion off, no panel, activity stopped | `J/PresentationController.java:65,366,411-413` | same | shell (common) |
| Idle timeout | none; `KEEP_SCREEN_ON` | none | — |

## 3. Commonality map and ledger

### 3a. Waist fields (state words)

This table is machine-checked by `tests/test_companion_waist.py`.

- Rows with **landed** status must be exactly the fields of `CompanionState` and the words of
  `companion_state_words`, in this order, matching the Java index constants.
- Rows with **proposed** status must not yet be in the header.

When a field lands, change its status here in the same commit.

<!-- waist-fields:begin -->
| Word | Field | Unit / values | Unknown | Status | Ledger |
|---|---|---|---|---|---|
| 0 | status | `COMPANION_STATE_*`: 0 none, 1 starting, 2 unsupported, 3 title, 4 loading, 5 ready | 0 | landed | L01 |
| 1 | in_game | 0/1: a save is loaded, world running or in its own menu | 0 | landed | L02 |
| 2 | menu_open | 0/1: the game's pause menu (Purah Pad or Sheikah Slate) is up | 0 | landed | L03 |
| 3 | tablet_owned | 0/1: 0 only where the tablet can be missing and is | 1 | landed | L04 |
| 4 | life | quarter hearts, including temporary (yellow) hearts | -1 | landed | L05 |
| 5 | max_life | quarter hearts, 4..400 | -1 | landed | L05 |
| 6 | page | the screen's own page number | -1 | landed | L06 |
| 7 | age_ms | milliseconds since the sample; consumers drop it above 2000 | — | landed | L07 |
| 8 | stamina | game units, 1000 = one wheel; current, including temporary extra | -1 | proposed | L09 |
| 9 | stamina_max | game units, 1000 = one wheel; capacity including extra wheels | -1 | proposed | L09 |
| 10 | stamina_wheel | 0 hidden, 1 shown (derived above the adapters: below max, held 1500 ms after refilling) | -1 | proposed | L10 |
| 11 | event | 0/1: an event owns the player or camera (cutscene, dialogue, event flow, prerendered movie), raw | -1 | proposed | L11 |
| 12 | event_kind | 0 unsplit, 1 dialogue, 2 cutscene, 3 movie | -1 | proposed | L12 |
| 13 | dim | 0/1: the screen-dim / cutscene signal; `event` latched on after 200 ms, off after 400 ms | -1 | proposed | L13 |
<!-- waist-fields:end -->

**Validity and age.** Every word is from the same sample. `age_ms` is the age of that sample. A word that its source
does not vouch for is -1, never a guess. That is the rule both producers already follow
(`R/companion_snapshot.c:729-747`, `D/duo_state.h:23-39`). The in-game words are believed only while `status` is
ready (`J/CompanionState.java:39-42`). The proposed words follow the same rule.

**Derived words** (`stamina_wheel`, `dim`) are computed in one common place: the state getter, `native_state` in
`R/companion_jni.c:167-181`. It already post-processes both games' samples (`page`). The proposal moves this into an
MIT `companion_state_finish(CompanionState *, int64_t now_ms)` in `companion_state.h`. Both games then get the same
latch constants, and BotW's page dim (the same 200 ms / 400 ms constants, `M/botw_states.h:177`) and the LED hand-off
switch together.

### 3b. Ledger

Column key:

- **Games:** `both`, `TotK`, `BotW`, or `runtime` (a game-agnostic service both can use).
- **Waist:** `S:` state word, `A:` action verb, `R:` runtime service. `—` means none: the endpoint stays game-specific.
- **Effort:** S < 1 day, M a few days, L needs reverse engineering, a device or the BotW load plan.
- **Order:** the milestones in section 4 and what each one unlocks.

Licence boundary for every row: waist headers are MIT and in this repository. BotW adapters change only port-owned MIT
files (`D/duo_state.h`, `D/duo_port.h`) or record a local change to a vendored GPL file in `D/vendor/README.md`. The
TotK build compiles nothing in `D/`.

#### State: the words, and what the LEDs and the shell need

| # | Endpoint | Games | Same semantics? | Waist entry | TotK adapter | BotW adapter | Licence | Effort | Risk | Order |
|---|---|---|---|---|---|---|---|---|---|---|
| L01 | status | both | yes; TotK cannot yet tell loading apart (L08) | S: `status` **landed** | `gate()` → `companion_state` | `duo_state_fill` | MIT both sides | — | — | done |
| L02 | in game | both | yes | S: `in_game` **landed** | ready | valid, not title, not loading | MIT | — | — | done |
| L03 | menu open | both | close: TotK = PouchMgr byte from the frame-end tick; BotW = any `menu_screen` | S: `menu_open` **landed** | `companion_action_state` | `botw.state.menu_open == 1` | MIT | — | low: TotK's value is up to 1 s old after the tick stops (it then reads `stale`) | done |
| L04 | tablet owned | both | yes (TotK: always owned) | S: `tablet_owned` **landed** | always 1 | `slate_obtained != 0` | MIT | — | — | done |
| L05 | life, max life | both | close: BotW `life` includes yellow hearts; TotK's `PlayerStatus.Life` excludes `TemporaryLife` | S: `life`, `max_life` **landed**; define as "including temporary" | add `TemporaryLife` to `life` once confirmed on device (the snapshot already reads it, T-R04) | as is | MIT | S | low (LED low-hearts threshold) | W1 |
| L06 | page | both | renderer-defined numbers | S: `page` **landed** | Java `nativePage` | -1 (the page keeps its own); could map `botw.state.page` | MIT | — | — | done |
| L07 | sample age, validity | both | yes | S: `age_ms` **landed** | computed at read: 0 | seqlock timestamp (`R/duo_port.c:56-81`) | MIT | — | — | done |
| L08 | loading separate from title | both | BotW yes; TotK has the bit (T-R16) but reports title | S: `status` = LOADING | in `companion_state()`: ready actor plus the scene's loading-pause bit → LOADING; also the settle window (`R/companion_actions.c:395-403`) | as is | MIT | S | low; TotK's title-or-transition fallback stays as it is | W1; gives TotK the loading card |
| L09 | stamina current, max | both | yes: both in game units, 1000 per wheel (BotW 1000..3000; TotK `MaxStamina`, 1000 per wheel) | S: `stamina`, `stamina_max` (proposed) | max: `MaxStamina` + `ExtraStamina` (read today). Current: find the GameData float hash the stamina object writes (`owner+0x1390`, hashes at `+0x64`/`+0x6c`, per `$S/bifrost/research.md` §2) in **one device run**, then `gd_float` by hash behind a guard. -1 until then | Interim: `stamina = percent × 10`, `stamina_max = 1000` (exact fraction, wheel count unknown) by adding `botw.ready.stamina.{valid,percent}` to `DUO_STATE_NAMES`. Final: a one-line **local change** in `MOD:1213-1215` publishing raw `.current` / `.max` (GPL side, listed in `D/vendor/README.md`) | MIT map; the GPL change stays in libduo_host | BotW S, TotK M/L | TotK: unknown hash; a device is needed | W1 (BotW), W2 (TotK); unlocks the LED stamina gauge |
| L10 | stamina wheel visible | both | neither reads the HUD gauge flag; BotW `depleted` = below max | S: `stamina_wheel` (proposed, derived) | none: derived above | none: derived above | MIT | S | low; the game's exact hold time is unconfirmed | after L09; unlocks the LED gauge on/off |
| L11 | event raw (cutscene, dialogue, event flow, movie) | both | close: TotK covers any event flow, unsplit; BotW covers in-engine events | S: `event` (proposed) | `CompanionView.event` (`R/companion_snapshot.c:577`) `\|\| movie_host_active()`; fill in `companion_state()`, which already calls `companion_action_state` (`R/companion_snapshot.c:739`) | `botw.state.event` `\|\| movie_host_active()` | MIT; `movie_host.h` is runtime code | S | medium: TotK's "unreadable = event" must not hold the dim on at the title (gate on ready) | **W1**; unlocks L13 |
| L12 | event kind | both | no: TotK does not split | S: `event_kind` (proposed) | 0 (unsplit) or 3 (movie) | `botw.state.dialogue` / `cutscene` → 1 / 2; movie → 3 | MIT | S | low | W1 |
| L13 | screen dim / cutscene signal | both | yes once latched in one place | S: `dim` (proposed, derived) | none: latched above from L11 | none: latched above, the same constants as the module's own `botw.state.dim` | MIT | S | low | **W1**; unlocks the LED cutscene fade and hand-off, and a TotK page dim reusing BotW's look |
| L14 | prerendered movie | runtime | yes | folded into L11 / L12 | `movie_host_active()` (`R/movie_host.h:37`) | same | MIT | S | low; whether TotK's event pointer is also set during movies is unconfirmed (OR covers it) | W1 |

#### Page content: a common vocabulary, kept below the waist until a second consumer needs it

| # | Endpoint | Games | Same semantics? | Waist entry | TotK adapter | BotW adapter | Licence | Effort | Risk | Order |
|---|---|---|---|---|---|---|---|---|---|---|
| L15 | rupees | both | yes (int count) | later S: `rupees` (-1) if an overlay or LED needs it; until then — | `PlayerStatus.CurrentRupee` | `botw.ready.rupees` | MIT | S | low | W5 |
| L16 | position, heading, map layer | both | close: TotK has 3 layers and `y` up; BotW has MainField only and `y = -z` | later: `pos_x`, `pos_z` (world units, int), `heading` (degrees, 0 = north), `layer` (enum) | actor fields | `botw.map.*` | MIT | M | low | W5 (shared minimap or overlay) |
| L17 | equipped gear, durability | both | partly: TotK has fuses and Zonai; BotW has modifiers | — (renderer content); an item token for actions (L32) | snapshot | `botw.ready.weapon.*` | — | — | — | stays |
| L18 | food and cook effects | both | no: TotK has one cook buff, BotW up to 3 effects with timers | — | — | — | — | — | — | stays: different models |
| L19 | special ability / rune | both | no: different sets (Ultrahand vs Sheikah runes) | — | — | — | — | — | — | stays |
| L20 | inventory lists | both | no: TotK GameData arrays with partitions; BotW PouchMgr list in grid order | — | — | — | — | — | — | stays: each renderer owns its grid; only the item token crosses (L32) |
| L21 | map pins and stamps | both | close: `{x, z, type, layer}`; BotW can also edit them | — (renderer content) | snapshot | `botw.map.*` | — | — | — | stays until a shared map |
| L22 | time of day | BotW | — | later S: `minutes` (0..1439, -1) | not read in TotK: reverse engineering needed | `botw.ready.clock.minutes` | MIT | L (TotK) | unknown | W5; useful as an LED tint |
| L23 | weather, temperature | BotW | — | — | — | — | — | — | — | stays game-specific until TotK reads it |
| L24 | area name | BotW | — | — | — | — | — | — | — | stays: text from game files |
| L25 | quests | BotW | — | — | — | — | — | — | — | stays |
| L26 | trackers (Koroks, shrines, towers) | BotW | — | — | — | — | — | — | — | stays |
| L27 | sages | TotK | — | — | — | — | — | — | — | stays |
| L28 | extra, break and temporary hearts | TotK | BotW folds yellow hearts into life | covered by L05; gloom (`breakLife`) stays page content | — | — | — | — | — | stays |
| L29 | battery / energy | TotK | — | — | — | — | — | — | — | stays (TotK-only resource) |
| L30 | map type, shrine | BotW | — | — | — | — | — | — | — | stays |
| L31 | noise, sensor | BotW | — | — | — | — | — | — | — | stays |

#### Actions: verbs and the request/result protocol

| # | Endpoint | Games | Same semantics? | Waist entry | TotK adapter | BotW adapter | Licence | Effort | Risk | Order |
|---|---|---|---|---|---|---|---|---|---|---|
| L32 | action protocol: request → seq, result → pending / done / refused / failed plus reason | both | yes in intent | A: `companion_verbs.h` (MIT), the TotK contract promoted: `request(verb, item_token, arg)`, `result(seq)`, codes BUSY / REFUSED / UNSUPPORTED / INVALID (`R/companion.h:53-62`). The item token is an opaque string the game's own snapshot produces | wraps `companion_request` / `companion_result` (token = category, slot, actor, partition, pouchGen) | stub returns UNSUPPORTED until the mailbox exists; then `on_action` names, with the outcome from the mailbox | MIT header; BotW mapping in libduo_host (GPL) | M (TotK S) | low for TotK (same code); BotW blocked on the load plan | W4 |
| L33 | equip, unequip | both | yes | A: `item.equip`, `item.unequip` | `equip`, `unequip` | `inventory_equip` / `inventory_remove` | as L32 | S (TotK) / L (BotW) | BotW: load plan | W4; BotW after `$S/duo/PLAN.md` M3 |
| L34 | use / eat / drink | both | yes | A: `item.use` (the adapter picks eat, drink or use) | `eat`, `drink`, `use` | `inventory_consume`, `heal_eat` | as L32 | S / L | same | W4 |
| L35 | take out, hold | both | close: TotK drops one item to the ground; BotW holds for cooking | A: `item.take_out` | `takeOut` | `inventory_hold_*` | as L32 | S / L | same | W4 |
| L36 | fuse to arrow | TotK | — | A: `item.fuse_arrow` (TotK only; BotW answers UNSUPPORTED) | `fuseArrow` | — | MIT | S | — | W4 |
| L37 | button shortcut (Plus, Minus, any Npad button) | runtime | yes: game-agnostic | A: `button.press(bit)` | `native_action` kind 1 (`R/companion_jni.c:198-216`) | the same function, reachable through the waist (the Duo page has no Java buttons today) | MIT | S | low; it is the same as a pad press | W4 |
| L38 | warp / fast travel | BotW | — | A: `map.warp(target)` reserved | — (TotK reverse engineering) | `warp_*` | — | L | high: a game state change | after BotW M3; TotK later |
| L39 | map pin edit | BotW | — | A: `map.pin_add/remove` reserved | — | `map_marker/delete` | — | L | medium | after M3 |
| L40 | quests, sort, recipes, quick picker, HUD suppression | BotW | — | — | — | — | — | — | — | stays: page-internal |
| L41 | action gate reason | both | yes: TotK has `menu`, `event`, `loading`, `saving`, `stale`, `unsupported`, `noPlayer`; BotW gates on World UI flags | A: a `gate()` reason enum in `companion_verbs.h`; optionally a later S word | `companion_action_state` (`R/companion_actions.c:275-281`) | derived from status, `menu_open` and `event` (the module gates on `flags == 0x1E09000000`) | MIT | S | low | W4; disabled-control styling the same on both games |

#### Runtime services: what each companion needs from the runtime

| # | Endpoint | Games | Same semantics? | Waist entry | TotK adapter | BotW adapter | Licence | Effort | Risk | Order |
|---|---|---|---|---|---|---|---|---|---|---|
| L42 | guest memory read (fault-free) | both | **identical** (`host_safe_read`, same signature) | R: `CompanionHost.read` | `CompanionEnv.rd` / `companion_read_fn` | `DuoPort.safe_read` | MIT | S | — | W3 |
| L43 | guest write, game call | both | no: TotK calls game functions natively on the frame-end thread; BotW needs the guest helper | — (stays in each adapter) | `CompanionEnv.call`, `call_f`, `wr` | load plan, mailbox (`D/PLAN.md` M2–M3) | — | — | — | stays: the mechanisms are incompatible by design |
| L44 | frame tick | both | close: TotK frame end while pending; BotW per presented frame plus a 16 ms clock | R: `CompanionHost.frames()` plus one `runtime_companion_frame(n)` hook both are called from | `NVN:721` hook | `R/game_patches_botw.c:159` → `runtime_duo_frame` | MIT | M | medium: frame-end timing is load-bearing for TotK actions; keep both call sites and share the counter only | W3 |
| L45 | monotonic clock | both | yes | R: `CompanionHost.now_ms` | `runtime_now_ns` | `host_now_ns` (`R/duo_port.c:59`) | MIT | S | — | W3 |
| L46 | save busy | runtime | yes: game-agnostic (`fs_host.c`) | R: `CompanionHost.save_busy` | used (`R/companion_actions.c:204`) | not given today; add it to DuoPort for the action gate | MIT | S | low | W3 |
| L47 | log | both | close: TotK uses tagged stderr lines, Duo uses levels | R: `CompanionHost.log(level, line)` | stderr | `port_log` | MIT | S | — | W3 |
| L48 | RomFS / AOC read | runtime | yes | R: `CompanionHost.romfs_read`, `aoc_read` | unused at run time (setup-time art) | used | MIT | S | — | W3 (TotK may use it later for text) |
| L49 | thread placement | runtime | yes | R: `CompanionHost.place_thread` | none (the Java poller) | used | MIT | S | — | W3 |
| L50 | build identity, unsupported build | both | close: TotK SHA-256 guards; BotW manifest plus module build list | R: `title_id`, `build_id` in `CompanionHost`; outcome is S: `status = UNSUPPORTED` | own guards (stay) | own check (stays) | MIT | S | — | W3 |
| L51 | settings gates | both | `TOTK_COMPANION` is shared (`R/duo_port.c:127`); the rest are per renderer | R: `companion_enabled()` (one reader) | `native_enabled` | `start()` | MIT | S | — | W3 |
| L52 | art, font, text pipeline | both | no: TotK builds at setup; BotW decodes from RomFS during play | — | `stage_companion.c`, `AssetStore` | manifest sources | — | — | — | stays: renderer-specific |
| L53 | display and presentation | both | yes | Java `PresentationController`, `CompanionScreen` **landed** | `CompanionView` | `DuoSurfaceView` | MIT | — | — | done |
| L54 | touch routing | both | delivery shared; gestures per renderer | Java shell (focus reclaim, Bifrost touch pulse) **landed**; gestures stay | Java gestures | `nativeTouch` (off by default) | MIT shell; Duo gestures GPL | — | BotW touch untested on a device | BotW track |
| L55 | game paused (Android) | runtime | yes | R: `CompanionHost.paused()` | `runtime_android_game_paused` | not used; the action gate would need it | MIT | S | — | W3 |

#### Exposure: what other consumers get

| # | Endpoint | Games | Same semantics? | Waist entry | TotK adapter | BotW adapter | Licence | Effort | Risk | Order |
|---|---|---|---|---|---|---|---|---|---|---|
| L56 | state words to Java, the shell and the LEDs | both | yes | `CompanionNative.state(long[])` **landed** | `companion_state()` | `runtime_duo_state()` | MIT | — | — | done; extended in W1 |
| L57 | page from Java to native | both | yes | `nativePage` **landed** | used | ignored (-1) | MIT | — | — | done |
| L58 | touch pulse to Bifrost | both | yes | `PresentationController.touchListener` **landed** | — | — | MIT | — | — | done |
| L59 | full JSON snapshot | TotK | — | — | `companion_snapshot` | — | — | — | — | stays: the Canvas page's own feed |
| L60 | published value table | BotW | — | — | — | `LedTap` filter (`D/duo_host.cpp:431-442`) | — | — | — | stays: internal to the Eden runtime; only `duo_state.h` names cross |
| L61 | not-in-game screens (title, loading, unsupported) | both | yes once L08 lands | S: `status` **landed** | `J/Motion.java:24-27` | `page_binds` | MIT | — | — | done (TotK loading via L08) |
| L62 | stale sample handling | both | yes | S: `age_ms` **landed**; dropped above 2000 ms | `J/SnapshotPoller.java:52-54` holds 30 s | 4 Hz idle clock | MIT | — | — | done |

**Tally** (by the Games column):

| Group | Count | Rows |
|---|---|---|
| All endpoints | 62 | L01–L62 |
| `both` | 40 | — |
| `runtime` | 6 | L14, L37, L46, L48, L49, L55 |
| TotK-only | 5 | L27, L28, L29, L36, L59 |
| BotW-only | 11 | L22–L26, L30, L31, L38–L40, L60 |
| Already landed | 14 | L01–L07, L53, L54 (delivery), L56–L58, L61, L62 |
| Stays game-specific | 19 | L17–L21, L23–L31, L40, L43, L52, L59, L60 |
| New waist words proposed | 6 | 8–13 |
| Action verbs proposed | 8 | `item.equip`, `.unequip`, `.use`, `.take_out`, `.fuse_arrow`, `button.press`, and the reserved `map.warp` and `map.pin_*` |
| Host services proposed | 10 | listed in W3 |

## 4. Milestones

Each milestone keeps both apps working. Each one changes `companion_state.h`, `CompanionState.java` and this table in
the same commit, and the drift test enforces that.

| | Content | Unlocks | Verify |
|---|---|---|---|
| **W0** (this commit) | This ledger and `tests/test_companion_waist.py` | a reviewed plan; the header cannot drift from it | `python3 -m unittest tests.test_companion_waist` |
| **W1: state v2** | Append words 8–13 (L09–L13). BotW: add `botw.state.{event,dialogue,cutscene}` and `botw.ready.stamina.{valid,percent}` to `DUO_STATE_NAMES`, filled interim (L09). TotK: `event` from the event pointer, `LOADING` (L08), `stamina_max`. Both: `movie_host_active()`. Common: `companion_state_finish` (latch, wheel hold) in the getter. Java: `CompanionState`, `BifrostLooks.State` | LED features 1 and 2 (cutscene fade and hand-off; stamina gauge on BotW); a TotK page dim | `tests/test_companion_state.py` (extend the C and Java probes), `test_android_bifrost.py`, `D/test/host_test.cpp` |
| **W2: TotK current stamina** | One device run logs the GameData hashes at the stamina object; then `gd_float` by hash behind the guard list. Optional BotW raw stamina local change | TotK stamina gauge | `games/totk/tests/companion/test_companion.c` (fake image), then on device |
| **W3: host table** | `companion_host.h` (MIT): read, now_ms, frames, log, save_busy, paused, romfs/aoc, place_thread, identity. `CompanionEnv` and `DuoPort` become supersets of it (`DUO_PORT_ABI` 3) | BotW gets save-busy and pause for its future action gate; one place for new services | `test_companion_gate.py`, host test, `jni_probe.c` |
| **W4: action waist** | `companion_verbs.h`: verbs, item token, codes, gate reasons. TotK wraps its existing code 1:1. BotW stub answers UNSUPPORTED. TotK page dims on `dim` | one action API for both screens; BotW actions slot in after `$S/duo/PLAN.md` M3 | `test_companion_actions.py`, `ItemActionsProbe.java` |
| **W5: content words, only on demand** | rupees, position, time (L15, L16, L22) when an overlay or shared page asks for them | — | — |

**Ordering constraints:**

- W1 needs nothing else, and both LED requests need only W1. W2 needs a device.
- W3 comes before W4 so the BotW action adapter has save-busy and pause.
- BotW items in L33–L35 and L38–L39 are blocked on the load plan and mailbox, not on the waist.
