# The Legend of Zelda: Tears of the Kingdom — dual-screen companion

Title ID `0100F2C0115B6000`, game version **1.4.3** only (main module ID
`277178B7DBA1B6D4949A84778D4ABC58B31B34F5`). Reads the game; item actions only in TotK Native (see
below). Status: **0.3.0, tested in the desktop host harness only; not yet run on a device.**

## The rule this package follows

Whatever Tears of the Kingdom and Breath of the Wild have in common looks, reads and behaves the
same. The main page **is Breath of the Wild's Eden Duo Ready page, widget for widget, with Tears of
the Kingdom's values**: `tools/totk/gen_manifest.py` reads `packages/BreathOfTheWild/dualscreen/
manifest.json` and rewrites its Ready, main-menu and Loading pages with one table, instead of
drawing a page of its own. A slot Tears of the Kingdom cannot fill yet shows Breath of the Wild's
own empty state (`—`, `—°`, *Area unavailable*) or is left out (list below).

## Pages

- **Ready** (Breath of the Wild's layout):
  - the round minimap from the game's own map tiles (the finer ones), north up, Link's arrow in the
    middle, on the layer he is on (Surface, Sky, Depths); the green **stamina ring** round it while
    stamina is not full or Link is out of breath;
  - dials: **Lights of Blessing** (top left), **shrines found** (top right), the sensor (bottom
    left: *Shrine*, *Lightroot* in the Depths), temperature (bottom right: no read yet, `—°`);
  - corner cards: shield, bow, weapon (icon and durability bar: the item's durability over its own
    maximum, `MaxLife` in its actor pack, red at 20 % or less; *Breaking!* on the weapon; no bar
    when an item has no maximum), the ability; the arrow well (arrow and count) under the bow; the
    **Fuse** well (the fused material and its durability points) above the weapon, in Breath of the
    Wild's secondary-weapon frame and arc (owner decision C19; the arc shows its empty track only,
    because a material's maximum fuse durability is not read yet);
  - left pager: **Sages** in Breath of the Wild's Champions rows (one to five rows: icon, *On*,
    the recharge time, or *Off* when the sage is not with Link; the glow when ready); rupees under it;
  - right pane: the header names the layer (*Surface*, *Sky*, *Depths*; see below), then **Buffs**
    (the meal effect and its time left, *No buffs*) and, while the hearts are low, **Food** (what
    is missing and, on a tap, the food card: the four pouch foods closest to it, as Breath of the
    Wild ranks them); a vertical swipe turns the pane;
  - the area caption (*Area unavailable* until an area name is read; the layer is not repeated
    there); the command bar: **Inventory**, **Map** (two buttons centred: no Quest page yet);
  - the **quick picker**: a tap on the weapon, bow or shield corner opens that pouch over the page
    in Breath of the Wild's picker (panel, grid of cells with durability bars and fused materials, the
    equipped item lit and selected); a tap selects another, and the confirm bar beside *Back* shows its
    action (*Equip* / *Unequip*). It gives way to the pause menu, a cutscene and loading;
  - a cutscene dims the page; with the pause menu open it says *Purah Pad open on the main
    screen*. The title screen, loading and another game version show Breath of the Wild's covers
    (*Choose your adventure on the main screen*, *Now loading...*, *The Purah Pad cannot connect /
    Requires Tears of the Kingdom version 1.4.3*).
- **Inventory** (Tears of the Kingdom's own page, in Breath of the Wild's palette and art): the eight
  categories as icon grids with counts (`x12`), durability bars and fused materials; a tap shows the
  item's in-game name, description and facts (durability as a % of the item's maximum, C43), and its
  actions as Breath of the Wild's Inventory action bar (up to three, the primary rightmost: *Equip*,
  *Eat*, *Take out*, *Fuse to arrows*, ...), each disabled one dimmed with its reason, and above them
  the latest request's line (pending, done, refused or failed).
  Both pages end with the Ready page's overlay block: the pause-menu band (input blocked), the
  cutscene dim and the title, loading, unknown and wrong-build covers. The Map page's marker is
  Breath of the Wild's full-map one (101 px, yellow).
- **Map**: the whole MainField map, pan and zoom, Surface / Sky / Depths or *Follow Link*,
  *Recenter*, and Link's coordinates (`X -254  Y +120  Z -1102`, spec C48).

### The right pane's header

On Breath of the Wild's page the header is the clock. The owner's rule for Tears of the Kingdom:
the time of day on the Surface and in the Sky, the layer name (*Depths*) while Link is in the
Depths. There is no time-of-day read yet, so the header names the layer everywhere. The switch is
`HeaderShowsClock` / `ClockText()` in `native/modules/0100F2C0115B6000.cpp`: once `ClockText()`
returns the time, set the switch and the Surface and Sky show the clock; nothing in the package
changes.

## Values

The module publishes Breath of the Wild's names with `totk.` in front (`botw.ready.shield` becomes
`totk.ready.shield`), so the page needs no other renaming. Exceptions, from the generator's
`RENAME`: Spirit Orbs are `totk.ready.lights`, the clock is `totk.ready.header.*`, the recent weapon
is `totk.ready.fuse.*`.

| Values | From |
|---|---|
| `totk.state.page`, `.title`, `.loading`, `.unsupported`, `.dim` | the reader's status and actionState |
| `totk.state.life`, `.max_life`, `.menu_open` | the common state words (`companion_state`), quarter hearts; **-1 when unknown**, published in every status |
| `totk.ready.stamina.percent`, `.depleted` | `PlayerStatus.Stamina` / `MaxStamina`, `PlayerStatus.IsTired` (new, `totk_reader/totk_gamedata.c`) |
| `totk.ready.shrine_count` | `DungeonState.Dungeon000..151` in Open, Enter or Clear (new, `totk_gamedata.c`) |
| `totk.ready.lights` | the count of the key item `Obj_DungeonClearSeal` |
| `totk.ready.shield`, `.bow`, `.weapon` (+ `.valid`, `.durability_pct`, `.low`) | the pouch: equipped item and its durability over its maximum (`MaxLife` in `Pack/Actor/<actor>.pack.zs`, `Life/LifeParameters/<actor>.game__life__LifeParameters.bgyml`); `-1` (no bar) when the pack has none |
| `totk.ready.rune`, `.arrow`, `.arrow.count`, `.fuse`, `.fuse.life` | the ability, the arrows, the weapon's fused material |
| `totk.ready.rupees.text` | `PlayerStatus.CurrentRupee`, grouped (`12,845`) |
| `totk.ready.champion_slot.N.*`, `.champions.count` | the sages' companion flags |
| `totk.ready.effect.0.*`, `.effects.count` | `PlayerStatus.CookBuff`, `CookBuffTime` |
| `totk.ready.heal.*` | life, max life and the food pouch (`LifeRecover`, effect) |
| `totk.ready.header.text`, `totk.ready.sensor.*` | the layer (see above) |
| `totk.map.*`, `totk.mini.tile.N`, `totk.coords` | the player actor, `MapData.CurrentLayer` |
| `totk.fx.spin.N` | the loading spinner's dots, as Breath of the Wild's |
| `totk.quick.*` | the quick picker: open, title, cells (`.N.icon`, `.equipped`, `.selected`, `.durability_pct`, ...), the selection |
| `totk.inv.act.*`, `totk.quick.act.*` | the selected item's actions: `.count`, per button `.N.on`, `.enabled`, `.disabled`, `.label`, `.reason`, `.reason_text` |
| `totk.act.*` | the latest request: `.state` (0 none, 1 pending, 2 done, 3 refused, 4 failed), one flag each, `.reason` (the code, unchanged), `.line`; `.ready`, `.ready_reason`: the action host's own state |

## Breath of the Wild's Ready widgets, kept and left out

Kept and rewritten: background and lift, the disc, its frames, halo and minimap, the stamina ring,
the player arrow, the N badge, the four dials, the four corner cards and their bars, the arrow and
secondary (Fuse) wells, the pager panes, edges and the right pager's dots, the Champions rows (as
Sages, plus a five-row layout), the Buffs title and first row, the clock plate (as the header), the
Food page and food card, the rupees, the area caption, the footer rule and the Inventory and Map
buttons, the dims and pause-menu band, and the title, loading, unknown and wrong-build covers.

Left out (`READY_DROP` in the generator), and why:

| Widgets | Why |
|---|---|
| map pins, quest marks on the disc | no pin or quest read yet |
| shrine panel | no in-shrine read yet |
| quest page of the left pager, its dots, the Quest button | no quest read yet; the left pager has one page (Sages) |
| second and third buff rows | Tears of the Kingdom has one meal effect at a time |
| weather page | the right pane carries Buffs and Food (spec C25) |
| sensor ping glyphs, compass card | no sensor-target or nearest-shrine read yet |
| temperature glows, dial halves and needle, clothing card | no temperature read yet (the dial shows `—°`) |
| attack and defence numbers on the corner cards | not read yet |
| area name, marker caption | no area read yet (*Area unavailable* shows) |
| quick picker (Ready 320-571, 593-807, 864-989) and the corner and well taps | Breath of the Wild's picker runs on its module's actions; Tears of the Kingdom's own picker, in its style, replaces it (the three gear corners open it) |
| secondary swap hint, arrow up/down hints | Tears of the Kingdom has no weapon swap; arrow selection is not offered (the condition arc is kept, as its empty track: no maximum fuse durability is read) |
| view cone, context caption, fx probe | Breath of the Wild-only reads |
| cover frames (`state-native-frame`) | Breath of the Wild's conductor-frame art; Tears of the Kingdom has none |

## How it works

- **Game state**: `native/modules/0100F2C0115B6000.cpp` runs TotK Native's companion reader
  (`native/modules/totk_reader/`, MIT, unchanged) over Eden Duo's `read_memory`: the GameData flag
  tables (`gmd::GameDataMgr`), the pouch, the scene's player actor and the pause-menu / event
  state. Before trusting any offset it checks a SHA-256 of eight instructions of `main` that use
  them, so any other build is refused (*unsupported*), and a sample taken while the game tears
  its state down is retaken rather than shown half-changed. `totk_reader/totk_gamedata.c` (MIT)
  reads the stamina and shrine flags with the same table layout, only after the reader has
  accepted the build. The module writes nothing and calls no game code.
- **Item actions only in TotK Native.** Equipping, eating and taking out an item means calling the
  game's own pouch functions, which Eden Duo cannot do on Android (no guest calls under NCE). TotK
  Native's app embeds this package with Eden Duo's runtime and gives the module a table
  (`native/modules/totk_native_actions.h`, bound with `totk_native_port_bind_actions`, which Eden's
  loader never calls): the actions its own runtime offers for an item (the game's menu rules), its
  state, and requests that its runtime carries out on the game's frame-end thread, one at a time,
  re-checked there. The module only shows them and forwards a tap (`totk.act`). In Eden Duo there is
  no table: the buttons show what the game's menus would offer, disabled, *Item actions need the
  TotK Native app*, and a tap requests nothing. The old `- Map` / `+ Menu` buttons are gone (C28).
- **Art and text** are decoded at runtime from the player's own romfs, as `module:totk:` images:
  item icons (`UI/Tex/Icon/<actor>.bntx.zs`), layout textures from
  `UI/LayoutArchive/Common.Product.140.Nin_NX_NVN.blarc.zs` (`timg/__Combined.bntx`; the generator's
  `TEX` table maps each Breath of the Wild texture to Tears of the Kingdom's), the map tiles
  (`UI/Map/MainField/`), item names and descriptions (`Mals/USen.Product.*.sarc.zs`,
  `ActorMsg/PouchContent.msbt`) and the game's Rodin NTLG B font (`Font/Font.Nin_NX_NVN.bfarc.zs`,
  rasterised with the Super Mario Bros. Wonder module's font code). Every TotK `.zs` file is a
  zstd frame compressed with a dictionary from `Pack/ZsDic.pack.zs` (`totk_romfs.cpp`).
- **Package art**: `dualscreen/assets/` holds Breath of the Wild's Eden Duo-generated frames, masks,
  command-bar plates and stamina ring, copied unchanged by the generator; `ASSETS.provenance.json`
  names each file's generator and the Breath of the Wild commit it was copied from.

## Files

| File | What it is |
|---|---|
| `dualscreen/manifest.json` | The pages. Generated by `tools/totk/gen_manifest.py` from the Breath of the Wild package |
| `dualscreen/assets/`, `dualscreen/ASSETS.provenance.json` | Generated art copied from the Breath of the Wild package (no game art) |
| `dualscreen/map_tiles.txt` | Which variant of each map tile to draw (file names only, no game data). Generated by `tools/totk/gen_map_tiles.py` from a romfs file list |
| `dualscreen/totk_font.txt` | The font spec the module's `decode_font` reads: `<scft font>:<cap height px>` |

The package ships **no game assets**: no art, text, audio, fonts or level data.

## Build

```sh
python3 tools/totk/gen_manifest.py
cmake -S native/modules -B build-native-android -G Ninja -DEDEN_SOURCE_ROOT=/abs/eden-duo \
  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-24 -DANDROID_STL=c++_static -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build-native-android --target dsmod-totk
llvm-strip --strip-all -o totk-android.so build-native-android/0100F2C0115B6000.so
python3 tools/build_dualscreen_package.py --package packages/TearsOfTheKingdom --output dist \
  --version 0.3.0 --min-runtime 22 --module android-arm64-v8a=totk-android.so \
  --build-id 277178B7DBA1B6D4949A84778D4ABC58B31B34F5000000000000000000000000
```

## Tested

Only on a desktop: Eden Duo's runtime built with this module linked in, against the real 1.4.3
`main` image and a laid-out fake game state (hearts, stamina, shrine states, gear, pouch with
food, Lights of Blessing, sages, position), reading the owner's own romfs files. Scenes checked
against Breath of the Wild's render: in game (Surface, Sky, Depths), pause menu, cutscene, title
screen, loading, low hearts (Food page, food card open and closed), another game version;
Inventory with a selected item; the Map page; the pause-menu band over Inventory and Map (a tap
under it does not leave the page). Item actions with a fake action table standing in for TotK
Native's (a request reaches it with the tapped item and ends as done; a second tap while one is
pending is dropped; the quick picker equips from its confirm bar) and without one (every action
disabled with its reason, nothing requested).

## Not yet

- A device run in Eden Duo (AYN Thor / Retroid Pocket Duo), and the module's Android build for
  this version.
- Reads: the time of day (then the clock in the header), temperature (dial and clothing card),
  quests (left pager, Quest page, quest marks, distance), map pins and stamps, an area name, the
  nearest shrine or Lightroot (compass card), the in-shrine state, attack and defence numbers,
  a fused material's maximum fuse durability (then the Fuse arc fills), which effect a meal buff's id stands for (the Buffs row shows a
  plain effect mark), a hearty meal's extra hearts.
- On another game version the module does not load, so that cover has no logo and uses the host's
  built-in font. Breath of the Wild's cover reads its font and logo through plain `romfs:` paths;
  every Tears of the Kingdom file is zstd-compressed with a dictionary, so only the module can
  decode them, and the host loads a module only on a build its `build_ids` lists (MODULE_GUIDE §3:
  a module never implies support for an unlisted build).
- Languages other than English (US) for item names and descriptions.
- Item actions in Eden Duo itself (they need guest calls; a load-time helper is the researched
  route). TotK Native's route is not yet checked on a device through this page.

## Credits

Game-state reader and research: TotK Native
([P1R4N351/botw-android](https://github.com/P1R4N351/botw-android), MIT), built on
[AurelioB/totk-android](https://github.com/AurelioB/totk-android) and
[ly05010419/MacNativeGames](https://github.com/ly05010419/MacNativeGames). Page design and generated
art: this repository's Breath of the Wild package. Font and BNTX decoding: this repository's Super
Mario Bros. Wonder module.
