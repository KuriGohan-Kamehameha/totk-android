// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "botw_key.h"
#include "botw_map_landmarks.h"
#include "botw_saved_map_icons.h"
#include "botw_map_koroks.h"
#include "botw_tracker_world_metadata.h"
#include "botw_tracker_dlc_locations.h"
#include "botw_map_special.h"
namespace botw {
// Companion-only UI state. This controller never reads or writes guest memory.
struct MapUi {
  enum Layer {Shrines,Towers,Settlements,Pins,Found,Unfound,Horse,Death,LayerCount};
  std::array<bool,LayerCount> layers{{true,true,true,true,false,false,true,false}};
  int popup{},selected{-1};uint32_t epoch{};bool entered{},view_valid{};
  float cx{},cy{},span_x{1354.369f},span_y{1179.612f};
  MapLandmarks observed{};
  // Iteration 12 item 13a: the placed Travel Medallion (MapData+480, botw.map.special.3) is one
  // more inspectable marker -- full_map_hits slot MedallionHit, inspect name = native item name.
  static constexpr unsigned MedallionHit=351,HitCount=352;
  MapSpecial::Entry medallion{};bool medallion_selected{};
  bool medallion_shown() const {return medallion.visible&&medallion.kind==42;}
  void close(){popup=0;selected=-1;medallion_selected=false;}
  KorokFilter korok_filter() const {return layers[Found]&&layers[Unfound]?KorokFilter::All:layers[Unfound]?KorokFilter::Unfound:KorokFilter::Found;}
  bool action(const char* name,int64_t argument,bool allowed) {
    if(!name)return false;
    const bool own=std::strncmp(name,"map_ui_",7)==0;
    const bool inspect=std::strcmp(name,"map_marker")==0&&argument>=200;
    if(!own&&!inspect){if(std::strcmp(name,"map_tool")==0||std::strcmp(name,"map_tap")==0||std::strcmp(name,"map_marker")==0){popup=0;selected=-1;medallion_selected=false;}return false;}
    if(!allowed){close();return true;}
    if(std::strcmp(name,"map_ui_popup")==0){const int p=int(argument);if(p>=0&&p<=3){popup=popup==p?0:p;selected=-1;medallion_selected=false;}return true;}
    if(std::strcmp(name,"map_ui_layer")==0){if(argument>=0&&argument<LayerCount)layers[size_t(argument)]=!layers[size_t(argument)];return true;}
    if(std::strcmp(name,"map_ui_close")==0){close();return true;}
    if(inspect&&argument==int64_t(MedallionHit)){if(medallion_shown()){medallion_selected=!medallion_selected;selected=-1;popup=0;}return true;}
    if(inspect){const int i=int(argument-200);if(i>=0&&i<int(observed.count)&&observed.valid&&observed.entries[size_t(i)].visible){selected=selected==i?-1:i;popup=0;medallion_selected=false;}return true;}
    return true;
  }
  static uint32_t medallion_name_key(){uint32_t key=2166136261u;for(const char* id="Obj_WarpDLC";*id;++id)key=(key^uint8_t(*id))*16777619u;return key;}
  static uint32_t name_key(const MapLandmarks::Entry& e) {
    const char* id=nullptr;
    auto match=[&](const auto& list){for(const auto& x:list)if(std::abs(e.xyz[0]-x.x)<.1f&&std::abs(e.xyz[2]-x.z)<.1f){id=x.id;break;}};
    if(e.kind==3)match(tracker_towers);else {match(tracker_shrines);if(!id)match(tracker_dlc_shrine_locations);}
    if(!id)return 0;
    uint32_t key=2166136261u;for(;*id;++id)key=(key^uint8_t(*id))*16777619u;return key;
  }
  void publish(const EdenDsmodHostApi& h,bool active,bool allowed,uint32_t current_epoch,const MapPosition& position,const MapLandmarks& landmarks,const SavedMapIcons& icons,float ready_width=824.f,float ready_window=900.f,const MapSpecial* specials=nullptr) {
    if(epoch!=current_epoch){close();view_valid=false;epoch=current_epoch;}
    // 2026-10-18 iteration 4 relayout (owner item 4, Map/Korok-map "void at open"): the
    // view_rect_* binds are only published below when view_valid is true. The old code left
    // view_valid false until position.valid fired, so on the page's first frame(s) (or any
    // frame where the position read has not landed yet) the bind went unpublished and the map
    // widget fell back to its own default/zero view -- a dark void at the world edge. Publish a
    // centred fallback view immediately on entry ((0,0) sits near the middle of BOTW's
    // playable coordinate range, x +-6000 / z +-5000), then still snap once to the real player
    // position the first time it becomes valid, exactly as before.
    if(!active||!allowed){close();entered=false;}
    else {
      if(!view_valid){cx=0.f;cy=0.f;const float ppw=ready_width/ready_window;span_x=1240.f/ppw;span_y=1080.f/ppw;view_valid=true;}
      if(!entered&&position.valid){cx=position.x;cy=position.y;entered=true;}
    }
    observed=allowed?landmarks:MapLandmarks{};
    medallion=allowed&&specials&&specials->valid?specials->entries[3]:MapSpecial::Entry{};
    if(medallion_selected&&!medallion_shown())medallion_selected=false;
    auto number=[&](const char* key,int64_t value){h.publish_i64(h.userdata,key,value);};
    auto scalar=[&](const char* key,double value){if(h.publish_f64)h.publish_f64(h.userdata,key,value);};
    auto text=[&](const char* key,const char* value){h.publish_text(h.userdata,key,value);};
    for(int i=1;i<=3;++i)number(Key("botw.map.ui.popup.",unsigned(i)).c_str(),active&&allowed&&popup==i);
    for(unsigned i=0;i<LayerCount;++i){Key k("botw.map.ui.layer.",i);number(k.c_str(),layers[i]);text(k(".text"),layers[i]?"On":"Off");}
    number("botw.map.ui.koroks",layers[Found]||layers[Unfound]);
    for(unsigned i=0;i<151;++i){const auto& e=observed.entries[i];const bool shown=observed.valid&&i<observed.count&&e.visible&&layers[e.kind==3?Towers:Shrines];
      Key k("botw.map.ui.poi.",i);number(k(".kind"),shown?e.kind:-1);
      number(k(".kind_shrine"),shown&&e.kind!=3?e.kind:-1);number(k(".kind_tower"),shown&&e.kind==3?e.kind:-1);}

    for(unsigned i=0;i<HitCount;++i){Key k("botw.map.ui.hit.",i);bool visible=false;int kind=-1;std::array<float,3> xyz{};
      if(i<200){const auto& e=icons.icons[i];visible=allowed&&icons.valid&&layers[Pins]&&e.kind>=0;kind=e.kind;xyz=e.xyz;}
      else if(i==MedallionHit){visible=allowed&&medallion_shown();kind=medallion.kind;xyz=medallion.xyz;}
      else {const auto& e=observed.entries[i-200];visible=observed.valid&&i-200<observed.count&&e.visible&&layers[e.kind==3?Towers:Shrines];kind=e.kind;xyz=e.xyz;}
      number(k(".kind"),visible?kind:-1);if(visible){scalar(k(".x"),xyz[0]);scalar(k(".y"),-xyz[2]);}
    }
    if(view_valid){scalar("botw.map.ui.x0",cx-span_x/2);scalar("botw.map.ui.x1",cx+span_x/2);scalar("botw.map.ui.y0",cy-span_y/2);scalar("botw.map.ui.y1",cy+span_y/2);}
    const bool med=active&&allowed&&medallion_selected&&medallion_shown();
    const bool landmark=active&&allowed&&selected>=0&&selected<int(observed.count)&&observed.valid&&observed.entries[size_t(selected)].visible&&layers[observed.entries[size_t(selected)].kind==3?Towers:Shrines];
    const bool inspected=landmark||med;
    number("botw.map.ui.inspect",inspected);
    // 2026-10-18 relayout: a popover's Close owns the map's bottom-left Back slot while open.
    number("botw.map.ui.open",(active&&allowed&&popup>=1&&popup<=3)||inspected);
    // Medallion name: fnv("Obj_WarpDLC") -> native ActorType/Item Obj_WarpDLC_Name (already in the text_map).
    number("botw.map.ui.inspect.name",med?medallion_name_key():landmark?name_key(observed.entries[size_t(selected)]):0);
    const char* status="";if(landmark){const auto& e=observed.entries[size_t(selected)];status=e.kind==3?(e.opened?"Activated":"Discovered"):(e.cleared?"Completed":e.opened?"Travel gate activated":"Discovered");}
    text("botw.map.ui.inspect.status",status);
    // A Travel button is deliberately not published without a verified native adapter.
    number("botw.map.ui.travel.available",0);
  }
};
} // namespace botw
