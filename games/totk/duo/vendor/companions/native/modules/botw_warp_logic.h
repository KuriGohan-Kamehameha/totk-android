// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Travel (fast travel) request guard + one-shot transaction. Pure logic: no guest reads/writes.
// The native allow-check inputs are sampled by botw_warp.h; the guest helper re-checks every
// native condition at commit (DUOH, prototype/warp-ui-actions.inc).
#include <chrono>
#include <cstdint>
namespace botw::warp {
enum class Block : uint32_t {
  None=0,Unsupported,Busy,NotWorld,Event,Menu,Loading,Prohibited,NotTravelPoint,InFlight,Stale
};
// Current-location inputs. `prohibited` is the game's own Travel allow-check result.
struct Env {
  bool valid{},world{},event{},menu{},loading{},prohibited{},latch{};
  bool operator==(const Env&) const = default;
};
enum class Phase : uint8_t {Idle,Queued,Issued,Out,Loading,Arrival,Uncertain};
inline Block block(const Env& e,bool helper,bool bridge_pending,Phase phase,bool target_enabled) {
  if(!helper)return Block::Unsupported;
  if(phase!=Phase::Idle)return Block::InFlight;
  if(bridge_pending)return Block::Busy;
  if(!e.valid)return Block::NotWorld;
  if(e.loading)return Block::Loading;
  if(e.event||e.latch)return Block::Event;
  if(e.menu)return Block::Menu;
  if(!e.world)return Block::NotWorld;
  if(e.prohibited)return Block::Prohibited;
  if(!target_enabled)return Block::NotTravelPoint;
  return Block::None;
}
inline const char* block_text(Block b) {
  switch(b){
    case Block::None:return "";
    case Block::Unsupported:return "Travel unavailable";
    case Block::Busy:return "Another request is running";
    case Block::NotWorld:return "Travel unavailable here";
    case Block::Event:return "Can't travel during an event";
    case Block::Menu:return "Close the game menu to travel";
    case Block::Loading:return "Can't travel while loading";
    case Block::Prohibited:return "You can't travel from here."; // native AppSystemWindow_00 0016
    case Block::NotTravelPoint:return "Not a travel gate yet";
    case Block::InFlight:return "Already traveling";
    case Block::Stale:return "Map changed; try again";
  }
  return "";
}
// Lifecycle of one issued request. Nothing here can issue a second request: a new one needs a
// new tap AND phase Idle, and Idle is only re-entered from a quiet World observed after the
// arrival (or a refused/never-issued request). A missed lifecycle becomes Uncertain, which only
// clears after the world has been quiet for `settle` -- never by a bare timeout.
struct Transaction {
  using Clock=std::chrono::steady_clock;
  Phase phase{Phase::Idle};
  Clock::time_point at{},quiet_since{};
  bool arrived{};
  static constexpr auto settle=std::chrono::seconds(4);
  void queued(Clock::time_point now){phase=Phase::Queued;at=now;quiet_since={};arrived=false;}
  // Guest acknowledgement: 1 = native request accepted, 2 = refused before any native call,
  // 3 = native call made but its result was not a plain success (treated as in flight).
  void ack(uint32_t result,Clock::time_point now) {
    if(phase!=Phase::Queued)return;
    if(result==2){phase=Phase::Idle;return;}
    phase=result==1?Phase::Issued:Phase::Uncertain;at=now;
  }
  // Host-side cancel of an unacknowledged request: only safe while still Queued, because the
  // guest refuses a request whose permit was withdrawn. Anything later cannot be recalled.
  void cancel_unsent(){if(phase==Phase::Queued)phase=Phase::Uncertain;}
  void observe(const Env& e,Clock::time_point now) {
    const bool quiet=e.valid&&e.world&&!e.event&&!e.loading&&!e.latch&&!e.menu;
    if(!quiet)quiet_since={};else if(quiet_since==Clock::time_point{})quiet_since=now;
    switch(phase){
      case Phase::Idle:return;
      case Phase::Queued:
        // No guest answer: the helper may still run it -> never retry, wait for a quiet world.
        if(now-at>std::chrono::seconds(3)){phase=Phase::Uncertain;at=now;}
        return;
      case Phase::Issued:
        if(e.valid&&e.loading){phase=Phase::Loading;at=now;}
        else if(e.valid&&(e.event||e.latch)){phase=Phase::Out;at=now;}
        else if(now-at>std::chrono::seconds(5)){phase=Phase::Uncertain;at=now;}
        return;
      case Phase::Out:
        if(e.valid&&e.loading){phase=Phase::Loading;at=now;}
        else if(quiet&&now-quiet_since>=settle){phase=Phase::Uncertain;at=now;}
        // Absolute fallback, same shape as Issued's: captured warp arrivals (incl. loading)
        // ran 22-46 s (3 captures); if reads never settle quiet
        // for `settle` and never see a loading screen, don't stay stuck here forever (bug
        // audit 2026-10-19, MEDIUM #2) -- recover to Uncertain, which has its own quiet-based
        // exit back to Idle, so a further Travel tap works again instead of "Already traveling".
        else if(now-at>std::chrono::seconds(60)){phase=Phase::Uncertain;at=now;}
        return;
      case Phase::Loading:
        if(e.valid&&!e.loading){phase=Phase::Arrival;at=now;}
        return;
      case Phase::Arrival:
        if(quiet&&now-quiet_since>=std::chrono::milliseconds(800)){phase=Phase::Idle;arrived=true;}
        return;
      case Phase::Uncertain:
        if(quiet&&now-quiet_since>=settle){phase=Phase::Idle;}
        return;
    }
  }
  const char* text() const {
    switch(phase){
      case Phase::Queued:return "Starting travel";
      case Phase::Issued:case Phase::Out:case Phase::Loading:case Phase::Arrival:return "Traveling...";
      case Phase::Uncertain:return "Travel status unknown; waiting";
      default:return "";
    }
  }
};
} // namespace botw::warp
