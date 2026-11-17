/* runtime/duo_port.c: the Eden Duo companion host (the Android builds of every game; duo/duo_port.h is its interface).

   Weak on ELF: the Android runtime links duo_port.c (android/Makefile), but other links of the same game code do not
   (the Mac and iOS apps, the timing fixtures), so a caller outside the Android-only files checks the address first:
       if (runtime_duo_frame) runtime_duo_frame();          (inside #if DUO_LINK_WEAK)
   Mach-O has no weak undefined symbols without extra linker flags, so there the calls are left out at compile time
   (DUO_LINK_WEAK is 0) and the companion does not exist. */
#pragma once
#include "runtime.h"
#if defined(__ELF__)
#define DUO_LINK_WEAK 1
#define DUO_LINK_ATTR __attribute__((weak))
#else
#define DUO_LINK_WEAK 0
#define DUO_LINK_ATTR
#endif
void runtime_duo_main(const Module *m) DUO_LINK_ATTR;   /* the loader's patch pass: main is mapped at m->base, m->size bytes */
void runtime_duo_frame(void) DUO_LINK_ATTR;             /* once per presented frame; the first call opens the companion library */
/* The second screen's common state (companion_state.h) as the companion host last handed it over, and how old that
   sample is. 0 before the first one: no library, the companion off, or the host not ticking yet. Any thread. */
struct CompanionState;
int runtime_duo_state(struct CompanionState *out, int64_t *age_ms) DUO_LINK_ATTR;
