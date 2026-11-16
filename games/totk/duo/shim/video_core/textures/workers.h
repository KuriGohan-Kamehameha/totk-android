// Port shim: stands in for Eden's video_core/textures/workers.h. Everything the vendored Duo runtime needs from the emulator is declared
// in one place, duo_shim.h.
#pragma once
#include "duo_shim.h"
namespace Tegra::Texture {
Common::ThreadWorker& GetThreadWorkers();
}
