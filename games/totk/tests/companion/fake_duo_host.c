/* A stand-in for libduo_host.so (duo/duo_port.h) for duo_route_probe.c: keeps the DuoPort the runtime hands over, so
   the probe can call through the table exactly as the companion host would. MIT, like the runtime. */
#include "../../duo/duo_port.h"
#include <stdatomic.h>

static DuoPort kept;
static _Atomic int started, frames;

__attribute__((visibility("default"))) int duo_host_start(const DuoPort *port) {
    if (port->abi != DUO_PORT_ABI || port->size != sizeof(DuoPort)) return 0;
    kept = *port;
    atomic_fetch_add(&started, 1);
    return 1;
}
__attribute__((visibility("default"))) void duo_host_frame(void) { atomic_fetch_add(&frames, 1); }
__attribute__((visibility("default"))) const DuoPort *fake_duo_port(void) { return atomic_load(&started) ? &kept : 0; }
__attribute__((visibility("default"))) int fake_duo_frames(void) { return atomic_load(&frames); }
