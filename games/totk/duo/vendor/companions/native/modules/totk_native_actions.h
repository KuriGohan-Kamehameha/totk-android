// SPDX-FileCopyrightText: Copyright 2026 P1R4N351
// SPDX-License-Identifier: GPL-3.0-or-later

// Item actions for the Tears of the Kingdom module (0100F2C0115B6000.cpp), offered only by a host that can carry
// them out: TotK Native's embedded Eden Duo port (P1R4N351/botw-android, games/totk/duo), whose runtime runs the
// game's own pouch functions on its frame-end thread (runtime/companion_actions.c, MIT).
//
// Eden Duo itself never provides this. The module makes no guest calls and writes no guest memory for an action: it
// shows the actions the game's menus would offer and hands a request to the table below. Without a table (every Eden
// build) each action is shown disabled with the reason "notAvailableYet" ("Item actions need the TotK Native app").
//
// How the table arrives: the module defines totk_native_port_bind_actions. Eden's loader never calls it (the module's
// shared library exports only eden_dsmod_get_module, eden_dsmod_get_extensions and eden_dsmod_get_font_extensions);
// the port, which links the module into its own library, calls it at configure time with the instance create returned.
// A null table unbinds. The table is copied; its functions must stay valid while the instance lives.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TOTK_NATIVE_ACTIONS_VERSION 1u

// One action the game's menus offer for an item: id is one of "equip", "unequip", "eat", "drink", "use", "hold",
// "takeOut", "fuseArrow"; reason is "" when enabled, otherwise why not ("weaponLocked", "menu", "noStock", ...).
// Both strings are static in the host.
typedef struct TotkNativeAction {
    const char* id;
    int32_t enabled;
    const char* reason;
} TotkNativeAction;

// Pouch keys are the companion snapshot's, numbered: 0 weapons, 1 bows, 2 shields, 3 arrows, 4 armor, 5 materials,
// 6 food, 7 zonai, 8 keyItems. Every function may be called from any thread.
typedef struct TotkNativeActions {
    uint32_t version;     // TOTK_NATIVE_ACTIONS_VERSION
    uint32_t struct_size; // sizeof(TotkNativeActions)
    void* userdata;
    // The actions (at most 3) for the item now at (key, slot) of the current pouch partition, by the game's own
    // rules and the game state of the last frame end. 0: none, or the game is not in play.
    uint32_t (*item_actions)(void* userdata, int32_t key, int32_t slot, TotkNativeAction out[3]);
    // Whether actions may run now, and if not why ("menu", "event", "loading", "saving", "stale", ...).
    void (*action_state)(void* userdata, int32_t* ok, const char** reason);
    // The host's generation of the current pouch partition (bumped whenever anything in it changes). A request names
    // the generation its item was chosen from; a request from an older one is refused ("itemMoved").
    int64_t (*pouch_gen)(void* userdata);
    // Queue one action for the game's next frame end: seq > 0, or -1 busy (one is pending), -2 refused now,
    // -3 unsupported (another game build, the companion off), -4 invalid.
    int32_t (*request)(void* userdata, const char* action, const char* category, int32_t slot, const char* actor,
                       int32_t partition, int64_t pouch_gen);
    // The latest request's state as {"seq":n,"state":"pending|done|refused|failed","reason":"..."} in out (NUL-
    // terminated); returns its length, 0 for any other seq or when it does not fit.
    size_t (*result)(void* userdata, int32_t seq, char* out, size_t cap);
} TotkNativeActions;

void totk_native_port_bind_actions(void* instance, const TotkNativeActions* actions);

#ifdef __cplusplus
}
#endif
