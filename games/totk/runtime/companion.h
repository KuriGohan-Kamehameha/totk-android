/* Dual-screen companion (Android app): a JSON snapshot of the running game's state for the second screen, and item
   actions that call the game's own pouch functions.
   companion_snapshot.c reads the game's GameData tables and player actor straight from memory; companion_actions.c runs
   item requests on the game's frame-end thread; companion_jni.c hands both to the Java companion
   (com.aure.totk.companion.CompanionNative) and turns its button shortcuts into synthetic controller presses. The
   snapshot schema and the actions are documented in android/APP.md ("Dual-screen companion"). */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "companion_state.h"

/* Copies n bytes from address a without faulting on a bad address: 1 on success (host_safe_read in the app). */
typedef int (*companion_read_fn)(uint64_t a, void *out, size_t n);

typedef struct { char *data; size_t len, cap; int failed; } CompanionBuf;

enum { COMPANION_LIVE = 0, COMPANION_FULL = 1 };

/* The snapshot of the main module mapped at main_base (0 while the game has not been loaded) as one NUL-terminated JSON
   object in out (grown with realloc; free out->data when done). detail: COMPANION_LIVE (status and player) or
   COMPANION_FULL (also inventory with each item's actions, actionState, pouchGen, equipment, sages, map pins). Returns
   the JSON length, 0 if memory ran out. Safe to call from any thread; every guest read goes through rd, and nothing in
   the game is written or called. */
size_t companion_snapshot(uintptr_t main_base, int detail, companion_read_fn rd, CompanionBuf *out);

/* The common second-screen state (companion_state.h) of the same game, from the same reads and with the same care as
   the live snapshot: its status ("ready" is in game; a sample that moved while it was taken is the title), the hearts
   the snapshot's player section shows, and the pause-menu flag of its actionState. The page is left unknown (-1): the
   Java screen owns it. Safe from any thread; nothing in the game is written or called. */
void companion_state(uintptr_t main_base, companion_read_fn rd, CompanionState *out);

/* MurmurHash3 x86_32 with seed 0: the game's GameData flag and enum value hash. */
uint32_t companion_hash(const char *s);

/* ---------- item actions (companion_actions.c) ---------- */

/* What the host provides. call runs the game function at fn with up to 6 integer arguments in x0..x5 and returns x0;
   call_f runs one with x0 and s0 and returns s0; wr writes n bytes of game memory (1 on success). All three are only
   ever used on the game's frame-end thread (companion_tick). save_busy: the runtime is writing save data
   (fs_host.c). set_pending(1) asks for companion_tick at the next frame ends, set_pending(0) stops asking; it is only
   called with the actions lock held, so a request never loses its wake-up. */
typedef struct {
    companion_read_fn rd;
    uint64_t (*call)(uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5);
    float (*call_f)(uint64_t fn, uint64_t a0, float s0);
    int (*wr)(uint64_t a, const void *src, size_t n);
    int (*save_busy)(void);
    uint64_t (*now_ms)(void);
    void (*set_pending)(int on);
} CompanionEnv;
void companion_actions_init(const CompanionEnv *env);

/* Request codes (CompanionNative.request): seq > 0 accepted, or one of these. */
enum { COMPANION_BUSY = -1, COMPANION_REFUSED = -2, COMPANION_UNSUPPORTED = -3, COMPANION_INVALID = -4 };
/* From the companion's poller thread: queue one item action for the next frame end. action: equip, unequip, eat, drink,
   use, hold, takeOut, fuseArrow; category: the snapshot's pouch key; slot, actor, partition, pouch_gen: the item and
   snapshot it was chosen from. One request at a time: anything while one is pending is dropped (COMPANION_BUSY). */
int companion_request(uintptr_t main_base, const char *action, const char *category, int slot, const char *actor,
                      int partition, long long pouch_gen);
/* {"seq":n,"state":"pending|done|refused|failed","reason":"…"} for the latest request, 0 (nothing written) for any
   other seq. */
size_t companion_result(int seq, CompanionBuf *out);
/* On the game's frame-end thread while set_pending is on: re-checks and runs the queued request, reads back the outcome
   of the one already run, and refreshes the game-side state the snapshot's action rules use. A different main_base than
   the last tick's (the runtime relaunched the game) ends a pending request as "failed"/"teardown". */
void companion_tick(uintptr_t main_base, uint64_t frame);

/* ---------- between the snapshot and the actions (internal) ---------- */
enum { COMPANION_CATS = 9, COMPANION_NAME = 64 };
/* The snapshot's pouch keys are numbered weapons, bows, shields, arrows, armor, materials, food, zonai, keyItems:
   companion_category gives a key's number (-1 unknown), companion_category_game the game's pouch category for it. */
int companion_category(const char *key);
int companion_category_game(int key);
const char *companion_category_key(int key);

/* One item and the game state around it, read from memory (no calls), for the action rules and their re-check. */
typedef struct {
    const char *status;                     /* the snapshot's status; the rest is only valid when "ready" */
    unsigned part;                          /* current pouch partition */
    uint64_t ident;                         /* identity of the whole current pouch partition (names, durability, fuse,
                                               stock, equipment); never 0 when read */
    uint64_t pouch;                         /* PouchMgr * */
    uint64_t actor;                         /* the player actor (0 before "ready"): a new one means a reload */
    int32_t receiver;                       /* *(pouch+0x478): the player's pouch actor, -1 none */
    uint64_t player_flags;                  /* *(player state+0x658) */
    int event, loading;                     /* an event flow runs / the scene's loading pause */
    int pause_menu;                         /* *(pouch+0x1fcc0) != 0: the pause menu (any of its tabs) is open */
    int32_t life, max_life, break_life;     /* PlayerStatus, quarter hearts */
    int zonai_unlocked;                     /* GameData bool 0x1c96382f (pause menu's Zonai Hold) */
    /* the item at (key, slot) */
    char name[COMPANION_NAME], fuse[COMPANION_NAME];
    int32_t life_item, stock;               /* durability (weapons, bows, shields) / count (stackables), INT32_MIN unknown */
    int equipped;                           /* weapons, bows, shields, arrows: EquipIndex == slot; armor: worn */
    int32_t equip_index;                    /* weapons, bows, shields, arrows: EquipIndex */
    int has_effect;                         /* food: Effect.Type is not None */
} CompanionView;
/* key: a pouch key number, or -1 for the state only. Returns 1 when the game is "ready". */
int companion_view(uintptr_t main_base, companion_read_fn rd, int key, int slot, CompanionView *v);
/* SHA-256 of n bytes as lowercase hex (companion_actions.c): the code guards compare hashes, never game code. */
void companion_sha256_hex(const void *data, size_t n, char out[65]);

typedef struct { const char *id; int enabled; const char *reason; } CompanionAction;
/* The actions the game's own menus offer for one item, from the game-side state of the last frame-end tick (a view of
   the item read just before); returns how many (at most 3). Unknown item facts are requested from the next tick. */
unsigned companion_item_actions(int key, const CompanionView *v, CompanionAction out[3]);
/* The snapshot's actionState and pouchGen. companion_pouch_gen bumps the generation when ident differs from the last
   one it was given and returns it. Both keep the game-side state refreshing while the companion reads. */
void companion_action_state(int *ok, const char **reason, int *pause_menu, int *aiming);
long long companion_pouch_gen(uint64_t ident);
