// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Travel controller: Map-page inspect (shrine/tower, or the placed Travel Medallion since
// iteration 12) + Trackers row -> confirm overlay -> one bridge request,
// then the one-shot transaction. Publishes botw.warp.* (tools/warp-travel.py draws them).
#include "botw_warp.h"
#include "botw_map_ui.h"
#include "botw_tracker_view.h"
#include "botw_bridge.h"
namespace botw {
struct TravelUi {
  using Clock=std::chrono::steady_clock;
  warp::Transaction tx;
  warp::Env env;Clock::time_point env_at{};
  bool confirm{};int target{-1};warp::Point point;uint32_t name_key{};
  int map_index{-1};warp::Point map_point;Clock::time_point map_at{};
  std::string trk_id;int trk_index{-1};warp::Point trk_point;Clock::time_point trk_at{};
  // Item 9 (iteration 9, 2026-10-19): Ready-page shrine panel Travel. Resolves the CURRENT
  // shrine (botw.state.shrine.id/number, the one the player is physically standing inside) to
  // its native travel point via the static tracker_shrines table (botw_tracker_world_metadata.h
  // -- the SAME table botw_shrine_compass.h already reads for the nearest-shrine compass) +
  // warp::find()/warp::point(), the same functions the Map/Trackers paths already use. No new
  // RE: the shrine's own native MessageID IS the identity warp::find() already matches on.
  std::string rdy_id;int rdy_index{-1};warp::Point rdy_point;Clock::time_point rdy_at{};
  uint32_t seen_ack{},epoch{};
  std::string message;Clock::time_point message_until{};
  std::string target_message;std::array<float,3> target_xyz{};float arrival_distance{-1};
  void say(std::string text,Clock::time_point now,int seconds=4){message=std::move(text);message_until=now+std::chrono::seconds(seconds);}
  void reset(){confirm=false;target=-1;point={};map_index=-1;map_point={};trk_id.clear();trk_index=-1;trk_point={};}
  // The point the current page offers (map inspect or trackers row); refreshed at most twice a second.
  void resolve(const EdenDsmodHostApi& h,const char* page,const MapUi& map_ui,const TrackerView& trackers,const MapLandmarks& marks,bool shrine_active,int shrine_number,Clock::time_point now) {
    const bool map=page&&std::strcmp(page,"map")==0,trk=page&&std::strcmp(page,"trackers")==0,rdy=page&&std::strcmp(page,"ready")==0;
    // Iteration 12 item 13a: an inspected Travel Medallion resolves to warp::MedallionIndex.
    const int inspected=map&&map_ui.medallion_selected&&map_ui.medallion_shown()?int(warp::MedallionIndex):
      map&&map_ui.observed.valid&&map_ui.selected>=0&&map_ui.selected<int(map_ui.observed.count)?map_ui.selected:-1;
    if(inspected!=map_index||now>=map_at){map_index=inspected;map_point=inspected>=0?warp::point(h,unsigned(inspected)):warp::Point{};map_at=now+std::chrono::milliseconds(500);}
    const auto* row=trk?trackers.travel_row():nullptr;const std::string id=row?row->id:"";
    if(id!=trk_id||now>=trk_at){trk_id=id;trk_index=row?warp::find(h,marks,id,row->x,-row->y):-1;
      trk_point=trk_index>=0?warp::point(h,unsigned(trk_index)):warp::Point{};trk_at=now+std::chrono::seconds(1);}
    char buf[32]{};if(rdy&&shrine_active&&shrine_number>=0&&shrine_number<1000)std::snprintf(buf,sizeof(buf),"Dungeon%03d",shrine_number);
    const std::string rid=buf;
    if(rid!=rdy_id||now>=rdy_at){
      rdy_id=rid;
      // Unlike map/trackers, the Ready page already knows the exact shrine id (no on-screen
      // marker to disambiguate) and must resolve it from INSIDE the shrine, where the overworld
      // landmarks table (`marks`) is never valid (botw_map_landmarks.h gates to MainField stage;
      // live 2026-10-19: botw.map.poi.valid==0 the whole time standing inside a shrine). Go
      // straight to the MapData array by native MessageID instead of warp::find()/marks.
      rdy_index=rid.empty()?-1:warp::find_shrine_by_id(h,rid);
      rdy_point=rdy_index>=0?warp::point(h,unsigned(rdy_index)):warp::Point{};rdy_at=now+std::chrono::seconds(1);
    }
  }
  bool open(int index,const warp::Point& p,uint32_t key,Clock::time_point now) {
    if(index<0||!p.enabled()||tx.phase!=warp::Phase::Idle)return false;
    confirm=true;target=index;point=p;name_key=key;message.clear();message_until=now;return true;
  }
  bool action(const char* name,const EdenDsmodHostApi& h,const char* page,MapUi& map_ui,SelectionBridge& bridge,Clock::time_point now) {
    if(std::strcmp(name,"warp_cancel")==0){const bool was=confirm;confirm=false;return was;}
    // Owner item 11 (iteration 10, 2026-10-20): "warp_open_ready" removed from the opening
    // set -- the Ready-page shrine panel no longer offers Travel (see the action handler below
    // and botw_bridge.h's warp_scope()).
    const bool opening=std::strcmp(name,"warp_open_map")==0||std::strcmp(name,"warp_open_tracker")==0;
    if(opening){
      // Same location allow-check as the confirm tap, so a refusal is explained up front.
      env=warp::env(h);env_at=now;
      const auto b=warp::block(env,bridge.healthy&&bridge.warp_supported,bridge.pending!=0,tx.phase,true);
      if(b!=warp::Block::None){say(warp::block_text(b),now);return false;}
    }
    if(std::strcmp(name,"warp_open_map")==0){
      if(!page||std::strcmp(page,"map")!=0||map_index<0)return false;
      // Name: the catalogue id at this position (same key the inspect label uses), else the
      // record's own native MessageID.
      const uint32_t key=map_index==int(warp::MedallionIndex)?MapUi::medallion_name_key():
        size_t(map_index)<map_ui.observed.entries.size()?MapUi::name_key(map_ui.observed.entries[size_t(map_index)]):0;
      // Owner 2026-10-20: this inspection already names the selected shrine;
      // Travel commits here through the SAME fresh guards, with no second dialog.
      if(!open(map_index,map_point,key?key:warp::fnv(map_point.message),now))return false;
      const bool ok=commit(h,map_ui,bridge,now);
      confirm=false; // refusals stay on the original inspect surface with status.
      return ok;
    }
    if(std::strcmp(name,"warp_open_tracker")==0){
      if(!page||std::strcmp(page,"trackers")!=0||trk_index<0)return false;
      return open(trk_index,trk_point,warp::fnv(trk_id),now);
    }
    // Owner item 11 (iteration 10, 2026-10-20): the "warp_open_ready" handler is removed (the
    // manifest no longer emits that action -- tools/i9-ready-travel.py is dropped from the
    // pipeline). rdy_id/rdy_index/rdy_point/rdy_at in resolve() above are left in place
    // (harmless, unused state) rather than threading a signature change through every
    // sample()/resolve() call site in 01007EF00011E000.cpp for a dead branch.
    if(std::strcmp(name,"warp_confirm")!=0)return false;
    if(!confirm||target<0)return false;
    return commit(h,map_ui,bridge,now);
  }
  bool commit(const EdenDsmodHostApi& h,MapUi& map_ui,SelectionBridge& bridge,Clock::time_point now) {
    // Fresh allow-check on the tap itself; refusals keep the overlay open with the reason.
    env=warp::env(h);env_at=now;
    const auto fresh=warp::point(h,unsigned(target));
    const auto b=warp::block(env,bridge.healthy&&bridge.warp_supported,bridge.pending!=0,tx.phase,fresh.valid&&fresh==point&&fresh.enabled());
    if(b!=warp::Block::None){say(warp::block_text(b),now);return false;}
    if(!bridge.request_warp(unsigned(target),fresh)){say(bridge.warp_refusal&512?warp::block_text(warp::Block::Stale):"Travel unavailable",now);return false;}
    tx.queued(now);seen_ack=bridge.warp_acked;confirm=false;map_ui.close();
    target_message=fresh.message;target_xyz=fresh.xyz;arrival_distance=-1;message.clear();
    return true;
  }
  void sample(const EdenDsmodHostApi& h,const char* page,MapUi& map_ui,const TrackerView& trackers,const MapLandmarks& marks,
              SelectionBridge& bridge,UiContext context,const MapPosition& position,Clock::time_point now,
              bool shrine_active=false,int shrine_number=-1) {
    if(epoch!=bridge.epoch){epoch=bridge.epoch;reset();if(tx.phase==warp::Phase::Queued)tx.phase=warp::Phase::Uncertain;}
    if(bridge.warp_acked!=seen_ack){seen_ack=bridge.warp_acked;tx.ack(bridge.warp_result,now);
      if(bridge.warp_result==2)say("Travel refused by the game",now);}
    // Owner item 11: "ready" dropped (see warp_scope() in botw_bridge.h, same reasoning).
    const bool active=page&&(std::strcmp(page,"map")==0||std::strcmp(page,"trackers")==0);
    if((active||tx.phase!=warp::Phase::Idle||confirm)&&now-env_at>=std::chrono::milliseconds(150)){env=warp::env(h);env_at=now;}
    const bool was_busy=tx.phase!=warp::Phase::Idle;
    tx.observe(env,now);
    if(was_busy&&tx.phase==warp::Phase::Idle&&tx.arrived){
      arrival_distance=position.valid?std::hypot(position.x-target_xyz[0],position.y+target_xyz[2]):-1.f;
      say(arrival_distance>=0&&arrival_distance<60?"Arrived":"Travel finished",now,5);
    }
    if(!active||context!=UiContext::World){confirm=false;}
    if(active)resolve(h,page,map_ui,trackers,marks,shrine_active,shrine_number,now);else{map_index=-1;trk_index=-1;trk_id.clear();rdy_index=-1;rdy_id.clear();}
  }
  void publish(const EdenDsmodHostApi& h,bool map_page,bool trk_page,bool rdy_page,bool helper,Clock::time_point now) const {
    auto n=[&](const char* k,int64_t v){h.publish_i64(h.userdata,k,v);};
    const bool idle=tx.phase==warp::Phase::Idle;
    n("botw.warp.supported",helper);
    n("botw.warp.map.available",helper&&map_page&&!confirm&&idle&&map_index>=0&&map_point.enabled());
    n("botw.warp.tracker.available",helper&&trk_page&&!confirm&&idle&&trk_index>=0&&trk_point.enabled());
    n("botw.warp.ready.available",helper&&rdy_page&&!confirm&&idle&&rdy_index>=0&&rdy_point.enabled());
    n("botw.warp.confirm",confirm&&(map_page||trk_page||rdy_page));
    n("botw.warp.confirm.name",confirm?int64_t(name_key):-1);
    n("botw.warp.phase",int64_t(tx.phase));
    const std::string text=now<message_until&&!message.empty()?message:tx.text();
    h.publish_text(h.userdata,"botw.warp.status",text.c_str());
    n("botw.warp.status.visible",!text.empty());
    h.publish_text(h.userdata,"botw.warp.target",target_message.c_str());
    if(h.publish_f64)h.publish_f64(h.userdata,"botw.warp.arrival_distance",arrival_distance);
    n("botw.warp.env",env.valid?int64_t(env.world|env.event<<1|env.menu<<2|env.loading<<3|env.prohibited<<4|env.latch<<5):-1);
  }
};
} // namespace botw
