// SPDX-License-Identifier: GPL-3.0-or-later
// Ready-page temperature glow + clothing card (card id 2). Owns the danger
// latch, the plan and the guarded Equip-all / Eat request sequence through the native bridge.
#pragma once
#include "botw_temperature_card.h"
#include "botw_bridge.h"
#include "botw_consumables.h"
#include "botw_key.h"
#include "botw_effects.h"
#include "botw_inventory.h"
#include <chrono>
#include <cstring>
namespace botw {
struct TemperatureCardController {
  using Clock=std::chrono::steady_clock;
  DangerLatch latch;TemperatureSample sample;ClothingPlan plan;TempDanger shown{};
  std::vector<uint64_t> queue;uint32_t queue_epoch{};Clock::time_point queue_deadline{};bool queue_eat{};
  uint64_t in_flight{};std::string status;Clock::time_point status_at{};
  void clear(){latch.clear();sample={};plan={};shown={};queue.clear();in_flight=0;}
  // 100 ms pass, World with a valid pouch and validated player.
  void update(const EdenDsmodHostApi& h,const Snapshot& data,uint64_t player,int64_t now_ms) {
    sample=player ? temperature_sample(h,player) : TemperatureSample{};
    shown=latch.update(sample,now_ms);
    if(shown==TempDanger::None || !data.valid){plan={};return;}
    std::vector<FoodOption> food;const int effect=temp_food_effect(shown);
    for(const auto& i:data.items)if(i.type==8 && !i.equipped && i.value>0) {
      const auto c=consumable(h,i);
      if(c.valid && c.effect==effect && c.level>0 && c.frames>0)food.push_back({i.address,i.actor,c.level,c.frames/30});
    }
    plan=plan_clothing(shown,needed_level(sample,shown),current_level(sample,shown),active_food_level(sample,shown),armor_options(data.items,shown),food);
  }
  void lost(){sample={};shown=latch.update(sample,INT64_MAX/4);plan={};queue.clear();in_flight=0;}
  // Row 1/2 equip all pieces of that row; row 3 eats. Rebuilt from the current plan at tap time.
  bool request(int row,uint32_t epoch) {
    if(!plan.valid || !queue.empty() || in_flight)return false;
    const CardRow& r=row==1?plan.quick:row==2?plan.best:plan.food;
    if(!r.valid)return false;
    if(row==3){if(!r.food)return false;queue={r.food};queue_eat=true;}
    else {if(r.equip.empty())return false;queue=r.equip;queue_eat=false;}
    queue_epoch=epoch;queue_deadline=Clock::now()+std::chrono::seconds(4+3*int(queue.size()));
    set_status(row==3?"Eating":"Equipping");return true;
  }
  void set_status(const char* s){status=s;status_at=Clock::now();}
  // Every sample: issue the next queued piece once the bridge is idle; same guards as the
  // Inventory actions (fresh pouch, epoch, Ready scope, World, no held items).
  void pump(SelectionBridge& b,const Snapshot& data,bool card_open,bool world) {
    if(queue.empty() && !in_flight)return;
    const auto now=Clock::now();
    if(!card_open || !world || !b.healthy || b.epoch!=queue_epoch || now>queue_deadline || !data.valid) {
      queue.clear();in_flight=0;set_status(queue_eat?"Eat cancelled":"Equip cancelled");return;
    }
    if(b.pending)return;
    if(in_flight) {
      // The bridge finished the previous piece: continue only if it was confirmed.
      const bool ok=b.status && (std::strcmp(b.status,"Equipped")==0 || std::strcmp(b.status,"Consumed")==0);
      in_flight=0;
      if(!ok){queue.clear();set_status(b.status&&*b.status?b.status:"Not confirmed");return;}
      if(queue.empty()){set_status(queue_eat?"Eaten":"Equipped");return;}
    }
    if(queue.empty())return;
    const uint64_t next=queue.front();
    const Item* item=nullptr;for(const auto& i:data.items)if(i.address==next)item=&i;
    if(!item){queue.clear();set_status("Item changed");return;}
    if(!queue_eat && item->equipped){queue.erase(queue.begin());if(queue.empty())set_status("Equipped");return;}
    const bool sent=queue_eat ? b.request_consume(next,data,true) : b.request_inventory(next,data,false,false,true);
    if(!sent)return; // transient refusal (freshness/heartbeat): retried next sample until the deadline
    queue.erase(queue.begin());in_flight=next;
  }
  bool busy() const {return !queue.empty() || in_flight;}
  void publish(const EdenDsmodHostApi& h,bool card_open,bool can_act) const {
    auto i64=[&](const char* k,int64_t v){h.publish_i64(h.userdata,k,v);};
    auto txt=[&](const char* k,const char* v){h.publish_text(h.userdata,k,v);};
    const int d=int(shown);
    i64("botw.temperature.valid",sample.valid);
    i64("botw.temperature.danger",d);
    i64("botw.temperature.glow.hot",shown==TempDanger::Heat || shown==TempDanger::Burn);
    i64("botw.temperature.glow.cold",shown==TempDanger::Cold);
    i64("botw.temperature.glow.any",d!=0);
    i64("botw.temperature.glow.burn",shown==TempDanger::Burn);
    i64("botw.temperature.native_hud",sample.valid ? sample.hud_state : 2);
    i64("botw.temperature.need",plan.valid?plan.need:0);
    i64("botw.temperature.level",plan.valid?plan.current:0);
    i64("botw.card.temperature.close_request",card_open && d==0 && !busy());
    char title[64]{};
    if(plan.valid)std::snprintf(title,sizeof(title),"%s",shown==TempDanger::Heat?"Too hot":shown==TempDanger::Cold?"Too cold":"Burning");
    txt("botw.card.temperature.title",title);
    char need[64]{};
    // "Lv N <resistance>": the level the game needs at this temperature / fire level.
    if(plan.valid)std::snprintf(need,sizeof(need),"Lv %d %s",plan.need,shown==TempDanger::Heat?"heat resistance":shown==TempDanger::Cold?"cold resistance":"flame guard");
    txt("botw.card.temperature.need_text",need);
    char compact_need[32]{};
    if(plan.valid)std::snprintf(compact_need,sizeof(compact_need),"Need Lv %d",plan.need);
    txt("botw.card.temperature.need_level_text",compact_need);
    i64("botw.card.temperature.empty",plan.valid && plan.empty);
    const bool fresh=Clock::now()-status_at<std::chrono::seconds(3) || busy();
    txt("botw.card.temperature.status",fresh?status.c_str():"");
    const CardRow* rows[3]={&plan.quick,&plan.best,&plan.food};
    static const char* const heads[3]={"Quickest fix","Best outfit","Eat"};
    for(int r=0;r<3;++r) {
      Key k("botw.card.temperature.row.",uint64_t(r+1));const CardRow& row=*rows[r];
      const bool v=plan.valid && row.valid;
      i64(k(".valid"),v);
      txt(k(".heading"),v?heads[r]:"");
      for(int ic=0;ic<3;++ic)txt(k(".icon.",uint64_t(ic),""),v && size_t(ic)<row.icons.size()?row.icons[size_t(ic)].c_str():"");
      i64(k(".icons"),v?int64_t(row.icons.size()):0);
      char pieces[32]{};
      if(v && r!=2)std::snprintf(pieces,sizeof(pieces),"%d %s",row.pieces,row.pieces==1?"piece":"pieces");
      txt(k(".pieces_text"),pieces);
      // Name key for the native item name (single item rows) via the manifest text_map.
      i64(k(".name"),v && row.icons.size()==1 ? int64_t(actor_key(row.icons[0])) : -1);
      char detail[64]{};
      if(v) {
        // Pieces show as icons; the line carries the resulting level (and why it falls short).
        char lv[24];
        if(row.reaches)std::snprintf(lv,sizeof(lv),"Lv %d",row.level);else std::snprintf(lv,sizeof(lv),"Lv %d of %d",row.level,plan.need);
        if(r==2){char t[16];format_effect_time(row.seconds,t);std::snprintf(detail,sizeof(detail),"%s \xC2\xB7 %s",lv,t);}
        // Only the flame-guard set bonus changes this danger natively (Fireproof, level 3).
        else if(row.set_bonus && plan.kind==TempDanger::Burn)std::snprintf(detail,sizeof(detail),"%s \xC2\xB7 set bonus",lv);
        else if(row.worn)std::snprintf(detail,sizeof(detail),"%s \xC2\xB7 worn",lv);
        else std::snprintf(detail,sizeof(detail),"%s",lv);
      }
      txt(k(".detail"),detail);
      // Iteration 13 item 5: one level/pieces line per radial slice ("Lv 2 · 3 pieces").
      char line[sizeof(detail)+sizeof(pieces)+4]{};
      if(v && pieces[0])std::snprintf(line,sizeof(line),"%s \xC2\xB7 %s",detail,pieces);else if(v)std::snprintf(line,sizeof(line),"%s",detail);
      txt(k(".level_line"),line);
      i64(k(".reaches"),v && row.reaches);
      const bool actionable=v && can_act && !busy() && (r==2 ? row.food!=0 : !row.equip.empty());
      i64(k(".action"),actionable);
      txt(k(".action_label"),!v?"":r==2?"Eat":row.worn?"Worn":"Equip");
    }
  }
};
} // namespace botw
