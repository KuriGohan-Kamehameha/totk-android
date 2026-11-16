// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Iteration 12 item 5 (owner 2026-10-22): "do not block the bottom screen while waiting for game
// content; during cutscene/dialogue the bottom screen stays shown, dimmed (background dim)".
//
// During an event the UI flags carry bit 0x800 (0x1E09003800 world events/dialogue/warp, 0x0E0902382C
// memory playback, RE-FINDINGS 2026-10-18 section 1). That signature is not World/Menu, so
// botw::context() answers Unknown and every reader fails closed: the pages emptied and the
// "Waiting for game context" screen covered them for the whole event.
//
// Event hold: on the reader cadence (100 ms) the module records everything one World/Menu sample
// published (the runtime clears its snapshot every tick, so a skipped publish would blank a page).
// While the game is in a valid event sample (not title, not loading) right after World/Menu, the
// module republishes that record unchanged, then the fresh game-state keys on top (event, dim
// latch, page). Readers and the native bridge are not run during the hold; native actions refuse
// anyway outside the World signature (they re-check it live). Loading/title end the hold through
// the ordinary path (their own native-styled screens).
#include "botw_context.h"
#include "botw_states.h"
#include <string>
#include <vector>
namespace botw {
// `dim` = the module's dim latch: an event end passes 0x3000 -> 0x4000 -> 0 (bit 0x800 already
// clear, still not the World signature) for a few frames; holding while the latch is still on
// keeps those frames from flashing the "Waiting" screen between the event and World.
inline bool event_hold(UiContext previous, UiContext now, const GameStates &g, bool recorded, bool dim = false) {
  return recorded && (previous == UiContext::World || previous == UiContext::Menu) &&
         now == UiContext::Unknown && g.valid && (g.event || dim) && !g.title && !g.loading;
}
// Flat, capacity-reusing record of one sample's publishes (no per-sample allocation once warm).
class PublishReplay {
public:
  void begin() { ni_ = nf_ = nt_ = 0; recording_ = true; }
  void commit() { recording_ = false; valid_ = true; }
  void discard() { recording_ = false; valid_ = false; }
  bool valid() const { return valid_; }
  bool recording() const { return recording_; }
  void i64(const char *k, int64_t v) { auto &e = slot(ints_, ni_); e.k.assign(k); e.v = v; }
  void f64(const char *k, double v) { auto &e = slot(floats_, nf_); e.k.assign(k); e.v = v; }
  void text(const char *k, const char *v) { auto &e = slot(texts_, nt_); e.k.assign(k); e.v.assign(v ? v : ""); }
  void replay(const EdenDsmodHostApi &h) const {
    if (!valid_) return;
    if (h.publish_i64) for (size_t i = 0; i < ni_; ++i) h.publish_i64(h.userdata, ints_[i].k.c_str(), ints_[i].v);
    if (h.publish_f64) for (size_t i = 0; i < nf_; ++i) h.publish_f64(h.userdata, floats_[i].k.c_str(), floats_[i].v);
    if (h.publish_text) for (size_t i = 0; i < nt_; ++i) h.publish_text(h.userdata, texts_[i].k.c_str(), texts_[i].v.c_str());
  }
  size_t size() const { return ni_ + nf_ + nt_; }
  // Host API copy whose publish_* forward to `real` and record into `this` while recording.
  // userdata is untouched, so every other host call behaves exactly as with `real`.
  const EdenDsmodHostApi *wrap(const EdenDsmodHostApi *real) {
    real_ = real;
    host_ = *real;
    active_ = this;
    if (real->publish_i64) host_.publish_i64 = [](void *u, const char *k, int64_t v) {
      active_->real_->publish_i64(u, k, v); if (active_->recording_) active_->i64(k, v); };
    if (real->publish_f64) host_.publish_f64 = [](void *u, const char *k, double v) {
      active_->real_->publish_f64(u, k, v); if (active_->recording_) active_->f64(k, v); };
    if (real->publish_text) host_.publish_text = [](void *u, const char *k, const char *v) {
      active_->real_->publish_text(u, k, v); if (active_->recording_) active_->text(k, v); };
    return &host_;
  }
private:
  template <class V> struct Entry { std::string k; V v{}; };
  template <class V> static Entry<V> &slot(std::vector<Entry<V>> &v, size_t &n) {
    if (n == v.size()) v.emplace_back();
    return v[n++];
  }
  std::vector<Entry<int64_t>> ints_;
  std::vector<Entry<double>> floats_;
  std::vector<Entry<std::string>> texts_;
  size_t ni_{}, nf_{}, nt_{};
  bool recording_{}, valid_{};
  const EdenDsmodHostApi *real_{};
  EdenDsmodHostApi host_{};
  static inline PublishReplay *active_{};
};
} // namespace botw
