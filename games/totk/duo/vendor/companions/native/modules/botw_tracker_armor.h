// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "botw_pouch.h"
#include "botw_tracker_metadata.h"
#include <algorithm>
#include <map>
#include <set>
namespace botw::trackers {
struct ArmorTarget { std::string actor; int tier{}; };
struct ArmorPlanRow {
 std::uint64_t address{}; std::string actor,base_actor;
 int current_tier{},target_tier{},max_tier{}; bool selected{},upgradeable{};
};
struct ArmorNeed { std::string actor; int required{},owned{},missing{}; };
struct ArmorPlan {
 bool valid{}; std::vector<ArmorPlanRow> rows; std::vector<ArmorNeed> materials;
 int selected_count{},cancelled_count{};
};
// Read-only planning. Targets belong to exact live pouch instances, not actor names.
// A changed actor, disappearance, or epoch cancels a target. Native upgrades may
// change actor identity; select the upgraded instance again rather than guessing.
class ArmorPlanner {
 std::map<std::uint64_t,ArmorTarget> selected_;
 std::uint64_t epoch_{};
 static bool armor_item(const Item& item) { return item.type>=4 && item.type<=6; }
 static bool validate(const Snapshot& snapshot) {
  if(!snapshot.valid || snapshot.items.size()>420) return false;
  std::set<std::uint64_t> addresses;
  for(const auto& item:snapshot.items) {
   if(!item.address || !addresses.insert(item.address).second) return false;
   if(item.type==7 && (item.value<0 || item.value>999999)) return false;
   if(armor_item(item)) {
    const auto* meta=armor_tier(item.actor);
    if(!meta || !meta->tracked || meta->slot!=item.type-4) return false;
   }
  }
  return true;
 }
 void session(std::uint64_t epoch) { if(epoch_!=epoch) {selected_.clear();epoch_=epoch;} }
 public:
 void clear() { selected_.clear();epoch_=0; }
 const std::map<std::uint64_t,ArmorTarget>& targets() const { return selected_; }
 bool choose(const Snapshot& snapshot,std::uint64_t address,int tier,std::uint64_t epoch) {
  session(epoch);
  if(!epoch || !validate(snapshot) || !address || tier<0 || tier>4) return false;
  const auto it=std::find_if(snapshot.items.begin(),snapshot.items.end(),[&](const Item& i){return i.address==address;});
  if(it==snapshot.items.end() || !armor_item(*it)) return false;
  const auto* meta=armor_tier(it->actor);
  if(tier==0 || tier<=meta->tier) {selected_.erase(address);return true;}
  if(tier>meta->max_tier) return false;
  selected_[address]={it->actor,tier};return true;
 }
 bool choose_all(const Snapshot& snapshot,int tier,std::uint64_t epoch) {
  session(epoch);
  if(!epoch || !validate(snapshot) || tier<1 || tier>4) return false;
  selected_.clear();
  for(const auto& item:snapshot.items) if(armor_item(item)) {
   const auto* meta=armor_tier(item.actor);
   if(meta->tier<tier && meta->max_tier>=tier) selected_[item.address]={item.actor,tier};
  }
  return true;
 }
 ArmorPlan plan(const Snapshot& snapshot,std::uint64_t epoch) {
  session(epoch);ArmorPlan out;
  if(!epoch || !validate(snapshot)) return out;
  std::map<std::string,int> stock,needed;
  for(const auto& item:snapshot.items) if(item.type==7) {
   // Multiple native stacks remain distinct quantities. Sum exactly once before
   // allocating any required material across selected armor instances.
   if(stock[item.actor]>999999-item.value) return {};
   stock[item.actor]+=item.value;
  }
  for(auto it=selected_.begin();it!=selected_.end();) {
   const auto item=std::find_if(snapshot.items.begin(),snapshot.items.end(),[&](const Item& i){return i.address==it->first;});
   if(item==snapshot.items.end() || !armor_item(*item) || item->actor!=it->second.actor) {
    it=selected_.erase(it);++out.cancelled_count;
   } else ++it;
  }
  for(const auto& item:snapshot.items) if(armor_item(item)) {
   const auto* meta=armor_tier(item.actor);const auto target=selected_.find(item.address);
   const bool selected=target!=selected_.end();const int tier=selected?target->second.tier:meta->tier;
   out.rows.push_back({item.address,item.actor,std::string(meta->base_actor),meta->tier,tier,meta->max_tier,selected,meta->max_tier>0});
   if(!selected) continue;
   ++out.selected_count;const ArmorTier* cursor=meta;
   for(int stage=meta->tier;stage<tier;++stage) {
    if(!cursor || !cursor->tracked || cursor->tier!=stage || cursor->next_actor.empty() ||
       std::size_t(cursor->cost_begin)+cursor->cost_count>armor_costs.size()) return {};
    for(unsigned j=0;j<cursor->cost_count;++j) {
     const auto& cost=armor_costs[cursor->cost_begin+j];auto& sum=needed[std::string(cost.actor)];
     if(cost.quantity<=0 || sum>999999-cost.quantity) return {};
     sum+=cost.quantity;
    }
    cursor=armor_tier(cursor->next_actor);
   }
  }
  for(const auto& [actor,required]:needed) {
   const int owned=stock[actor];out.materials.push_back({actor,required,owned,std::max(0,required-owned)});
  }
  out.valid=true;return out;
 }
};
} // namespace botw::trackers
