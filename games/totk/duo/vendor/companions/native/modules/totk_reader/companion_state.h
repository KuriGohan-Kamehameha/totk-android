/* The second screen's common game state: the few facts every game's companion and the app's shell agree on, whatever
   draws the page (MIT, like the runtime; no code of any companion is in here).

   Tears of the Kingdom fills it from the snapshot reader (companion_snapshot.c companion_state); Breath of the Wild
   from the values its Eden Duo module publishes every tick (duo/duo_state.h, handed over through DuoPort.state and kept
   by duo_port.c). Java reads it through one getter, CompanionNative.state(long[]) (companion_jni.c), as
   COMPANION_STATE_FIELDS words in the order of companion_state_words below; com.aure.totk.companion.CompanionState
   names them. The stick-LED bridge and the second screen's shell both read that. */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* status */
enum {
    COMPANION_STATE_NONE = 0,       /* nothing read: no game yet, no sample, companion off */
    COMPANION_STATE_STARTING = 1,   /* the game is there, its state is not readable yet */
    COMPANION_STATE_UNSUPPORTED = 2,/* another build of the game than the reader knows */
    COMPANION_STATE_TITLE = 3,      /* the title screen */
    COMPANION_STATE_LOADING = 4,    /* a loading screen (only where the game side can tell it from the title) */
    COMPANION_STATE_READY = 5,      /* a save is loaded */
};

typedef struct CompanionState {
    int32_t status;                 /* COMPANION_STATE_* */
    int32_t in_game;                /* 1: a save is loaded and the world is running or paused in its own menu */
    int32_t menu_open;              /* 1: the game's pause menu (the pad or slate itself) is up */
    int32_t tablet_owned;           /* 0 only where the tablet can be missing and is (Breath of the Wild's start) */
    int32_t life, max_life;         /* quarter hearts; -1 when not read */
    int32_t page;                   /* the second screen's current page, as its screen numbers them; -1 unknown */
} CompanionState;

static inline void companion_state_clear(CompanionState *s) {
    s->status = COMPANION_STATE_NONE; s->in_game = 0; s->menu_open = 0; s->tablet_owned = 1;
    s->life = s->max_life = -1; s->page = -1;
}

/* What CompanionNative.state(long[]) returns: the fields in this order, then the sample's age in milliseconds. */
enum { COMPANION_STATE_FIELDS = 8 };
static inline void companion_state_words(const CompanionState *s, int64_t age_ms, int64_t out[COMPANION_STATE_FIELDS]) {
    out[0] = s->status; out[1] = s->in_game; out[2] = s->menu_open; out[3] = s->tablet_owned;
    out[4] = s->life; out[5] = s->max_life; out[6] = s->page; out[7] = age_ms;
}

#ifdef __cplusplus
}
#endif
