# The TotK companion's main page, on BotW's Ready grid

The owner asked for Tears of the Kingdom's second-screen main page to look and work more like Breath of the Wild's.
This page records what BotW's main page (the Eden Duo package's `ready` page) is, what TotK's was, and how the new TotK
page maps one onto the other. The code is `CompanionView.ready()` (MIT, Canvas). Nothing of Eden Duo is copied:
no source, no package art. The layout was measured from the manifest's widget rectangles and reimplemented.

The design was checked against three renders at 1240 × 1080 (BotW Ready, TotK before, TotK after). They are not
published here because they show game art (map tiles, item icons) prepared from the owner's game files.

The renders were 1240 × 1080, the AYN Thor's bottom screen in landscape. The companion draws at that size: the
Presentation is full screen and the display reports 369 dpi (density 2.30625).

- **BotW:** the vendored Duo host's own rasteriser (`duo/test/host_test.cpp`, run on a Mac against the real 1.6.0
  `main`), fed the owner's RomFS fonts, layouts, icons and map tiles. The module's published values were overridden
  with a plausible mid-game state, since a fake image has no game running.
- **TotK:** the real `CompanionView`, `AssetStore` and `MapRenderer` drawn by a desktop Java harness over a `java.awt`
  stand-in for `android.graphics`. It uses the art the app prepared on the Thor from the owner's files, and the preview
  app's `ready` fixture.

The harnesses are scratch tools, not part of the repository or the APKs. The pictures contain game art, like the
other screenshots in `docs/images`.

## 1. BotW's Ready page: regions and behaviour

The canvas is 1240 × 1080 with a near-black background (`#0C0F11`), cream text (`#FFFCC6`), Sheikah-blue accents
(`#0090FD`) and HUD numerals with an outline.

| Region (manifest rect) | Shows | Touch |
|---|---|---|
| Centre disc 800 × 800 at (220, 64) | Minimap from the game's map tiles, north up, Link's arrow in the middle. Quest marks and pins. A green stamina ring (822 px) appears while stamina is below full | Only when a card is open (tap closes it) |
| North badge (590, 36) | `N` disc | — |
| Four dials round the disc (84 px) | TL spirit orbs, TR shrines found, BL Sheikah Sensor target, BR temperature dial | BL opens the *Nearest shrine* compass card in the disc; BR, when temperature glows, opens the clothing card |
| Four corner cards 184 × 192 | TL shield, TR bow, BL rune, BR weapon: icon, stat number, durability bar (red ≤ 20 %, *Breaking!*) | Opens the quick picker for that slot over the disc, with Confirm/Close (equip needs the action bridge, which is off in the port) |
| Arrow well under the bow (1048, 206) | Equipped arrow and count, ▲▼ | Tap opens the picker; swipe up/down cycles arrows |
| Secondary well above the weapon (1048, 686) | Last weapon used, its condition ring, swap hint | Tap opens the picker; swipe swaps |
| Left pager 184 × 468 (blue left edge, dots at x 206) | Main quest / side quest / champions | Tap opens the Quest page; swipe up/down turns it |
| Rupees under it | Icon and count | — |
| Right pane 192 × 236 with clock plate | Time (blue); then weather forecast / buffs / food advice (dots at x 1016) | Swipe turns it; Food opens the food card in the disc |
| Under the disc (620, 898) | Area name or map marker caption | — |
| Command bar at y 952 | Inventory, Map, Quest | Goes to that page (130 ms fade) |
| Overlays | Event or cutscene: whole screen dimmed `#90000000`, input blocked. Pause menu: dim plus a black band *Sheikah Slate open on the main screen*. Title, loading, unknown and wrong build each get a full framed page. Before the slate the page shows no slate content (`slate_obtained`) | Blocked |

## 2. TotK's Adventure page before

The page was a responsive two-column scroll layout under three icon tabs (Adventure, Inventory, Map):

- **Left column:**
  - hearts (quarters, gloom, extra);
  - stamina wheels and battery cells;
  - rupees and Poes;
  - three gear cards (weapon, bow with the arrow count, shield);
  - ability;
  - sages;
  - meal effect.
- **Right column:**
  - a square minimap and the coordinates;
  - `− Map` and `+ Menu` (the game's buttons);
  - three armor cards.

A gear card opened the item in its Inventory category, where its actions are. There was no dim during cutscenes and
nothing showed that the pause menu was open.

## 3. Mapping

| BotW Ready | TotK main page | Kept, changed, or why it differs |
|---|---|---|
| 1240 × 1080 fixed grid | Same grid, scaled uniformly to fit (`READY_W`/`READY_H`) on landscape screens 0.9–2.0 wide and at least 480 px tall | Kept. Portrait phones and small panels keep the responsive Adventure layout |
| Page tabs | None on the main page; the command bar leads away. Inventory and Map keep their tabs | Kept from BotW |
| Round minimap, Link centred, north up, `N` badge | Same, with TotK's map tiles on the player's layer (Surface, Sky, Depths). A tap opens the Map page | Kept. The tap is TotK's |
| Stamina ring | None | TotK reads stamina capacity, not current stamina (ledger L09) |
| Dials: orbs / shrines / sensor / temperature | Poes / stamina wheels (+ extra in gold) / nearest shrine or Lightroot distance / battery cells (+ extra) | Same slots and look, filled with what TotK reads. The sensor slot keeps its job |
| Sensor tap: *Nearest shrine* compass card | Same card: *Nearest shrine* (*Nearest Lightroot* in the Depths), the shrine's name, an arrow and the distance. Tap closes | Kept, from the map markers in the prepared art. No new native value |
| Corner cards: shield, bow, rune, weapon | Shield, bow, ability (Ultrahand…), weapon. Durability bar on top (bottom for the weapon), red under 5 with *Breaking!* on the weapon. Item name instead of a stat | The game's slot frame. No attack value is read, so the name is shown |
| Corner tap: quick picker | Opens the item in its Inventory category, where Equip and the other actions are (unchanged path; the ability card has no action) | TotK's verified actions live there; no second action UI |
| Arrow well, swipe cycles | Arrow well with icon and count; tap opens the arrow in Bows & Arrows | No proven arrow-select action on TotK |
| Secondary-weapon well | **Fuse** well: the weapon's fused material and its fuse durability | TotK's own idea in BotW's slot |
| Left pager: quests / champions | **Hearts** (quarters, gloom, extra; `11¾ / 20`) / **Sages** (with you, cooldown, away, not joined) | Hearts stay on the page: TotK's page always had them. Sages take the champions' place |
| Low hearts | Hearts pane edge and count turn red at the stick LEDs' threshold (`BifrostLooks.lowOn`) | Shared rule, checked by a test |
| Rupees under the left pager | Same | Kept |
| Right pane: clock plate, weather / buffs / food | **Layer plate** (Surface, Sky, Depths, in teal); pages **Armor** (three worn pieces) / **Meal** (effect, level, time) | TotK reads no clock or weather |
| Area name under the disc | Coordinates | No area lookup in TotK yet |
| Command bar: Inventory, Map, Quest | Inventory, Map, `− Map`, `+ Menu` (the last two press the game's buttons; drawn dimmer) | TotK has no quest page; its button shortcuts stay |
| Pager tap / vertical swipe; dots | Same, two pages each | Kept |
| Event dim, input blocked | Same dim when the snapshot's `actionState.reason` is `event` | Kept, from a value TotK already reads |
| Pause-menu band *Sheikah Slate open…* | *Purah Pad open on the main screen*, from `actionState.pauseMenu` or the common state's `menu_open`. The command bar stays live (so `+ Menu` can close it) | TotK's name. The live bar is a deliberate difference |
| No slate yet | — | The Purah Pad is always owned (`tablet_can_be_missing = false`) |
| Title / loading / wrong build pages | Unchanged TotK full-panel views (logo, heading, words; turning mark while loading) | Already equivalent |
| Fade 130 ms between pages | TotK's slide (220 ms), heart swell and marker glide (`Motion`) | TotK's motion stays |
| Sheikah blue `#0090FD`, black panels | Purah Pad teal `#00FFC3` edges and plate (the LED colour), the same translucent black panels and cream `#FFFCC6` text, TotK's palette background, slot frame, tab plates and Rodin font from the prepared art | Identity |

## 4. Limits

- **On the device:** none of this has run on the Thor.
  - Fit, touch targets, and the event dim and menu band from live `actionState`, are seen only in host renders.
  - `actionState` is refreshed on the frame-end tick while the companion watches, as the action buttons already rely on.
- **Not shown:**
  - current stamina, which waits for ledger L09/W2;
  - time of day;
  - an area name.
