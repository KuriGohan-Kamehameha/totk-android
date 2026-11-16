// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "botw_pouch.h"
namespace botw {
// Companion history, never an inference about native shortcut sorting (seed() is an explicit fallback).
// Address and actor identity distinguish duplicate actors and reject reused slots.
struct RecentGear {
  struct Identity {uint64_t address{};std::string actor;bool operator==(const Identity&) const=default;};
  Identity current,previous,pinned;
  const Item* find(const Snapshot& pouch,const Identity& id,int type) const {
    if(!pouch.valid || !id.address)return nullptr;
    const Item* found{};
    for(const auto& i:pouch.items)if(i.address==id.address){
      if(found || i.actor!=id.actor || i.type!=type || i.value<=0)return nullptr;
      found=&i;
    }
    return found;
  }
  void update(const Snapshot& pouch,int type,const std::string& attached) {
    if(type!=0 && type!=1){*this={};return;}
    // A transient invalid pouch read (mid-transition, e.g. a weapon breaking) keeps the history;
    // find() re-validates every identity against the next valid pouch.
    if(!pouch.valid)return;
    const Item* equipped{};
    bool ambiguous=false;
    for(const auto& i:pouch.items)if(i.type==type && i.equipped){
      if(equipped || !i.address || i.value<=0 || i.actor!=attached){ambiguous=true;break;}
      equipped=&i;
    }
    // Ambiguous attachment (e.g. a broken weapon still flagged equipped while the game removes it):
    // drop only `current`; the explicit secondary (`pinned`) and `previous` survive and are
    // re-validated below and on every lookup.
    if(ambiguous){current={};if(pinned.address && !find(pouch,pinned,type))pinned={};
      const auto* old=find(pouch,previous,type);if(!old || old->equipped)previous={};return;}
    // FIX ROUND 1 (2026-10-19, item F): an explicit unequip (item 1's new None cell, or any tick
    // with no equipped instance of this type) used to hit the old `if(!equipped){*this={};return;}`
    // and wipe the ENTIRE struct, including `pinned` -- so the companion's own secondary-weapon
    // history (a companion-only convenience; the native game has no "secondary weapon slot"
    // concept at all to compare against, so there is nothing for a real unequip to legitimately
    // invalidate here) was lost the instant the main weapon was unequipped. The apparent
    // "recovery" after re-equipping (seen live) was never a real restore: with `pinned`/`previous`
    // both zeroed, seed()'s cold-load fallback (best-scoring unequipped same-type instance) simply
    // re-picked a candidate by score on the next tick, which happened to coincide with the same
    // weapon in that save.
    //
    // Fix: a tick with nothing equipped only clears `current` (there is no new equipped instance
    // to adopt, and leaving the OLD `current` in place would let the next real equip wrongly
    // "demote" an already-stale, already-unequipped identity into `previous` -- caught by
    // botw_recent_gear_test.cpp's own multi-step-transition case, which a first attempt at this
    // fix regressed). `pinned` is left untouched here entirely; both it and `previous` still go
    // through the existing staleness checks below (dropped only if genuinely gone from the pouch,
    // e.g. consumed/broken), so a pin set before the unequip survives the unequipped gap and is
    // still there, unmodified, whatever gets equipped next.
    if(equipped) {
      Identity next{equipped->address,equipped->actor};
      if(current!=next){if(find(pouch,current,type))previous=current;current=next;}
      // A deliberate A/B swap makes the former primary the backup. Other native
      // equipment changes retain the explicit choice while its exact instance exists.
      if(pinned==next)pinned=previous;
    } else current={};
    if(pinned.address && !find(pouch,pinned,type))pinned={};
    const auto* old=find(pouch,previous,type);
    if(!old || old->equipped)previous={};
  }
  // Owner-approved cold-load fallback: with no real history, offer the best-scoring unequipped
  // instance of the same type. Real swaps replace it through update(); duplicate addresses are skipped.
  template<class Score> void seed(const Snapshot& pouch,int type,Score score) {
    if(!find(pouch,current,type) || find(pouch,previous,type))return;
    const Item* best{};int best_score{-1};
    for(const auto& i:pouch.items) {
      if(i.type!=type || i.equipped || !i.address || i.value<=0)continue;
      unsigned same{};for(const auto& j:pouch.items)same+=j.address==i.address;
      if(same!=1)continue;
      const int value=score(i);
      if(value>best_score || (value==best_score && best && i.address<best->address)){best=&i;best_score=value;}
    }
    if(best)previous={best->address,best->actor};
  }
  bool pin(const Snapshot& pouch,const Identity& id,int type) {
    const auto* item=find(pouch,id,type);
    if(!item || item->equipped)return false;
    pinned=id;return true;
  }
  const Item* target(const Snapshot& pouch,int type) const {
    const auto* old=find(pouch,pinned.address?pinned:previous,type);
    return old && !old->equipped ? old : nullptr;
  }
};
} // namespace botw
