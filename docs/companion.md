# The dual-screen companion

> **Now the Eden Duo page.** TotK Native's second screen is now the Tears of the Kingdom Eden Duo page, the same
> runtime as BotW Native's (`libduo_host.so`, GPL-3.0; [APP.md, *Dual-screen companion*](../games/totk/android/APP.md#dual-screen-companion)):
> Breath of the Wild's main page with Tears of the Kingdom's values, plus Inventory and Map, drawn from the game's own
> files at run time. Item actions (equip, eat, take out, fuse) are on the page's Inventory detail and its quick picker
> (the gear corners), carried out by this app's own runtime as before. Touch is on (`TOTK_DUO_TOUCH=0` turns it off). The setup no longer
> prepares *Second-screen art*. The rest of this page describes the earlier companion (`CompanionView`), which stays
> in the source, unused, until it is either brought back or removed.

On handhelds with a built-in second display (AYN Thor, Retroid Pocket Duo), TotK Native shows a companion on that
display while you play: Link's status, the map and the inventory. This page is the player-facing reference; the
implementation (snapshot format, polling, the native action checks and the device evidence for each action) is in
[games/totk/android/APP.md, *Dual-screen companion*](../games/totk/android/APP.md#dual-screen-companion).

## What it shows

- **Adventure** (the main page): on the Thor's second screen and other landscape screens it is laid out like Breath of
  the Wild's Ready page ([design](companion-totk-main-page.md)). A round minimap sits in the middle. The shield, bow,
  ability and weapon are in the corners, with the arrows and the weapon's fused material beside them. Four dials round
  the map show Poes, stamina wheels, battery cells and the distance to the nearest shrine (Lightroot in the Depths); tap
  that dial for a compass. Two pagers show hearts or sages, and armor or the meal effect: tap or swipe up and down to
  turn them. Below are the rupees, the position and a command bar (Inventory, Map, `− Map`, `+ Menu`). During a cutscene
  the page dims; with the pause menu open it says so, and only the command bar can be touched. Tapping an equipped item
  opens it in its inventory category. Portrait and small screens keep the earlier scrolling layout.
- **Inventory**: all eight categories (weapons, bows & arrows, shields, armor, materials, food, Zonai devices, key
  items). Every item is shown under its in-game name and description, also the ones the game names after another item
  (caught fish and insects, bundles, the opening's Master Sword and Champion's Leathers). Tap an item for its details
  and actions; swipe sideways for categories, drag to scroll.
- **Map**: three layers (sky, surface, depths). Drag and pinch to move and zoom, pick a layer by hand; *Recenter* goes
  back to following Link.

Pages slide in from the side they are on, hearts that change swell for a moment, and Link's marker glides on the map
and minimap instead of jumping between readings. The title screen, loading screens (and the game's start) and an
unsupported game version each get a screen of their own with the game's logo, instead of a dimmed last page. Nothing
is redrawn while nothing changes.

The pictures (map, icons, item art) are made on your device from your own game data during setup (*Second-screen art*)
and can be rebuilt from *Game data*. They are optional: the game runs without them. Art made by an earlier version of
the app is updated on its own when the menu opens; the game stays playable meanwhile.

## Item actions

While playing (no game menu open), the selected item offers the actions the game itself would allow:

| Items | Actions |
|---|---|
| Weapons, bows, shields, armor | Equip, unequip |
| Food and edible materials | Eat (when it would have an effect, e.g. not with full hearts for food that only heals) |
| Elixirs | Drink |
| Fairies | Use |
| Materials and Zonai devices | Take out (placed in front of Link, as the game's own shortcut does) |
| Materials, with the bow drawn | Fuse to arrows |

Each tap does exactly what the game would do, and the result is read back from the game. A button is greyed out with
the reason when the game would not allow the action, for example:

- *Close the game menu first* (pause menu or map open), *In a cutscene*, *Wait for loading to finish*, *The game is
  saving*, *Load a save first* (title screen), *Link isn't in the world right now*;
- *Your hearts are already full*, *This can't be eaten*, *None left*, *No arrows left*, *Aim your bow to fuse*,
  *Weapons can't be changed right now*;
- *Not available from the companion yet*: for actions not yet proven on a device. In 1.0.1 these are **energy-recovering
  materials** (recharging the battery) and **fusing Zonai devices to arrows**.

Only one action runs at a time; a second tap while one is pending is ignored. The pause menu's *Hold* is not offered.
The optional minus / plus shortcuts only press the game's map / menu buttons.

## Which display is used

On dual-screen devices the game always opens on the main (top) screen, even when you start it from the second
screen's launcher or menu. With **Second screen** on and its art prepared, the companion appears on the second
(bottom) screen. Turning the companion off does not move the game to that screen. Single-screen devices are unchanged.

The companion uses a display that is built into the device, found among the displays present, never guessed from the
chip: a display counts as built in when Android reports it as built in, or as an unknown connection with the same panel
maker's id as the main screen. External monitors (HDMI, DisplayPort, USB-C), cast and screen-recording displays and
the developer *Simulate secondary displays* overlay are ignored; the Screen tab then says *External display ·
companion off* and has no *Second screen* option. The companion is shown on one secondary display only (never the
game's), and only while the game is in front: Home dismisses it and returning shows it again.

*Graphics → Screen → Second screen* turns it off for play. Developers can also disable it with `TOTK_COMPANION=0` in
`totk.env` ([developer settings](developer-settings.md)).

Android sends controller buttons to the screen that was touched last. When you touch the companion, or an overlay
shown on the second screen (such as AYN's performance overlay), the game takes the controller back: right after a
companion touch, and about 0.3 s after anything else took it. If a window on the second screen keeps taking it (for
example the keyboard of another app shown there), the game stops after three quick tries until you touch the
companion again. Apps cannot see other apps' windows, so such a window can lose the controller up to three times first.

## Preparing the pictures

Setup builds the optional second-screen art alongside shaders only on devices with a built-in second screen.
On single-screen devices, including phones and the Odin 3, the stage is recorded as **skipped**, with **Not needed
on this device**; no art is generated. External and cast displays do not make the art necessary.

If a built-in second screen is present when you press **Start game**, but the art is missing (skipped, never built,
failed, or deleted), you can **Prepare second-screen art** before playing or **Play without it** immediately.
Preparing shows the normal stage progress and starts the game only after the art is ready; a failure or cancellation
returns to Home. **Play without it** is remembered across launches for this install, so you are not asked again.
**Game data → Second-screen art** still builds it on demand on any device, and clears that choice. Deleting imported
data also clears it.

On a Retroid Pocket Duo with the imported game on internal storage, preparing the art alone took **12.8 seconds**
(about 15 seconds; warm filesystem cache, measured from the choice to the committed art on 2026-11-04), producing
35.3 MB. Other devices and slower storage may take longer.

On dual-screen devices, old-but-present art is updated automatically when Home opens, as before; **Play now** can stop
the update and use the old art. On single-screen devices there is no automatic art update or rebuild reminder.


## Stick LEDs (AYN Thor, Bifrost)

When [Bifrost](https://github.com/Pollux-MoonBench/Bifrost) is installed, the game app tells it what the stick rings
show while the game is in the foreground, the way Bifrost's Fallout 4 Pip-Boy plugin is fed: one `PIPBOY` effect in the
tablet's colour, re-sent every half second (Bifrost drops an override that is silent for 1.5 s), a dip when the second
screen is touched or the pause menu opens or closes, a short scramble when a save has loaded, a red stutter while
hearts are low, and a dip on a hit. Leaving the game (pause, another app, the launcher menu) hands the rings back at
once. Without Bifrost nothing runs: no thread, no read of the game, no broadcast.

| | Tears of the Kingdom | Breath of the Wild |
|---|---|---|
| In game | teal `#00ffc3` | blue `#00c4ff`; orange `#ff7b00` before the Sheikah Slate |
| Pause menu open | lime `#d4ea74` | light blue `#a6ebff`; `#ffb870` before the slate |
| Low hearts (at or under one heart or a quarter of the maximum; ends one heart above) | red `#ff1100`, stutter | the same |
| Title, loading, no state | nothing sent | nothing sent |

The state is the second screen's common game state (`runtime/companion_state.h`: status, in game, menu open, tablet
owned, hearts, page), which both games fill and the app reads through one getter (`CompanionNative.state`): from the
Eden Duo module's published values (`duo/duo_state.h`, one table per game, so with the companion library running);
in Tears of the Kingdom, until the library has a sample or without it, from the companion's snapshot reader (so only
with the companion on).
The table and the timing are in `android/java/com/aure/totk/BifrostLooks.java`; `tests/test_android_bifrost.py` runs
them against Bifrost's own validator and rate limiter rules.

In Bifrost: start its LED service and turn on **Allow third-party LED control**. Installing the game's plugin from
Bifrost's Plugin Store adds the brightness policy (30 % cap, scaled by the bottom screen's brightness) and a preset in
the game's colour for the moments the game sends nothing.

`totk.env`: `TOTK_BIFROST=0` turns the bridge off; `TOTK_BIFROST=2` uses Bifrost's public effects (breath, static,
pulse) instead of the Pip-Boy feed. There is no row for it in the launcher menu.
