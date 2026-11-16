/* runtime/duo_port.c: the Eden Duo companion host (Breath of the Wild builds; duo/duo_port.h is its interface). */
#pragma once
#include "runtime.h"
void runtime_duo_main(const Module *m);   /* the loader's patch pass: main is mapped at m->base, m->size bytes */
void runtime_duo_frame(void);             /* once per presented frame; the first call opens the companion library */
/* The second screen's common state (companion_state.h) as the companion host last handed it over, and how old that
   sample is. 0 before the first one: no library, the companion off, or the host not ticking yet. Any thread. */
struct CompanionState;
int runtime_duo_state(struct CompanionState *out, int64_t *age_ms);
