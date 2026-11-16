/* The Eden Duo companion for Breath of the Wild (duo/): the runtime's side of duo/duo_port.h. Built into the Breath of
   the Wild runtime only (android/Makefile VERSION=botw160).

   The companion host is a separate library, libduo_host.so (C++, GPL-3.0-or-later: Eden Duo's runtime and its Breath
   of the Wild module), that this file opens with dlopen and talks to through one table of plain C functions. Nothing of
   it is linked into the runtime, and nothing here depends on it: without the library (a build that leaves it out, a
   load error) or with TOTK_DUO=0 the game starts and plays as before, and one line in the log says why the companion
   is off.

   The host gets: where main is and how large (runtime_duo_main, from the loader's patch pass), the fault-free reader
   (host_os.c host_safe_read), the stateless RomFS readers (romfs.c, aoc_romfs.c), the log, and one call per presented
   frame (runtime_duo_frame, from runtime_frame_step_update). The first of those calls opens the library, so the game's
   modules are loaded and published by then; the host does everything slow on its own thread. It only reads: this
   milestone installs no load plan and patches no game code. */
#ifdef GAME_BOTW
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "runtime.h"
#include "host_os.h"
#include "duo_link.h"
#include "companion_state.h"
#include "../duo/duo_port.h"
#include <dlfcn.h>
#include <pthread.h>
#if defined(__linux__)
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int64_t runtime_romfs_read_rel(const char *rel, uint64_t offset, void *out, uint64_t n);   /* romfs.c */
int64_t runtime_aoc_read_rel(const char *rel, uint64_t offset, void *out, uint64_t n);     /* aoc_romfs.c */
#ifdef TOTK_ANDROID_APP
const char *runtime_android_private_files(void);                                           /* frontend_android.c */
#endif

static uint64_t main_base, main_size;
static _Atomic uint64_t frames;
static void (*host_frame)(void);
static pthread_once_t start_once = PTHREAD_ONCE_INIT;

static int port_safe_read(uint64_t a, void *out, size_t n) { return host_safe_read(a, out, n); }
static void port_log(int level, const char *line) { fprintf(stderr, "[duo]%s %s\n", level >= 4 ? " error:" : level == 3 ? " warning:" : "", line); }
static uint64_t port_frames(void) { return atomic_load_explicit(&frames, memory_order_relaxed); }

/* The common game state (companion_state.h): the host's tick thread writes each complete sample here (DuoPort.state),
   the app reads the last one from any thread (companion_jni.c). One writer; a reader that meets a write in flight
   takes the sample again. */
static _Atomic uint32_t state_seq;            /* odd while a sample is being written */
static _Atomic int32_t state_words[7];
static _Atomic int64_t state_at = -1;         /* monotonic milliseconds of the last sample; -1: none yet */
static int64_t state_now_ms(void) { return (int64_t)(host_now_ns() / 1000000u); }
static void port_state(const CompanionState *s) {
    const int32_t words[7] = {s->status, s->in_game, s->menu_open, s->tablet_owned, s->life, s->max_life, s->page};
    atomic_fetch_add_explicit(&state_seq, 1, memory_order_acq_rel);
    for (int i = 0; i < 7; i++) atomic_store_explicit(&state_words[i], words[i], memory_order_relaxed);
    atomic_store_explicit(&state_at, state_now_ms(), memory_order_relaxed);
    atomic_fetch_add_explicit(&state_seq, 1, memory_order_acq_rel);
}
int runtime_duo_state(CompanionState *out, int64_t *age_ms) {
    for (int tries = 0; tries < 8; tries++) {
        uint32_t before = atomic_load_explicit(&state_seq, memory_order_acquire);
        if (before & 1) continue;
        int32_t w[7];
        for (int i = 0; i < 7; i++) w[i] = atomic_load_explicit(&state_words[i], memory_order_relaxed);
        int64_t at = atomic_load_explicit(&state_at, memory_order_relaxed);
        if (atomic_load_explicit(&state_seq, memory_order_acquire) != before) continue;
        if (at < 0) return 0;
        *out = (CompanionState){w[0], w[1], w[2], w[3], w[4], w[5], w[6]};
        *age_ms = state_now_ms() - at;
        return 1;
    }
    return 0;   /* a write was in flight every time: the caller asks again in a moment */
}

/* Where the companion's threads run. The host starts its first thread from the presenting thread, so without this
   every thread of the companion (its tick, its software rasteriser, its decoders) would inherit that thread's cores and
   priority and compete with the game's rendering for them. Each gets the cores the game's threads are not confined to
   (the process's set minus the caller's inherited one; all of them when the game is not confined, or with
   TOTK_DUO_AFFINITY=all) and a lower priority than any game thread. The page may draw a little slower for it; the game
   does not. */
static void port_place_thread(const char *name) {
#if defined(__linux__)
    cpu_set_t all, game, rest; int cores = -1; const char *decision = "inherited";
    const char *option = getenv("TOTK_DUO_AFFINITY");
    if (!sched_getaffinity(getpid(), sizeof all, &all) && !sched_getaffinity(0, sizeof game, &game)) {   /* getpid(): the main thread, never pinned */
        CPU_ZERO(&rest);
        for (int c = 0; c < CPU_SETSIZE; c++) if (CPU_ISSET(c, &all) && !CPU_ISSET(c, &game)) CPU_SET(c, &rest);
        int whole = (option && !strcmp(option, "all")) || !CPU_COUNT(&rest);
        const cpu_set_t *set = whole ? &all : &rest;
        if (!sched_setaffinity(0, sizeof *set, set)) { cores = CPU_COUNT(set); decision = whole ? "all cores" : "the cores the game leaves"; }
    }
    int nice_error = setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 4);
    fprintf(stderr, "[duo] thread %s: %s (%d), nice %d%s (TOTK_DUO_AFFINITY=all: every core)\n", name, decision, cores,
            getpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid)), nice_error ? " (setpriority failed)" : "");
#else
    (void)name;
#endif
}

/* libduo_host.so beside this library (the app's native library folder, or the folder of the headless program), then by
   name through the linker's search path. TOTK_DUO_LIB names another file (development). */
static void *open_host(void) {
    const char *named = getenv("TOTK_DUO_LIB");
    if (named && *named) return dlopen(named, RTLD_NOW | RTLD_LOCAL);
    Dl_info info; char path[1200];
    if (dladdr((void *)open_host, &info) && info.dli_fname) {
        const char *slash = strrchr(info.dli_fname, '/');
        if (slash && snprintf(path, sizeof path, "%.*s/libduo_host.so", (int)(slash - info.dli_fname), info.dli_fname) < (int)sizeof path) {
            void *lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
            if (lib) return lib;
        }
    }
    return dlopen("libduo_host.so", RTLD_NOW | RTLD_LOCAL);
}

static void start(void) {
    const char *option = getenv("TOTK_DUO"), *companion = getenv("TOTK_COMPANION");
    if (option && !strcmp(option, "0")) { fprintf(stderr, "[duo] off (TOTK_DUO=0)\n"); return; }
    if (companion && !strcmp(companion, "0")) { fprintf(stderr, "[duo] off (TOTK_COMPANION=0: no second screen content)\n"); return; }
    if (!main_base) { fprintf(stderr, "[duo] off: main was not loaded through the patch pass\n"); return; }
    void *lib = open_host();
    if (!lib) { fprintf(stderr, "[duo] off: no companion library (%s)\n", dlerror()); return; }
    int (*host_start)(const DuoPort *) = (int (*)(const DuoPort *))dlsym(lib, "duo_host_start");
    void (*frame)(void) = (void (*)(void))dlsym(lib, "duo_host_frame");
    if (!host_start || !frame) { fprintf(stderr, "[duo] off: libduo_host.so lacks its entry points\n"); return; }
    /* Where packages are unpacked (<root>/<name>/dualscreen/) and where the host keeps its own files. The app: its
       private files folder, filled from the APK by DuoSurfaceView.prepare. Elsewhere: TOTK_DUO_DIR. */
    static char root[1200], data[1200];
    DuoPort port = {.abi = DUO_PORT_ABI, .size = sizeof port, .main_base = main_base, .main_size = main_size,
        .title_id = 0x01007EF00011E000ull,
#ifdef GAME_BOTW160
        .build_id = {0x8E, 0x99, 0x78, 0xD5, 0x0B, 0xDD, 0x20, 0xB4, 0xC8, 0x39, 0x5A, 0x10, 0x6C, 0x27, 0xFF, 0xDE},   /* 1.6.0 main */
#endif
        .safe_read = port_safe_read, .romfs_read = runtime_romfs_read_rel, .aoc_read = runtime_aoc_read_rel,
        .log = port_log, .frames = port_frames, .place_thread = port_place_thread, .state = port_state};
    const char *dir = getenv("TOTK_DUO_DIR");
#ifdef TOTK_ANDROID_APP
    const char *files = runtime_android_private_files();
    if ((!dir || !*dir) && files) { snprintf(root, sizeof root, "%s/duo", files); snprintf(data, sizeof data, "%s/duo-data", files); port.flags |= DUO_PORT_WAIT_PACKAGE; }
    else
#endif
    if (dir && *dir) { snprintf(root, sizeof root, "%s", dir); snprintf(data, sizeof data, "%s/.data", dir); }
    if (*data) mkdir(data, 0700);
    port.package_root = *root ? root : NULL; port.data_dir = *data ? data : NULL;
    if (!host_start(&port)) { fprintf(stderr, "[duo] off: the companion library refused this runtime\n"); return; }
    __atomic_store_n(&host_frame, frame, __ATOMIC_RELEASE);
    fprintf(stderr, "[duo] companion library started (TOTK_DUO=0: off, TOTK_DUO=module: the module alone, values in this log)\n");
}

void runtime_duo_main(const Module *m) { main_base = (uint64_t)(uintptr_t)m->base; main_size = m->size; }

void runtime_duo_frame(void) {
    atomic_fetch_add_explicit(&frames, 1, memory_order_relaxed);
    pthread_once(&start_once, start);
    void (*frame)(void) = __atomic_load_n(&host_frame, __ATOMIC_ACQUIRE);
    if (frame) frame();
}
#endif
