// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Native BOTW UI loops sampled as curves (owner 2026-10-22: "do all now 1.0.0"). The player's own
// Layout/Common.sblarc animations (loaded once by botw_layout_fx.h's renderer on the asset worker)
// drive widgets that already draw the player's romfs art: the module evaluates the native keys at
// the game's UI rate and publishes alpha levels / scales, so nothing is rendered or packaged.
//   pins       PaMapIconBtn_00_Loop        P_Flash_08 alpha 150<->78, GradationFire_00 texture scroll
//   quest ring PaMapIconChallenge_00_Loop  P_Effect_00 scale + alpha, P_Icon_00 white blue 100->0->100
//   spinner    PaLoadingIconNoWait_00_Loop Circle8_00..11 scale (six arms, dot + faint halo)
//   weather    PaWeather_00_Loop           N_Loop_00 alpha 255->0->255 (the current slot's corner glow)
//   temp halo  PaTempMeterGaugeLoop_00_Loop P_Glow_00 alpha 64<->255
//   sensor     PaAppAllRadarGauge_00_Loop  N_Loop_01 alpha 255<->150 (while the sensor tracks)
//   champions  PaSinJu_00_Loop             N_Loop_00 alpha 100->0->100 (relative to the glow's own alpha)
// Each falls back to its previous behaviour when the game files cannot be read.
#include "botw_layout_fx.h"
#include <array>
#include <cmath>
#include <optional>
namespace botw::fx {
/// BOTW's UI layouts advance one key frame per 30 fps game frame (the orb/Medallion parity in
/// the layout-animation captures matched the native captures at exactly this rate).
inline constexpr double kUiFps = 30.0;
inline constexpr int kAlphaLevels = 16; ///< tint_colors steps for the alpha-driven widgets
struct CurveRef {
  const Track *track = nullptr;
  float period = 0;
  float At(double native_frame) const {
    if (!track)
      return 0;
    const double f = period > 0 ? std::fmod(native_frame, double(period)) : native_frame;
    return Eval(*track, float(f < 0 ? f + period : f));
  }
};
struct NativeCurves {
  CurveRef flash, flame, ring_scale, ring_alpha, quest_blue, weather, temp, sensor, champ;
  std::array<CurveRef, 12> spin{};
  bool pins = false, quest = false, spinner = false;
};
inline CurveRef Resolve(const Renderer &r, const char *anim, std::string_view entry, std::string_view magic,
                        int index, int target) {
  CurveRef c;
  const auto it = r.anims_.find(anim);
  if (it == r.anims_.end())
    return c;
  for (const auto &t : it->second.tracks)
    if (t.entry == entry && t.magic == magic && t.index == index && t.target == target && !t.keys.empty()) {
      c.track = &t;
      c.period = float(it->second.frames);
      break;
    }
  return c;
}
inline NativeCurves ResolveAll(const Renderer &r) {
  NativeCurves n;
  n.flash = Resolve(r, "PaMapIconBtn_00_Loop", "P_Flash_08", "FLVC", 0, 16);
  n.flame = Resolve(r, "PaMapIconBtn_00_Loop", "GradationFire_00", "FLTS", 1, 1);
  n.ring_scale = Resolve(r, "PaMapIconChallenge_00_Loop", "P_Effect_00", "FLPA", 0, 6);
  n.ring_alpha = Resolve(r, "PaMapIconChallenge_00_Loop", "P_Effect_00", "FLVC", 0, 16);
  n.quest_blue = Resolve(r, "PaMapIconChallenge_00_Loop", "P_Icon_00", "FLMC", 0, 6);
  n.weather = Resolve(r, "PaWeather_00_Loop", "N_Loop_00", "FLVC", 0, 16);
  n.temp = Resolve(r, "PaTempMeterGaugeLoop_00_Loop", "P_Glow_00", "FLVC", 0, 16);
  n.sensor = Resolve(r, "PaAppAllRadarGauge_00_Loop", "N_Loop_01", "FLVC", 0, 16);
  n.champ = Resolve(r, "PaSinJu_00_Loop", "N_Loop_00", "FLVC", 0, 16);
  bool spin_ok = true;
  for (int k = 0; k < 12; ++k) {
    char name[16];
    std::snprintf(name, sizeof name, "Circle8_%02d", k);
    n.spin[size_t(k)] = Resolve(r, "PaLoadingIconNoWait_00_Loop", name, "FLPA", 0, 6);
    spin_ok = spin_ok && n.spin[size_t(k)].track;
  }
  n.pins = n.flash.track && n.flame.track;
  n.quest = n.ring_scale.track && n.ring_alpha.track && n.quest_blue.track;
  n.spinner = spin_ok;
  return n;
}
/// The resolved curves once the player's layouts are loaded, else nullptr (fallbacks apply).
inline const NativeCurves *Curves() {
  if (Availability().load() != 1)
    return nullptr;
  static const NativeCurves n = ResolveAll(Shared());
  return &n;
}
inline int Level(double unit, int levels = kAlphaLevels) {
  if (!std::isfinite(unit))
    unit = 0;
  return int(std::lround(std::clamp(unit, 0.0, 1.0) * (levels - 1)));
}
/// Values published every tick (see Publish); `loaded` = 0 means every field is its fallback.
struct FxValues {
  int loaded = 0, weather = kAlphaLevels - 1, temp = kAlphaLevels - 1, sensor = kAlphaLevels - 1,
      champ = kAlphaLevels - 1;
  std::array<int, 12> spin{}; ///< scale x1000 (dots 500..1000, halos 1500..2000)
};
/// Spinner fallback: the native shape (dots 0.5 -> 1 -> 0.5 every 20 frames, odd arms half a cycle
/// later; halos the same +1.0) as a cosine, for a player whose files cannot be read.
inline int SpinFallback(int k, double native_frame) {
  const bool odd = (k % 2) == 1, halo = k >= 6;
  const double ph = std::fmod(native_frame + (odd ? 10.0 : 0.0), 20.0) / 20.0;
  const double s = 0.75 - 0.25 * std::cos(2.0 * 3.14159265358979323846 * ph) + (halo ? 1.0 : 0.0);
  return int(std::lround(s * 1000.0));
}
inline FxValues Sample(const NativeCurves *n, double seconds) {
  FxValues v;
  const double f = std::max(0.0, seconds) * kUiFps;
  for (int k = 0; k < 12; ++k)
    v.spin[size_t(k)] = (n && n->spinner) ? int(std::lround(n->spin[size_t(k)].At(f) * 1000.0)) : SpinFallback(k, f);
  if (!n)
    return v;
  v.loaded = 1;
  if (n->weather.track)
    v.weather = Level(n->weather.At(f) / 255.0);
  if (n->temp.track)
    v.temp = Level(n->temp.At(f) / 255.0);
  if (n->sensor.track)
    v.sensor = Level(n->sensor.At(f) / 255.0);
  if (n->champ.track)
    v.champ = Level(n->champ.At(f) / 100.0);
  return v;
}
inline void Publish(const EdenDsmodHostApi &h, const FxValues &v) {
  h.publish_i64(h.userdata, "botw.fx.loaded", v.loaded);
  h.publish_i64(h.userdata, "botw.fx.weather", v.weather);
  h.publish_i64(h.userdata, "botw.fx.temp", v.temp);
  h.publish_i64(h.userdata, "botw.fx.sensor", v.sensor);
  h.publish_i64(h.userdata, "botw.fx.champ", v.champ);
  for (int k = 0; k < 12; ++k) {
    char key[32];
    std::snprintf(key, sizeof key, "botw.fx.spin.%d", k);
    h.publish_i64(h.userdata, key, v.spin[size_t(k)]);
  }
}
} // namespace botw::fx
