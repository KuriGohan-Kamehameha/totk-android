/* runtime/duo_port.c: the Eden Duo companion host (Breath of the Wild builds; duo/duo_port.h is its interface). */
#pragma once
#include "runtime.h"
void runtime_duo_main(const Module *m);   /* the loader's patch pass: main is mapped at m->base, m->size bytes */
void runtime_duo_frame(void);             /* once per presented frame; the first call opens the companion library */
