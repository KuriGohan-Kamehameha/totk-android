// Port shim: stands in for Eden's video_core/surface.h. Only the pixel formats the texture-readback header names;
// the port has no texture cache to read back from, so none of them is ever produced.
#pragma once
#include "duo_shim.h"
namespace VideoCore::Surface {
enum class PixelFormat : u32 { A8B8G8R8_UNORM, A8B8G8R8_SRGB, B8G8R8A8_UNORM, B8G8R8A8_SRGB, R8_UNORM, Invalid };
}
