# Tears of the Kingdom game-state reader (MIT)

These files are TotK Native's dual-screen companion reader, from
[P1R4N351/botw-android](https://github.com/P1R4N351/botw-android) `games/totk/runtime/` at commit
`03b984c1767ca104f0ba9efeb5204cdbe9e5e7c3`, under the MIT licence in [`LICENSE.MIT`](LICENSE.MIT).
They are used by the GPL-3.0 module `../0100F2C0115B6000.cpp`, which is allowed: MIT is
compatible with the GPL.

| File | Origin |
|---|---|
| `companion_snapshot.c`, `companion.h`, `companion_state.h` | Copied unchanged. Keep them identical to the upstream copies, so fixes can be compared and taken across |
| `game_version.h` | The 1.4.3 lines of upstream `game_version.h` that `companion_snapshot.c` uses |
| `totk_gamedata.c`, `totk_gamedata.h` | Written for Eden Duo: GameData values the snapshot does not report (current stamina and `PlayerStatus.IsTired`, shrines found from `DungeonState.Dungeon000..151`), read with the same table layout. MIT |
| `totk_reader_ro.c`, `totk_reader_ro.h` | Written for Eden Duo: read-only stand-ins for the parts of upstream `companion_actions.c` the snapshot calls (the SHA-256 is copied verbatim; no item actions, no guest calls, no writes) |

The reader's offsets, guards and data layout are documented upstream in
`games/totk/android/APP.md` (*Dual-screen companion*) and `docs/companion-waist.md`.
