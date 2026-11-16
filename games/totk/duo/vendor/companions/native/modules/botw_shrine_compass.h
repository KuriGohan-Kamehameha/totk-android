// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "botw_map.h"
#include "botw_map_view.h"
#include "botw_tracker_data.h"      // TrackerFlagReader, tracker_crc, tracker_dlc_sample, tracker_shrines
#include "botw_tracker_dlc_locations.h"
#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <vector>
namespace botw {

// Opt-in shrine compass (owner decision, 2026-10-18 ITER5-BRIEF.md "shrine compass"): the Ready
// sensor dial keeps the game's own proximity signal unconditionally (botw_sensor.h, untouched by
// this file -- "it beep for the one who not want to spoiled"). Tapping the dial opts in to a
// spoiler: a big arrow to the nearest UNDISCOVERED shrine. This reuses the exact position/flags
// the Trackers already read -- botw_map.h map_position(), botw_map_view.h map_view()/
// view_rotation()/turn_degrees(), and botw_tracker_data.h's TrackerFlagReader + tracker_shrines +
// tracker_dlc_shrine_locations (the same table/flags botw_tracker_data.h's shrine() lambda
// reads) -- no new RE chain and no new live memory layout.
//
// Deliberately NOT shown: the shrine's name or region. A per-shrine region label would need a
// live region-boundary RE chain (botw_map_regions.h only reads which of the 8 broad regions is
// currently *visible on the minimap*, not "which region contains an arbitrary world point") that
// is out of scope for this offline-only round; the brief's own fallback ("the shrine's region or
// nothing") is taken literally rather than guessing a boundary and risking a wrong/spoiler-ish
// label. Only distance + an optional static "Nearest shrine" caption are shown.

struct ShrineCompass {
  bool valid{};                 // this sample's readers (position + GameData flags) agreed twice
  bool has_target{};            // an undiscovered shrine was found and selected
  bool all_found{};             // every *considered* shrine (base, + DLC if detected) is found
  bool previous_target_found{}; // when a previous_target_id was given: is it found now?
  double bearing{};             // screen-space clockwise degrees [0,360): world bearing to the
                                 // target + the current map view rotation (botw.map.view.rotation)
                                 // -- the same "+rotation" rule botw_map_view.h's view_heading()
                                 // already applies to the player arrow, so the compass arrow turns
                                 // with the camera exactly like the rotating circle map.
  double distance{};            // metres: game world units, straight 2D distance on the map plane
                                 // (same scale botw.ready.quest.distance already uses for quests).
  std::string target_id;        // "DungeonNNN" of the selected shrine; empty when !has_target
};

// Same "found" test the Trackers status line already uses (botw_tracker_data.h's shrine()
// lambda: Clear_<id> true -> "Completed"; else Location_<id> (seen) > 0 -> "Discovered"). Kept as
// one function, templated over the flags reader so host tests can supply a fake with no live
// memory (mirrors botw_tracker_data_test.cpp's `Flags` fixture for tracker_data_records<Flags>).
template <class Flags>
inline bool shrine_found(Flags& flags, const std::string& id) {
  const auto cleared = flags.boolean(tracker_crc("Clear_" + id));
  if (cleared && *cleared) return true;
  const auto seen = flags.integer(tracker_crc("Location_" + id));
  return seen && *seen > 0;
}

struct ShrineCandidate {
  std::string id;
  float x{}, y{};
  bool found{};
};

// Candidate list from the real shrine tables. DLC shrines are included only when `dlc_on` is
// true (unknown/false both exclude them, matching the brief: "consider DLC shrines only if DLC
// detected"). Templated over Flags for the same reason as shrine_found() above.
template <class Flags>
inline std::vector<ShrineCandidate> shrine_candidates(Flags& flags, std::optional<bool> dlc_on) {
  std::vector<ShrineCandidate> out;
  out.reserve(136);
  for (const auto& e : tracker_shrines) out.push_back({e.id, e.x, -e.z, shrine_found(flags, e.id)});
  if (dlc_on && *dlc_on)
    for (const auto& l : tracker_dlc_shrine_locations)
      if (std::isfinite(l.x) && std::isfinite(l.z)) out.push_back({l.id, l.x, -l.z, shrine_found(flags, l.id)});
  return out;
}

// Pure nearest-undiscovered search + bearing/distance math over an already-assembled candidate
// list. No host/live memory involved -- directly host-testable (nearest selection, bearing wrap,
// the "all discovered" case).
inline ShrineCompass nearest_undiscovered(const std::vector<ShrineCandidate>& candidates, double px, double py, double rotation) {
  ShrineCompass out;
  out.valid = true;
  if (candidates.empty()) return out;
  unsigned found_count = 0;
  double best = -1;
  const ShrineCandidate* pick = nullptr;
  for (const auto& c : candidates) {
    if (c.found) {
      ++found_count;
      continue;
    }
    const double dx = double(c.x) - px, dy = double(c.y) - py;
    const double d = std::hypot(dx, dy);
    if (!std::isfinite(d)) continue;
    if (!pick || d < best) {
      best = d;
      pick = &c;
    }
  }
  out.all_found = found_count == candidates.size();
  if (pick) {
    out.has_target = true;
    out.target_id = pick->id;
    out.distance = best;
    const double dx = double(pick->x) - px, dy = double(pick->y) - py;
    // Compass bearing (clockwise from north, matching botw_map.h's own heading convention:
    // atan2(forward_x, -forward_z) i.e. atan2(east offset, north offset)), then turned into
    // screen space by adding the live map rotation and wrapping -- turn_degrees() (botw_map_
    // view.h) both wraps to [0,360) and rounds to the companion's 0.5 deg grid, same as every
    // other rotate_bind value the rotating map publishes.
    const double world_bearing = std::atan2(dx, dy) * 180.0 / 3.14159265358979323846;
    out.bearing = turn_degrees(world_bearing + rotation);
  }
  return out;
}

// Close-on-discovery / close-on-all-found decision, pulled out as its own pure function so the
// "auto-close when the shrine becomes discovered" and "all discovered -> closes" rules are each
// one host-testable case, independent of how the sample was obtained.
//
// Iteration 12 item 8 (owner: "the shrine sensor, when activated, makes the Ready map blink"):
// on a save with every shrine found, tapping the dial opened card 1 (map dim + compass) and the
// very first 100 ms compass update closed it again ("all found -> closes"), so the Ready map
// dimmed for one or two frames and came back -- a blink -- and the "All shrines found" message
// was never shown. All-found now keeps the card open with that message for
// kShrineCompassAllFoundHold updates (~2 s at the 100 ms cadence) before closing.
inline constexpr unsigned kShrineCompassAllFoundHold = 20;
inline constexpr unsigned kShrineCompassMissHold = 10;
struct ShrineCompassDecision {
  bool close_requested{};
  std::string next_target; // target to remember for next tick's previous_target_found check
  bool all_found{};        // showing the "All shrines found" message this update
};
inline ShrineCompassDecision shrine_compass_decide(const std::string& previous_target, bool previous_found, const ShrineCompass& sample,
                                                   unsigned all_found_shown = 0) {
  if (!previous_target.empty() && previous_found) return {true, "", false};
  if (sample.valid && sample.has_target) return {false, sample.target_id, false};
  if (sample.valid && sample.all_found) return {all_found_shown >= kShrineCompassAllFoundHold, "", true};
  return {false, "", false};
}

// "1,240 m" / "240 m"; "" for a negative/non-finite input (never fabricates a value).
inline std::string shrine_compass_distance_text(double metres) {
  if (!std::isfinite(metres) || metres < 0) return "";
  const long whole = std::lround(metres);
  std::string digits = std::to_string(whole);
  std::string grouped;
  int c = 0;
  for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
    if (c && c % 3 == 0) grouped.push_back(',');
    grouped.push_back(*it);
    ++c;
  }
  std::reverse(grouped.begin(), grouped.end());
  return grouped + " m";
}

// Live glue: assembles candidates from the actual game, via the already-RE'd readers (map_
// position, map_view/turn_degrees, TrackerFlagReader, tracker_dlc_sample). Not unit tested on its
// own -- same precedent as botw_tracker_data.h's tracker_data(), which wraps the tested
// tracker_data_records<Flags> the same way. Re-validates the DLC identity across the sample like
// tracker_data() does.
inline ShrineCompass shrine_compass_live(const EdenDsmodHostApi& h, const MapPosition& pos, double rotation,
                                          const std::string& previous_target_id, TrackerFlagReader* cache = nullptr) {
  if (!pos.valid || !std::isfinite(pos.x) || !std::isfinite(pos.y)) return {};
  TrackerFlagReader local;
  auto& flags = cache ? *cache : local;
  if (!flags.begin(h)) return {};
  const auto dlc = tracker_dlc_sample(h);
  const bool dlc_on = dlc.valid && dlc.version >= 0x300;
  const auto candidates = shrine_candidates(flags, std::optional<bool>(dlc_on));
  auto out = nearest_undiscovered(candidates, pos.x, pos.y, rotation);
  if (!previous_target_id.empty())
    for (const auto& c : candidates)
      if (c.id == previous_target_id) {
        out.previous_target_found = c.found;
        break;
      }
  if (!flags.finish() || tracker_dlc_sample(h) != dlc) return {};
  return out;
}

// Stateful wrapper the .cpp drives once per 100 ms tick while the overlay is open (never while
// closed -- "published at ~10 Hz only while the compass is open"). Remembers the shown target
// across ticks purely to apply shrine_compass_decide()'s close rule; every other computation is
// a fresh read each call (same "stateless reader, thin stateful wrapper" shape as botw_states.h's
// StateLatch / botw_tracker_view.h's TrackerView).
class ShrineCompassController {
  TrackerFlagReader flags_;
  std::string target_;
  unsigned all_found_shown_{};
  ShrineCompass last_;      // last valid sample, held through a short reader miss
  unsigned missed_{};

 public:
  bool close_requested{};
  ShrineCompass update(const EdenDsmodHostApi& h, bool open, const MapPosition& pos, double rotation) {
    close_requested = false;
    if (!open) {
      target_.clear();
      flags_.clear();
      all_found_shown_ = 0;
      last_ = {};
      missed_ = 0;
      return {};
    }
    const auto out = shrine_compass_live(h, pos, rotation, target_, &flags_);
    // Iteration 12 item 8: a single torn/unstable read (the double-read guard failing) used to
    // publish has_target=0 for one 100 ms update, hiding the arrow/caption/distance and
    // forgetting the target. Hold the last valid sample for up to kShrineCompassMissHold
    // updates; a longer outage still blanks the card as before.
    if (!out.valid && last_.valid && missed_ < kShrineCompassMissHold) {
      ++missed_;
      return last_;
    }
    missed_ = 0;
    last_ = out.valid ? out : ShrineCompass{};
    const auto decision = shrine_compass_decide(target_, out.previous_target_found, out, all_found_shown_);
    close_requested = decision.close_requested;
    target_ = decision.next_target;
    all_found_shown_ = decision.all_found && !close_requested ? all_found_shown_ + 1 : 0;
    return out;
  }
};

inline void publish_shrine_compass(const EdenDsmodHostApi& h, bool open, const ShrineCompass& out) {
  const bool show = open && out.valid;
  h.publish_i64(h.userdata, "botw.compass.shrine.open", open);
  h.publish_i64(h.userdata, "botw.compass.shrine.valid", show);
  h.publish_i64(h.userdata, "botw.compass.shrine.has_target", show && out.has_target);
  h.publish_i64(h.userdata, "botw.compass.shrine.all_found", show && out.all_found);
  publish_degrees(h, "botw.compass.shrine.bearing", show && out.has_target ? out.bearing : 0.);
  h.publish_text(h.userdata, "botw.compass.shrine.distance_text", show && out.has_target ? shrine_compass_distance_text(out.distance).c_str() : "");
  h.publish_text(h.userdata, "botw.compass.shrine.message", show && !out.has_target && out.all_found ? "All shrines found" : "");
}
} // namespace botw
