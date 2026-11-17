# Converging the two second screens: specification for the TotK Eden Duo page

The owner's rule: whatever the two companions have in common must look, read and behave the same. Anything only one
of them has needs a decision: add it to the other one, or remove it.

The owner later decided to keep **only Eden Duo second screens**. Tears of the Kingdom will get an Eden Duo package
like Breath of the Wild's. The MIT TotK companion (`CompanionView` and the rest of
`games/totk/android/java/com/aure/totk/companion/`) will not be developed further.

This document is therefore the **specification for the TotK Eden Duo package**. It compares two things:

- BotW's package as vendored at `03b984c` (`games/totk/duo/vendor/companions/packages/BreathOfTheWild/dualscreen/`,
  `P/` below);
- the MIT TotK companion as it was after `fe1edfe`, which is the only TotK design so far.

Each row says what the TotK Eden Duo page should do. "BotW" means the BotW package. "Old TotK" means the retired MIT
companion. Grid positions are on the 1240 × 1080 canvas both use.

Status words:

- **Same**: BotW does it this way and the TotK package copies it.
- **Needs read**: the TotK module must publish a value that the TotK reader does not read today.
- **Owner**: the decision is the owner's (section 6).

## 1. Font

**BotW's page draws all its text in FOT-Rodin NTLG Pro B (Rodin NTLG Bold). TotK's RomFS has the same face as an
outline font, and TotK uses it as its own "Normal" font.**

- **What the BotW package names.** Both `font` and `font_atlas` are `romfs:/Font/Font_US.sbfarc#Normal_00.bffnt`. That
  is the game's own bitmap font from BotW's RomFS (the Yaz0 SARC `Font/Font_US.sbfarc`), not a bundled file.
  - Sheet: 963 code points, cells 31 × 39 px, ascent 32, line feed 39. Two 1024 × 1024 sheets.
  - The Duo canvas scales it so that `text_scale` × 5 px is the line height (`mod_ui_text.cpp`).
  - Upright and italic come from widget flags on the same face:
    - `text_role: "numeral_readout"` is upright, often with an outline;
    - `text_role: "menu_text"` is `italic: true, slant_deg: 9`, a slant the renderer adds.
- **Which face it is.** The bitmap has no face name, so it was matched against every font in TotK's
  `Font/Font.Nin_NX_NVN.bfarc.zs`, read from the owner's v1.4.3 dump on the Thor:
  - **Advances.** 66 Latin letters and digits (from the CWDH table), fitted with one scale per face:

    | Face | Scale (px/em) | RMS error |
    |---|---|---|
    | `ninP_RodinNTLG-B_DH_003_subset` | 30.43 | **0.27 px**, which is the rounding floor of integer advances |
    | `nintendo_NTLG-DB_DH_002` | | 0.47 px |
    | `ninP_RodinNTLG-M_DH_003_subset` | | 0.93 px |
    | the others (Raglan, Zelda glyphs, Ancient) | | 1.2–4.5 px |

  - **Ink widths.** Rodin B differs by +0.22 px on average (sd 0.35). Rodin M differs by 0.89 px (sd 0.86).
  - **Coverage.** Every character the BotW sheet has is in the Rodin B subset (7,767 code points). The subset also has
    `¼ ½ ¾ × — ° … − ★ ▲ ▼`. BotW's sheet lacks U+2212 (minus).
- **TotK's own font setup.** TotK maps its fonts in `fcpx/*.bfcpx`:
  - `Normal_00`: Symbol + `ninP_RodinNTLG-B_DH_003_subset.bfotf`. This is the same face as BotW's Normal.
  - `Caption_00`: Rodin NTLG M.
  - `NormalS_00`: `nintendo_NTLG-DB_DH_002`.
- **Hashes.** The files cannot be identical: BotW stores a pre-rasterised BFFNT, TotK an XOR-wrapped OpenType CFF.

  | File | SHA-256 (first 16 hex digits) |
  |---|---|
  | decoded `ninP_RodinNTLG-B_DH_003_subset` (OTF) | `04926ec626ad968e` |
  | decoded Rodin M | `48593f83cd87bd3f` |
  | the old TotK companion's prepared `fonts/main.otf` | `48593f83cd87bd3f` |

  The last row shows that **the old TotK companion used the wrong weight**: Caption's Rodin M, extracted by
  `stage_companion.c` `font_task()`, instead of Normal's Rodin B.
- **What the TotK Eden Duo page should do:**
  - Draw everything in **Rodin NTLG B from the owner's TotK RomFS**:
    `romfs:/Font/Font.Nin_NX_NVN.bfarc.zs#scft/ninP_RodinNTLG-B_DH_003_subset.bfotf`. The archive is zstd with the
    `zs.zsdic` dictionary from `Pack/ZsDic.pack.zs`. The member is decoded as in `cg_font()`: XOR key
    `word0 ^ 0x18029a7f`, then a big-endian size.
  - The Duo host has no outline rasteriser. It reads a BFFNT, or glyphs handed over by the module's font extension
    (`ModuleDecodeFont`, `EdenDsmodFontGlyph`).
  - So the TotK module should rasterise the OTF into an atlas through that extension, at about 30.4 px/em (BotW's
    sheet size) so that both games' lettering has the same weight on screen.
  - The alternative: the setup's art step bakes the atlas on the device from the owner's files. Never bundle the font.
  - Use the same `text_role`, italic and 9° slant rules as BotW, widget for widget.

## 2. Hearts

**Decision: neither page has a hearts panel. The TotK Eden Duo page should not have the old Hearts pager.**

Why:

- Both games' HUD draws the hearts on the top screen at all times, with quarters, gloom (TotK) and yellow hearts. A
  copy on the bottom screen adds nothing you cannot already see.
- The one thing it added was the warning for low hearts. That warning already exists on both games, the same way:
  - the stick-LED bridge turns red and stutters at the same threshold (`BifrostLooks.lowOn`), from the common state's
    hearts word, which both games fill;
  - it is the place the eyes don't need to go.
- BotW's Ready page, which both games now follow, has no hearts. Adding them to BotW would mean:
  - new GPL package art and widgets on a page the owner already likes;
  - fitting a pager page that would push quest or champions content further back.
- The bottom screen's room is better spent on what the HUD does not show: quests, the sages or champions, gear
  durability and food.

Hearts stay where they are **information rather than status**, on both games: in the Inventory item detail ("restores
N hearts", BotW's `botw.inventory.selected.recovery.heart.*`), and in the food card. The TotK package should draw
recovery the same way, from TotK's `heal` value, in quarter hearts.

If the owner prefers hearts on both, the place is the left pager's first page, and both pages need the same
threshold colour.

## 3. Shared art

Every file under `P/assets/` is listed in `ASSETS.provenance.json` as **generated** by the Eden Duo project's tools
(GPL-3.0): the command-bar plates, ring masks, the stamina ring, the secondary-weapon condition arcs, and solid colour
layers. None of it is game art.

- **Generic art.** The TotK package is GPL too, so it should **use these files as they are**, not redraw them: the
  corner card, circle frame and masks, the N badge mask, `cmdbar-*`, `stamina-000…100`, `pg-disc-*`, the
  `ready-native-*` frames.
- **Game art.** BotW takes its game art from its own RomFS (`romfs:/Pack/Bootup.pack#Layout/Common.sblarc#…`). The
  TotK package should take the same slots from TotK's RomFS. Several textures have the same name in both games:

| Slot | BotW texture | TotK texture (from the old companion's `uiSources`) | Decision |
|---|---|---|---|
| Rupee | `Nt_RupeeIconNew_00` | `Nt_RupeeIconNew_00` (same name) | Same: same size (36 × 56 at (90, 720)) and position |
| Player arrow on the minimap | `Nt_MapPlayerIconNew_00` + `…Sh_00` shadow | `Nt_MapPlayerIconNew_00` (same name) | Same: arrow 30 × 36 with shadow at the disc centre |
| Corner of gear cards and grids | `Nt_PauseCorner_00` | `Nt_PauseCorner_00` (same name) | Same |
| Map pins | `Nt_MapMapPinIcon1–5_00/01` | `MapPinNewFLSmall0_00` (one shape, tinted) | Same 86 px icon, 104 px glow, flame. Each game's own pin texture |
| N badge | `Nt_AppMapNorthMark_00` in the `ready-native-north` composite | old TotK drew a disc with a letter | Same composite and mask, filled from TotK's north mark (the old `map_north` decor source) |
| Empty gear icons | `Nt_PauseIconL_00/01/02` | `IconWeapon/Bow/Shield_72x72` | Same 64 px at 0x60 alpha, with the italic label (*Shield*, *Bow*, *Weapon*) |
| Rune / ability | `Obj_DRStone_Get` and rune icons | `UI/Tex/Icon/Obj_<Ability>.bntx.zs` | Same slot; label *Ability* when empty |
| Stamina ring | generated `stamina-NNN.png` (green) | none on the page | Same files. Needs read (section 4, C08) |
| Heart (food card, item detail) | `Nt_HeartNewS_00` | `HeartW_00` | Same size and row |
| Loading dots | `Circle8_00` × 12, spun | eight drawn dots | Same 12-dot spinner at (97, 916), from TotK's `Circle8_00` if present, else the generated disc |
| Game logo | `Logo3DGameTitle_00` / `Logo2D_00` | TotK title logo (old `ui/logo`) | Same rectangles: title 384 × 285 at (428, 201); loading 232 × 172 at (936, 880); wrong build |
| Durability bar | widget `bar`, cream on a 0x40 track, red ≤ 20 % | drawn bar, red under 5 points | Same widget and rule (C14) |
| Command-bar plates | generated `cmdbar-*` | old TotK used its slot plate | Same `cmdbar-*` files |

## 4. Difference ledger

**82 differences** were found. For each one, the TotK Eden Duo page takes BotW's look and behaviour unless the row
says otherwise. Nothing in the BotW package needs to change, except where a row says BotW changes.

**Ready (main page).**

| # | Element | BotW | Old TotK | The TotK Eden Duo page should | Status |
|---|---|---|---|---|---|
| C01 | Canvas, background | 1240 × 1080, `#0C0F11`, fixed, scaled to fit | Same grid over TotK's palette background and vignette | Use BotW's background and lift gradient. The canvas is one look for both games | Same |
| C02 | Font | Rodin NTLG B (bitmap), italic menu text, upright numerals | Rodin NTLG M, all upright | Use Rodin NTLG B from TotK's RomFS (section 1) with BotW's roles, through a module font extension | Needs work |
| C03 | Text colour | cream `#FFFCC6`, numerals white `#FFFFFF` with outline `#C7900A19`, 2 px | cream with a black 1/7 outline | Use BotW's colours and outline | Same |
| C04 | Accent | Sheikah blue `#0090FD`: pager edges, clock | Purah Pad teal `#00FFC3`: pager edges, layer plate | Keep the teal. It is the game's identity colour and the LED colour, declared per game in `games.toml` | Identity |
| C05 | Minimap | 800 px disc, map tiles, north up, Link centred, frames and halo | Same size with TotK's tiles on Link's layer | Same, with TotK's layer tiles. Tapping the disc does nothing (BotW) | Same |
| C06 | Map marks on the disc | 5 pins and 5 quest marks, with flame and glow | TotK pins and stamps from the art | Same widgets. TotK pins and stamps fill the pin slots; quest marks wait for a quest read | Needs read (quests) |
| C07 | N badge | composite at (590, 36) | disc with a letter | Use the BotW composite (section 3) | Same |
| C08 | Stamina ring | 822 px green ring while stamina is not full (`botw.ready.stamina.percent/depleted`) | none (capacity only) | Show the same ring from **current** stamina. The TotK module must read it (waist L09/W2) | Needs read |
| C09 | TL dial | Spirit Orbs | Poes | **Lights of Blessing** (TotK's equivalent of orbs: exchanged for hearts or stamina) | Needs read |
| C10 | TR dial | shrines found | stamina wheels (+ extra in gold) | **Shrines found**, as BotW. Stamina capacity leaves the page: the HUD shows the wheel | Needs read |
| C11 | BL dial | Sheikah Sensor target, label *Shrine*; tap opens *Nearest shrine* | nearest shrine or Lightroot distance in metres; tap opens a compass | BotW's dial and label (*Shrine*; *Lightroot* in the Depths). Same compass card (C29) | Same |
| C12 | BR dial | temperature `21°C`, tap opens the clothing card | battery cells (+ extra) | **Temperature** with BotW's gauge and card. The battery leaves the page: the HUD shows it when it matters | Needs read |
| C13 | Corner cards: content | stat number (attack or defence) upright, outlined, at the top; icon below | icon, then the item name | BotW: stat number, then icon. TotK publishes attack and defence (its pouch has them; fused attack included) | Needs read |
| C14 | Durability bar | top bar for shield and bow, bottom for the weapon, red at ≤ 20 % | red under 5 points | BotW's rule (≤ 20 % of the item's maximum) | Same |
| C15 | *Breaking!* | weapon only, italic, `#FF4530` | weapon only | Same | Same |
| C16 | BL corner | rune icon, no label | ability icon and name | Icon only, as BotW | Same |
| C17 | Corner tap | quick picker over the disc (Equip, Unequip, Set secondary, Back) | jumped to the item in Inventory | BotW's quick picker, with the TotK verified actions inside it (C63) | Same |
| C18 | Arrow well | count, ▲▼, swipe cycles the arrow | `×86`, no swipe | BotW's well: plain count (no `×`), ▲▼, swipe cycles when TotK's arrow select is proven, otherwise ▲▼ hidden | Same |
| C19 | Secondary well (above the weapon) | last-used weapon with its condition arc; swipe swaps | Fuse: fused material and fuse durability | **Owner**: BotW's secondary weapon (needs a TotK read of the last-used weapon) or TotK's Fuse in the same frame and arc. Recommended: Fuse, since TotK has no swap action | Owner |
| C20 | Left pager | Main quest / side quest / Champions; blue edge; dots at x 206 | Hearts / Sages; teal edge | Main quest / side quest (when read) / **Sages** in the Champions page's rows (icon, name, charge right-aligned). No Hearts (section 2) | Needs read (quests) |
| C21 | Pager distance | the quest page shows the distance to the quest (`412 m`) | — | Same, when quests are read | Needs read |
| C22 | Rupees | icon and count `1,284` (outlined numerals, comma thousands) | Same | Same | Same |
| C23 | Second currency | none on the page (Mon not shown) | Poes dial | No Poes on the main page. Inventory shows them where BotW would show Mon (C45) | Same |
| C24 | Right pane header | clock plate `03:05 PM` | layer plate *Surface*, *Sky*, *Depths* | **Owner**: time of day (needs a read) or the layer. Recommended: clock, as BotW, with the layer named in the area caption (C26) | Owner |
| C25 | Right pane pages | weather forecast / Buffs / Food | Armor / Meal | **Buffs** (TotK meal effect: icon, name, level, time; *No buffs*) and **Food** (same card). Armor leaves the page (BotW shows armor only in the clothing card) | Same |
| C26 | Caption under the disc | area name (`Area unavailable` when unknown) or the marker caption | coordinates `+x +y +z` | Area name from TotK's location markers (the old art step already extracts `places`), with *(Sky)* / *(Depths)* | Same |
| C27 | Coordinates | not shown | under the disc | **Removed** from the main page. On the Map page (C48) | Same |
| C28 | Command bar | Inventory, Map, Quest; `cmdbar` plates; italic, scale 6 | Inventory, Map, `− Map`, `+ Menu` | Inventory, Map, Quest (when the quest page exists, else 2 buttons centred as BotW lays out 2). `− Map` and `+ Menu` leave: pressing the game's buttons from the bottom screen is not on BotW, and the controller is in the player's hands | Owner (removes a feature) |
| C29 | Compass card | *Nearest shrine* caption `#BBA778`, arrow `Nt_MapPlayerIconNew`, distance; all-found text | same idea, teal arrow, *Tap to close* | BotW's card. No *Tap to close* line (BotW closes on any tap without saying so) | Same |
| C30 | Food card | food that restores hearts, need, *Close* | none | Add, from the TotK pouch's `heal` values (*No food that restores hearts*) | Same |
| C31 | Clothing card (temperature tap) | owned gear that helps, *No owned gear helps* | none | Add, with TotK's armor effects (cold, heat, flame resistance) | Needs read (temperature) |
| C32 | Shrine panel (in a shrine) | disc replaced by the shrine's name, subtitle, chests | none | Add: TotK shrines have chests too | Needs read |
| C33 | Event dim | `#90000000`, input blocked | Same | Same | Same |
| C34 | Pause-menu band | black band, *Sheikah Slate open on the main screen*, bar not live | *Purah Pad open on the main screen*, command bar live | The band with *Purah Pad open on the main screen* and the bar **not** live, as BotW (no `+ Menu` any more, C28) | Same |
| C35 | Before the tablet | page hidden, *The Sheikah Slate awaits.* | n/a (the Purah Pad is always owned) | Not needed (`tablet_can_be_missing = false`) | Identity |
| C36 | Haptics | light tap and select, confirm on write, reject on refusal | none | BotW's haptics block as it is | Same |
| C37 | Page transition | fade 90–130 ms, ease in-out | slide 220 ms, heart swell, marker glide | BotW's fades. The marker glide is the Duo map widget's own | Same |
| C38 | Tablet name in texts | *Sheikah Slate* | *Purah Pad* | *Purah Pad* everywhere BotW says *Sheikah Slate* | Identity |

**Inventory.**

| # | Element | BotW | Old TotK | The TotK Eden Duo page should | Status |
|---|---|---|---|---|---|
| C39 | Getting there and back | command bar; *Back* to Ready | page tabs (Adventure, Inventory, Map) | BotW: no page tabs; *Back* | Same |
| C40 | Categories | 7 icon tabs (no Zonai) | 8 icon tabs, sideways swipe | 8 tabs in BotW's tab style (TotK has Zonai devices). No swipe: BotW has none | Same |
| C41 | Count format | `x12`, ASCII x, outlined | `×12` (U+00D7) | ASCII `x` and the outline, as BotW | Same |
| C42 | Equipped mark | game corner (`Nt_PauseCorner`) | `◆` glyph | The corner | Same |
| C43 | Item detail | name, stat, durability %, modifier, description, recovery hearts | name, stats list (*Durability*, *Fuse durability*, *Recovery (¼ ♥)*…) | BotW's detail layout. Durability as a %, recovery as hearts. TotK's fuse durability as the modifier row | Same |
| C44 | Actions | Equip, Eat, Remove, Put away, long press to hold | Equip, Unequip, Eat, Drink, Use, Take out, Fuse to arrows | BotW's buttons with TotK's verified set. *Take out* replaces BotW's hold-and-drop; *Fuse to arrows* is TotK-only (keep, declared) | Identity |
| C45 | Currency in Inventory | — | — | Rupees and Poes on the Inventory header; BotW adds rupees and Mon there | Needs work |
| C46 | Empty texts | *No items in this category* | Same, plus *Pouch temporarily unavailable* | BotW's text. The unavailable case uses BotW's whole-page state | Same |
| C47 | Recipes | Recipes page (BotW only) | none | **Owner**: TotK has a recipe book too. Recommended: later, not in the first package | Owner |

**Map.**

| # | Element | BotW | Old TotK | The TotK Eden Duo page should | Status |
|---|---|---|---|---|---|
| C48 | Coordinates | none | under the minimap, `%+.0f` × 3 | On the Map page of **both**, in one format: `X -254  Y +120  Z -1102`, whole metres, signed, in the game's axis order (east, height, south). BotW changes too (its module has Link's position) | Needs work |
| C49 | Getting there and back | *Back* | page tab | *Back* | Same |
| C50 | Layers | BotW map layers (Hyrule, …) via *Layers* | Sky / Surface / Depths tabs, automatic | *Layers* sheet listing Sky, Surface, Depths, following Link by default | Same |
| C51 | Pins / stamps tools | *Pins*, *Stamps* (*Choose a symbol, then tap the map*), *Delete pin* | display only | Same tools when the TotK pin write is proven; until then hidden | Needs read |
| C52 | Filters | Shrines, Towers, Settlements, Koroks, Horse, Last death, ON/OFF | none | Same list with TotK's (Lightroots, Shrines, Towers, Settlements, Koroks…) | Same |
| C53 | Gestures | drag and pinch, warp confirm | drag, pinch, *Recenter* | Same as BotW. *Recenter* is the Duo map's follow | Same |
| C54 | Unavailable | *Map unavailable in this area* | — | Same text | Same |

**Quest, Trackers.**

| # | Element | BotW | Old TotK | The TotK Eden Duo page should | Status |
|---|---|---|---|---|---|
| C55 | Quest page | Main, Shrine, Side; *Set destination*; *Active quest*; *No quests in this view*; *Quest data unavailable* | none | Add, from a TotK quest read (the main quests, shrine quests, side adventures, side quests) | Needs read |
| C56 | Trackers, tracker map | collection trackers (armor, Koroks…) | none | **Owner**: later | Owner |

**Title, loading and unsupported screens.**

| # | Element | BotW | Old TotK | The TotK Eden Duo page should | Status |
|---|---|---|---|---|---|
| C57 | Title | logo; *Choose your adventure on the main screen* | logo; *Waiting for a save*; *Choose Continue or load a save…* | BotW's page and text | Same |
| C58 | Loading | black; 12-dot spinner bottom-left; *Loading*; small logo bottom-right | logo, 8 dots, *Loading* + sentence; *Starting* before the first state | BotW's page; no *Starting* variant | Same |
| C59 | Unknown state | *The Sheikah Slate is waking...* | (folded into Starting) | *The Purah Pad is waking...* | Same |
| C60 | Wrong build | *The Sheikah Slate cannot connect* / reason / *Requires Breath of the Wild version 1.6.0 or 1.9.0* / *Your adventure continues on the main screen.* | *Unsupported game build* + one sentence | BotW's three lines: *The Purah Pad cannot connect* / … / *Requires Tears of the Kingdom version 1.4.3* (the build the module supports) | Same |
| C61 | Ellipses | three dots `...` | `…` | `...` as BotW (the strings are shared) | Same |
| C62 | App changed | host's *Companion paused* notice | n/a | The host's (same for both) | Same |

**Messages, wording and controls.**

| # | Element | BotW | Old TotK | The TotK Eden Duo page should | Status |
|---|---|---|---|---|---|
| C63 | Action refusal texts | BotW's set (*Hands full · Put away first*, …), shown in the picker | 25 reasons (*Close the game menu first*, *In a cutscene*, *Your hearts are already full*…) | One list for both, in sentence case, no full stop, written as the TotK set already is. BotW maps its refusals onto the same words | Needs work |
| C64 | Capitalisation | sentence case labels, title-case page names | Same | Same | Same |
| C65 | Empty values | `—`, `—:—`, `—°` | `—` | Same | Same |
| C66 | Thousands | `1,284` (comma) | `12,845` | Same | Same |
| C67 | Distances | `412 m` (space, whole metres) | `101 m`; `1.2 km` past 1,000 | `m` with a space; past 1,000 m `1.2 km`, on both (BotW changes if it prints more than 999 m) | Needs work |
| C68 | Fractions | n/a (no hearts) | `11¾` | n/a | Same |
| C69 | Signs | n/a | `+0.5` extras; `−` (U+2212) on buttons | Hyphen-minus for negative coordinates (BotW's sheet has no U+2212) | Same |
| C70 | Time | clock `03:05 PM` | meal time `mm:ss` | BotW's formats (12-hour clock; buff time `mm:ss`) | Same |
| C71 | Swipes | pagers up/down; arrow and secondary wells up/down | pagers up/down; inventory sideways | BotW's set | Same |
| C72 | Pager tap | turns the page | Same | Same | Same |
| C73 | Physical buttons | none on the page | `− Map`, `+ Menu` buttons pressed the game's keys | None (C28) | Owner |
| C74 | Touch while dimmed | blocked | blocked except the bar under the menu | Blocked (C34) | Same |
| C75 | Controller hand-back after a touch | host-side, same code for both (`PresentationController`) | Same | Same | Same |
| C76 | LED bridge | common state | common state | Unchanged: the TotK Duo module fills the same state words | Same |

**Portrait, small screens, and other differences.**

| # | Element | BotW | Old TotK | The TotK Eden Duo page should | Status |
|---|---|---|---|---|---|
| C77 | Portrait or small panels | the fixed canvas scaled to fit (letterboxed) | a responsive scrolling Adventure layout | The fixed canvas like BotW; there is no second layout | Same |
| C78 | Content description (accessibility) | none | *Purah Pad companion: …* | **Owner**: worth adding to the Duo host for both | Owner |
| C79 | Idle redraws | host redraws on change | Same | Same | Same |
| C80 | Art preparation | none: assets read live from RomFS by `romfs:` URIs | prepared WebP and OTF in app storage at setup | Live `romfs:` reads, like BotW. The setup's *Second-screen art* step and its prompts go away for TotK | Same |
| C81 | Item names | game MSBT by language | game MSBT labels in the prepared art | MSBT through the package's `msbt` entries, like BotW | Same |
| C82 | Languages | `msbt_lang_fallback` | English labels in code | Same as BotW | Same |

**Headline:**

- **82 differences.**
- **54 resolve to "Same":** the TotK package copies BotW, or both already agree.
- **4 are declared identity:**
  - the teal accent;
  - the *Purah Pad* name;
  - no pre-tablet state;
  - TotK-only actions.
- **12 need a new value from the TotK module:** current stamina, Lights of Blessing, shrines found, temperature,
  attack and defence, quests (pager, distance, page, map marks), shrine state, pin write.
- **5 need work, on both or in the module:**
  - coordinates on both Map pages;
  - currencies in both Inventories;
  - one refusal list;
  - km past 999 m;
  - the Rodin B atlas through a module font extension.
- **7 are the owner's** (section 6, which also asks the owner to confirm the hearts and dial choices).
- **Top remaining by value:**
  1. current stamina (C08);
  2. the quest read (C20, C21, C55), which also fills the left pager;
  3. attack and defence numbers on the corner cards (C13);
  4. temperature (C12, C31).

## 5. What changes in the repository's rules

**Done** on `totk-duo` (the package vendored at P1R4N351/eden-duo-companions `4c99f88`): `games.toml` totk
`companion = "duo"`, `gpl_companion = true` and the new per-game `duo` variable; the audit admits each game's own
package; the setup stops offering the art (`ArtPolicy.used`, the stage's code kept); `CompanionView` stays compiled but
unused. Item actions are a later step (DuoPort ABI 3).

- **`games.toml`:**
  - `companion` becomes `duo` for TotK;
  - `gpl_companion` becomes `true` for the TotK APK;
  - `audit_apk.py` then allows the Eden Duo host, package and licence texts in the TotK APK, as it already does for
    BotW.
- **`companion-waist.md`.** The waist stays as it is: both modules fill the same state words. Rows that only the MIT
  view used (snapshot JSON, `CompanionView`) become historical.
- **Setup.** The *Second-screen art* stage for TotK is no longer needed (C80). Its removal is for the session that
  builds the package.

## 6. Decisions for the owner

| # | Question | Recommendation |
|---|---|---|
| C19 | Secondary well: BotW's last-used weapon, or TotK's Fuse? | Fuse, drawn in BotW's frame and arc (TotK has no weapon swap) |
| C24 | Right pane header: clock (needs a read) or the layer name? | The clock, as BotW; the layer goes in the area caption |
| C28, C73 | Drop `− Map` and `+ Menu` (game buttons from the bottom screen)? | Drop. BotW has none, and the controller is in the player's hands |
| C47 | TotK recipe page | Later |
| C56 | TotK trackers | Later |
| C78 | Accessibility descriptions in the Duo host for both | Yes, in the host, once |
| §2 | Hearts on neither page | Neither (section 2) |
| C10, C12 | Stamina wheels and battery cells leave the main page | Yes: the HUD shows them, and BotW's dial slots carry shrines and temperature |
