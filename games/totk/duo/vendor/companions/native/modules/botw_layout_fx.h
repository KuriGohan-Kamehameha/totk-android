// SPDX-License-Identifier: GPL-3.0-or-later
// Native BOTW UI layout animations rendered on the device from the player's own game files
// (owner 2026-10-22: "drop runtime request and build instead"). Nothing here ships game art: the
// layouts, animations and textures are read at runtime from romfs
//   Pack/Bootup.pack#Layout/Common.sblarc  (Yaz0 SARC: blyt/*.bflyt, anim/*.bflan, timg/__Combined.bntx)
// and composed on the module asset worker into RGBA module images ("module:fx/...").
// Readers ported/generalised from the companions repo's ACNH layout + texture code (acnh_lyt.*,
// acnh_tex.*: BFLYT/BFLAN section layouts, Tegra block-linear deswizzle, BRTI channel selectors),
// extended for BOTW's v8 layouts: material texmap wrap/SRT/TEV (indirect, modulate, replace),
// blend modes (additive, multiply, alpha), threshold alpha interpolation, FLPA/FLVC/FLMC/FLTS/FLVI
// hermite curves, framebuffer-capture panes. Calibrated against the old native captures (private).
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace botw::fx {
/// The player's own layout archive (read once, on the module asset worker).
inline constexpr const char* kCommonPath = "romfs:/Pack/Bootup.pack#Layout/Common.sblarc";
/// 0 = not tried yet, 1 = rendering works, 2 = the game files are unusable (static icons stay).
inline std::atomic<int>& Availability() {
  static std::atomic<int> a{0};
  return a;
}
using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
namespace detail {
inline u16 L16(const std::vector<u8> &b, size_t o) {
  return o + 2 <= b.size() ? u16(b[o] | b[o + 1] << 8) : 0;
}
inline u32 L32(const std::vector<u8> &b, size_t o) {
  return o + 4 <= b.size() ? u32(b[o] | b[o + 1] << 8 | b[o + 2] << 16 | u32(b[o + 3]) << 24) : 0;
}
inline u64 L64(const std::vector<u8> &b, size_t o) {
  return u64(L32(b, o)) | u64(L32(b, o + 4)) << 32;
}
inline float F32(const std::vector<u8> &b, size_t o) {
  const u32 v = L32(b, o);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
inline u16 B16(const std::vector<u8> &b, size_t o) {
  return o + 2 <= b.size() ? u16(b[o] << 8 | b[o + 1]) : 0;
}
inline u32 B32(const std::vector<u8> &b, size_t o) {
  return o + 4 <= b.size() ? u32(u32(b[o]) << 24 | b[o + 1] << 16 | b[o + 2] << 8 | b[o + 3]) : 0;
}
inline std::string Str(const std::vector<u8> &b, size_t o, size_t max) {
  std::string s;
  for (size_t i = 0; i < max && o + i < b.size() && b[o + i]; ++i)
    s += char(b[o + i]);
  return s;
}
} // namespace detail
// ---- containers -------------------------------------------------------------------------------
inline bool Yaz0(const std::vector<u8> &in, std::vector<u8> &out) {
  if (in.size() < 16 || std::memcmp(in.data(), "Yaz0", 4) != 0) {
    out = in;
    return true;
  }
  const u32 size = detail::B32(in, 4);
  if (size > (64u << 20))
    return false;
  out.clear();
  out.reserve(size);
  size_t i = 16;
  while (out.size() < size) {
    if (i >= in.size())
      return false;
    const u8 code = in[i++];
    for (int bit = 0; bit < 8 && out.size() < size; ++bit) {
      if (code & (0x80 >> bit)) {
        if (i >= in.size())
          return false;
        out.push_back(in[i++]);
      } else {
        if (i + 2 > in.size())
          return false;
        const u8 b1 = in[i], b2 = in[i + 1];
        i += 2;
        const size_t dist = size_t(((b1 & 0xF) << 8) | b2) + 1;
        size_t n = b1 >> 4;
        if (n == 0) {
          if (i >= in.size())
            return false;
          n = size_t(in[i++]) + 0x12;
        } else
          n += 2;
        if (dist > out.size())
          return false;
        const size_t s = out.size() - dist;
        for (size_t k = 0; k < n; ++k)
          out.push_back(out[s + k]);
      }
    }
  }
  return true;
}
/// Members of a SARC whose names pass `want` (copied out; the archive buffer can be freed).
template <class Pred>
bool Sarc(const std::vector<u8> &b, Pred want, std::map<std::string, std::vector<u8>, std::less<>> &out) {
  if (b.size() < 0x14 || std::memcmp(b.data(), "SARC", 4) != 0)
    return false;
  const bool le = b[6] == 0xFF && b[7] == 0xFE;
  auto R16 = [&](size_t o) { return le ? detail::L16(b, o) : detail::B16(b, o); };
  auto R32 = [&](size_t o) { return le ? detail::L32(b, o) : detail::B32(b, o); };
  const size_t hdr = R16(4);
  const u32 data = R32(0xC);
  size_t o = hdr;
  if (o + 12 > b.size() || std::memcmp(b.data() + o, "SFAT", 4) != 0)
    return false;
  const u16 n = R16(o + 6);
  o += 12;
  const size_t ents = o;
  o += size_t(n) * 16;
  if (o + 8 > b.size() || std::memcmp(b.data() + o, "SFNT", 4) != 0)
    return false;
  const size_t names = o + 8;
  for (u16 k = 0; k < n; ++k) {
    const size_t e = ents + size_t(k) * 16;
    const u32 attr = R32(e + 4), s = R32(e + 8), en = R32(e + 12);
    const std::string name = detail::Str(b, names + size_t(attr & 0xFFFF) * 4, 256);
    if (!want(name))
      continue;
    if (en < s || size_t(data) + en > b.size())
      return false;
    out[name] = std::vector<u8>(b.begin() + data + s, b.begin() + data + en);
  }
  return true;
}
// ---- textures ---------------------------------------------------------------------------------
struct Tex {
  int w = 0, h = 0;
  std::vector<float> px;
}; ///< straight RGBA 0..1
namespace detail {
inline u32 Pdep(u32 v, u32 mask) {
  u32 r = 0;
  for (u32 bit = 1; mask; bit += bit) {
    if (v & bit)
      r |= mask & (~mask + 1);
    mask &= mask - 1;
  }
  return r;
}
inline bool Unswizzle(const u8 *src, size_t n, u32 bpp, u32 w, u32 h, u32 bh, std::vector<u8> &out) {
  if (!bpp || bh > 5)
    return false;
  const u64 row = u64(w) * bpp, aw = (row + 63) & ~u64(63),
            ah = (u64(h) + (u64(8) << bh) - 1) & ~((u64(8) << bh) - 1);
  if (aw * ah > n)
    return false;
  out.assign(size_t(row * h), 0);
  const u32 gobs = u32(aw) >> 6, block = gobs << (9 + bh), hm = (1u << bh) - 1;
  for (u32 y = 0; y < h; ++y) {
    const u32 sy = Pdep(y, 0b011010000), by = y >> 3, oy = (by >> bh) * block + ((by & hm) << 9);
    for (u32 x = 0; x < w; ++x) {
      const u32 bx = x * bpp;
      const u64 off = u64(oy) + (u64(bx >> 6) << (9 + bh)) + (Pdep(bx, 0b100101111) | sy);
      if (off + bpp > n)
        return false;
      std::memcpy(out.data() + size_t(y) * row + bx, src + off, bpp);
    }
  }
  return true;
}
inline void Bc4Block(const u8 *s, u8 v[16]) {
  const int a0 = s[0], a1 = s[1];
  int pal[8] = {a0, a1};
  if (a0 > a1)
    for (int i = 1; i < 7; ++i)
      pal[i + 1] = ((7 - i) * a0 + i * a1) / 7;
  else {
    for (int i = 1; i < 5; ++i)
      pal[i + 1] = ((5 - i) * a0 + i * a1) / 5;
    pal[6] = 0;
    pal[7] = 255;
  }
  u64 bits = 0;
  for (int i = 0; i < 6; ++i)
    bits |= u64(s[2 + i]) << (8 * i);
  for (int i = 0; i < 16; ++i)
    v[i] = u8(pal[(bits >> (3 * i)) & 7]);
}
inline void Bc1Block(const u8 *s, u8 rgba[64], bool bc1alpha) {
  const u16 c0 = u16(s[0] | s[1] << 8), c1 = u16(s[2] | s[3] << 8);
  int c[4][4];
  auto ex = [](u16 v, int *o) {
    o[0] = ((v >> 11) & 31) * 255 / 31;
    o[1] = ((v >> 5) & 63) * 255 / 63;
    o[2] = (v & 31) * 255 / 31;
    o[3] = 255;
  };
  ex(c0, c[0]);
  ex(c1, c[1]);
  for (int k = 0; k < 3; ++k) {
    if (c0 > c1 || !bc1alpha) {
      c[2][k] = (2 * c[0][k] + c[1][k]) / 3;
      c[3][k] = (c[0][k] + 2 * c[1][k]) / 3;
    } else {
      c[2][k] = (c[0][k] + c[1][k]) / 2;
      c[3][k] = 0;
    }
  }
  c[2][3] = 255;
  c[3][3] = (c0 > c1 || !bc1alpha) ? 255 : 0;
  const u32 idx = u32(s[4] | s[5] << 8 | s[6] << 16 | u32(s[7]) << 24);
  for (int i = 0; i < 16; ++i) {
    const int q = (idx >> (2 * i)) & 3;
    for (int k = 0; k < 4; ++k)
      rgba[i * 4 + k] = u8(c[q][k]);
  }
}
} // namespace detail
/// One BRTI texture of a BNTX, decoded to straight float RGBA with its channel selectors applied.
inline bool DecodeBntx(const std::vector<u8> &b, std::string_view name, Tex &out) {
  using namespace detail;
  if (b.size() < 0x40 || std::memcmp(b.data(), "BNTX", 4) != 0)
    return false;
  const u32 count = L32(b, 0x24);
  const u64 arr = L64(b, 0x28);
  if (!count || count > 8192 || arr + u64(count) * 8 > b.size())
    return false;
  for (u32 i = 0; i < count; ++i) {
    const u64 o = L64(b, arr + u64(i) * 8);
    if (o + 0xA0 > b.size() || std::memcmp(b.data() + o, "BRTI", 4) != 0)
      continue;
    const u64 no = L64(b, o + 0x60);
    if (no + 2 > b.size())
      continue;
    const u16 nl = L16(b, no);
    if (no + 2 + nl > b.size() ||
        std::string_view(reinterpret_cast<const char *>(b.data() + no + 2), nl) != name)
      continue;
    const u32 w = L32(b, o + 0x24), h = L32(b, o + 0x28), fmt = L32(b, o + 0x1C), size = L32(b, o + 0x50),
              arrays = L32(b, o + 0x30), layout = L32(b, o + 0x34), cmap = L32(b, o + 0x58);
    const u16 tile = L16(b, o + 0x12);
    const u64 mip = L64(b, o + 0x70);
    if (mip + 8 > b.size())
      return false;
    const u64 img = L64(b, mip);
    if (!w || !h || w > 4096 || h > 4096 || !arrays || img + size / arrays > b.size())
      return false;
    const u32 t = fmt >> 8;
    u32 bw = 1, bpb = 0, ch = 0;
    switch (t) {
    case 0x02:
      bpb = 1;
      ch = 1;
      break;
    case 0x09:
      bpb = 2;
      ch = 2;
      break;
    case 0x0b:
      bpb = 4;
      ch = 4;
      break;
    case 0x1a:
      bw = 4;
      bpb = 8;
      ch = 4;
      break;
    case 0x1c:
      bw = 4;
      bpb = 16;
      ch = 4;
      break;
    case 0x1d:
      bw = 4;
      bpb = 8;
      ch = 1;
      break;
    case 0x1e:
      bw = 4;
      bpb = 16;
      ch = 2;
      break;
    default:
      return false;
    }
    const u32 bx = (w + bw - 1) / bw, by = (h + bw - 1) / bw;
    std::vector<u8> blocks;
    if (tile != 0)
      return false;
    if (!Unswizzle(b.data() + img, size / arrays, bpb, bx, by, layout & 7, blocks))
      return false;
    std::vector<u8> chan(size_t(w) * h * ch, 0);
    if (t == 0x02 || t == 0x09 || t == 0x0b) {
      if (blocks.size() < chan.size())
        return false;
      std::copy_n(blocks.begin(), chan.size(), chan.begin());
    } else
      for (u32 y = 0; y < by; ++y)
        for (u32 x = 0; x < bx; ++x) {
          const u8 *s = blocks.data() + (size_t(y) * bx + x) * bpb;
          u8 tmp[64];
          if (t == 0x1d) {
            u8 v[16];
            Bc4Block(s, v);
            for (int k = 0; k < 16; ++k)
              tmp[k] = v[k];
          } else if (t == 0x1e) {
            u8 r[16], g[16];
            Bc4Block(s, r);
            Bc4Block(s + 8, g);
            for (int k = 0; k < 16; ++k) {
              tmp[k * 2] = r[k];
              tmp[k * 2 + 1] = g[k];
            }
          } else if (t == 0x1a)
            Bc1Block(s, tmp, true);
          else {
            u8 a[16];
            Bc4Block(s, a);
            Bc1Block(s + 8, tmp, false);
            for (int k = 0; k < 16; ++k)
              tmp[k * 4 + 3] = a[k];
          }
          for (int py = 0; py < 4; ++py)
            for (int px = 0; px < 4; ++px) {
              const u32 X = x * 4 + px, Y = y * 4 + py;
              if (X >= w || Y >= h)
                continue;
              for (u32 c = 0; c < ch; ++c)
                chan[(size_t(Y) * w + X) * ch + c] = tmp[(py * 4 + px) * ch + c];
            }
        }
    out.w = int(w);
    out.h = int(h);
    out.px.assign(size_t(w) * h * 4, 0.f);
    for (size_t p = 0; p < size_t(w) * h; ++p)
      for (u32 c = 0; c < 4; ++c) {
        const u8 sel = u8(cmap >> (c * 8));
        out.px[p * 4 + c] = sel == 0   ? 0.f
                            : sel == 1 ? 1.f
                                       : (sel - 2u < ch ? chan[p * ch + sel - 2] / 255.f : 0.f);
      }
    return true;
  }
  return false;
}
// ---- layouts ----------------------------------------------------------------------------------
struct Mat {
  std::string name;
  std::array<float, 4> black{}, white{1, 1, 1, 1};
  u32 flags = 0;
  std::vector<std::string> tex;
  std::vector<std::array<u8, 2>> wrap;
  std::vector<std::array<float, 5>> srt;
  std::vector<std::array<u8, 4>> tev;
  std::optional<std::array<u8, 4>> blend;
  std::optional<std::array<float, 3>> ind;
};
struct Pane {
  std::string tag, name;
  float x = 0, y = 0, rz = 0, sx = 1, sy = 1, w = 0, h = 0;
  u8 flags = 0, alpha = 255;
  bool visible = true;
  std::array<std::array<float, 4>, 4> vtx{};
  int mat = -1;
  std::vector<int> children;
};
struct Layout {
  std::vector<std::string> textures;
  std::vector<Mat> mats;
  std::vector<Pane> panes;
};
inline bool ParseBflyt(const std::vector<u8> &b, Layout &L) {
  using namespace detail;
  if (b.size() < 0x14 || std::memcmp(b.data(), "FLYT", 4) != 0)
    return false;
  size_t o = L16(b, 6);
  std::vector<int> stack{-1};
  int last = -1;
  while (o + 8 <= b.size()) {
    const std::string tag(reinterpret_cast<const char *>(b.data() + o), 4);
    const u32 size = L32(b, o + 4);
    if (size < 8 || o + size > b.size())
      break;
    const size_t s = o;
    if (tag == "txl1") {
      const u16 n = L16(b, s + 8);
      for (u16 i = 0; i < n; ++i)
        L.textures.push_back(Str(b, s + 0xC + L32(b, s + 0xC + 4 * i), 256));
    } else if (tag == "mat1") {
      const u16 n = L16(b, s + 8);
      for (u16 i = 0; i < n; ++i) {
        size_t mo = s + L32(b, s + 0xC + 4 * i);
        Mat m;
        m.name = Str(b, mo, 0x1C);
        const u32 f = m.flags = L32(b, mo + 0x1C);
        for (int k = 0; k < 4; ++k) {
          m.black[k] = b.size() > mo + 0x24 + k ? b[mo + 0x24 + k] / 255.f : 0;
          m.white[k] = b.size() > mo + 0x28 + k ? b[mo + 0x28 + k] / 255.f : 1;
        }
        size_t p = mo + 0x2C;
        const u32 nt = f & 3, nsrt = (f >> 2) & 3, ntcg = (f >> 4) & 3, ntev = (f >> 6) & 7;
        for (u32 t = 0; t < nt; ++t) {
          const u16 ti = L16(b, p);
          m.tex.push_back(ti < L.textures.size() ? L.textures[ti] : std::string{});
          m.wrap.push_back(
              {u8(p + 2 < b.size() ? b[p + 2] & 3 : 0), u8(p + 3 < b.size() ? b[p + 3] & 3 : 0)});
          p += 4;
        }
        for (u32 t = 0; t < nsrt; ++t) {
          m.srt.push_back({F32(b, p), F32(b, p + 4), F32(b, p + 8), F32(b, p + 12), F32(b, p + 16)});
          p += 20;
        }
        p += size_t(ntcg) * 16;
        for (u32 t = 0; t < ntev; ++t) {
          if (p + 4 <= b.size())
            m.tev.push_back({b[p], b[p + 1], b[p + 2], b[p + 3]});
          p += 4;
        }
        if ((f >> 9) & 1)
          p += 8;
        if (((f >> 10) & 1) && p + 4 <= b.size()) {
          m.blend = std::array<u8, 4>{b[p], b[p + 1], b[p + 2], b[p + 3]};
          p += 4;
        }
        if ((f >> 12) & 1)
          p += 4;
        if ((f >> 14) & 1) {
          m.ind = std::array<float, 3>{F32(b, p), F32(b, p + 4), F32(b, p + 8)};
          p += 12;
        }
        L.mats.push_back(std::move(m));
      }
    } else if (tag == "pan1" || tag == "pic1" || tag == "txt1" || tag == "wnd1" || tag == "prt1" ||
               tag == "bnd1" || tag == "scr1" || tag == "ali1" || tag == "cpt1") {
      Pane P;
      P.tag = tag;
      P.flags = b[s + 8];
      P.alpha = b[s + 10];
      P.name = Str(b, s + 0xC, 0x18);
      P.x = F32(b, s + 0x2C);
      P.y = F32(b, s + 0x30);
      P.rz = F32(b, s + 0x40);
      P.sx = F32(b, s + 0x44);
      P.sy = F32(b, s + 0x48);
      P.w = F32(b, s + 0x4C);
      P.h = F32(b, s + 0x50);
      P.visible = (P.flags & 1) != 0;
      for (auto &c : P.vtx)
        c = {1, 1, 1, 1};
      if (tag == "pic1") {
        for (int i = 0; i < 4; ++i)
          for (int k = 0; k < 4; ++k)
            P.vtx[i][k] = b.size() > s + 0x54 + 4 * i + k ? b[s + 0x54 + 4 * i + k] / 255.f : 1;
        P.mat = L16(b, s + 0x64);
        if (P.mat >= int(L.mats.size()))
          P.mat = -1;
      }
      const int idx = int(L.panes.size());
      if (stack.back() >= 0)
        L.panes[stack.back()].children.push_back(idx);
      L.panes.push_back(std::move(P));
      last = idx;
    } else if (tag == "pas1") {
      if (stack.size() > 64)
        return false;
      stack.push_back(last);
    } else if (tag == "pae1") {
      if (stack.size() > 1)
        stack.pop_back();
    }
    o += size;
  }
  return !L.panes.empty();
}
struct Key {
  float f, v, s;
};
struct Track {
  std::string entry, magic;
  u8 index = 0, target = 0, curve = 0;
  std::vector<Key> keys;
};
struct Anim {
  u16 frames = 0;
  std::vector<Track> tracks;
};
inline bool ParseBflan(const std::vector<u8> &b, Anim &A) {
  using namespace detail;
  if (b.size() < 0x14 || std::memcmp(b.data(), "FLAN", 4) != 0)
    return false;
  size_t o = L16(b, 6);
  while (o + 8 <= b.size()) {
    const u32 size = L32(b, o + 4);
    if (size == 0 || o + size > b.size())
      break;
    if (std::memcmp(b.data() + o, "pai1", 4) == 0) {
      const size_t end = o + size;
      auto in = [&](size_t at, size_t n) { return at <= end && n <= end - at; };
      A.frames = L16(b, o + 8);
      const u16 nent = L16(b, o + 14);
      const u32 eoff = L32(b, o + 16);
      for (u16 e = 0; e < nent && in(o + eoff + 4 * size_t(e), 4); ++e) {
        const size_t eo = o + L32(b, o + eoff + 4 * e);
        if (!in(eo, 0x20))
          break;
        const std::string name = Str(b, eo, 0x1C);
        const u8 ntag = b[eo + 0x1C];
        for (u8 t = 0; t < ntag && in(eo + 0x20 + 4 * t, 4); ++t) {
          const size_t to = eo + L32(b, eo + 0x20 + 4 * t);
          if (!in(to, 8))
            break;
          const std::string magic(reinterpret_cast<const char *>(b.data() + to), 4);
          const u8 n = b[to + 4];
          for (u8 k = 0; k < n && in(to + 8 + 4 * k, 4); ++k) {
            const size_t te = to + L32(b, to + 8 + 4 * k);
            if (!in(te, 12))
              break;
            Track tr;
            tr.entry = name;
            tr.magic = magic;
            tr.index = b[te];
            tr.target = b[te + 1];
            tr.curve = b[te + 2];
            const u16 nk = L16(b, te + 4);
            const u32 ko = L32(b, te + 8);
            const size_t st = tr.curve == 1 ? 8 : 12;
            if (!in(te + ko, st * nk))
              continue;
            for (u16 q = 0; q < nk; ++q) {
              const size_t ka = te + ko + st * q;
              if (tr.curve == 1)
                tr.keys.push_back({F32(b, ka), float(L16(b, ka + 4)), 0});
              else
                tr.keys.push_back({F32(b, ka), F32(b, ka + 4), F32(b, ka + 8)});
            }
            A.tracks.push_back(std::move(tr));
          }
        }
      }
    }
    o += size;
  }
  return true;
}
/// Curve value at `frame`: hermite (curve 2), step (0/1); clamps outside the keys.
inline float Eval(const Track &t, float frame) {
  const auto &k = t.keys;
  if (k.empty())
    return 0;
  if (frame <= k.front().f || k.size() == 1)
    return k.front().v;
  if (frame >= k.back().f)
    return k.back().v;
  for (size_t i = 0; i + 1 < k.size(); ++i) {
    const auto &a = k[i];
    const auto &c = k[i + 1];
    if (frame < a.f || frame > c.f)
      continue;
    if (t.curve != 2 || c.f == a.f)
      return frame < c.f ? a.v : c.v;
    const float d = c.f - a.f, x = (frame - a.f) / d, x2 = x * x, x3 = x2 * x;
    return (2 * x3 - 3 * x2 + 1) * a.v + (x3 - 2 * x2 + x) * d * a.s + (-2 * x3 + 3 * x2) * c.v +
           (x3 - x2) * d * c.s;
  }
  return k.back().v;
}
// ---- compositor -------------------------------------------------------------------------------
struct Frame {
  int w = 0, h = 0;
  std::vector<float> c;
}; ///< accumulated RGBA (rgb may exceed 1 under additive)
struct Lookup {
  std::vector<std::pair<const Anim *, float>> anims; ///< first match wins (loop first, then state)
  std::optional<float> Get(std::string_view e, std::string_view m, int index, int target) const {
    for (const auto &[a, f] : anims) {
      if (!a)
        continue;
      for (const auto &t : a->tracks)
        if (t.index == index && t.target == target && t.magic == m && t.entry == e)
          return Eval(t, f);
    }
    return std::nullopt;
  }
};
struct Options {
  bool skip_capture = false; ///< drop the framebuffer-capture chain (measured ~0 contribution)
  int size = 96;
  float scale = 1, cx = 0, cy = 0;
  std::string capture_root;
};
class Composer {
public:
  Composer(const Layout &L, const std::map<std::string, Tex, std::less<>> &T, const Lookup &A,
           const Options &o)
      : L_(L), T_(T), A_(A), o_(o) {
    out_.w = out_.h = o.size;
    out_.c.assign(size_t(o.size) * o.size * 4, 0.f);
  }
  Frame Run() {
    for (size_t i = 0; i < L_.panes.size(); ++i)
      if (L_.panes[i].name == "RootPane") {
        Walk(int(i), Ident(), 1.f);
        break;
      }
    return std::move(out_);
  }

private:
  using M3 = std::array<float, 6>; // a b tx / c d ty
  static M3 Ident() { return {1, 0, 0, 0, 1, 0}; }
  static M3 Mul(const M3 &a, const M3 &b) {
    return {a[0] * b[0] + a[1] * b[3], a[0] * b[1] + a[1] * b[4], a[0] * b[2] + a[1] * b[5] + a[2],
            a[3] * b[0] + a[4] * b[3], a[3] * b[1] + a[4] * b[4], a[3] * b[2] + a[4] * b[5] + a[5]};
  }
  float PV(const Pane &P, int t, float d) const {
    auto v = A_.Get(P.name, "FLPA", 0, t);
    return v ? *v : d;
  }
  void Walk(int i, const M3 &M, float pa) {
    const Pane &P = L_.panes[size_t(i)];
    if (!o_.capture_root.empty() && P.name == o_.capture_root && o_.skip_capture)
      return;
    if (!o_.capture_root.empty() && P.name == o_.capture_root) {
      Frame keep = std::move(out_);
      out_ = keep;
      std::fill(out_.c.begin(), out_.c.end(), 0.f);
      Inner(i, M, pa);
      fb_ = std::move(out_);
      out_ = std::move(keep);
      have_fb_ = true;
      return;
    }
    Inner(i, M, pa);
  }
  void Inner(int i, const M3 &M, float pa) {
    const Pane &P = L_.panes[size_t(i)];
    const auto vis = A_.Get(P.name, "FLVI", 0, 0);
    if ((!vis && !P.visible) || (vis && *vis < 0.5f))
      return;
    const float tx = PV(P, 0, P.x), ty = PV(P, 1, P.y), rz = PV(P, 5, P.rz), sx = PV(P, 6, P.sx),
                sy = PV(P, 7, P.sy), w = PV(P, 8, P.w), h = PV(P, 9, P.h);
    const auto av = A_.Get(P.name, "FLVC", 0, 16);
    const float alpha = (av ? *av : float(P.alpha)) / 255.f;
    const float r = rz * 3.14159265f / 180.f, c = std::cos(r), s = std::sin(r);
    const M3 Mi = Mul(M, M3{c * sx, -s * sy, tx, s * sx, c * sy, ty});
    const float ga = pa * alpha;
    if (P.tag == "pic1" && P.mat >= 0)
      Pic(P, Mi, w, h, ga);
    const float ca = (P.flags & 2) ? ga : pa;
    for (int ch : P.children)
      Walk(ch, Mi, ca);
  }
  static void Sample(const Tex &t, float u, float v, u8 wu, u8 wv, float o[4]) {
    auto wrap = [](float c, u8 m) {
      if (m == 1)
        return c - std::floor(c);
      if (m == 2) {
        float k = c - 2 * std::floor(c / 2);
        return k > 1 ? 2 - k : k;
      }
      return std::clamp(c, 0.f, 1.f);
    };
    const float uu = wrap(u, wu) * t.w - 0.5f, vv = wrap(v, wv) * t.h - 0.5f;
    const int x0 = int(std::floor(uu)), y0 = int(std::floor(vv));
    const float fx = uu - x0, fy = vv - y0;
    auto at = [&](int x, int y) -> const float * {
      x = wu == 1 ? ((x % t.w) + t.w) % t.w : std::clamp(x, 0, t.w - 1);
      y = wv == 1 ? ((y % t.h) + t.h) % t.h : std::clamp(y, 0, t.h - 1);
      return &t.px[(size_t(y) * t.w + x) * 4];
    };
    const float *a = at(x0, y0), *b = at(x0 + 1, y0), *c = at(x0, y0 + 1), *d = at(x0 + 1, y0 + 1);
    for (int k = 0; k < 4; ++k)
      o[k] = a[k] * (1 - fx) * (1 - fy) + b[k] * fx * (1 - fy) + c[k] * (1 - fx) * fy + d[k] * fx * fy;
  }
  /// Texture SRT, precomputed once per pane (centre-based rotate/scale, then translate).
  struct Srt {
    float c = 1, s = 0, isx = 1, isy = 1, tx = 0, ty = 0;
    explicit Srt(const std::array<float, 5> &st) {
      const float r = st[2] * 3.14159265f / 180.f;
      c = std::cos(r);
      s = std::sin(r);
      isx = 1.f / (st[3] != 0 ? st[3] : 1);
      isy = 1.f / (st[4] != 0 ? st[4] : 1);
      tx = st[0];
      ty = st[1];
    }
    void Apply(float &u, float &v) const {
      const float cu = u - 0.5f, cv = v - 0.5f;
      u = (c * cu + s * cv) * isx + 0.5f - tx;
      v = (-s * cu + c * cv) * isy + 0.5f - ty;
    }
  };
  void Pic(const Pane &P, const M3 &M, float w, float h, float ga) {
    const Mat &m = L_.mats[size_t(P.mat)];
    const float det = M[0] * M[4] - M[1] * M[3];
    if (std::fabs(det) < 1e-9f || ga <= 0 || w == 0 || h == 0)
      return;
    std::array<float, 4> bk = m.black, wt = m.white;
    for (int k = 0; k < 8; ++k)
      if (auto v = A_.Get(m.name, "FLMC", 0, k))
        (k < 4 ? bk : wt)[size_t(k % 4)] = *v / 255.f;
    std::vector<std::array<float, 5>> srt = m.srt;
    for (size_t k = 0; k < srt.size(); ++k)
      for (int tg = 0; tg < 5; ++tg)
        if (auto v = A_.Get(m.name, "FLTS", int(k), tg))
          srt[k][size_t(tg)] = *v;
    std::vector<Srt> srt_m;
    for (const auto &st : srt)
      srt_m.emplace_back(st);
    std::array<std::array<float, 4>, 4> vc = P.vtx;
    for (int k = 0; k < 16; ++k)
      if (auto v = A_.Get(P.name, "FLVC", 0, k))
        vc[size_t(k / 4)][size_t(k % 4)] = *v / 255.f;
    std::vector<const Tex *> tx;
    for (const auto &n : m.tex) {
      auto it = T_.find(n);
      tx.push_back(it == T_.end() ? nullptr : &it->second);
    }
    const bool fbtex = !m.tex.empty() && m.tex[0].rfind("FBLayout", 0) == 0;
    if (fbtex && o_.skip_capture)
      return;
    // output-pixel bounds of the pane quad
    const int N = o_.size;
    float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
    for (int k = 0; k < 4; ++k) {
      const float X = (k & 1 ? 0.5f : -0.5f) * w, Y = (k & 2 ? 0.5f : -0.5f) * h;
      const float lx = M[0] * X + M[1] * Y + M[2], ly = M[3] * X + M[4] * Y + M[5];
      const float px = (lx - o_.cx) * o_.scale + N / 2.f, py = -(ly - o_.cy) * o_.scale + N / 2.f;
      minx = std::min(minx, px);
      maxx = std::max(maxx, px);
      miny = std::min(miny, py);
      maxy = std::max(maxy, py);
    }
    const int x0 = std::max(0, int(std::floor(minx))), x1 = std::min(N - 1, int(std::ceil(maxx))),
              y0 = std::max(0, int(std::floor(miny))), y1 = std::min(N - 1, int(std::ceil(maxy)));
    if (x0 > x1 || y0 > y1)
      return;
    const float ia = M[4] / det, ib = -M[1] / det, ic = -M[3] / det, id = M[0] / det;
    const bool thresh = (m.flags & 0x40000) != 0;
    const bool indirect = !m.tev.empty() && m.tev[0][0] == 11 && tx.size() >= 2;
    for (int py = y0; py <= y1; ++py)
      for (int px = x0; px <= x1; ++px) {
        const float lx = (px + 0.5f - N / 2.f) / o_.scale + o_.cx,
                    ly = -(py + 0.5f - N / 2.f) / o_.scale + o_.cy;
        const float dx = lx - M[2], dy = ly - M[5];
        const float X = ia * dx + ib * dy, Y = ic * dx + id * dy;
        const float u = X / w + 0.5f, v = 0.5f - Y / h;
        if (u < 0 || u > 1 || v < 0 || v > 1)
          continue;
        float col[4] = {0, 0, 0, 0};
        size_t next = 1;
        auto tsample = [&](size_t k, float uu, float vv, float o4[4]) -> bool {
          if (k >= tx.size() || !tx[k])
            return false;
          if (k < srt_m.size())
            srt_m[k].Apply(uu, vv);
          Sample(*tx[k], uu, vv, k < m.wrap.size() ? m.wrap[k][0] : 0, k < m.wrap.size() ? m.wrap[k][1] : 0,
                 o4);
          return true;
        };
        auto fbsample = [&](float uu, float vv, float o4[4]) {
          if (!have_fb_) {
            o4[0] = o4[1] = o4[2] = o4[3] = 0;
            return;
          }
          const float Xs = (uu - 0.5f) * w, Ys = (0.5f - vv) * h;
          const float lx2 = M[0] * Xs + M[1] * Ys + M[2], ly2 = M[3] * Xs + M[4] * Ys + M[5];
          const float sx2 = ((lx2 - o_.cx) * o_.scale + N / 2.f) / N,
                      sy2 = ((-(ly2 - o_.cy)) * o_.scale + N / 2.f) / N;
          const float fu = std::clamp(sx2, 0.f, 1.f) * N - 0.5f, fv = std::clamp(sy2, 0.f, 1.f) * N - 0.5f;
          const int ix = std::clamp(int(std::floor(fu)), 0, N - 1),
                    iy = std::clamp(int(std::floor(fv)), 0, N - 1);
          for (int k = 0; k < 4; ++k)
            o4[k] = std::clamp(fb_.c[(size_t(iy) * N + ix) * 4 + size_t(k)], 0.f, 1.f);
        };
        if (indirect) {
          float ind[4];
          if (tsample(1, u, v, ind)) {
            const auto ip = m.ind.value_or(std::array<float, 3>{0, 0.1f, 0.1f});
            const float du = (ind[0] - 0.5f) * ip[1] * 2, dv = (ind[1] - 0.5f) * ip[2] * 2;
            if (fbtex)
              fbsample(u + du, v + dv, col);
            else if (!tsample(0, u + du, v + dv, col))
              continue;
          } else
            continue;
          next = 2;
        } else {
          if (fbtex)
            fbsample(u, v, col);
          else if (!tsample(0, u, v, col))
            continue;
        }
        const size_t stage0 = indirect ? 1 : 0;
        for (size_t k = next, st = stage0; k < tx.size(); ++k, ++st) {
          float t2[4];
          if (!tsample(k, u, v, t2))
            continue;
          const u8 mode = st < m.tev.size() ? m.tev[st][0] : 1;
          if (mode == 0) {
            col[0] = t2[0];
            col[1] = t2[1];
            col[2] = t2[2];
          } else if (mode == 2) {
            // Add: the mask's colour weighted by its own coverage; the warped shape keeps its alpha
            // (calibrated on the Rito flames, the only layouts using it).
            for (int q = 0; q < 3; ++q)
              col[q] = std::min(1.f, col[q] + t2[q] * t2[3]);
          } else if (indirect) {
            // Modulate after an indirect stage (orb flames): colour product, geometric-mean alpha.
            for (int q = 0; q < 3; ++q)
              col[q] *= t2[q];
            col[3] = std::sqrt(col[3] * t2[3]);
          } else
            for (int q = 0; q < 4; ++q)
              col[q] *= t2[q];
        }
        float rgb[3];
        for (int q = 0; q < 3; ++q)
          rgb[q] = bk[size_t(q)] + col[q] * (wt[size_t(q)] - bk[size_t(q)]);
        float al = thresh ? std::clamp((col[3] - bk[3]) / std::max(wt[3] - bk[3], 1e-3f), 0.f, 1.f)
                          : bk[3] + col[3] * (wt[3] - bk[3]);
        const float uu = std::clamp(u, 0.f, 1.f), vv = std::clamp(v, 0.f, 1.f);
        float vcol[4];
        for (int q = 0; q < 4; ++q)
          vcol[q] = vc[0][size_t(q)] * (1 - uu) * (1 - vv) + vc[1][size_t(q)] * uu * (1 - vv) +
                    vc[2][size_t(q)] * (1 - uu) * vv + vc[3][size_t(q)] * uu * vv;
        for (int q = 0; q < 3; ++q)
          rgb[q] = std::clamp(rgb[q] * vcol[q], 0.f, 1.f);
        al = std::clamp(al * vcol[3] * ga, 0.f, 1.f);
        if (al <= 0)
          continue;
        float *d = &out_.c[(size_t(py) * N + px) * 4];
        if (m.blend && (*m.blend)[0] == 1 && (*m.blend)[2] == 1) {
          float mx = 0;
          for (int q = 0; q < 3; ++q) {
            const float a = rgb[q] * al;
            d[q] += a;
            mx = std::max(mx, a);
          }
          d[3] = std::clamp(d[3] + mx * (1 - d[3]), 0.f, 1.f);
        } else if (m.blend && (*m.blend)[0] == 1 && (*m.blend)[1] == 2) {
          for (int q = 0; q < 3; ++q)
            d[q] = d[q] * (1 - al) + d[q] * rgb[q] * al;
        } else {
          for (int q = 0; q < 3; ++q)
            d[q] = rgb[q] * al + d[q] * (1 - al);
          d[3] = al + d[3] * (1 - al);
        }
      }
  }
  const Layout &L_;
  const std::map<std::string, Tex, std::less<>> &T_;
  const Lookup &A_;
  Options o_;
  Frame out_, fb_;
  bool have_fb_ = false;
};
/// Straight RGBA8 for a module image sink.
inline std::vector<u8> ToRgba8(const Frame &f) {
  std::vector<u8> o(size_t(f.w) * f.h * 4);
  for (size_t p = 0; p < size_t(f.w) * f.h; ++p) {
    for (int q = 0; q < 3; ++q)
      o[p * 4 + q] = u8(std::clamp(f.c[p * 4 + q], 0.f, 1.f) * 255.f + 0.5f);
    o[p * 4 + 3] = u8(std::clamp(f.c[p * 4 + 3], 0.f, 1.f) * 255.f + 0.5f);
  }
  return o;
}
// ---- effects ----------------------------------------------------------------------------------
/// One native effect: a layout, its loop animation (+ optional state animation at a frame), the
/// framing on a 96 px canvas (calibrated against the native captures) and the loop length at 20 fps:
/// one true period of the animation (orbs on 8 s, off 4 s -- every track repeats there, measured
/// frame-identical; the Medallion's LoopRandom is a real 32 s loop), which keeps a full Key Items page
/// inside the runtime's 64 MB module-image budget instead of re-rendering evicted frames.
struct Effect {
  const char *id;
  const char *layout;
  const char *loop;
  const char *state_anim;
  float state_frame;
  float scale, cy;
  int frames20;
};
inline constexpr std::array<Effect, 17> kEffects{{
    {"orb_zora_on", "PaEiketuEfB_00", "PaEiketuEfB_00_LoopRandom", nullptr, 0, 1.0f, 8, 160},
    {"orb_zora_off", "PaEiketuEfB_00", "PaEiketuEfB_00_LoopRandom", "PaEiketuEfB_00_Check", 4, 1.0f, 8, 80},
    {"orb_rito_on", "PaEiketuEfG_00", "PaEiketuEfG_00_LoopRandom", nullptr, 0, 1.0f, 8, 160},
    {"orb_rito_off", "PaEiketuEfG_00", "PaEiketuEfG_00_LoopRandom", "PaEiketuEfG_00_Check", 4, 1.0f, 8, 80},
    {"orb_goron_on", "PaEiketuEfR_00", "PaEiketuEfR_00_LoopRandom", nullptr, 0, 1.0f, 8, 160},
    {"orb_goron_off", "PaEiketuEfR_00", "PaEiketuEfR_00_LoopRandom", "PaEiketuEfR_00_Check", 4, 1.0f, 8, 80},
    {"orb_gerudo_on", "PaEiketuEfY_00", "PaEiketuEfY_00_LoopRandom", nullptr, 0, 1.0f, 8, 160},
    {"orb_gerudo_off", "PaEiketuEfY_00", "PaEiketuEfY_00_LoopRandom", "PaEiketuEfY_00_Check", 4, 1.0f, 8, 80},
    {"orbdlc_zora_on", "PaEiketuEfBDLC_00", "PaEiketuEfBDLC_00_LoopRandom", nullptr, 0, 1.0f, 8, 160},
    {"orbdlc_zora_off", "PaEiketuEfBDLC_00", "PaEiketuEfBDLC_00_LoopRandom", "PaEiketuEfBDLC_00_Check", 4,
     1.0f, 8, 80},
    {"orbdlc_rito_on", "PaEiketuEfGDLC_00", "PaEiketuEfGDLC_00_LoopRandom", nullptr, 0, 1.0f, 8, 160},
    {"orbdlc_rito_off", "PaEiketuEfGDLC_00", "PaEiketuEfGDLC_00_LoopRandom", "PaEiketuEfGDLC_00_Check", 4,
     1.0f, 8, 80},
    {"orbdlc_goron_on", "PaEiketuEfRDLC_00", "PaEiketuEfRDLC_00_LoopRandom", nullptr, 0, 1.0f, 8, 160},
    {"orbdlc_goron_off", "PaEiketuEfRDLC_00", "PaEiketuEfRDLC_00_LoopRandom", "PaEiketuEfRDLC_00_Check", 4,
     1.0f, 8, 80},
    {"orbdlc_gerudo_on", "PaEiketuEfYDLC_00", "PaEiketuEfYDLC_00_LoopRandom", nullptr, 0, 1.0f, 8, 160},
    {"orbdlc_gerudo_off", "PaEiketuEfYDLC_00", "PaEiketuEfYDLC_00_LoopRandom", "PaEiketuEfYDLC_00_Check", 4,
     1.0f, 8, 80},
    {"medallion", "PaWarpEfDLC_00", "PaWarpEfDLC_00_LoopRandom", nullptr, 0, 0.9f, 8, 640},
}};
/// Native UI loops sampled as curves (botw_fx_curves.h): no frames rendered, the module publishes
/// the native alpha/scale values each tick for widgets that draw the player's romfs art directly.
inline constexpr std::array<const char *, 7> kCurveAnims{{
    "PaMapIconBtn_00_Loop", "PaMapIconChallenge_00_Loop", "PaLoadingIconNoWait_00_Loop",
    "PaWeather_00_Loop", "PaTempMeterGaugeLoop_00_Loop", "PaAppAllRadarGauge_00_Loop",
    "PaSinJu_00_Loop"}};
/// 1x1 transparent image every page shows, so the asset worker loads the layouts (and with them the
/// curves) even on a page without an animated frame.
inline constexpr std::string_view kProbeEffect = "probe";
inline const Effect *FindEffect(std::string_view id) {
  for (const auto &e : kEffects)
    if (id == e.id)
      return &e;
  return nullptr;
}
/// Effect id for a key item actor ("" = not animated).
inline std::string EffectFor(std::string_view actor, bool on) {
  struct R {
    std::string_view actor, id;
  };
  static constexpr R map[]{
      {"Obj_HeroSoul_Zora", "orb_zora"},          {"Obj_HeroSoul_Rito", "orb_rito"},
      {"Obj_HeroSoul_Goron", "orb_goron"},        {"Obj_HeroSoul_Gerudo", "orb_gerudo"},
      {"Obj_DLC_HeroSoul_Zora", "orbdlc_zora"},   {"Obj_DLC_HeroSoul_Rito", "orbdlc_rito"},
      {"Obj_DLC_HeroSoul_Goron", "orbdlc_goron"}, {"Obj_DLC_HeroSoul_Gerudo", "orbdlc_gerudo"}};
  if (actor == "Obj_WarpDLC")
    return "medallion";
  for (const auto &r : map)
    if (r.actor == actor)
      return std::string(r.id) + (on ? "_on" : "_off");
  return {};
}
/// "module:fx/<effect>/<frame>" (frame at 20 fps inside the effect's loop).
inline std::string FrameKey(std::string_view effect, int frame) {
  return "module:fx/" + std::string(effect) + "/" + std::to_string(frame);
}
inline bool ParseFrameKey(std::string_view key, std::string &effect, int &frame) {
  constexpr std::string_view p = "module:fx/";
  if (key.substr(0, p.size()) != p)
    return false;
  key.remove_prefix(p.size());
  const auto slash = key.find('/');
  if (slash == std::string_view::npos || slash == 0)
    return false;
  effect = std::string(key.substr(0, slash));
  frame = 0;
  const auto num = key.substr(slash + 1);
  if (num.empty() || num.size() > 5)
    return false;
  for (char c : num) {
    if (c < '0' || c > '9')
      return false;
    frame = frame * 10 + (c - '0');
  }
  if (effect == kProbeEffect)
    return frame == 0;
  const auto *e = FindEffect(effect);
  return e && frame < e->frames20;
}
/// Reads the player's Common.sblarc once (on first need), keeps only what the effects use, and
/// renders frames. Thread-safe; meant for the module asset worker.
class Renderer {
public:
  using ReadFn = std::function<bool(const char *path, std::vector<u8> &out)>;
  /// Loads layouts/anims/textures; false (sticky) when the game files are not readable.
  bool Ensure(const ReadFn &read) {
    std::scoped_lock lock{mu_};
    if (state_ == 1)
      return true;
    if (state_ == 2)
      return false;
    if (Load(read)) {
      state_ = 1;
      Availability().store(1);
      return true;
    }
    // A read can fail transiently (romfs not owned yet); give up only after a few tries.
    layouts_.clear();
    anims_.clear();
    tex_.clear();
    if (++attempts_ >= 3) {
      state_ = 2;
      Availability().store(2);
    }
    return false;
  }
  bool Render(std::string_view effect, int frame20, std::vector<u8> &rgba, int &w, int &h) {
    if (effect == kProbeEffect) {
      std::scoped_lock lock{mu_};
      if (state_ != 1)
        return false;
      rgba.assign(4, 0);
      w = h = 1;
      return true;
    }
    const Effect *e = FindEffect(effect);
    if (!e)
      return false;
    std::scoped_lock lock{mu_};
    if (state_ != 1)
      return false;
    const auto li = layouts_.find(e->layout);
    const auto ai = anims_.find(e->loop);
    if (li == layouts_.end() || ai == anims_.end())
      return false;
    Lookup lk;
    lk.anims.push_back({&ai->second, frame20 * 1.5f});
    if (e->state_anim) {
      const auto si = anims_.find(e->state_anim);
      if (si != anims_.end())
        lk.anims.push_back({&si->second, e->state_frame});
    }
    Options o;
    o.size = 96;
    o.scale = e->scale;
    o.cy = e->cy;
    o.capture_root = "N_Capture_00";
    o.skip_capture = skip_capture_;
    Composer c(li->second, tex_, lk, o);
    const Frame f = c.Run();
    rgba = ToRgba8(f);
    w = f.w;
    h = f.h;
    return true;
  }

private:
  bool Load(const ReadFn &read) {
    std::vector<u8> raw, sarc;
    if (!read(kCommonPath, raw) || !Yaz0(raw, sarc))
      return false;
    raw.clear();
    raw.shrink_to_fit();
    std::map<std::string, std::vector<u8>, std::less<>> files;
    auto want = [](const std::string &n) {
      if (n == "timg/__Combined.bntx")
        return true;
      for (const char *c : kCurveAnims)
        if (n == "anim/" + std::string(c) + ".bflan")
          return true;
      for (const auto &e : kEffects) {
        const std::string L = e.layout;
        if (n == "blyt/" + L + ".bflyt" || n == "anim/" + std::string(e.loop) + ".bflan" ||
            (e.state_anim && n == "anim/" + std::string(e.state_anim) + ".bflan"))
          return true;
      }
      return false;
    };
    if (!Sarc(sarc, want, files))
      return false;
    sarc.clear();
    sarc.shrink_to_fit();
    for (const auto &e : kEffects) {
      if (!layouts_.count(e.layout)) {
        Layout L;
        auto it = files.find("blyt/" + std::string(e.layout) + ".bflyt");
        if (it == files.end() || !ParseBflyt(it->second, L))
          return false;
        layouts_[e.layout] = std::move(L);
      }
      for (const char *an : {e.loop, e.state_anim}) {
        if (!an || anims_.count(an))
          continue;
        Anim A;
        auto it = files.find("anim/" + std::string(an) + ".bflan");
        if (it == files.end() || !ParseBflan(it->second, A))
          return false;
        anims_[an] = std::move(A);
      }
    }
    for (const char *c : kCurveAnims) {
      if (anims_.count(c))
        continue;
      Anim A;
      auto it = files.find("anim/" + std::string(c) + ".bflan");
      if (it != files.end() && ParseBflan(it->second, A))
        anims_[c] = std::move(A); // optional: a missing curve keeps its widget's fallback
    }
    const auto bntx = files.find("timg/__Combined.bntx");
    if (bntx == files.end())
      return false;
    for (const auto &[n, L] : layouts_)
      for (const auto &m : L.mats)
        for (const auto &t : m.tex) {
          if (t.empty() || t.rfind("FBLayout", 0) == 0 || tex_.count(t))
            continue;
          Tex T;
          if (DecodeBntx(bntx->second, t, T))
            tex_[t] = std::move(T);
        }
    return !tex_.empty();
  }
  std::mutex mu_;
  int state_ = 0;
  int attempts_ = 0;

 public:
  bool skip_capture_ = true; ///< tests/parity tool may render the full capture chain
  std::map<std::string, Layout, std::less<>> layouts_;
  std::map<std::string, Anim, std::less<>> anims_;
  std::map<std::string, Tex, std::less<>> tex_;
};
/// The module's one renderer (asset worker renders; the tick reads curves after Availability()==1:
/// layouts/anims/textures are immutable once loaded, and the atomic store publishes them).
inline Renderer &Shared() {
  static Renderer r;
  return r;
}
} // namespace botw::fx
