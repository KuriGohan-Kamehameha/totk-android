// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "botw_consumables.h"
#include "botw_status_metadata.h"
#include "botw_key.h"
#include "botw_text_measure.h"
#include "botw_layout_fx.h"
#include <algorithm>
#include <string>
namespace botw {
// Release boundary (asset compliance, owner 2026-10-22: "static icons in release"): the package ships
// no extracted or pre-rendered game pictures. Items are named by their actor (the manifest's
// sprite_map turns that into the player's own romfs art). The champion orbs and the Travel
// Medallion animate as on the native pause menu (owner 2026-10-22: "build instead"): their frames are
// composed on the device from the player's own layouts (botw_layout_fx.h) and named by
// "module:fx/<effect>/<frame>" keys at 20 fps, with the static actor art as the fallback whenever the
// game files cannot be read.
inline bool animated_art(const Item* item) {
  return item && !fx::EffectFor(item->actor, item->equipped).empty() && fx::Availability().load() != 2;
}
inline std::string inventory_art(const Item* item,int64_t milliseconds) {
  if(!item)return {};
  if(!animated_art(item))return item->actor;
  const auto effect=fx::EffectFor(item->actor,item->equipped);
  const auto* e=fx::FindEffect(effect);
  if(!e)return item->actor;
  const int count=e->frames20;
  // Native loop at 20 fps; an actor-hash phase keeps the orbs from pulsing in lockstep.
  const int frame=int((std::max<int64_t>(0,milliseconds)/50+actor_key(item->actor)%count)%count);
  return fx::FrameKey(effect,frame);
}
// Iteration 13 item K: native Key Items draw their art at ~85 % of the cell. Static icons fill their
// canvas (x1.4); the animated frames keep the native framing of a 96 px canvas (orbs x1.45 -- their
// flames touch the cell top at 1.7 -- and the Medallion x1.7). Value x1000, 0 = unscaled.
inline int key_item_art_scale(const Item* item){
  if(!item||item->type!=9)return 0;
  if(!animated_art(item))return 1400;
  return item->actor=="Obj_WarpDLC"?1700:1450;
}
inline int detail_art_scale(const Item* item){return animated_art(item)?1150:0;}
// Iteration 13 item 16: grid art is cell-centred. The animated Medallion's ink sits 9.4 % of its
// canvas low (~15 px at x1.7), so it is lifted by that much; static art is canvas-centred.
inline int key_item_art_dy(const Item* item){return item&&item->type==9&&item->actor=="Obj_WarpDLC"&&animated_art(item)?-15:0;}
inline std::string_view food_badge(int effect) {
  switch(effect) {
    case 2:return "Heart";case 4:return "Hot";case 5:return "Cool";
    case 6:return "Elect";case 10:return "AttackCook";case 11:return "ShieldCook";
    case 12:return "Noise";case 13:return "GoGo";case 14:return "GanbariRe";
    case 15:return "Ganbari";case 16:return "Fire";default:return {};
  }
}
inline std::string badge_source(std::string_view icon) {
  return icon.empty()?std::string():"status:"+std::string(icon);
}
inline std::string item_badge(const EdenDsmodHostApi& h,const Item* item) {
  if(!item)return {};
  if(item->type>=4&&item->type<=6)return badge_source(armor_badge(item->actor));
  if(item->type!=7&&item->type!=8)return {};
  const auto* meta=consume_info(item->actor);
  if(!meta)return {};
  if(item->type==7)return badge_source(food_badge(meta->effect)); // cooking potential, as native grid
  const auto food=consumable(h,*item);
  return food.valid?badge_source(food_badge(food.effect)):std::string();
}
struct Recovery {bool valid{},full{};int quarters{},extra{};};
inline Recovery recovery(const EdenDsmodHostApi& h,const Item* item) {
  if(!item)return {};
  const auto c=consumable(h,*item);
  if(!c.valid)return {};
  return {true,c.effect==2,c.health,c.effect==2?c.level:0};
}
inline void publish_recovery(const EdenDsmodHostApi& h,const std::string& prefix,Recovery r) {
  h.publish_i64(h.userdata,(prefix+".valid").c_str(),r.valid);
  h.publish_i64(h.userdata,(prefix+".full").c_str(),r.valid&&r.full);
  h.publish_i64(h.userdata,(prefix+".extra").c_str(),r.valid?r.extra:0);
  h.publish_text(h.userdata,(prefix+".label").c_str(),r.valid&&r.full?"Full recovery":"");
  std::string extra;
  if(r.valid&&r.full&&r.extra>0) {
    static constexpr const char* fractions[]={"","¼","½","¾"};
    extra="+"+(r.extra>=4?std::to_string(r.extra/4):std::string())+fractions[r.extra%4];
  }
  h.publish_text(h.userdata,(prefix+".extra_text").c_str(),extra.c_str());
  h.publish_i64(h.userdata,(prefix+".extra_valid").c_str(),!extra.empty());
  // Iteration 13: text at the well's 8 px inset (816 + 8), heart 8 px after it, 8 px right pad.
  h.publish_i64(h.userdata,(prefix+".full_icon_x").c_str(),824+MeasureTextPx("Full recovery",5)+8);
  h.publish_i64(h.userdata,(prefix+".extra_icon_x").c_str(),824+MeasureTextPx(extra,5)+8);
  for(int row=0;row<3;++row) {
    const int count=r.valid&&!r.full?std::clamp((r.quarters+3)/4-row*10,0,10):0;
    int width=count?count*34+12:0;
    if(r.valid&&r.full&&row==0)width=MeasureTextPx("Full recovery",5)+54;
    if(r.valid&&r.full&&row==1&&!extra.empty())width=MeasureTextPx(extra,5)+54;
    h.publish_i64(h.userdata,(prefix+".row_width"+std::to_string(row)).c_str(),width);
  }
  for(int i=0;i<30;++i) {
    const int q=r.valid&&!r.full?std::clamp(r.quarters-i*4,0,4):0;
    const char* texture=q==4?"Nt_HeartNew_00^f":q==3?"Nt_HeartNew_05^f":q==2?"Nt_HeartNew_04^f":q==1?"Nt_HeartNew_03^f":"";
    const std::string key=prefix+".heart."+std::to_string(i);
    h.publish_text(h.userdata,key.c_str(),q?(std::string("romfs:/Pack/Bootup.pack#Layout/Common.sblarc#timg/__Combined.bntx#")+texture).c_str():"");
  }
}
} // namespace botw
