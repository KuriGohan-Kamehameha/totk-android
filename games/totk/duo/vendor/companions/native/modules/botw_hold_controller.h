// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "botw_hold.h"
#include <atomic>
#include <chrono>
namespace botw {
// Runtime captures each published token at pointer-down. A changed row, quantity,
// category, page or epoch retires it: even a delayed callback cannot target the
// replacement item. Tokens are never an address and missing payload0 is refused.
struct HoldTargets {
  using Clock=std::chrono::steady_clock;
  struct Cell {int64_t token{};Item item;};
  std::vector<Cell> cells;uint32_t epoch{};Clock::time_point sampled{};
  static bool same(const Item&a,const Item&b){
    return a.address==b.address&&a.actor==b.actor&&a.type==b.type&&a.value==b.value&&a.equipped==b.equipped;
  }
  static int64_t issue(){
    static std::atomic<uint64_t> serial{1};
    const auto value=serial.fetch_add(1,std::memory_order_relaxed);
    return value&&value<=uint64_t(INT64_MAX)?int64_t(value):0;
  }
  void clear(){cells.clear();epoch=0;sampled={};}
  void observe(const Snapshot& data,int category,uint32_t session,Clock::time_point now){
    if(!data.valid||!session||category!=4){clear();return;}
    if(epoch!=session||(epoch&&now-sampled>std::chrono::milliseconds(250)))clear();
    const auto rows=inventory_items(data,category);std::vector<Cell> next;
    for(size_t i=0;i<rows.size();++i){const auto& item=*rows[i];
      const bool unchanged=i<cells.size()&&same(cells[i].item,item);
      next.push_back({holdable_material(item)?(unchanged?cells[i].token:issue()):0,item});
    }
    cells=std::move(next);epoch=session;sampled=now;
  }
  const Item* target(int64_t token,uint32_t session,Clock::time_point now)const{
    if(token<=0||!epoch||session!=epoch||now<sampled||now-sampled>std::chrono::milliseconds(100))return nullptr;
    for(const auto& cell:cells)if(cell.token==token&&holdable_material(cell.item))return &cell.item;
    return nullptr;
  }
};
// A guest acknowledgment is not completion: validate one reserved native unit,
// including every alias of a repeated item. Actual carry readiness is checked
// separately by the bridge against the native carry manager/link.
inline bool held_add_confirmed(const HeldTray& before,const HeldTray& after,const Item& item){
  if(!before.valid||!after.valid||before.manager!=after.manager||before.count>=5||
     after.count!=before.count+1||!holdable_material(item))return false;
  unsigned added=5;for(unsigned i=0;i<5;++i)if(!before.slots[i].address){added=i;break;}
  if(added==5)return false;
  for(unsigned i=0;i<5;++i){const auto& old=before.slots[i];const auto& now=after.slots[i];
    if(i==added){if(now.address!=item.address||now.actor!=item.actor||now.remaining!=item.value-1||now.flag8!=1||now.flag9)return false;}
    else if(old.address){
      const bool duplicate=old.address==item.address;
      if(now.address!=old.address||now.actor!=old.actor||now.remaining!=old.remaining-int(duplicate)||
         now.flag8!=1||now.flag9!=(duplicate?1:old.flag9))return false;
    }else if(now.address)return false;
  }
  return true;
}
// Owner item 8d, iteration 8 round 2 (2026-10-18): tap a single occupied tray box -> remove just
// that ONE unit, not the whole tray. New RE this round (dsmod-re-analyst, see NOTES.md) found no
// native function that releases a single reserved slot -- native_put_away (main+0x10C5F50) is an
// unrolled, unconditional, all-5-slots routine with no slot-index parameter, and native_hold
// (main+0x10C3BE0) is add-only. The analyst's own recommended composition: issue the existing
// bulk put-away (op 12) once, then re-issue the existing per-item hold (op 11) once for each KEPT
// slot's original item address, in original slot order, as a sequence of already-proven native
// calls -- never a new/undiscovered native entry point. This is a pure, independently testable
// helper for the KEPT list the sequence must replay; the sequence itself (bridge round trips,
// one per step, each verified against a fresh held_tray() read before advancing) lives in
// 01007EF00011E000.cpp's State (HoldUnpick) next to the existing, structurally identical
// `recipe_hold` sequencer (Recipes "Hold ingredients": also a vector + index driven forward one
// confirmed native call at a time).
inline std::vector<uint64_t> hold_unpick_keep(const HeldTray& tray,unsigned target){
  std::vector<uint64_t> out;
  if(!tray.valid||target>=5||!tray.slots[target].address)return out;
  for(unsigned i=0;i<5;++i)if(i!=target&&tray.slots[i].address)out.push_back(tray.slots[i].address);
  return out;
}
inline bool held_return_confirmed(const HeldTray& before,const HeldTray& after,const Snapshot& inventory){
  if(!before.valid||!before.count||!after.valid||after.count||before.manager!=after.manager||!inventory.valid)return false;
  for(unsigned i=0;i<5;++i){const auto& old=before.slots[i];if(!old.address)continue;
    unsigned units{};for(const auto& slot:before.slots)if(slot.address==old.address)++units;
    const Item* item{};unsigned matches{};
    for(const auto& candidate:inventory.items)if(candidate.address==old.address){item=&candidate;++matches;}
    if(matches!=1||item->actor!=old.actor||item->type!=7||item->equipped||item->value!=old.remaining+int(units))return false;
  }
  return true;
}
} // namespace botw
