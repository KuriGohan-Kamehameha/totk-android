/* totk_reader_ro.c: the game state the Eden Duo module needs besides the snapshot (MIT, LICENSE.MIT). */
#pragma once
#include "companion.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    const char *status;   /* companion_view's status: "ready", "title", "starting", "unsupported", "noPlayer" */
    const char *reason;   /* "" in play; otherwise why the world is not running ("event", "menu", "loading", ...) */
    int ready, pause_menu, event, loading;
} TotkReaderState;
/* Reads the state once (memory reads only) and keeps it for companion_action_state; call before companion_snapshot. */
void totk_reader_refresh(uintptr_t main_base, companion_read_fn rd, TotkReaderState *out);
#ifdef __cplusplus
}
#endif
