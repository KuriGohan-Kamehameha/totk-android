// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Iteration 13 (owner item: System command-bar buttons): pressed-state flash.
// The runtime has no pressed/touch-down widget state (runtime 22 and duo-1.4.0 checked), so the
// module reports the last module action it received for a short window and the manifest shows each
// button's pressed art gated on that action's id (hide_bind botw.ui.pressed, keep_min=keep_max=id).
// tools/i13-buttons.py computes the same id, so both sides must keep this exact hash.
#include <chrono>
#include <cstdint>
// Desktop-harness capture builds may lengthen the flash (-DBOTW_PRESS_HOLD_MS=4000) so a screenshot
// can catch it; packaged builds never define it.
#ifndef BOTW_PRESS_HOLD_MS
#define BOTW_PRESS_HOLD_MS 220
#endif
namespace botw {
// FNV-1a 32 over "<action>:<argument decimal>", masked to 31 bits; 0 is reserved for "none".
constexpr int64_t press_id(const char* action, int64_t argument) {
  uint32_t h = 2166136261u;
  auto mix = [&h](unsigned char c) { h ^= c; h *= 16777619u; };
  for (const char* p = action; p && *p; ++p) mix(static_cast<unsigned char>(*p));
  mix(':');
  char digits[24]{}; int n = 0;
  const bool neg = argument < 0;
  uint64_t v = neg ? uint64_t(0) - uint64_t(argument) : uint64_t(argument);
  do { digits[n++] = char('0' + v % 10); v /= 10; } while (v && n < 23);
  if (neg) mix('-');
  while (n) mix(static_cast<unsigned char>(digits[--n]));
  const int64_t id = int64_t(h & 0x7FFFFFFFu);
  return id ? id : 1;
}
struct PressFlash {
  static constexpr auto hold = std::chrono::milliseconds(BOTW_PRESS_HOLD_MS);
  int64_t id{};
  std::chrono::steady_clock::time_point at{};
  void note(const char* action, int64_t argument, std::chrono::steady_clock::time_point now) {
    if (!action || !*action) return;
    id = press_id(action, argument);
    at = now;
  }
  int64_t value(std::chrono::steady_clock::time_point now) const {
    return id && now >= at && now - at < hold ? id : 0;
  }
};
} // namespace botw
