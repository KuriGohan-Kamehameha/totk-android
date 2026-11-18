// SPDX-FileCopyrightText: Copyright 2026 P1R4N351
// SPDX-License-Identifier: GPL-3.0-or-later

// Which map layer Link is on (Surface, Sky, Depths), decided the way Tears of the Kingdom 1.4.3 decides it.
//
// The game computes it from the position in game::ui CalcMapLayer (main+0x401ad0; SeqQueryCalcMapLayer calls it
// with force = 1, the pause map with force = 0). It never reads the GameData enum MapData.CurrentLayer, which is a
// saved UI value (default "Sky") and is what made the page show the Sky on the ground. The rule, from the
// disassembly (main+0x401ad0..0x401fd4 and SkyRate main+0x402180..0x402504):
//
//   world = *( *(main+0x3a83968) + 0xb8 );  wp = *( *(world+0x28) + 0x150 )      (game::wm::WorldParam)
//   r = (UndergroundHeight - y) / UndergroundMargin;  if r >= 0 and min(r,1) > 0.5  -> Depths
//   A = min-biased sample of the 20x16 sky grid (500 m cells), T = max-biased sample of the 100x80 terrain
//       grid (100 m cells), both from *(world+0x218) (WorldMgr/AreaHeight/<field>.beco), origin (-5000,-4000)
//   H = max(A, T + SkyMargin);  s = clamp(1 - (H - y)/SkyMargin, 0, 1);  s >= 0.4  -> Sky
//   otherwise -> Surface
//
// WorldParam fields: +0xec SkyMargin, +0x104 UndergroundHeight, +0x108 UndergroundMargin, each with a has-value
// byte (+0x126, +0x127, +0x128 bit 0); the built-in defaults (main+0x2d17a28) are SkyHeight 1000, SkyMargin 100,
// UndergroundHeight -350, UndergroundMargin 50. MainField sets SkyMargin 150 and UndergroundHeight -40, so on the
// main field: Depths below y = -65, Sky from y >= max(A - 90, T + 60).
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace TotkLayer {

enum Layer : int { Ground = 0, Sky = 1, Underground = 2 };   // the companion's order (Surface, Sky, Depths)

struct Grid {
    int w = 0, h = 0;
    std::vector<std::uint16_t> data;   // w*h, row-major (z rows of x)
    bool Valid() const { return w > 0 && h > 0 && data.size() == static_cast<size_t>(w) * h; }
};

struct Params {
    float underground_height = -40, underground_margin = 50, sky_margin = 150;   // MainField
    Grid sky;       // section 0 of the .beco: sky-island threshold, min-biased, 500 m cells
    Grid terrain;   // section 2: terrain maximum, max-biased, 100 m cells
    bool have_area_height = true;   // false: the game has no area-height object (never Sky)
};

/// The game's CalcMapLayer(pos, force = 1): Link's physical layer at this position.
Layer Calc(const Params& p, float x, float y, float z);

/// The two grids from a WorldMgr/AreaHeight/*.beco file (u32 version, u32 count, then per grid a 4-byte header
/// with width at byte 2 and height at byte 3, then w*h u16): sections 0 (sky) and 2 (terrain).
bool ParseBeco(std::span<const std::uint8_t> file, Params& out);

/// A fault-safe guest read (1 = copied).
using ReadFn = int (*)(std::uint64_t address, void* out, std::size_t size);

/// The live parameters of the running world (see above). nullopt when the world object, its WorldParam or the
/// area-height grids cannot be read, or when the WorldParam does not set UndergroundHeight itself (not the main
/// field: shrines and other interiors take their parent's defaults, and their layer means nothing for the map).
/// `cache` keeps the grids between calls: they are re-read only when the area-height object changes.
std::optional<Params> ReadLive(std::uint64_t main_base, ReadFn read, Params& cache, std::uint64_t& cache_key);

} // namespace TotkLayer
