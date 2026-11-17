/* totk_gamedata.c: GameData values the Eden Duo page needs beyond companion_snapshot.c (MIT, LICENSE.MIT). */
#pragma once
#include "companion.h"
#ifdef __cplusplus
extern "C" {
#endif
enum { TOTK_SHRINES = 152 };   /* DungeonState.Dungeon000 .. Dungeon151 */
typedef struct {
    int stamina_ok;            /* PlayerStatus.Stamina and PlayerStatus.MaxStamina read */
    float stamina, max_stamina;
    int tired_ok, tired;       /* PlayerStatus.IsTired (stamina ran out; regenerating) */
    int shrines_ok;            /* every DungeonState.DungeonNNN enum read */
    int shrines_found;         /* states Open, Enter or Clear */
    int shrines_clear;         /* state Clear */
} TotkGameData;
/* Reads the values above (memory reads only). Call only after companion_snapshot reported "ready" for this main_base:
   that is the check that main is the 1.4.3 build whose GameDataMgr slot this uses. Returns 0 when the manager is not
   there; fields that could not be read keep their _ok at 0. with_shrines: also read the 152 shrine states (about 450
   reads; the module does it on its slower full samples). */
int totk_gamedata_read(uintptr_t main_base, companion_read_fn rd, int with_shrines, TotkGameData *out);
#ifdef __cplusplus
}
#endif
