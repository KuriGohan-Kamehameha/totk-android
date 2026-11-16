/* Breath of the Wild's side of the common second-screen state (runtime/companion_state.h): which of the numbers the
   Eden Duo title module publishes are kept, and what they mean in the contract's terms.

   Like duo_port.h this header is part of the port (MIT): it names published values and holds the port's own mapping,
   and contains no Eden Duo code. duo_host.cpp keeps the values (LedTap) and hands the result to the runtime
   (DuoPort.state) once a sample is complete. */
#pragma once
#include <stdint.h>
#include "../runtime/companion_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The kept values, in this order. The module writes botw.state.title and .loading twice in a sample (publish_states,
   then the UI context's answer): the last one stands, as in the runtime's own snapshot. */
enum { DUO_STATE_VALID, DUO_STATE_TITLE, DUO_STATE_LOADING, DUO_STATE_MENU_OPEN, DUO_STATE_SLATE, DUO_STATE_HEAL_VALID,
       DUO_STATE_LIFE, DUO_STATE_MAX_LIFE, DUO_STATE_VALUES };
#define DUO_STATE_NAMES \
    "botw.state.valid", "botw.state.title", "botw.state.loading", "botw.state.menu_open", "botw.state.slate_obtained", \
    "botw.ready.heal.valid", "botw.ready.heal.life", "botw.ready.heal.max"

/* One complete sample as the contract. A sample the module did not vouch for (valid != 1) is "starting": the game is
   there, its state is not. In game: valid, not the title, not a loading screen. The Sheikah Slate counts as owned
   unless the module read that it is not (-1, not read, counts as owned). Hearts only when the module's heal advisor
   vouches for them and they are plausible. The page stays unknown: the native page keeps its own. */
static inline void duo_state_fill(const int64_t v[DUO_STATE_VALUES], CompanionState *out) {
    companion_state_clear(out);
    if (v[DUO_STATE_VALID] != 1) { out->status = COMPANION_STATE_STARTING; return; }
    out->status = v[DUO_STATE_TITLE] != 0 ? COMPANION_STATE_TITLE
                : v[DUO_STATE_LOADING] != 0 ? COMPANION_STATE_LOADING : COMPANION_STATE_READY;
    if (out->status != COMPANION_STATE_READY) return;
    out->in_game = 1;
    out->menu_open = v[DUO_STATE_MENU_OPEN] == 1;
    out->tablet_owned = v[DUO_STATE_SLATE] != 0;
    if (v[DUO_STATE_HEAL_VALID] == 1 && v[DUO_STATE_LIFE] >= 0 && v[DUO_STATE_MAX_LIFE] >= 4 && v[DUO_STATE_MAX_LIFE] <= 400) {
        out->life = (int32_t)v[DUO_STATE_LIFE]; out->max_life = (int32_t)v[DUO_STATE_MAX_LIFE];
    }
}

#ifdef __cplusplus
}
#endif
