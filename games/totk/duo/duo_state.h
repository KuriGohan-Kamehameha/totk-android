/* Each game's side of the common second-screen state (runtime/companion_state.h): which of the numbers the Eden Duo
   title module publishes are kept, and what they mean in the contract's terms. One table per title module; the host
   picks the one for the title it runs (duo_state_table, by DuoPort.title_id), so nothing here is chosen at compile time.

   Like duo_port.h this header is part of the port (MIT): it names published values and holds the port's own mapping,
   and contains no Eden Duo code. duo_host.cpp keeps the values (LedTap) and hands the result to the runtime
   (DuoPort.state) once a sample is complete. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "../runtime/companion_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Breath of the Wild (01007EF00011E000.cpp) ----
   The kept values, in this order. The module writes botw.state.title and .loading twice in a sample (publish_states,
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

/* ---- Tears of the Kingdom (0100F2C0115B6000.cpp) ----
   The module reads the game through TotK Native's own reader (vendor/.../totk_reader, the runtime's
   companion_snapshot.c byte for byte) and publishes the contract's words directly: totk.status is already a
   COMPANION_STATE_* number (1 starting, 2 another build, 3 title, 5 a save loaded), and the state words are -1 while
   no save is loaded. */
enum { DUO_TOTK_STATUS, DUO_TOTK_MENU_OPEN, DUO_TOTK_LIFE, DUO_TOTK_MAX_LIFE, DUO_TOTK_VALUES };
#define DUO_TOTK_STATE_NAMES "totk.status", "totk.state.menu_open", "totk.state.life", "totk.state.max_life"

/* The status as published, anything else (no sample yet: 0) is "starting". In game exactly when a save is loaded; the
   Purah Pad is there from the start (tablet_owned stays 1). Hearts only when both are read and plausible, as above. */
static inline void duo_state_fill_totk(const int64_t v[DUO_TOTK_VALUES], CompanionState *out) {
    companion_state_clear(out);
    const int64_t status = v[DUO_TOTK_STATUS];
    out->status = status == COMPANION_STATE_UNSUPPORTED || status == COMPANION_STATE_TITLE || status == COMPANION_STATE_READY
                ? (int32_t)status : COMPANION_STATE_STARTING;
    if (out->status != COMPANION_STATE_READY) return;
    out->in_game = 1;
    out->menu_open = v[DUO_TOTK_MENU_OPEN] == 1;
    if (v[DUO_TOTK_LIFE] >= 0 && v[DUO_TOTK_MAX_LIFE] >= 4 && v[DUO_TOTK_MAX_LIFE] <= 400) {
        out->life = (int32_t)v[DUO_TOTK_LIFE]; out->max_life = (int32_t)v[DUO_TOTK_MAX_LIFE];
    }
}

/* ---- the table the host uses ---- */
enum { DUO_STATE_MAX_VALUES = 8 };   /* the longest table's count: what the host keeps per sample */
typedef char duo_state_max_values_check[(int)DUO_STATE_VALUES <= (int)DUO_STATE_MAX_VALUES &&
                                        (int)DUO_TOTK_VALUES <= (int)DUO_STATE_MAX_VALUES ? 1 : -1];
typedef struct DuoStateTable {
    uint64_t title_id;
    /* Hundreds of values pass LedTap each tick: only names starting with one of these are compared further. */
    const char *prefixes[2];
    size_t count;
    const char *const *names;
    void (*fill)(const int64_t *values, CompanionState *out);
} DuoStateTable;

static const char *const duo_state_names_botw[DUO_STATE_VALUES] = {DUO_STATE_NAMES};
static const char *const duo_state_names_totk[DUO_TOTK_VALUES] = {DUO_TOTK_STATE_NAMES};
static inline void duo_state_fill_botw_any(const int64_t *v, CompanionState *out) { duo_state_fill(v, out); }
static inline void duo_state_fill_totk_any(const int64_t *v, CompanionState *out) { duo_state_fill_totk(v, out); }
static const DuoStateTable duo_state_tables[] = {
    {0x01007EF00011E000ull, {"botw.state.", "botw.ready.heal."}, DUO_STATE_VALUES, duo_state_names_botw, duo_state_fill_botw_any},
    {0x0100F2C0115B6000ull, {"totk.status", "totk.state."}, DUO_TOTK_VALUES, duo_state_names_totk, duo_state_fill_totk_any},
};

/* The table of this title, NULL for a title the port has no mapping for (the host then hands over no state). */
static inline const DuoStateTable *duo_state_table(uint64_t title_id) {
    for (size_t i = 0; i < sizeof duo_state_tables / sizeof duo_state_tables[0]; i++)
        if (duo_state_tables[i].title_id == title_id) return &duo_state_tables[i];
    return NULL;
}
/* Whether `name` is one of the table's: its index, or -1. */
static inline int duo_state_index(const DuoStateTable *t, const char *name) {
    if (!t || !name || (strncmp(name, t->prefixes[0], strlen(t->prefixes[0])) != 0 &&
                        strncmp(name, t->prefixes[1], strlen(t->prefixes[1])) != 0)) return -1;
    for (size_t i = 0; i < t->count; i++)
        if (strcmp(name, t->names[i]) == 0) return (int)i;
    return -1;
}

#ifdef __cplusplus
}
#endif
