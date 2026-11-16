// SPDX-License-Identifier: GPL-3.0-or-later
// Internal to libduo_host: the port's service table as the shim and the host share it.
#pragma once
#include <cstdint>
#include <vector>

#include "duo_port.h"

namespace Duo {
/// The table duo_host_start was given (a copy that lives as long as the library); null before that.
const DuoPort* Port();
void SetPort(const DuoPort* port);
/// A tick is starting: answers cached during the previous one ("is this page readable?") are dropped.
void BeginTick();

// What the second screen's presenter uses, and the host test (test/host_test.cpp) in its place.
/// The surface the page is shown on: its size in pixels and whether it exists. The runtime draws only while it does.
void SetSurfaceSize(uint32_t width, uint32_t height, bool present);
/// The current page (0xAARRGGBB words) when it is newer than `serial`; false when nothing new was published.
bool TakeFrame(std::vector<uint32_t>& pixels, uint32_t& width, uint32_t& height, uint64_t& serial);
/// One page frame into a window buffer of out_w x out_h pixels (rows `stride` pixels apart) whose bytes are R, G, B, A:
/// 1:1 when the sizes agree, otherwise scaled into the rectangle the runtime also maps touches through, black around it.
void BlitFrame(uint32_t* out, uint32_t out_w, uint32_t out_h, uint32_t stride, const std::vector<uint32_t>& pixels,
               uint32_t width, uint32_t height);
uint64_t Ticks();
/// Ends the tick thread and the runtime (tests only: the app's process ends with _exit).
void Stop();
} // namespace Duo
