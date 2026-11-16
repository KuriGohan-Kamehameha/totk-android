/* Breath of the Wild game-code patches. The lock-ordering fix and the real-time frame step on Android. The other functions the shared
   runtime calls answer "not available" until BotW's own sites are ported (MacNativeGames game_patches_botw.c). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "game_patches.h"
#include "game_version.h"
#include "duo_link.h"

/* ---- The real-time frame step, from MacNativeGames games/totk/runtime/game_patches_botw.c (MIT, commit 49ff9ec),
   lines 59-449: the 1.6.0 sites, checked word by word before they are replaced. Host differences only: the clock, no
   iOS build-time patching, and no trap handler for TOTK_PAD_LOG. The Mac file's other patches are not here: the font
   raster and capture sizes scale by canvas height / 720 (1.0 at this port's 1280x720), and the output size, aspect
   ratio, MetalFX and VR parts are for the Mac's window. ---- */
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>
#define CODE_PATCHED_AT_BUILD 0
#define PAD_LOG_HOOK 0
static int ios_code(void) { return 0; }
uint32_t runtime_thread_current_handle(void);
static uint64_t host_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static void put_words(Module *m, uint64_t off, const uint32_t *words, size_t n, const char *what) { (void)what; memcpy(m->base + off, words, 4 * n); }
static void put_word(Module *m, uint64_t off, uint32_t word, const char *what) { put_words(m, off, &word, 1, what); }
static uint32_t adrp_reg(unsigned rd, uint64_t site, uint64_t target);
static uint32_t branch_to(uint64_t site, uint64_t target);

/* Real-time frame step (TOTK_REALTIME=0: as shipped). ksys::VFR (singleton pointer main+0x2d1e170, built at main+0x183eb98)
   keeps one frame ratio at +0x3c: the frame's length in 30 fps frames. Every per-core step the game reads derives from it:
   delta frames (+0x108, about 1,500 readers) = time-speed multiplier x ratio, delta time (+0x138) = that x 1/30 s (+0x158).
   The per-frame update main+0x1c01180 sets the ratio from the present interval (interval / 2: 1.0 at the shipped 30 fps)
   and writes +0x3c only when it changes, so the game ran at twice its speed at 60 fps. Two of its instructions now take the
   ratio from the loader's data page after main, which the host refreshes at each present with the measured frame time x 30
   (the page layout of Tears of the Kingdom's patch: low float ratio, high float seconds):
     main+0x1c0120c  fcmp s1, s0       -> adrp x9, <data page>
     main+0x1c01214  b.eq #0x1c01220   -> ldr s1, [x9]
   and the store of s1 to +0x3c after them now runs every frame. The interval bookkeeping (+0x28 changed, +0x30 / +0x34
   interval ratio and interval, +0x38 / +0x40 previous) stays as shipped.
   One more reader steps by the interval itself instead of the ratio: a fade (0 -> 1 in 5 frames at 30 fps; main+0x1821548
   reads the VFR's interval, +0x48 per core) adds max(interval, 1) x 0.5 x 0.2 per frame. It now adds ratio x 0.2, the
   same value at the shipped 30 fps:
     main+0x1821568  ldr w8, [x8]               -> adrp x8, <data page>
     main+0x182156c  cmp w8, #0                 -> ldr s0, [x8]
     main+0x1821570  csinc w8, w8, wzr, ne      -> nop
     main+0x1821574  ucvtf s0, w8               -> nop
     main+0x1821588  fmul s0, s0, s11 (x 0.5)   -> nop
   (x8 is reloaded two instructions later, the flags are not read again.) The UI's animation step is the same sum
   (main+0x18522e0: max(interval, 1) x 0.5 into +0xb20 of the layout system): at 120 fps
   the game still asks for interval 2, so menus and the HUD animated four times too fast (2026-10-20). It now takes the
   ratio too: main+0x18522ec ldr w8 -> adrp x8, main+0x18522f0 cmp -> ldr s0, [x8], and the csinc / ucvtf / fmul become
   nops (s1 keeps its 0.5; x8 and the flags are not read again before they are set).
   Physics: the frame loop calls the physics step (main+0x1c9c920) with the frame time, the frame ratio and the time
   speed, and the step runs round(6 x ratio) solver substeps (main+0x1c9c958): six at the shipped 30 fps, two at 120 fps,
   where staggered enemies' ragdolls blew apart and flew off (2026-10-20). The ratio argument
   is now always 1.0 (main+0x1618480 ldr s9, [x8] -> fmov s9, #1.0): six substeps every frame, the frame time still sets
   the step. MacNativeGames games/botw/scripts/
   verify_realtime_patch.py runs both with Unicorn. */
static _Atomic uint64_t *frame_step_data;
static uint8_t *frame_step_main;
static uint64_t last_present_ns, report_ns, report_frames;
static double frame_samples[5], report_seconds, report_game, report_short_ms, report_long_ms;
static unsigned frame_sample_count, frame_sample_next, report_short, report_long;
static _Atomic uint32_t pinned_step_us;
#ifdef GAME_BOTW160
enum { VFR_INSTANCE = 0x2d1e170, VFR_RATIO = 0x3c, VFR_DELTA_TIME = 0x138 };
#else   /* 1.9.0: the singleton pointer is main+0x1e93890, the object's layout is the same */
enum { VFR_INSTANCE = 0x1e93890, VFR_RATIO = 0x3c, VFR_DELTA_TIME = 0x138 };
#endif
static _Atomic uint32_t *pad_tick;   /* data page +8: 1 when the next frame's game time crosses a 1/30 s boundary */
static double pad_phase;
static uint64_t report_ticks;
static int g_pad_log_on; static _Atomic uint64_t g_pad_dir_until;   /* TOTK_PAD_LOG */
#ifdef GAME_BOTW160
static uint8_t *g_pad_main;
#endif

static int realtime_enabled(void) { const char *e = getenv("TOTK_REALTIME"); return ios_code() || !(e && !strcmp(e, "0")); }
static uint64_t frame_step_bits(float seconds) {
    float ratio = seconds * 30.0f; uint32_t lo, hi;
    memcpy(&lo, &ratio, 4); memcpy(&hi, &seconds, 4);
    return (uint64_t)lo | ((uint64_t)hi << 32);
}
/* Diagnostics (TOTK_INPUT_FILE "framestep <ms>"): pin the frame step to a fixed length, 0 back to real time. */
void runtime_frame_step_pin(double ms) {
    uint32_t us = ms > 0 ? (uint32_t)(ms * 1000.0 + 0.5) : 0;
    atomic_store(&pinned_step_us, us);
    if (frame_step_data && us) atomic_store_explicit(frame_step_data, frame_step_bits((float)(us / 1e6)), memory_order_release);
    if (!us) fprintf(stderr, "[botw] frame step back to real time\n");
    else fprintf(stderr, "[botw] frame step pinned at %.3f ms (ratio %.3f, diagnostics)%s\n", us / 1000.0, us / 1e6 * 30.0,
                 frame_step_data ? "" : " -- but the real-time patch is off, nothing changes");
}
/* Every 30 s one line: frames and their real time, the game time the steps added up to, what the game actually reads (the
   VFR's ratio and core 0's delta time), and the intervals outside 1/120..1/15 s with what clamping each of them on its own
   would have added or dropped. */
static void frame_step_report(uint64_t now_ns) {
    if (!report_ns) { report_ns = now_ns; return; }
    if (now_ns - report_ns < 30000000000ull) return;
    float ratio = -1, delta = -1;
    uint64_t vfr = *(const uint64_t *)(const void *)(frame_step_main + VFR_INSTANCE);
    if (vfr) {
        memcpy(&ratio, (const void *)(uintptr_t)(vfr + VFR_RATIO), 4);
        uint64_t p = *(const uint64_t *)(uintptr_t)(vfr + VFR_DELTA_TIME);
        if (p) memcpy(&delta, (const void *)(uintptr_t)p, 4);
    }
    double span = (double)(now_ns - report_ns) * 1e-9;
    char game[96];
    if (atomic_load(&pinned_step_us)) snprintf(game, sizeof game, "step pinned");
    else snprintf(game, sizeof game, "steps add up to %.2f s (%.4f of the frames' time)", report_game, report_seconds > 0 ? report_game / report_seconds : 0);
    char ticks[64] = "";
    if (pad_tick) snprintf(ticks, sizeof ticks, "; pad counts %.1f ticks/s", report_ticks / span);
    fprintf(stderr, "[botw] real-time frame step: %llu frames in %.1f s (%.1f fps, %.2f ms apart on average), %s; the game's VFR ratio "
                    "%.3f, delta time %.2f ms; %u intervals under 1/120 s (clamping each: +%.0f ms), %u over 1/15 s (-%.0f ms)%s\n",
            (unsigned long long)report_frames, span, report_frames / span, report_frames ? report_seconds / report_frames * 1e3 : 0, game,
            ratio, delta * 1e3, report_short, report_short_ms, report_long, report_long_ms, ticks);
    report_ns = now_ns; report_frames = report_ticks = 0; report_seconds = report_game = report_short_ms = report_long_ms = 0; report_short = report_long = 0;
}
static void frame_step_measure(uint64_t now_ns) {
    frame_step_report(now_ns);
    uint64_t prev = last_present_ns;
    last_present_ns = now_ns;
    if (!prev) return;
    double dt = (double)(now_ns - prev) * 1e-9;
    if (dt > 0.25 || dt < 0.001) { frame_sample_count = frame_sample_next = 0; return; }
    report_frames++; report_seconds += dt;
    if (dt < 1.0 / 120.0) { report_short++; report_short_ms += (1.0 / 120.0 - dt) * 1e3; }
    if (dt > 1.0 / 15.0) { report_long++; report_long_ms += (dt - 1.0 / 15.0) * 1e3; }
    if (atomic_load_explicit(&pinned_step_us, memory_order_relaxed)) return;
    /* The raw intervals are averaged and the average is clamped, not each interval: at about 95 fps under the 120 Hz
       clock, intervals shorter than 1/120 s alternate with longer ones, and raising each short one to 1/120 s made the
       game run 6% fast (measured: botw.md). Averaged, the steps add up to the real time at any frame rate (no lower bound:
       a display above 120 Hz presents more often, and a floor of 1/120 s made the game run fast there); only an average
       above 1/15 s is cut, and a hitch is spread over the next frames instead. */
    frame_samples[frame_sample_next++ % 5] = dt;
    if (frame_sample_count < 5) frame_sample_count++;
    double sum = 0;
    for (unsigned i = 0; i < frame_sample_count; i++) sum += frame_samples[i];
    double step = sum / frame_sample_count;
    if (step > 1.0 / 15.0) step = 1.0 / 15.0;
    report_game += step;
    atomic_store_explicit(frame_step_data, frame_step_bits((float)step), memory_order_release);
}
/* The pad tick follows the game time the next frame adds (the step on the page, pinned or measured): one tick per 1/30 s,
   at most one per frame (below 30 fps the counts advance once a frame, as shipped). */
static void pad_tick_advance(void) {
    uint32_t hi = (uint32_t)(atomic_load_explicit(frame_step_data, memory_order_relaxed) >> 32);
    float seconds; memcpy(&seconds, &hi, 4);
    pad_phase += (double)seconds * 30.0;
    uint32_t tick = 0;
    if (pad_phase >= 1.0) { tick = 1; pad_phase -= 1.0; if (pad_phase >= 1.0) pad_phase = 0; }
    report_ticks += tick;
    atomic_store_explicit(pad_tick, tick, memory_order_release);
    if (g_pad_log_on) {
        uint64_t now = host_ns();
        if (now < atomic_load(&g_pad_dir_until)) fprintf(stderr, "[pad] %.4f present thread %#x: tick %u (step %.2f ms)\n", now * 1e-9, runtime_thread_current_handle(), tick, seconds * 1e3);
    }
}
void runtime_frame_step_update(uint64_t now_ns) {
    runtime_duo_frame();   /* the second-screen companion ticks with the game's frames (duo_port.c); never blocks */
    if (!frame_step_data || !now_ns) return;
    frame_step_measure(now_ns);
    if (pad_tick) pad_tick_advance();
}
static uint32_t adrp_reg(unsigned rd, uint64_t site, uint64_t target) {
    int64_t pages = (int64_t)(target >> 12) - (int64_t)(site >> 12);
    if (pages < -(1 << 20) || pages >= (1 << 20)) runtime_fail("real-time frame-step data outside ADRP range");
    uint32_t imm = (uint32_t)pages & 0x1fffff;
    return 0x90000000u | ((imm & 3u) << 29) | ((imm >> 2) << 5) | (rd & 31u);
}
static uint32_t branch_to(uint64_t site, uint64_t target) {   /* b <target>, both offsets in main */
    int64_t delta = (int64_t)target - (int64_t)site;
    if ((delta & 3) || delta < -(1 << 27) || delta >= (1 << 27)) runtime_fail("patch branch outside range");
    return 0x14000000u | ((uint32_t)(delta >> 2) & 0x3ffffffu);
}
#ifdef GAME_BOTW160
/* Pad repeat and hold counts by real time (TOTK_PAD_REPEAT=0: as shipped; botw.md). sead::ControllerBase::
   updateDerivativeParams_ (main+0x126e3b0) counts the frames each button has been held (+0x30 + 4 i) and raises its
   repeat bit (+0x10) when the count reaches the button's delay (+0xb0 + i) and then every pulse (+0xd0 + i) frames.
   The menus set delay 6 / pulse 2 for the stick and d-pad directions (main+0x1a40958) or take them from their data
   (main+0x12143e4); at 120 fps the cursor moved four times as fast as at the shipped 30 fps. The loop over the buttons
   (main+0x126e490..0x126e528, 38 instructions) is rewritten in place: a held button is counted, and its repeat tested,
   only on frames whose game time crosses a 1/30 s boundary (the tick at data page +8, from the same step as the VFR
   ratio), so counts, delays and pulses are 1/30 s again. Released buttons still reset at once; the trigger and release
   bits are computed before the loop and stay per frame. New registers: x16 (the page, then the tick) and x17 (the count's
   address; the shipped loop reused x11 for it and reloaded the hold mask every iteration). MacNativeGames
   games/botw/scripts/verify_pad_repeat_patch.py runs both loops with Unicorn. */
enum { PAD_LOOP = 0x126e490, PAD_LOOP_WORDS = 38, PAD_ADRP = 7 };
static const uint32_t pad_loop_shipped[PAD_LOOP_WORDS] = {
    0xb9410268u, 0x7100051fu, 0xb901166bu, 0xb900127fu, 0x5400044bu, 0x2a1f03e8u, 0xaa1f03e9u, 0x320003eau, 0x14000002u, 0xb941166bu,
    0x1ac9214cu, 0x6a0c017fu, 0x54000180u, 0x8b09026eu, 0x394341cdu, 0x8b090a6bu, 0x340001edu, 0xb940316fu, 0x3942c1ceu, 0x6b0e01eeu,
    0x540000e1u, 0x2a0c0108u, 0xb9001268u, 0x14000008u, 0x8b090a6bu, 0xb900317fu, 0x14000008u, 0x54000089u, 0x1acd09cfu, 0x1b0db9edu,
    0x34fffeedu, 0xb940316cu, 0x1100058cu, 0xb900316cu, 0xb981026bu, 0x91000529u, 0xeb0b013fu, 0x54fffc8bu,
};
static const uint32_t pad_loop_ticked[PAD_LOOP_WORDS] = {
    0xb9410268u, /* ldr   w8, [x19, #0x100]   (button count) */
    0x7100051fu, /* cmp   w8, #1 */
    0xb901166bu, /* str   w11, [x19, #0x114]  (hold) */
    0xb900127fu, /* str   wzr, [x19, #0x10]   (repeat = 0) */
    0x5400044bu, /* b.lt  end */
    0x2a1f03e8u, /* mov   w8, wzr */
    0xaa1f03e9u, /* mov   x9, xzr */
    0,           /* adrp  x16, <data page> */
    0xb9400a10u, /* ldr   w16, [x16, #8]      (tick) */
    0x5280002au, /* mov   w10, #1 */
    0x1ac9214cu, /* loop: lsl w12, w10, w9 */
    0x8b090a71u, /* add   x17, x19, x9, lsl #2 */
    0x6a0c017fu, /* tst   w11, w12 */
    0x54000180u, /* b.eq  released */
    0x34000290u, /* cbz   w16, next           (no tick: count and repeat unchanged) */
    0x8b09026eu, /* add   x14, x19, x9 */
    0x394341cdu, /* ldrb  w13, [x14, #0xd0]   (pulse) */
    0x340001cdu, /* cbz   w13, count */
    0xb940322fu, /* ldr   w15, [x17, #0x30]   (count) */
    0x3942c1ceu, /* ldrb  w14, [x14, #0xb0]   (delay) */
    0x6b0e01eeu, /* subs  w14, w15, w14 */
    0x540000c1u, /* b.ne  past */
    0x2a0c0108u, /* repeat: orr w8, w8, w12 */
    0xb9001268u, /* str   w8, [x19, #0x10] */
    0x14000007u, /* b     count */
    0xb900323fu, /* released: str wzr, [x17, #0x30] */
    0x14000008u, /* b     next */
    0x54000089u, /* past: b.ls count */
    0x1acd09cfu, /* udiv  w15, w14, w13 */
    0x1b0db9edu, /* msub  w13, w15, w13, w14 */
    0x34ffff0du, /* cbz   w13, repeat */
    0xb940322cu, /* count: ldr w12, [x17, #0x30] */
    0x1100058cu, /* add   w12, w12, #1 */
    0xb900322cu, /* str   w12, [x17, #0x30] */
    0xb981026fu, /* next: ldrsw x15, [x19, #0x100] */
    0x91000529u, /* add   x9, x9, #1 */
    0xeb0f013fu, /* cmp   x9, x15 */
    0x54fffcabu, /* b.lt  loop */
};
/* The pause menu's own count (main+0x1214474..0x1214498, 9 instructions, in the menu update main+0x12125b0): each
   direction has a record {mask, 60, countdown, delay 9, pulse 3, fast delay 60, fast pulse 1} (built at main+0x1207d0c);
   while the direction is held, every frame takes one off the countdown, and at zero the controllers' delay / pulse
   switch to the fast pair (at the shipped 30 fps the held count is 60 at that moment, so the cursor goes on at one step
   per 1/30 s without a pause). Per frame, at 120 fps the switch came at 0.5 s with the held count at 25, and the cursor
   stopped until the count reached 60. The countdown now takes one off only on a tick as well (same registers; the
   record address is formed in two adds instead of four instructions, and the "already negative" test is dropped: a
   negative countdown keeps falling by one per tick, which is harmless):
     add x10, x27, x10, lsl #4 / add x11, x10, #0x64000 / adrp x13, <page> / ldr w13, [x13, #8] / cbz w13, done /
     ldr w12, [x11, #0xa00] / sub w13, w12, #1 / str w13, [x11, #0xa00] / cbnz w12, done */
enum { MENU_CD = 0x1214474, MENU_CD_WORDS = 9, MENU_CD_ADRP = 2 };
static const uint32_t menu_cd_shipped[MENU_CD_WORDS] = { 0x8b0a136au, 0x5289400bu, 0x72a000cbu, 0x8b0b014bu, 0xb940016cu, 0x37f80accu, 0x5100058du, 0xb900016du, 0x35000a6cu };
static const uint32_t menu_cd_ticked[MENU_CD_WORDS] = { 0x8b0a136au, 0x9141914bu, 0, 0xb94009adu, 0x34000aedu, 0xb94a016cu, 0x5100058du, 0xb90a016du, 0x35000a6cu };
static void pad_log_arm(Module *m) {
    const char *l = getenv("TOTK_PAD_LOG");
    if (!l || !*l || !strcmp(l, "0") || ios_code()) return;
    if (!PAD_LOG_HOOK) { fprintf(stderr, "[botw] TOTK_PAD_LOG: its trap handler is not ported to Android; ignored\n"); return; }
    /* diagnostics: a brk on the loop's first instruction (ldr w8, [x19, #0x100], the same shipped and patched), see botw_pad_hook */
    uint32_t b = 0xd4200000u | (0x7c0u << 5); memcpy(m->base + PAD_LOOP, &b, 4);
    g_pad_main = m->base; g_pad_log_on = 1;
    fprintf(stderr, "[botw] TOTK_PAD_LOG: every controller update while a direction is held, and the tick at each present\n");
}
static void patch_pad_repeat(Module *m, uint64_t page) {
    const char *e = getenv("TOTK_PAD_REPEAT");
    if (!ios_code() && e && !strcmp(e, "0")) { fprintf(stderr, "[botw] pad repeat and hold counts per frame, as shipped (TOTK_PAD_REPEAT=0)\n"); pad_log_arm(m); return; }
    for (unsigned i = 0; !CODE_PATCHED_AT_BUILD && i < PAD_LOOP_WORDS; i++) {
        uint32_t w; memcpy(&w, m->base + PAD_LOOP + 4 * i, 4);
        if (w != pad_loop_shipped[i]) {
            fprintf(stderr, "[botw] pad repeat main+%#x: found %08x expected %08x\n", PAD_LOOP + 4 * i, w, pad_loop_shipped[i]);
            runtime_fail("pad repeat by real time requires the 1.6.0 sead::ControllerBase loop");
        }
    }
    for (unsigned i = 0; !CODE_PATCHED_AT_BUILD && i < MENU_CD_WORDS; i++) {
        uint32_t w; memcpy(&w, m->base + MENU_CD + 4 * i, 4);
        if (w != menu_cd_shipped[i]) {
            fprintf(stderr, "[botw] menu countdown main+%#x: found %08x expected %08x\n", MENU_CD + 4 * i, w, menu_cd_shipped[i]);
            runtime_fail("pad repeat by real time requires the 1.6.0 pause menu countdown");
        }
    }
    pad_tick = (_Atomic uint32_t *)(uintptr_t)(page + 8);
    atomic_store(pad_tick, 1);
    for (unsigned i = 0; i < PAD_LOOP_WORDS; i++) {
        uint32_t w = i == PAD_ADRP ? adrp_reg(16, (uintptr_t)m->base + PAD_LOOP + 4 * i, page) : pad_loop_ticked[i];
        put_word(m, PAD_LOOP + 4 * i, w, "pad repeat");
    }
    for (unsigned i = 0; i < MENU_CD_WORDS; i++) {
        uint32_t w = i == MENU_CD_ADRP ? adrp_reg(13, (uintptr_t)m->base + MENU_CD + 4 * i, page) : menu_cd_ticked[i];
        put_word(m, MENU_CD + 4 * i, w, "menu countdown");
    }
    fprintf(stderr, "[botw] pad repeat and hold counts by real time: held buttons count once per 1/30 s of game time, and so does "
                    "the pause menu's fast-repeat countdown (%u + %u instructions, all verified; TOTK_PAD_REPEAT=0: once per frame, as shipped)\n",
            PAD_LOOP_WORDS, MENU_CD_WORDS);
    pad_log_arm(m);
}
/* Sound-link timers by real time (TOTK_SLINK_REALTIME=0: as shipped). xlink2 steps its asset timers (delays, durations)
   by the frame rate its system holds at +0x84. The effect system's rate gets the VFR's ratio each frame
   (main+0x1be7918); the sound system's ([main+0x2cc2028]) gets max(raw ratio, 1.0) (main+0x1c59fac..0x1c5a014),
   which is the ratio on the Switch, where a frame is never shorter than 1/30 s. Above 30 fps the 1.0 made every SLink
   timer count rendered frames: Cryonis' placement marker, emitted again when its sound event ends after about 24
   frames, restarted 4-5 times a second at 120 fps instead of about 1.3, so the ice block preview never grew (measured
   2026-10-23; 1.3 a second at 120 fps after the fix). The store now takes the unclamped ratio from the frame-step page;
   the audio frame values computed from the clamped number just before it are unchanged. x9 and s0 are free there (a call follows):
     main+0x1c5a014  str s0, [x8, #0x84]  -> b <veneer>
     veneer          adrp x9, <page> / ldr s0, [x9] / str s0, [x8, #0x84] / b main+0x1c5a018 */
static void patch_slink_rate(Module *m, uint64_t page) {
    const char *e = getenv("TOTK_SLINK_REALTIME");
    if (!ios_code() && e && !strcmp(e, "0")) { fprintf(stderr, "[botw] sound-link timers at least one 30 fps frame per frame, as shipped (TOTK_SLINK_REALTIME=0)\n"); return; }
    enum { SITE = 0x1c5a014, BACK = 0x1c5a018 };
    static const struct { uint32_t off, original; } check[] = {
        {0x1c59fa8u, 0xbd400100u}, {0x1c59facu, 0x1e2e1001u}, {0x1c59fb0u, 0x1e214800u},
        {0x1c5a00cu, 0x90008348u}, {0x1c5a010u, 0xf9401508u}, {SITE, 0xbd008500u}, {BACK, 0x91090300u}, {0x1c5a01cu, 0x94133c99u},
    };
    for (size_t i = 0; !CODE_PATCHED_AT_BUILD && i < sizeof check / sizeof *check; i++) {   /* the phone's text is patched already */
        uint32_t w; memcpy(&w, m->base + check[i].off, 4);
        if (w != check[i].original) {
            fprintf(stderr, "[botw] sound-link rate main+%#x: found %08x expected %08x\n", check[i].off, w, check[i].original);
            runtime_fail("sound-link timers by real time require the 1.6.0 rate update instructions");
        }
    }
    const uint64_t stub = (m->text_end + 15) & ~15ULL;
    const uint32_t veneer[] = {
        adrp_reg(9, (uintptr_t)m->base + stub, page),
        0xbd400120u,   /* ldr s0, [x9]         (the measured ratio) */
        0xbd008500u,   /* str s0, [x8, #0x84]  (the sound system's rate) */
        branch_to(stub + 12, BACK),
    };
    if (stub + sizeof veneer > ((m->text_end + 0xfff) & ~0xfffULL)) runtime_fail("sound-link veneer layout");
    for (size_t i = 0; !CODE_PATCHED_AT_BUILD && i < sizeof veneer; i++) if (m->base[stub + i]) runtime_fail("sound-link veneer padding not empty");
    put_words(m, stub, veneer, sizeof veneer / sizeof *veneer, "sound-link veneer");
    put_word(m, SITE, branch_to(SITE, stub), "sound-link rate");
    fprintf(stderr, "[botw] sound-link timers follow the measured frame ratio (veneer +%#llx; TOTK_SLINK_REALTIME=0: as shipped)\n",
            (unsigned long long)stub);
}
/* Blends start with one 30 fps frame (#1266; TOTK_BLEND_FIRST_STEP=0: as shipped). The camera's blend timer
   (main+0xdddfd0, on a mode's +408: step = 1 / (frames x scale), progress += step x the VFR's ratio, t = (1 - cos(pi x
   progress)) / 2) is stepped with a ratio that is never under 1 on the Switch, so a blend's first frame is at least
   1 / length of the way: a one-frame blend is a cut. The real-time step gives ratios of 0.2-0.3, and the first frame of
   such a cut showed a camera between the two shots that the game never shows. The cutscene after Vah Medoh's main
   control unit cuts to a long shot whose near distance (1000 m) is set at once while fov and position blend; its first
   frame at t 0.107 (fov 0.79 instead of 0.081) made the near-plane collision sphere (main+0xdd6440) 800 m wide, a Havok
   collector took 131,072 terrain points, the 16 MB array did not fit HavokMainHeap (45 MiB) and the game wrote through
   null (measured 2026-11-03). Now the first step of a blend (progress still 0) takes max(ratio, 1), so its first frame is
   the Switch's first frame; later steps follow the measured ratio. s1 is reloaded by the next original instruction and
   s8 holds 1.0 (main+0xdddfec):
     main+0xdde044  ldr s0, [x8]  -> b <veneer>
     veneer         ldr s0, [x8] / ldr s1, [x19, #20] / fcmp s1, #0.0 / b.ne +8 / fmax s0, s0, s8 / b main+0xdde048 */
static void patch_blend_first_step(Module *m, uint64_t stub) {
    const char *e = getenv("TOTK_BLEND_FIRST_STEP");
    if (!ios_code() && e && !strcmp(e, "0")) { fprintf(stderr, "[botw] camera blends start with the measured ratio, as shipped (TOTK_BLEND_FIRST_STEP=0)\n"); return; }
    enum { SITE = 0xdde044, BACK = 0xdde048 };
    static const struct { uint32_t off, original; } check[] = {
        {0xdddfe4u, 0x2d410400u},   /* ldp s0, s1, [x0, #8]   (length, scale) */
        {0xdddfecu, 0x1e2e1008u},   /* fmov s8, #1.0 */
        {0xdddffcu, 0xbd001009u},   /* str s9, [x0, #16]      (step) */
        {SITE, 0xbd400100u},        /* ldr s0, [x8]           (the VFR's ratio) */
        {BACK, 0xbd401661u},        /* ldr s1, [x19, #20]     (progress) */
        {0xdde04cu, 0x1e282020u},   /* fcmp s1, s8 */
        {0xdde050u, 0x1e200920u},   /* fmul s0, s9, s0 */
        {0xdde090u, 0xbd001661u},   /* str s1, [x19, #20] */
    };
    for (size_t i = 0; !CODE_PATCHED_AT_BUILD && i < sizeof check / sizeof *check; i++) {
        uint32_t w; memcpy(&w, m->base + check[i].off, 4);
        if (w != check[i].original) {
            fprintf(stderr, "[botw] camera blend main+%#x: found %08x expected %08x\n", check[i].off, w, check[i].original);
            runtime_fail("blends starting with one 30 fps frame require the 1.6.0 camera blend timer");
        }
    }
    const uint32_t veneer[] = {
        0xbd400100u,   /* ldr s0, [x8]          */
        0xbd401661u,   /* ldr s1, [x19, #20]    */
        0x1e202028u,   /* fcmp s1, #0.0         */
        0x54000041u,   /* b.ne +8               */
        0x1e284800u,   /* fmax s0, s0, s8       */
        branch_to(stub + 20, BACK),
    };
    if (stub + sizeof veneer > ((m->text_end + 0xfff) & ~0xfffULL)) runtime_fail("camera blend veneer layout");
    for (size_t i = 0; !CODE_PATCHED_AT_BUILD && i < sizeof veneer; i++) if (m->base[stub + i]) runtime_fail("camera blend veneer padding not empty");
    put_words(m, stub, veneer, sizeof veneer / sizeof *veneer, "camera blend veneer");
    put_word(m, SITE, branch_to(SITE, stub), "camera blend first step");
    fprintf(stderr, "[botw] camera blends start with one 30 fps frame, then follow the measured ratio (veneer +%#llx; TOTK_BLEND_FIRST_STEP=0: as shipped)\n",
            (unsigned long long)stub);
}
static void patch_realtime(Module *m) {
    /* The loader reserved a writable page immediately after main's image; nothing else uses it in this game. */
    uint64_t data_off = (m->file_size + 0x3fff) & ~0x3fffULL;
    if (data_off + 0x4000 != m->size || data_off & 0xfff) runtime_fail("real-time frame-step page layout");
    /* Every word of both sequences, the replaced ones and their neighbours, must be the 1.6.0 original. */
    static const struct { uint32_t off, original; } check[] = {
        {0x1c0120cu, 0x1e202020u}, {0x1c01210u, 0x3900a008u}, {0x1c01214u, 0x54000060u}, {0x1c01218u, 0xbd003c01u},
        {0x1821564u, 0xf9400108u}, {0x1821568u, 0xb9400108u}, {0x182156cu, 0x7100011fu}, {0x1821570u, 0x1a9f1508u},
        {0x1821574u, 0x1e230100u}, {0x1821578u, 0x1e2c100bu}, {0x182157cu, 0xb0006f68u}, {0x1821580u, 0xbd409109u},
        {0x1821584u, 0xbd435ee1u}, {0x1821588u, 0x1e2b0800u}, {0x182158cu, 0x1e290800u},
        {0x18522ecu, 0xb9400108u}, {0x18522f0u, 0x7100011fu}, {0x18522f4u, 0x1a9f1508u}, {0x18522f8u, 0x1e230100u},
        {0x18522fcu, 0x1e2c1001u}, {0x1852300u, 0x1e210800u}, {0x1852304u, 0xbd0b22a0u},
        {0x161847cu, 0x1e290908u}, {0x1618480u, 0xbd400109u}, {0x1618484u, 0xb94c3680u},
    };
    for (size_t i = 0; !CODE_PATCHED_AT_BUILD && i < sizeof check / sizeof *check; i++) {
        uint32_t w; memcpy(&w, m->base + check[i].off, 4);
        if (w != check[i].original) {
            fprintf(stderr, "[botw] real-time frame step main+%#x: found %08x expected %08x\n", check[i].off, w, check[i].original);
            runtime_fail("real-time frame step requires the 1.6.0 VFR update instructions");
        }
    }
    const uint64_t page = (uintptr_t)m->base + data_off;
    const uint32_t nop = 0xd503201fu;
    const struct { uint32_t off, word; } put[] = {
        {0x1c0120cu, adrp_reg(9, (uintptr_t)m->base + 0x1c0120c, page)}, {0x1c01214u, 0xbd400121u},   /* ldr s1, [x9] */
        {0x1821568u, adrp_reg(8, (uintptr_t)m->base + 0x1821568, page)}, {0x182156cu, 0xbd400100u},   /* ldr s0, [x8] */
        {0x1821570u, nop}, {0x1821574u, nop}, {0x1821588u, nop},
        {0x18522ecu, adrp_reg(8, (uintptr_t)m->base + 0x18522ec, page)}, {0x18522f0u, 0xbd400100u},   /* ldr s0, [x8] */
        {0x18522f4u, nop}, {0x18522f8u, nop}, {0x1852300u, nop},
        {0x1618480u, 0x1e2e1009u},   /* fmov s9, #1.0: six physics substeps per frame at any frame rate */
    };
    for (size_t i = 0; i < sizeof put / sizeof *put; i++) put_word(m, put[i].off, put[i].word, "real-time step");
    frame_step_main = m->base;
    frame_step_data = (_Atomic uint64_t *)(void *)(m->base + data_off);
    atomic_store(frame_step_data, frame_step_bits(1.0f / 30.0f));
    fprintf(stderr, "[botw] real-time frame step: the VFR ratio, one interval-stepped fade and the UI animation step follow the measured frame time, physics keeps six substeps a frame "
                    "(%zu instructions, %zu verified); initial 33.33 ms, sampled at each present (TOTK_REALTIME=0: as shipped)\n",
            sizeof put / sizeof *put, sizeof check / sizeof *check);
    patch_pad_repeat(m, page);
    patch_slink_rate(m, page);
    patch_blend_first_step(m, ((m->text_end + 15) & ~15ULL) + 16);   /* after the sound-link veneer (4 words) */
}
#endif   /* GAME_BOTW160 */
/* Exclusive loads with acquire (TOTK_EXCL_ACQUIRE=0: as shipped), from MacNativeGames game_patches_botw.c: sead's spin
   locks in this game take the lock with a plain ldxr / stxr and only put a dmb ish before the release, so nothing orders
   the loads inside the critical section after the lock is taken. The Switch's Cortex-A57 rarely satisfies such a load
   early; wide out-of-order cores do (Apple's, and the Thor's Cortex-X3 / A715): two threads read the same "next job"
   index of a sead::JobQueue and run the same job twice. On the Thor that was a model job reading a vertex-buffer list
   entry another thread had consumed (nvnBufferMap of a garbage pointer from main+0x14ef6f4, about 20 s into the boot).
   Every plain ldxr / ldxp that opens a real exclusive window (a store-exclusive on the same base register within 16
   instructions) gets the acquire bit (ldaxr / ldaxp): one bit, the same registers and size, only stronger ordering. */
static int excl_ld(uint32_t w) { return (w & 0x3ffffc00u) == 0x085f7c00u || (w & 0xbfff8000u) == 0x887f0000u; }   /* ldxr{b,h} / ldxp */
static int excl_st(uint32_t w) { return (w & 0x3fe07c00u) == 0x08007c00u || (w & 0xbfe00000u) == 0x88200000u; }   /* st{l}xr{b,h} / st{l}xp */
static unsigned excl_acquire(uint8_t *code, size_t n) {
    unsigned done = 0; uint32_t *w = (uint32_t *)(void *)code; size_t words = n / 4;
    for (size_t i = 0; i < words; i++) {
        if (!excl_ld(w[i])) continue;
        uint32_t rn = (w[i] >> 5) & 31;
        for (size_t j = i + 1; j < words && j <= i + 16; j++) if (excl_st(w[j]) && ((w[j] >> 5) & 31) == rn) { w[i] |= 0x8000u; done++; break; }
    }
    return done;
}
void runtime_game_patches(Module *m) {
    fprintf(stderr, "[botw] %s game-code patches\n", TOTK_GAME_VERSION);
    runtime_duo_main(m);   /* the companion reads main where it is mapped; it patches nothing */
    if (realtime_enabled()) patch_realtime(m);
    else fprintf(stderr, "[botw] real-time frame step off (TOTK_REALTIME=0): the game advances by its present interval\n");
    const char *a = getenv("TOTK_EXCL_ACQUIRE");
    if (a && !strcmp(a, "0")) { fprintf(stderr, "[botw] exclusive loads as shipped (TOTK_EXCL_ACQUIRE=0)\n"); return; }
    unsigned t = excl_acquire(m->base, m->text_end), p = excl_acquire(m->base + m->patch_off, m->file_size - m->patch_off);
    fprintf(stderr, "[botw] exclusive loads with acquire: %u in the code, %u in the rewriter's patch area (TOTK_EXCL_ACQUIRE=0: as shipped)\n", t, p);
}
/* Output size, aspect and render-size controls: BotW's sites are not ported yet, so each answers "not available". */
int runtime_output_res(unsigned *w, unsigned *h) { (void)w; (void)h; return 0; }
int runtime_render_res(unsigned *w, unsigned *h) { (void)w; (void)h; return 0; }
uint64_t runtime_output_res_extra_bytes(void) { return 0; }
int runtime_output_res_toggle(void) { return 0; }
int runtime_output_mode_cycle(void) { return 0; }
float runtime_aspect_now(void) { return 0; }
int runtime_aspect_toggle(void) { return 0; }
/* The job-queue poll hook (MacNativeGames game_patches_botw.c botw_jq_hook, a SIGTRAP site): not installed here, so
   no trap is ours. */
int botw_jq_hook(void *context) { (void)context; return 0; }
