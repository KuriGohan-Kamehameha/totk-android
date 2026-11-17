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
/// One finger down at (x, y) in surface pixels, or every finger lifted (down false): the test's taps, which reach the
/// runtime as DuoSurfaceView's do (the Android path is DuoSurfaceView.nativeTouch, off unless TOTK_DUO_TOUCH=1).
void TestTouch(uint32_t x, uint32_t y, bool down);

// A few of the title module's published numbers become the second screen's common game state
// (runtime/companion_state.h; duo_state.h says which and how, per title): whether a save is running, whether the
// game's menu is up, whether the tablet is owned, the hearts. The app's shell and its stick-LED bridge read it from
// the port.
/// Tick thread, from the module's publish_i64 while it samples: remembers `name` if it is one of them.
void LedTap(const char* name, int64_t value);
/// Tick thread, when that sample is complete: the remembered values go to the port as one state (DuoPort.state).
void LedCommit();
/// Ends the tick thread and the runtime (tests only: the app's process ends with _exit).
void Stop();
} // namespace Duo
