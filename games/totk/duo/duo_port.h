/* The one interface between the port's runtime (libtotk.so, C) and the Eden Duo companion host (libduo_host.so, C++,
   GPL-3.0-or-later). libtotk fills a DuoPort and hands it to duo_host_start; the host never calls into libtotk any
   other way and libtotk calls nothing else in the host. Both sides check `abi` and `size`, so a library from another
   build is refused instead of misread.

   This header is part of the port (MIT, like the runtime): it describes services, it contains no Eden Duo code. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DUO_PORT_ABI 1u

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
