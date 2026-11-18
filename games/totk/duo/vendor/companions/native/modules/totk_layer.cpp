// SPDX-FileCopyrightText: Copyright 2026 P1R4N351
// SPDX-License-Identifier: GPL-3.0-or-later

#include "totk_layer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace TotkLayer {
namespace {

// One axis of the game's sampler (main+0x402200..0x402320): the cell of f, the neighbour on the side of the half
// the point lies in, and how far towards it (0 at the cell centre, 1 at the cell edge). Indices clamp to the grid.
struct Axis {
    int i, n;
    float t;
};
Axis AxisOf(float f, int dim) {
    const float fl = std::floor(f);
    const float fr = f - fl;
    int i = static_cast<int>(fl);
    int n = fr < 0.5f ? i - 1 : i + 1;
    const float t = fr < 0.5f ? 1.0f - 2.0f * fr : 2.0f * (fr - 0.5f);
    i = std::clamp(i, 0, dim - 1);
    n = std::clamp(n, 0, dim - 1);
    return {i, n, t};
}
// min-biased (keep a when a < b) or max-biased (keep a when a > b) blend, as the game's fcsel mi / gt
float Pick(float a, float b, float t, bool max_bias) {
    const bool keep = max_bias ? a > b : a < b;
    return keep ? a : a + (b - a) * t;
}
float Sample(const Grid& g, float fx, float fz, bool max_bias) {
    if (!g.Valid())
        return 1000.0f;   // the game's value for a missing grid (main+0x40257c / 0x4025a4)
    const Axis x = AxisOf(fx, g.w), z = AxisOf(fz, g.h);
    const auto at = [&](int ix, int iz) { return static_cast<float>(g.data[static_cast<size_t>(iz) * g.w + ix]); };
    const float r0 = Pick(at(x.i, z.i), at(x.n, z.i), x.t, max_bias);
    const float r1 = Pick(at(x.i, z.n), at(x.n, z.n), x.t, max_bias);
    return Pick(r0, r1, z.t, max_bias);
}

std::uint32_t Le32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8 | std::uint32_t(p[2]) << 16 | std::uint32_t(p[3]) << 24;
}

} // namespace

Layer Calc(const Params& p, float x, float y, float z) {
    if (p.underground_margin != 0.0f) {
        const float r = (p.underground_height - y) / p.underground_margin;
        if (r >= 0.0f && std::min(r, 1.0f) > 0.5f)
            return Underground;
    }
    if (p.have_area_height && p.sky_margin != 0.0f) {
        const float a = Sample(p.sky, (x + 5000.0f) / 500.0f, (z + 4000.0f) / 500.0f, false);
        const float t = Sample(p.terrain, (x + 5000.0f) / 100.0f, (z + 4000.0f) / 100.0f, true);
        const float base = std::max(a, t + p.sky_margin);
        const float s = std::clamp(1.0f - (base - y) / p.sky_margin, 0.0f, 1.0f);
        if (s >= 0.4f)
            return Sky;
    }
    return Ground;
}

bool ParseBeco(std::span<const std::uint8_t> f, Params& out) {
    if (f.size() < 8 || Le32(f.data()) != 1)
        return false;
    const std::uint32_t count = Le32(f.data() + 4);
    size_t at = 8;
    Grid grids[3];
    for (std::uint32_t s = 0; s < count && s < 3; ++s) {
        if (at + 4 > f.size())
            return false;
        Grid g;
        g.w = f[at + 2];
        g.h = f[at + 3];
        const size_t n = static_cast<size_t>(g.w) * g.h;
        at += 4;
        if (at + 2 * n > f.size())
            return false;
        g.data.resize(n);
        for (size_t i = 0; i < n; ++i)
            g.data[i] = static_cast<std::uint16_t>(f[at + 2 * i] | f[at + 2 * i + 1] << 8);
        at += 2 * n;
        grids[s] = std::move(g);
    }
    if (!grids[0].Valid() || !grids[2].Valid())
        return false;
    out.sky = std::move(grids[0]);
    out.terrain = std::move(grids[2]);
    return true;
}

namespace {
std::uint64_t Rd64(ReadFn read, std::uint64_t a) {
    std::uint64_t v = 0;
    return a >= 0x1000 && read(a, &v, 8) ? v : 0;
}
bool RdGrid(ReadFn read, std::uint64_t hdr, std::uint64_t data, int min_w, int min_h, Grid& g) {
    std::uint8_t h[4];
    if (!hdr || !data || !read(hdr, h, 4) || h[2] < min_w || h[3] < min_h)
        return false;
    g.w = h[2];
    g.h = h[3];
    g.data.resize(static_cast<size_t>(g.w) * g.h);
    return read(data, g.data.data(), g.data.size() * 2) != 0;
}
// A WorldParam float with its has-value byte; the game walks to the parent resource when the byte is clear, which
// on the main field only happens for UndergroundMargin (default 50).
bool Field(ReadFn read, std::uint64_t wp, std::uint32_t value, std::uint32_t flag, float& out) {
    std::uint8_t has = 0;
    if (!read(wp + flag, &has, 1) || !(has & 1))
        return false;
    return read(wp + value, &out, 4) && std::isfinite(out);
}
} // namespace

std::optional<Params> ReadLive(std::uint64_t main, ReadFn read, Params& cache, std::uint64_t& cache_key) {
    if (!main || !read)
        return std::nullopt;
    const std::uint64_t world = Rd64(read, Rd64(read, main + 0x3a83968) + 0xb8);
    if (!world)
        return std::nullopt;
    // The game calls a world override instead when vtable slot 0x290 is not the base (main+0x22acf4): not the
    // main field's world object; the rule below is for that one only.
    if (Rd64(read, Rd64(read, world) + 0x290) != main + 0x22acf4)
        return std::nullopt;
    const std::uint64_t wp = Rd64(read, Rd64(read, world + 0x28) + 0x150);
    if (!wp)
        return std::nullopt;
    Params p;
    if (!Field(read, wp, 0x104, 0x127, p.underground_height))
        return std::nullopt;   // inherits UndergroundHeight: not the main field
    if (!Field(read, wp, 0x108, 0x128, p.underground_margin))
        p.underground_margin = 50.0f;
    if (!Field(read, wp, 0xec, 0x126, p.sky_margin))
        p.sky_margin = 100.0f;
    const std::uint64_t area = Rd64(read, world + 0x218);
    if (!area) {
        p.have_area_height = false;
        return p;
    }
    if (area != cache_key || !cache.sky.Valid() || !cache.terrain.Valid()) {
        Params g;
        if (!RdGrid(read, Rd64(read, area + 0x340), Rd64(read, area + 0x348), 1, 1, g.sky) ||
            !RdGrid(read, Rd64(read, area + 0x360), Rd64(read, area + 0x368), 1, 1, g.terrain))
            return std::nullopt;
        cache.sky = std::move(g.sky);
        cache.terrain = std::move(g.terrain);
        cache_key = area;
    }
    p.sky = cache.sky;
    p.terrain = cache.terrain;
    return p;
}

} // namespace TotkLayer
