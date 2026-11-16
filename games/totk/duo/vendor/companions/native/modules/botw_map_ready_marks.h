// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "botw_key.h"
#include "botw_map.h"
#include "botw_map_koroks.h"
#include "botw_map_places.h"
#include "botw_map_landmarks.h"
#include "botw_saved_map_icons.h"
#include <algorithm>
#include <cstring>
#include <vector>
namespace botw {

// Perf round 2, shared contract with the manifest/redraw lane
// (internal notes "Ready compact marker lists", item 1). SUPERSEDES the earlier single-
// merged-list ready_map_marks()/publish_ready_map_marks() (one flat kind-encoded list every
// ReadyHyrule group read from) -- that approach hit a real runtime limitation lane B found and
// documented (internal notes item 2): dynamic_markers groups have no per-
// slot `size`, so a single shared list can't drive groups that draw at 5 different on-screen
// sizes (28/38/40/48/54/64/80/100px) without a runtime change. This version keeps the SAME idea
// (nearest-first, bounded, Ready-only) but publishes one compact list PER manifest group/source
// instead of one merged list, reusing each source's existing per-entry field names unchanged, so
// every group's `size` stays its own and no runtime change is needed. `tools/perf2-ready-compact.py`
// repoints each ReadyHyrule dynamic_markers group's x/y/kind*/icon*/tint* binds from the full
// botw.map.<source>.{i}.* prefix to the matching botw.map.ready.<source>.{i}.* prefix below and
// sets each group's `count` to the cap.
//
// Caps chosen so the realistic population within kReadyCullRadius (~736 world units) fits
// comfortably, matching the brief's examples: koroks 48, places 24 (shared by both the
// kind_place and kind_stable manifest groups -- one compact list, two groups reading the same
// slots), poi/landmarks 24 (shared by all six shrine/tower x halo/border/core manifest groups),
// stamps 16, edit (player-placed) records 32. moving (9) and special (4) are already <= any
// plausible cap and native_death has exactly one entry, so those three sources are left on their
// existing full per-entry publish (botw::publish_map_moving/special/death) -- compacting them
// would add code and candidate-collection cost for zero saved iterations.
inline constexpr unsigned kReadyKorokCap = 48;
inline constexpr unsigned kReadyPlaceCap = 24;
inline constexpr unsigned kReadyPoiCap = 24;
inline constexpr unsigned kReadyStampCap = 16;
inline constexpr unsigned kReadyEditCap = 32;

namespace ready_compact_detail {
// Nearest-`cap` selection shared by every source below: collect (index, dist2) for every
// currently-visible entry, partial_sort by squared distance to the player, keep the first `cap`.
// `cap` and the number of currently-visible entries are both small (<=48 after the window cull),
// so this is cheap; it replaces republishing every source's full (up to 900) entry array.
template <typename Entries, typename Visible>
inline std::vector<std::pair<unsigned, float>> nearest(const Entries& entries, unsigned n,
                                                         const MapPosition& pos, Visible&& visible) {
  std::vector<std::pair<unsigned, float>> cand;
  cand.reserve(64);
  if (!pos.valid) return cand;
  for (unsigned i = 0; i < n; ++i) {
    const auto& e = entries[i];
    if (!visible(e)) continue;
    const float x = e.xyz[0], y = -e.xyz[2];
    if (!WithinReadyWindow(e.xyz[0], e.xyz[2], pos)) continue;
    const float dx = x - pos.x, dy = y - pos.y;
    cand.emplace_back(i, dx * dx + dy * dy);
  }
  return cand;
}
inline void take_nearest(std::vector<std::pair<unsigned, float>>& cand, unsigned cap) {
  const unsigned n = std::min<unsigned>(cap, static_cast<unsigned>(cand.size()));
  std::partial_sort(cand.begin(), cand.begin() + n, cand.end(),
                     [](const auto& a, const auto& b) { return a.second < b.second; });
  cand.resize(n);
}
} // namespace ready_compact_detail

// --- koroks: botw.map.ready.koroks.{i}.{x,y,kind,tint} -----------------------------------------
inline void publish_ready_koroks(const EdenDsmodHostApi& h, const MapKoroks& v, KorokFilter filter,
                                  const MapPosition& pos) {
  using namespace ready_compact_detail;
  auto cand = v.valid ? nearest(v.entries, 900u, pos,
                                 [&](const MapKoroks::Entry& e) { return korok_shown(e, filter); })
                       : std::vector<std::pair<unsigned, float>>{};
  take_nearest(cand, kReadyKorokCap);
  for (unsigned i = 0; i < kReadyKorokCap; ++i) {
    Key key("botw.map.ready.koroks.", i);
    if (i < cand.size()) {
      const auto& e = v.entries[cand[i].first];
      h.publish_i64(h.userdata, key(".kind"), 17);
      h.publish_i64(h.userdata, key(".tint"), e.found ? 0xFFF6DC9D : 0xFF9DAEAB);
      if (h.publish_f64) {
        h.publish_f64(h.userdata, key(".x"), quantize_display(e.xyz[0]));
        h.publish_f64(h.userdata, key(".y"), quantize_display(-e.xyz[2]));
      }
    } else {
      h.publish_i64(h.userdata, key(".kind"), -1);
      h.publish_i64(h.userdata, key(".tint"), 0);
    }
  }
}

// --- places: botw.map.ready.places.{i}.{x,y,kind_place,kind_stable,icon,tint} ------------------
inline void publish_ready_places(const EdenDsmodHostApi& h, const MapPlaces& v, const MapPosition& pos) {
  using namespace ready_compact_detail;
  auto cand = v.valid ? nearest(v.entries, static_cast<unsigned>(v.entries.size()), pos,
                                 [&](const MapPlaces::Entry& e) { return e.visible && place_art(e.kind); })
                       : std::vector<std::pair<unsigned, float>>{};
  take_nearest(cand, kReadyPlaceCap);
  for (unsigned i = 0; i < kReadyPlaceCap; ++i) {
    Key key("botw.map.ready.places.", i);
    if (i < cand.size()) {
      const auto& e = v.entries[cand[i].first];
      const bool stable = e.kind == 1;
      h.publish_i64(h.userdata, key(".kind_place"), !stable ? e.kind : -1);
      h.publish_i64(h.userdata, key(".kind_stable"), stable ? e.kind : -1);
      h.publish_text(h.userdata, key(".icon"), place_icon_src(e.kind).c_str());
      const bool warp = e.kind == 4 || e.kind == 16;
      h.publish_i64(h.userdata, key(".tint"), warp ? (e.opened ? 0xFF00BFFF : 0xFFFF8A19) : 0xFF967828);
      if (h.publish_f64) {
        h.publish_f64(h.userdata, key(".x"), quantize_display(e.xyz[0]));
        h.publish_f64(h.userdata, key(".y"), quantize_display(-e.xyz[2]));
      }
    } else {
      h.publish_i64(h.userdata, key(".kind_place"), -1);
      h.publish_i64(h.userdata, key(".kind_stable"), -1);
      h.publish_text(h.userdata, key(".icon"), "");
      h.publish_i64(h.userdata, key(".tint"), 0);
    }
  }
}

// --- landmarks/poi: botw.map.ready.poi.{i}.{x,y,kind_shrine,kind_tower,halo,border,core,
//     border_tint,core_tint} ------------------------------------------------------------------
inline void publish_ready_poi(const EdenDsmodHostApi& h, const MapLandmarks& v, const MapPosition& pos) {
  using namespace ready_compact_detail;
  constexpr const char* common = "romfs:/Pack/Bootup.pack#Layout/Common.sblarc#timg/__Combined.bntx#";
  auto cand = v.valid ? nearest(v.entries, static_cast<unsigned>(v.entries.size()), pos,
                                 [&](const MapLandmarks::Entry& e) { return e.visible; })
                       : std::vector<std::pair<unsigned, float>>{};
  take_nearest(cand, kReadyPoiCap);
  for (unsigned i = 0; i < kReadyPoiCap; ++i) {
    Key key("botw.map.ready.poi.", i);
    if (i < cand.size()) {
      const auto& e = v.entries[cand[i].first];
      const bool tower = e.kind == 3;
      h.publish_i64(h.userdata, key(".kind_shrine"), !tower ? e.kind : -1);
      h.publish_i64(h.userdata, key(".kind_tower"), tower ? e.kind : -1);
      h.publish_i64(h.userdata, key(".border_tint"), e.opened ? 0xFF00BFFF : 0xFFFF8A19);
      h.publish_i64(h.userdata, key(".core_tint"), e.cleared ? 0xFF00BFFF : 0xFFFF8A19);
      for (const auto* part : {"core", "border", "halo"}) {
        const std::string art = std::string(common) + (tower ? "Nt_MapTowerNew" : "Nt_MapDungeonNew") +
            (std::strcmp(part, "core") == 0 ? "_00^t" : std::strcmp(part, "halo") == 0 ? (tower ? "_01^t" : "_02^t")
                                                                                        : (tower ? "_02^t" : "_01^t"));
        h.publish_text(h.userdata, key(".", part), art.c_str());
      }
      if (h.publish_f64) {
        h.publish_f64(h.userdata, key(".x"), quantize_display(e.xyz[0]));
        h.publish_f64(h.userdata, key(".y"), quantize_display(-e.xyz[2]));
      }
    } else {
      h.publish_i64(h.userdata, key(".kind_shrine"), -1);
      h.publish_i64(h.userdata, key(".kind_tower"), -1);
      for (const auto* part : {"core", "border", "halo"}) h.publish_text(h.userdata, key(".", part), "");
    }
  }
}

// --- stamps: botw.map.ready.stamps.{i}.{x,y,kind,icon} ------------------------------------------
inline void publish_ready_stamps(const EdenDsmodHostApi& h, const SavedMapIcons& v, const MapPosition& pos) {
  using namespace ready_compact_detail;
  std::vector<std::pair<unsigned, float>> cand;
  if (v.valid && pos.valid) {
    cand.reserve(32);
    for (unsigned i = 0; i < v.icons.size(); ++i) {
      const auto& icon = v.icons[i];
      if (icon.kind < 27 || icon.kind > 35) continue;
      if (!WithinReadyWindow(icon.xyz[0], icon.xyz[2], pos)) continue;
      const float x = icon.xyz[0], y = -icon.xyz[2];
      const float dx = x - pos.x, dy = y - pos.y;
      cand.emplace_back(i, dx * dx + dy * dy);
    }
  }
  take_nearest(cand, kReadyStampCap);
  for (unsigned i = 0; i < kReadyStampCap; ++i) {
    Key key("botw.map.ready.stamps.", i);
    if (i < cand.size()) {
      const auto& icon = v.icons[cand[i].first];
      h.publish_i64(h.userdata, key(".kind"), icon.kind - 27);
      if (h.publish_f64) {
        h.publish_f64(h.userdata, key(".x"), quantize_display(icon.xyz[0]));
        h.publish_f64(h.userdata, key(".y"), quantize_display(-icon.xyz[2]));
      }
      const std::string art = "composite:native-map-stamp-" + std::to_string(icon.kind - 27);
      h.publish_text(h.userdata, key(".icon"), art.c_str());
    } else {
      h.publish_i64(h.userdata, key(".kind"), -1);
      h.publish_text(h.userdata, key(".icon"), "");
    }
  }
}

// --- player-placed/editable records: botw.map.ready.edit.{i}.{x,y,kind} ------------------------
// Same source array (SavedMapIcons) and kind ranges as the "map_markers_full" editable_markers
// loop in 01007EF00011E000.cpp (kind 27..35 saved stamps shown as placed markers OR kind 37..41
// user pins), just capped+nearest-sorted the same way as the other four sources here.
inline void publish_ready_edit(const EdenDsmodHostApi& h, const SavedMapIcons& v, const MapPosition& pos) {
  using namespace ready_compact_detail;
  std::vector<std::pair<unsigned, float>> cand;
  if (v.valid && pos.valid) {
    cand.reserve(32);
    for (unsigned i = 0; i < v.icons.size(); ++i) {
      const auto& icon = v.icons[i];
      const bool active = (icon.kind >= 27 && icon.kind <= 35) || (icon.kind >= 37 && icon.kind <= 41);
      if (!active) continue;
      if (!WithinReadyWindow(icon.xyz[0], icon.xyz[2], pos)) continue;
      const float x = icon.xyz[0], y = -icon.xyz[2];
      const float dx = x - pos.x, dy = y - pos.y;
      cand.emplace_back(i, dx * dx + dy * dy);
    }
  }
  take_nearest(cand, kReadyEditCap);
  for (unsigned i = 0; i < kReadyEditCap; ++i) {
    Key key("botw.map.ready.edit.", i);
    if (i < cand.size()) {
      const auto& icon = v.icons[cand[i].first];
      h.publish_i64(h.userdata, key(".kind"), icon.kind);
      if (h.publish_f64) {
        h.publish_f64(h.userdata, key(".x"), quantize_display(icon.xyz[0]));
        h.publish_f64(h.userdata, key(".y"), quantize_display(-icon.xyz[2]));
      }
    } else {
      h.publish_i64(h.userdata, key(".kind"), -1);
    }
  }
}
} // namespace botw
