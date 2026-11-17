/* Read-only stand-ins for the parts of TotK Native's companion_actions.c that companion_snapshot.c calls, for a host
   that offers no guest calls (Eden Duo on Android). Nothing here calls or writes the game:
   - companion_sha256_hex: the same SHA-256 (copied verbatim from companion_actions.c);
   - companion_item_actions: no item actions (an Eden Duo page shows the inventory read-only);
   - companion_action_state: the game state the snapshot's "actionState" reports, kept by totk_reader_refresh() from
     companion_view() (memory reads only) with companion_actions.c's own state rules, minus the parts that need calls;
   - companion_pouch_gen: the same generation counter.
   MIT, like the reader (LICENSE.MIT). */
#include "companion.h"
#include "totk_reader_ro.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>

/* ---------- SHA-256 (FIPS 180-4), for the short prologues only ---------- */
static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
#define ROR(x, n) ((x) >> (n) | (x) << (32 - (n)))
static void sha256_block(uint32_t h[8], const uint8_t *p) {
    uint32_t w[64], s[8];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++)
        w[i] = w[i - 16] + (ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ w[i - 15] >> 3) + w[i - 7] + (ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ w[i - 2] >> 10);
    memcpy(s, h, sizeof s);
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = s[7] + (ROR(s[4], 6) ^ ROR(s[4], 11) ^ ROR(s[4], 25)) + ((s[4] & s[5]) ^ (~s[4] & s[6])) + K256[i] + w[i];
        uint32_t t2 = (ROR(s[0], 2) ^ ROR(s[0], 13) ^ ROR(s[0], 22)) + ((s[0] & s[1]) ^ (s[0] & s[2]) ^ (s[1] & s[2]));
        memmove(s + 1, s, 7 * sizeof *s); s[4] += t1; s[0] = t1 + t2;
    }
    for (int i = 0; i < 8; i++) h[i] += s[i];
}
/* hex digest of n bytes (n < 2^29); declared in companion.h */
void companion_sha256_hex(const void *data, size_t n, char out[65]) {
    const uint8_t *m = data;
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t tail[128] = {0};
    size_t full = n / 64 * 64, rest = n - full;
    for (size_t i = 0; i < full; i += 64) sha256_block(h, m + i);
    memcpy(tail, m + full, rest); tail[rest] = 0x80;
    size_t len = rest < 56 ? 64 : 128; uint64_t bits = (uint64_t)n * 8;
    for (int i = 0; i < 8; i++) tail[len - 1 - i] = (uint8_t)(bits >> 8 * i);
    for (size_t i = 0; i < len; i += 64) sha256_block(h, tail + i);
    for (int i = 0; i < 8; i++) snprintf(out + 8 * i, 9, "%08x", h[i]);
}

enum { BIT_CALC = 0 };   /* player state +0x658: clear while a save loads or the scene changes (companion_actions.c) */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static const char *reason = "stale";
static int pause_menu;
static uint64_t gen_ident; static long long gen;

void totk_reader_refresh(uintptr_t main_base, companion_read_fn rd, TotkReaderState *out) {
    CompanionView v;
    memset(out, 0, sizeof *out);
    companion_view(main_base, rd, -1, 0, &v);
    const char *r;
    if (strcmp(v.status, "ready"))
        r = !strcmp(v.status, "unsupported") || !strcmp(v.status, "title") || !strcmp(v.status, "noPlayer") ? v.status : "loading";
    else if (v.receiver == -1) r = "noPlayer";
    else if (v.loading) r = "loading";
    else if (v.event) r = "event";
    else if (!(v.player_flags >> BIT_CALC & 1)) r = "loading";
    else if (v.pause_menu) r = "menu";
    else r = "";
    out->status = v.status; out->reason = r;
    out->ready = !strcmp(v.status, "ready");
    out->pause_menu = out->ready && v.pause_menu;
    out->event = out->ready && v.event;
    out->loading = out->ready && (v.loading || !(v.player_flags >> BIT_CALC & 1));
    pthread_mutex_lock(&lock); reason = r; pause_menu = out->pause_menu; pthread_mutex_unlock(&lock);
}

unsigned companion_item_actions(int key, const CompanionView *v, CompanionAction out[3]) { (void)key; (void)v; (void)out; return 0; }
void companion_action_state(int *ok, const char **r, int *menu, int *aiming) {
    pthread_mutex_lock(&lock);
    *r = reason; *ok = 0; *menu = pause_menu; *aiming = 0;   /* ok stays 0: no actions are offered here */
    pthread_mutex_unlock(&lock);
}
long long companion_pouch_gen(uint64_t ident) {
    pthread_mutex_lock(&lock);
    if (ident != gen_ident) { gen_ident = ident; gen++; }
    long long g = gen;
    pthread_mutex_unlock(&lock);
    return g;
}
