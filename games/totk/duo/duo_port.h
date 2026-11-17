/* The one interface between the port's runtime (libtotk.so, C) and the Eden Duo companion host (libduo_host.so, C++,
   GPL-3.0-or-later). libtotk fills a DuoPort and hands it to duo_host_start; the host never calls into libtotk any
   other way and libtotk calls nothing else in the host. Both sides check `abi` and `size`, so a library from another
   build is refused instead of misread. The table only grows: a new service is appended at the end, and DUO_PORT_ABI is
   raised with it (3: the item actions, after `state`).

   This header is part of the port (MIT, like the runtime): it describes services, it contains no Eden Duo code. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct CompanionState;             /* runtime/companion_state.h */

#define DUO_PORT_ABI 3u

/* One action the game's menus offer for an item (runtime/companion.h CompanionAction, the same three fields): id is
   "equip", "unequip", "eat", "drink", "use", "hold", "takeOut" or "fuseArrow"; reason is "" when enabled, otherwise
   why not. Both strings are static in the runtime. */
typedef struct DuoPortAction {
    const char *id;
    int enabled;
    const char *reason;
} DuoPortAction;

typedef struct DuoPort {
    uint32_t abi, size;                 /* DUO_PORT_ABI, sizeof(DuoPort) */
    /* The game's main module as loaded: original addresses are main_base + offset. */
    uint64_t main_base, main_size;
    uint64_t title_id;
    uint8_t build_id[32];               /* the NSO build id, zero-padded */
    /* Copy n bytes from address a of this process without faulting: 1 on success, 0 if any byte is unreadable. */
    int (*safe_read)(uint64_t a, void *out, size_t n);
    /* Read from a file of the game's RomFS ("Pack/Bootup.pack") or of its add-on content, by path relative to the
       root. out == NULL: the file's size. Otherwise up to n bytes at offset. -1: no such file (or no such volume).
       The empty path asks for the volume itself: 0 when it is mounted, -1 when not.
       Stateless: nothing stays open between calls. */
    int64_t (*romfs_read)(const char *path, uint64_t offset, void *out, uint64_t n);
    int64_t (*aoc_read)(const char *path, uint64_t offset, void *out, uint64_t n);
    /* One line into the game's log. level: 1 debug, 2 info, 3 warning, 4 error. */
    void (*log)(int level, const char *line);
    /* Folder that holds companion packages: <package_root>/<name>/dualscreen/manifest.json. NULL: none. */
    const char *package_root;
    /* Writable folder for what the host keeps between runs (persisted page flags, caches). */
    const char *data_dir;
    /* Presented game frames so far (for the companion's @perf values); may be NULL. */
    uint64_t (*frames)(void);
    uint32_t flags;                     /* DUO_PORT_* */
    /* Called by the host on each thread it starts itself, before the thread does anything else; the threads those
       create inherit what it sets. The port puts the companion on the cores the game's threads do not use. May be NULL. */
    void (*place_thread)(const char *name);
    /* The second screen's common game state (runtime/companion_state.h), from the host's tick thread each time a
       sample of the game is complete. The port keeps the last one for the app (CompanionNative.state). May be NULL. */
    void (*state)(const struct CompanionState *state);
    /* ---- ABI 3: item actions ----------------------------------------------------------------------------------------
       The runtime's own item actions (runtime/companion_actions.c: the game's pouch functions, called on the game's
       frame-end thread, never from the host) for a title module that offers them (Tears of the Kingdom's). All NULL:
       the runtime has none (Breath of the Wild, a build without the app's companion) and the page shows every action
       unavailable. Each may be called from any host thread. A pouch key is the companion snapshot's number (companion.h:
       0 weapons, 1 bows, 2 shields, 3 arrows, 4 armor, 5 materials, 6 food, 7 zonai, 8 keyItems). */
    /* The actions (at most 3) the game's menus offer now for the item at (key, slot) of the current pouch partition,
       by the game's own rules and the game state of the last frame end; 0: none, or the game is not in play. Asking
       keeps the frame-end refresh of that state running for a few seconds. */
    unsigned (*item_actions)(int key, int slot, DuoPortAction out[3]);
    /* Whether actions may run now (*ok), and if not why ("menu", "event", "loading", "saving", "stale", ...). */
    void (*action_state)(int *ok, const char **reason);
    /* The generation of the current pouch partition (bumped whenever anything in it changes); 0: not in play. */
    long long (*pouch_gen)(void);
    /* Queues one item action for the game's next frame end (companion_request): seq > 0, or -1 busy (one is pending
       or the game is paused), -2 refused now, -3 unsupported (another game build, the companion off), -4 invalid.
       category is the pouch key's name ("weapons", ...); actor, partition and pouch_gen name the item as it was chosen. */
    int (*request)(const char *action, const char *category, int slot, const char *actor, int partition,
                   long long pouch_gen);
    /* The latest request as {"seq":n,"state":"pending|done|refused|failed","reason":"..."} in out (NUL-terminated);
       its length, or 0 for any other seq or when it does not fit in cap. */
    size_t (*result)(int seq, char *out, size_t cap);
} DuoPort;

/* package_root is being filled by someone else (the app unpacking its APK): wait for <package_root>/.ready. */
#define DUO_PORT_WAIT_PACKAGE 1u

/* Starts the host: copies *port, starts its threads and returns at once; everything slow (reading the package,
   opening the module) happens on the host's own thread. 1 if started, 0 if refused (the reason is logged). */
int duo_host_start(const DuoPort *port);
/* The game presented a frame: lets the host run one tick. Never blocks (posts a semaphore). */
void duo_host_frame(void);

#ifdef __cplusplus
}
#endif
