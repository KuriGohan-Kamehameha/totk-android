/* The runtime half of the Eden Duo companion's route (DuoPort, duo/duo_port.h), run for real: runtime/duo_port.c and
   runtime/companion_jni.c as the app links them, a stand-in companion host (fake_duo_host.c, opened through
   TOTK_DUO_LIB as the app opens libduo_host.so) and stubs for what lies below them (companion_actions.c, the game).
   The probe calls through the table the host is handed and checks that each pointer reaches its runtime function with
   its arguments and gates: TOTK_COMPANION=0, the paused game, invalid requests, result()'s cap. It also checks the
   build id the host is given (main's note, or Breath of the Wild's known id when main has none) and the common state's
   hand-over. Linux only (as the app: ELF weak symbols, dlopen); tests/test_companion_gate.py builds it twice, for Tears
   of the Kingdom and with -DGAME_BOTW.

       duo_route_probe <fake_duo_host.so> <work dir> note|nonote */
#include "../../runtime/companion.h"
#include "../../runtime/runtime.h"
#include "../../runtime/host_os.h"
#include "../../runtime/duo_link.h"
#include "../../duo/duo_port.h"
#include <assert.h>
#include <dlfcn.h>
#include <jni.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---------- below the runtime: the game, companion_actions.c and the app's bridges, recorded ---------- */
static _Atomic int paused, views, item_actions_calls, action_state_calls, gen_calls, requests, results, inits, modules;
static struct { int key, slot; } last_view;
static struct { char action[32], category[32], actor[64]; int slot, partition; long long gen; } last_request;
static const char RESULT[] = "{\"seq\":42,\"state\":\"done\",\"reason\":\"\"}";

int runtime_android_game_paused(void) { return atomic_load(&paused); }   /* android_pause.h, ANDROID_PAUSE_EXTERN */
int host_safe_read(uint64_t a, void *out, size_t n) { (void)a; memset(out, 0, n); return 1; }
uint64_t host_now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
uint8_t *runtime_module_base(const char *name) { (void)name; atomic_fetch_add(&modules, 1); return (uint8_t *)(uintptr_t)0x10000; }
int64_t runtime_romfs_read_rel(const char *rel, uint64_t offset, void *out, uint64_t n) { (void)rel; (void)offset; (void)out; (void)n; return -1; }
size_t companion_snapshot(uintptr_t main_base, int detail, companion_read_fn read, CompanionBuf *out) {
    (void)main_base; (void)detail; (void)read; (void)out; return 0;
}
void companion_state(uintptr_t main_base, companion_read_fn read, CompanionState *out) { (void)main_base; (void)read; companion_state_clear(out); }
void runtime_input_key_synthetic(unsigned code, int down) { (void)code; (void)down; }
void runtime_android_present_transform_hint(uint64_t epoch, int hint) { (void)epoch; (void)hint; }
void runtime_android_text_done(JNIEnv *env, jclass cls, jlong serial, jbyteArray utf8) { (void)env; (void)cls; (void)serial; (void)utf8; }
void runtime_android_game_focus(JNIEnv *env, jclass cls, jboolean focused, jboolean single) { (void)env; (void)cls; (void)focused; (void)single; }
void runtime_android_game_input_focus(JNIEnv *env, jclass cls, jboolean top) { (void)env; (void)cls; (void)top; }
void runtime_android_motion(JNIEnv *env, jclass cls, jfloat ax, jfloat ay, jfloat az, jfloat gx, jfloat gy, jfloat gz) {
    (void)env; (void)cls; (void)ax; (void)ay; (void)az; (void)gx; (void)gy; (void)gz;
}
void runtime_android_motion_live(JNIEnv *env, jclass cls, jboolean on, jstring source) { (void)env; (void)cls; (void)on; (void)source; }
jint runtime_android_feedback_enabled(JNIEnv *env, jclass cls) { (void)env; (void)cls; return 0; }
_Atomic int runtime_companion_pending;
void (*_Atomic runtime_companion_tick)(uint64_t frame);
int runtime_fs_save_busy(void) { return 0; }
uint64_t runtime_now_ns(void) { return host_now_ns(); }
void companion_actions_init(const CompanionEnv *env) { assert(env); atomic_fetch_add(&inits, 1); }
void companion_tick(uintptr_t main_base, uint64_t frame) { (void)main_base; (void)frame; }
int companion_view(uintptr_t main_base, companion_read_fn rd, int key, int slot, CompanionView *v) {
    assert(main_base == 0x10000 && rd);
    atomic_fetch_add(&views, 1);
    last_view.key = key; last_view.slot = slot;
    memset(v, 0, sizeof *v);
    v->status = "ready"; v->ident = 0x1234;
    return 1;
}
/* Five offered, as a buggy rule set might: the port passes on three. */
unsigned companion_item_actions(int key, const CompanionView *v, CompanionAction out[3]) {
    assert(v && v->ident == 0x1234);
    atomic_fetch_add(&item_actions_calls, 1);
    out[0] = (CompanionAction){"eat", 1, ""};
    out[1] = (CompanionAction){"takeOut", 1, ""};
    out[2] = (CompanionAction){"fuseArrow", 0, key == 5 ? "notAiming" : "other"};
    return 5;
}
void companion_action_state(int *ok, const char **reason, int *pause_menu, int *aiming) {
    atomic_fetch_add(&action_state_calls, 1);
    *ok = 0; *reason = "menu"; *pause_menu = 1; *aiming = 0;
}
long long companion_pouch_gen(uint64_t ident) { atomic_fetch_add(&gen_calls, 1); return (long long)ident * 2 + 1; }
int companion_request(uintptr_t main_base, const char *action, const char *category, int slot, const char *actor,
                      int partition, long long pouch_gen) {
    assert(main_base == 0x10000);
    atomic_fetch_add(&requests, 1);
    snprintf(last_request.action, sizeof last_request.action, "%s", action);
    snprintf(last_request.category, sizeof last_request.category, "%s", category);
    snprintf(last_request.actor, sizeof last_request.actor, "%s", actor);
    last_request.slot = slot; last_request.partition = partition; last_request.gen = pouch_gen;
    return 42;
}
size_t companion_result(int seq, CompanionBuf *out) {
    atomic_fetch_add(&results, 1);
    if (seq != 42) return 0;
    out->data = strdup(RESULT);
    return out->data ? (out->len = strlen(out->data)) : 0;
}

#define COUNTS (atomic_load(&views) + atomic_load(&item_actions_calls) + atomic_load(&action_state_calls) + \
                atomic_load(&gen_calls) + atomic_load(&requests) + atomic_load(&results) + atomic_load(&modules))

/* main as the loader maps it: optionally one GNU build-id note at the start of its read-only data. */
static const uint8_t NOTE_ID[20] = {0x27, 0x71, 0x78, 0xB7, 0xDB, 0xA1, 0xB6, 0xD4, 0x94, 0x9A,
                                    0x84, 0x77, 0x8D, 0x4A, 0xBC, 0x58, 0xB3, 0x1B, 0x34, 0xF5};
static uint8_t main_image[256];

#ifndef GAME_BOTW
static void check_actions(const DuoPort *port) {
    /* each pointer reaches its own runtime function */
    DuoPortAction a[3] = {{0}};
    int before = atomic_load(&item_actions_calls);
    assert(port->item_actions(5, 2, a) == 3);
    assert(atomic_load(&item_actions_calls) == before + 1 && last_view.key == 5 && last_view.slot == 2);
    assert(!strcmp(a[0].id, "eat") && a[0].enabled == 1 && !strcmp(a[0].reason, ""));
    assert(!strcmp(a[1].id, "takeOut") && a[1].enabled == 1);
    assert(!strcmp(a[2].id, "fuseArrow") && a[2].enabled == 0 && !strcmp(a[2].reason, "notAiming"));
    assert(atomic_load(&inits) == 1 && atomic_load(&runtime_companion_tick));   /* the frame-end hook, without JNI_OnLoad */

    int ok = -1; const char *reason = NULL;
    before = atomic_load(&action_state_calls);
    port->action_state(&ok, &reason);
    assert(atomic_load(&action_state_calls) == before + 1 && ok == 0 && reason && !strcmp(reason, "menu"));

    before = atomic_load(&gen_calls);
    assert(port->pouch_gen() == 0x1234 * 2 + 1 && atomic_load(&gen_calls) == before + 1 && last_view.key == -1);

    before = atomic_load(&requests);
    assert(port->request("equip", "weapons", 3, "Weapon_Sword_002", 1, 99) == 42 && atomic_load(&requests) == before + 1);
    assert(!strcmp(last_request.action, "equip") && !strcmp(last_request.category, "weapons") && last_request.slot == 3 &&
           !strcmp(last_request.actor, "Weapon_Sword_002") && last_request.partition == 1 && last_request.gen == 99);
    assert(port->request("equip", "weapons", 3, NULL, 1, 99) == COMPANION_INVALID && atomic_load(&requests) == before + 1);

    /* result(): the text with its NUL when it fits in cap, otherwise 0 and the buffer untouched */
    const size_t len = strlen(RESULT);
    char buf[128];
    memset(buf, 'x', sizeof buf);
    assert(port->result(42, buf, sizeof buf) == len && !strcmp(buf, RESULT));
    memset(buf, 'x', sizeof buf);
    assert(port->result(42, buf, len + 1) == len && !strcmp(buf, RESULT));
    memset(buf, 'x', sizeof buf);
    assert(port->result(42, buf, len) == 0 && buf[0] == 'x');
    assert(port->result(42, buf, 0) == 0 && buf[0] == 'x');
    assert(port->result(42, NULL, sizeof buf) == 0);
    assert(port->result(7, buf, sizeof buf) == 0 && buf[0] == 'x');

    /* the paused game: busy, and nothing queued */
    atomic_store(&paused, 1);
    before = atomic_load(&requests);
    assert(port->request("equip", "weapons", 3, "Weapon_Sword_002", 1, 99) == COMPANION_BUSY && atomic_load(&requests) == before);
    atomic_store(&paused, 0);

    /* TOTK_COMPANION=0: every entry its "off" value, without reaching the actions or the game */
    setenv("TOTK_COMPANION", "0", 1);
    const int counts = COUNTS;
    assert(port->item_actions(5, 2, a) == 0);
    port->action_state(&ok, &reason);
    assert(ok == 0 && !strcmp(reason, "unsupported"));
    assert(port->pouch_gen() == 0);
    assert(port->request("equip", "weapons", 3, "Weapon_Sword_002", 1, 99) == COMPANION_UNSUPPORTED);
    assert(port->result(42, buf, sizeof buf) == 0);
    assert(COUNTS == counts);
    unsetenv("TOTK_COMPANION");
    assert(port->request("equip", "weapons", 3, "Weapon_Sword_002", 1, 99) == 42);
    assert(atomic_load(&inits) == 1);
    printf("pass: item actions reach the runtime through the port\n");
}
#endif

int main(int argc, char **argv) {
    assert(argc == 4);
    const int note = !strcmp(argv[3], "note");
    setenv("TOTK_DUO_LIB", argv[1], 1);
    setenv("TOTK_DUO_DIR", argv[2], 1);
    unsetenv("TOTK_DUO"); unsetenv("TOTK_COMPANION");
    /* the GNU note (namesz 4, descsz 20, type 3, "GNU"), then padding up to the data segment */
    if (note) {
        const uint32_t head[3] = {4, sizeof NOTE_ID, 3};
        memcpy(main_image + 32, head, sizeof head);
        memcpy(main_image + 44, "GNU", 4);
        memcpy(main_image + 48, NOTE_ID, sizeof NOTE_ID);
    }
    Module m = {.name = "main", .size = sizeof main_image, .base = main_image, .ro_off = 16, .data_off = 128};
    runtime_duo_main(&m);
    runtime_duo_frame();
    runtime_duo_frame();
    void *host = dlopen(argv[1], RTLD_NOW | RTLD_NOLOAD);
    assert(host);
    const DuoPort *(*kept)(void) = (const DuoPort *(*)(void))dlsym(host, "fake_duo_port");
    int (*frames)(void) = (int (*)(void))dlsym(host, "fake_duo_frames");
    assert(kept && frames);
    const DuoPort *port = kept();
    assert(port && frames() == 2);
    assert(port->abi == DUO_PORT_ABI && port->size == sizeof(DuoPort));
    assert(port->main_base == (uint64_t)(uintptr_t)main_image && port->main_size == sizeof main_image);
    assert(port->safe_read && port->romfs_read && port->log && port->frames && port->place_thread && port->state);
    assert(port->frames() == 2);
    assert(port->package_root && !strcmp(port->package_root, argv[2]));

    /* the build id: main's own note; without one, Breath of the Wild's known 1.6.0 id and Tears of the Kingdom none */
    uint8_t want[32] = {0};
#ifdef GAME_BOTW
    static const uint8_t BOTW160[16] = {0x8E, 0x99, 0x78, 0xD5, 0x0B, 0xDD, 0x20, 0xB4, 0xC8, 0x39, 0x5A, 0x10, 0x6C, 0x27, 0xFF, 0xDE};
    assert(port->title_id == 0x01007EF00011E000ull);
    memcpy(want, note ? NOTE_ID : BOTW160, note ? sizeof NOTE_ID : sizeof BOTW160);
#else
    assert(port->title_id == 0x0100F2C0115B6000ull);
    if (note) memcpy(want, NOTE_ID, sizeof NOTE_ID);
#endif
    assert(!memcmp(port->build_id, want, sizeof want));
    printf("pass: build id %s\n", note ? "from main's note" : "without a note");

    /* the common state: what the host hands over is what the app reads, fresh */
    CompanionState s, got;
    int64_t age = -1;
    assert(!runtime_duo_state(&got, &age));
    companion_state_clear(&s);
    s.status = COMPANION_STATE_READY; s.in_game = 1; s.menu_open = 1; s.life = 47; s.max_life = 80; s.page = 2;
    port->state(&s);
    assert(runtime_duo_state(&got, &age) && !memcmp(&got, &s, sizeof s) && age >= 0 && age < DUO_STATE_MAX_AGE_MS);
    printf("pass: common state handed over\n");

#ifdef GAME_BOTW
    /* Breath of the Wild: this runtime has no item actions; the host is given none */
    assert(!port->item_actions && !port->action_state && !port->pouch_gen && !port->request && !port->result);
    printf("pass: no item actions for this game\n");
#else
    assert(port->item_actions && port->action_state && port->pouch_gen && port->request && port->result);
    assert(!atomic_load(&inits));   /* nothing set up before the first call */
    check_actions(port);
#endif
    return 0;
}
