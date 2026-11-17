/* The one Tears of the Kingdom build this reader is for (1.4.3), cut down from TotK Native's per-version header
   (games/totk/runtime/game_version.h on P1R4N351/botw-android): only what companion_snapshot.c uses. MIT, like the
   reader. */
#pragma once
#define TOTK_GAME_V143 1
#define TOTK_GAME_VERSION "1.4.3"
#define GAME_ADDR_JOIN(x) GAME_ADDR_##x
#define GAME_ADDR(x) GAME_ADDR_JOIN(x)
#define GAME_ACTOR_NAME 0x218            /* actor name pointer */
#define GAME_ACTOR_POS 0x2b4             /* live position, 3 floats */
#define GAME_ACTOR_ROT 0x2c0             /* live rotation, row-major 3x3 */
#define GAME_ADDR_0x4663f08 0x3add238    /* the scene module slot (1.0.0 offset 0x4663f08 mapped to 1.4.3) */
