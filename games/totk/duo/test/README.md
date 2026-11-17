# Desktop tests of the companion host

`libduo_host` built for the desktop (`-DDUO_HOST_TEST=ON`), with the port's services faked: no game running, no device.
Each build links one title module (`-DDUO_TITLE`, as on Android), so each title has its own test program, built as
`duo_host_test`. A clang with C++23 `<format>` is needed (Homebrew's LLVM on a Mac, the system clang on Debian).

The package root for both is the vendored `../vendor/companions/packages`: the runtime finds the package whose title
id is the running game's.

## Breath of the Wild (`host_test.cpp`)

```
cmake -S games/totk/duo -B /tmp/duo-botw -DDUO_HOST_TEST=ON
cmake --build /tmp/duo-botw -j4
/tmp/duo-botw/duo_host_test games/totk/duo/vendor/companions/packages <romfs dir or /nonexistent> out.png [image.bin] [ticks]
```

`image.bin` is the 1.6.0 main as loaded (`make_fake_image.py <exefs/main> image.bin`); without it "main" is zeroes and
the page shows its waiting state. Exit 0: a page was published with non-background pixels and a common state was
handed over.

## Tears of the Kingdom (`totk_host_test.cpp`)

```
cmake -S games/totk/duo -B /tmp/duo-totk -DDUO_HOST_TEST=ON -DDUO_TITLE=0100F2C0115B6000
cmake --build /tmp/duo-totk -j4
/tmp/duo-totk/duo_host_test games/totk/duo/vendor/companions/packages <romfs dir> <main.bin> <out prefix> [scene]
```

- `main.bin`: the real 1.4.3 main as loaded (text, rodata and data at their offsets; the reader's code guard hashes
  its words). The test lays a fake game state out behind it (the layout of `games/totk/tests/companion/test_companion.c`).
- `romfs dir`: a folder of the player's own RomFS files, at least `Pack/ZsDic.pack.zs`,
  `Font/Font.Nin_NX_NVN.bfarc.zs`, `UI/LayoutArchive/Common.Product.140.Nin_NX_NVN.blarc.zs`,
  `Mals/USen.Product.142.sarc.zs`, the map tiles `map_tiles.txt` names and the actor packs of the fixture's items.
- `scene`: `ready` (default; also taps Inventory, an item and Map), `menu`, `event`, `title`, `sky`, `depths`, `food`,
  `overlays`, `loading`, `actions`, `actions-off`, `actions-errors`; a sixth argument makes the build id another game version's.
  - `actions`: item actions through a fake runtime (the DuoPort ABI 3 functions, standing in for
    `runtime/companion_actions.c`): the Inventory's Equip on the second weapon, a second tap while it is pending (dropped),
    a material's three actions with the runtime's reason on the disabled one, and the quick picker on the weapon corner
    (the equipped weapon preselected, another equipped from its confirm bar). Each request must reach the fake with the
    tapped item, and the module's result line must say done.
  - `actions-off`: the same taps without those functions, as in Eden Duo: every action is shown disabled ("Item actions
    need the TotK Native app"), a tap is refused and nothing is requested.
  - `actions-errors`: the fake runtime's other answers, one per tap on Equip: busy, refused now and unsupported from
    the request itself, then a request whose result ends refused (`itemMoved`) and one that ends failed (`timeout`).
    Each must show as the page's state, reason and result line ("Not done: ...", "Failed: ..."), and none may block
    the next tap. The runtime's own side of these answers (duo_port.c, companion_jni.c) is run by
    `tests/test_companion_gate.py` (`test_duo_route`, Linux).

Exit 0: every page was written and, except for `loading` and another build, the common state the host handed over
(`duo_state.h`, Tears of the Kingdom's table) is the fixture's: status, in game, menu, hearts 47/80. The PNGs contain
art decoded from the player's files: they stay on the machine that made them.

`tests/test_duo_host_desktop.py` builds and runs both, a few minutes each, when asked: `TOTK_DUO_DESKTOP=1` (and
`DUO_CXX` / `DUO_CC` for the compiler when the default one lacks C++23 `<format>`). The Tears of the Kingdom run also
needs the game files, named by `TOTK_DUO_TEST_MAIN` (main.bin) and `TOTK_DUO_TEST_ROMFS` (the folder); without them it
is skipped.
