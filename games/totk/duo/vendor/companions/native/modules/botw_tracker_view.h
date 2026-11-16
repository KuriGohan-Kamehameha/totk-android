// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "botw_tracker_data.h"
#include "botw_tracker_armor.h"
#include "botw_inventory.h"
#include "botw_tracker_names.h"
#include <chrono>
namespace botw {
class TrackerView {
  friend struct TrackerViewFixtureAccess;
  TrackerFlagReader flags;
  TrackerData data;
  trackers::ArmorPlanner planner;
  trackers::ArmorPlan armor;
  Snapshot inventory;
  uint64_t epoch{};
  std::chrono::steady_clock::time_point next{};
  bool dirty{true},was_active{},was_map{};
  std::vector<std::pair<std::string,int64_t>> integers;
  std::vector<std::pair<std::string,std::string>> texts;
  std::vector<std::pair<std::string,double>> numbers;
  // Module publications are a fresh snapshot every tick. Cache formatting and
  // keys, but replay all bindings even when the underlying sample is unchanged.
  void publish_cached(const EdenDsmodHostApi& h) const {
    for(const auto& [key,value]:integers) h.publish_i64(h.userdata,key.c_str(),value);
    for(const auto& [key,value]:texts) h.publish_text(h.userdata,key.c_str(),value.c_str());
    if(h.publish_f64) for(const auto& [key,value]:numbers) h.publish_f64(h.userdata,key.c_str(),value);
  }
  std::vector<TrackerRecord> shopping_rows;
  std::vector<const TrackerRecord*> filtered;
  std::string selected_id;
  int family{},filter{},target{4},source_unknown{};bool shopping{};
 public:
  // Iteration 5 (owner items 2 and 4, 2026-10-18): no tab rows and no Prev/Next paging. The left
  // pane is one scrolling list of tracker families (Exploration split into its four groups),
  // and the right pane scrolls the whole filtered list of the selected family. Every row is
  // published (bounded by MaxRows, the 900 Koroks being the largest family); the runtime only
  // expands the rows a scroll region shows.
  struct Family{const char* title;int category;int group;}; // group -1 = every group of the category
  static constexpr Family families[]{{"Koroks",0,0},{"Shrines",0,1},{"Towers",0,2},{"DLC shrines",0,3},
    {"Boss medals",1,-1},{"Compendium",2,-1},{"Key items",3,-1},{"Pouch upgrades",4,-1},{"Armor upgrades",5,-1},{"Quest progress",6,-1}};
  static constexpr int FamilyCount=int(std::size(families));
  static constexpr int ArmorFamily=8;
  static constexpr int MaxRows=900;
 private:
  // Families that mix groups prefix the row status with the group, which the removed tabs named.
  static constexpr const char* group_labels[7][5]={{nullptr},{nullptr},{"Creatures","Monsters","Materials","Equipment","Treasure"},{nullptr},{nullptr},{nullptr},{"Main","Shrine","Side",nullptr,nullptr}};
  int category()const{return families[family].category;}
  bool in_family(const TrackerRecord& r)const{const int g=families[family].group;return g<0||r.group==g;}
  static std::string pretty(std::string s){for(auto& c:s)if(c=='_')c=' ';return s;}
  void rebuild() {
    filtered.clear();source_unknown=0;
    const bool armor_family=family==ArmorFamily;
    const auto& source=armor_family&&shopping?shopping_rows:data.categories[std::size_t(category())];
    for(const auto& r:source)if(in_family(r))source_unknown+=r.state<0;
    for(const auto& r:source)if(in_family(r) && (!filter || (filter==1&&r.state>=0&&r.state<2) || (filter==2&&r.state==2)))filtered.push_back(&r);
    // One list per family now that the group tabs are gone: keep each group together in tab order.
    if(families[family].group<0&&!armor_family)std::stable_sort(filtered.begin(),filtered.end(),[](const auto* a,const auto* b){return a->group<b->group;});
    if(filtered.size()>std::size_t(MaxRows))filtered.resize(std::size_t(MaxRows));
    if(std::none_of(filtered.begin(),filtered.end(),[&](const auto* r){return r->id==selected_id;}))selected_id=filtered.empty()?"":filtered.front()->id;
  }
  int selected_index()const {
    for(std::size_t i=0;i<filtered.size();++i)if(filtered[i]->id==selected_id)return int(i);
    return -1;
  }
  const TrackerRecord* selected()const {const int i=selected_index();return i<0?nullptr:filtered[std::size_t(i)];}
  void armor_rows() {
    armor=planner.plan(inventory,epoch);data.categories[5].clear();shopping_rows.clear();
    if(!armor.valid){data.summary[5]="Inventory unavailable";return;}
    for(const auto& a:armor.rows){TrackerRecord r;r.id=std::to_string(a.address);r.address=a.address;r.actor=a.actor;r.name=pretty(a.actor);r.name_key=actor_key(a.actor);r.state=a.max_tier==0?3:a.current_tier==a.max_tier?2:0;
      r.status="Tier "+std::to_string(a.current_tier)+(a.max_tier?" / "+std::to_string(a.max_tier):" (fixed)");
      r.detail=a.selected?"Planned: tier "+std::to_string(a.current_tier)+" to "+std::to_string(a.target_tier)+". Materials are counted once across the plan.":a.upgradeable?"Not planned. Tap to plan tier "+std::to_string(target)+".":"This armor cannot be upgraded.";
      if(a.selected)r.status+="  > "+std::to_string(a.target_tier);
      data.categories[5].push_back(std::move(r));
    }
    for(const auto& m:armor.materials){TrackerRecord r;r.id=m.actor;r.actor=m.actor;r.name=pretty(m.actor);r.name_key=actor_key(m.actor);r.state=m.missing?0:2;
      r.status="Missing "+std::to_string(m.missing)+"  |  Have "+std::to_string(m.owned);
      r.detail="Total required: "+std::to_string(m.required)+". In inventory: "+std::to_string(m.owned)+". Still needed: "+std::to_string(m.missing)+". Inventory is counted once across all selected armor.";shopping_rows.push_back(std::move(r));}
    data.summary[5]=std::to_string(armor.rows.size())+" owned / "+std::to_string(armor.selected_count)+" planned";
  }
  // Compact "done / total" for a family card and the tracker-map header; "-" when unknown.
  std::string family_count(int k,bool available)const {
    if(!available||!data.valid)return "-";
    const auto& f=families[k];int done=0,total=0,unknown=0;
    if(k==ArmorFamily&&!armor.valid)return "-";
    for(const auto& r:data.categories[std::size_t(f.category)])if(f.group<0||r.group==f.group){++total;done+=r.state==2;unknown+=r.state<0;}
    if(!total||unknown==total)return "-";
    return std::to_string(done)+" / "+std::to_string(total);
  }
  // A short state fact for an empty right pane (owner item 3: no instructional filler).
  std::string empty_text(bool available)const {
    if(!available||!data.valid)return "Progress unavailable";
    if(family==ArmorFamily){if(!armor.valid)return "Inventory unavailable";if(shopping)return armor.selected_count?"Nothing missing":"No armor planned";if(data.categories[5].empty())return "No armor owned";}
    if(source_unknown&&filter)return "Progress unknown";
    return filter==1?"All complete":filter==2?"None complete yet":"Progress unavailable";
  }
 public:
  void clear(){integers.clear();texts.clear();numbers.clear();flags.clear();data={};planner.clear();armor={};inventory={};epoch=0;next={};filtered.clear();shopping_rows.clear();selected_id.clear();source_unknown=0;dirty=true;was_active=false;was_map=false;}
  bool action(const char* name,int64_t arg) {
    if(!name||std::string_view(name).substr(0,8)!="tracker_")return false;
    const std::string_view a(name);
    if(a=="tracker_family"&&arg>=0&&arg<FamilyCount){family=int(arg);filter=0;shopping=false;selected_id.clear();}
    else if(a=="tracker_filter"&&arg>=0&&arg<=2)filter=int(arg); // the selection stays when it still matches
    else if(a=="tracker_select"&&arg>=0&&arg<int64_t(filtered.size())){
      const auto* r=filtered[std::size_t(arg)];selected_id=r->id;
      if(family==ArmorFamily&&!shopping&&r->address){const bool chosen=planner.targets().contains(r->address);planner.choose(inventory,r->address,chosen?0:target,epoch);armor_rows();}
    }
    else if(a=="tracker_marker"&&arg>=0&&arg<int64_t(filtered.size())&&filtered[std::size_t(arg)]->location)selected_id=filtered[std::size_t(arg)]->id;
    else if(a=="tracker_armor_target"&&arg>=1&&arg<=4){target=int(arg);armor_rows();}
    else if(a=="tracker_armor_all"&&family==ArmorFamily){planner.choose_all(inventory,target,epoch);armor_rows();}
    else if(a=="tracker_armor_clear"&&family==ArmorFamily){planner.clear();armor_rows();}
    else if(a=="tracker_shopping"&&family==ArmorFamily&&(arg==0||arg==1)){shopping=arg!=0;filter=0;selected_id.clear();}
    else return false;
    rebuild();dirty=true;return true;
  }
  // Travel: the selected Shrines / Towers / DLC shrines row with a native
  // position, or nullptr. Read-only; the warp controller resolves it to a native travel point.
  const TrackerRecord* travel_row()const {
    if(family<1||family>3)return nullptr;
    const auto* r=selected();return r&&r->location?r:nullptr;
  }
  void sample(const EdenDsmodHostApi& h,const Snapshot& pouch,const MapLandmarks& landmarks,uint64_t current_epoch,const char* page) {
    const bool active=page&&(std::strcmp(page,"trackers")==0||std::strcmp(page,"tracker_map")==0);
    const bool map_page=page&&std::strcmp(page,"tracker_map")==0;
    const auto now=std::chrono::steady_clock::now();
    if(epoch!=current_epoch){clear();epoch=current_epoch;}
    if(active&&!was_active){next={};dirty=true;}
    if(active!=was_active||map_page!=was_map) dirty=true;
    was_active=active;was_map=map_page;
    const bool available=inventory_context(context(h));
    if(!available){
      if(data.valid||inventory.valid||!filtered.empty()||!selected_id.empty()){clear();epoch=current_epoch;}
    }
    else if(active && now>=next){
      data=tracker_data(h,landmarks,&flags);inventory=pouch;armor_rows();rebuild();dirty=true;next=now+std::chrono::seconds(2);
    }
    if(!active) {
      h.publish_i64(h.userdata,"botw.tracker.valid",0);
      h.publish_i64(h.userdata,"botw.tracker.map.valid",0);
      h.publish_i64(h.userdata,"botw.tracker.map_available",0);
      return;
    }
    if(!dirty){publish_cached(h);return;}
    dirty=false;integers.clear();texts.clear();numbers.clear();
    auto n=[&](const std::string& k,int64_t v){integers.emplace_back("botw.tracker."+k,v);};
    auto t=[&](const std::string& k,const std::string& v){texts.emplace_back("botw.tracker."+k,v);};
    auto f=[&](const std::string& k,double v){numbers.emplace_back("botw.tracker."+k,v);};
    const bool armor_family=family==ArmorFamily;
    n("family",family);n("filter",filter);n("target",target);n("shopping",shopping);n("armor_controls",armor_family);n("valid",available&&data.valid);
    t("title",armor_family&&shopping?"Shopping list":families[family].title);
    for(int k=0;k<FamilyCount;++k){const auto key="family."+std::to_string(k);t(key+".label",families[k].title);t(key+".summary",family_count(k,available));}
    t("summary",family_count(family,available));
    // Scroll regions reset to the top when the list changes identity (family, filter, mode).
    n("list_token",family*8+filter*2+shopping);
    n("count",int64_t(filtered.size()));n("list_empty",filtered.empty());t("empty_text",filtered.empty()?empty_text(available):"");
    const int selected_row=selected_index();n("selected.index",selected_row);
    const auto* r=selected();
    const int cat=category();
    if(!map_page)for(std::size_t i=0;i<filtered.size();++i){const auto k="row."+std::to_string(i);const auto* e=filtered[i];
      const char* group=e->group>=0&&e->group<5&&families[family].group<0?group_labels[cat][e->group]:nullptr;
      n(k+".name_key",e->name_key);n(k+".native_name",tracker_native_name(e->name_key));t(k+".name",e->name);t(k+".actor",e->actor);t(k+".status",group?std::string(group)+" · "+e->status:e->status);}
    // Fix round A (CRITIQUE-2 Trackers S): armor that cannot be upgraded (state 3, max tier 0)
    // dims and blocks the tier controls instead of showing an active target tier.
    const bool fixed_armor=armor_family&&!shopping&&r&&r->state==3;
    n("armor.tiers_enabled",armor_family&&!fixed_armor);n("armor.tiers_disabled",fixed_armor);
    t("detail",r?r->detail:"");
    t("selected.name",r?r->name:"");n("selected.name_key",r?r->name_key:-1);n("selected.native_name",r&&tracker_native_name(r->name_key));t("selected.actor",r?r->actor:"");
    const std::string common="romfs:/Pack/Bootup.pack#Layout/Common.sblarc#timg/__Combined.bntx#";
    const int group=families[family].group;
    t("map.icon",cat==1?"composite:native-map-stamp-6":common+(group==0?"Nt_MapIconKolog^l":group==2?"Nt_MapTowerNew_00^t":"Nt_MapDungeonNew_00^t"));
    const bool map_ready=map_page&&available&&r&&r->location;
    n("map_available",available&&r&&r->location);n("map.valid",map_ready);
    if(r&&r->location){
      // 2026-10-18 iteration 4 relayout (owner item 3, Korok/tracker map "void at open"): a
      // 1000x800 window centred on the selected entry can run past the playable coordinate
      // range (x +-6000 / z +-5000, see the same bound botw_map_landmarks.h validates reads
      // against) when the entry sits near a world edge -- the overhang drew as a dark void
      // instead of terrain. Clamping to that +-6000/+-5000 rectangle (the manifest "map" area's
      // own min/max) stops the window running past the edge of the drawable picture, but most
      // of that rectangle's corners are themselves ocean/void in the native map art -- a
      // selection out near an actual coastline still showed a lot of empty space inside the
      // clamp (fix round 2, CRITIQUE-2 M2: "still ~35% out-of-terrain"). Fix round 2: clamp to
      // the real playable-landmass bounding box instead, derived from every known in-game
      // position this module carries (tracker_koroks + tracker_shrines + tracker_towers +
      // tracker_dlc_shrine_locations, 1119 points spread across the whole map including the far
      // corners and outlying islands): x in [-4876.9, 4817.2], z in [-3848.1, 3951.9] (y=-z in
      // [-3951.9, 3848.1]), rounded outward with a +-350 unit coastline margin. Still shifts
      // (never shrinks) the window so its size and the selected marker stay visible.
      float x0=r->x-500.f,x1=r->x+500.f,y0=r->y-400.f,y1=r->y+400.f;
      constexpr float X0=-5250.f,X1=5150.f,Y0=-4300.f,Y1=4200.f;
      if(x0<X0){x1+=X0-x0;x0=X0;} if(x1>X1){x0-=x1-X1;x1=X1;}
      if(y0<Y0){y1+=Y0-y0;y0=Y0;} if(y1>Y1){y0-=y1-Y1;y1=Y1;}
      f("map.x0",x0);f("map.x1",x1);f("map.y0",y0);f("map.y1",y1);
    }
    // Map markers only on the tracker map, the one page with the map widget.
    // Perf round 4: don't push a cache entry at all for a non-visible slot
    // (up to MaxRows=900) -- same "missing key == hidden" discipline as this round's Map-page
    // fixes (tracker_locations is a plain dynamic_markers group, hide_when_kind=-1). This also
    // shrinks the `integers`/`numbers` vectors publish_cached() replays EVERY tick (not just on
    // the ~2s rebuild), since most rows have no `location` at any one time -- a real per-tick win,
    // not just a rebuild-time one.
    if(map_page)for(int i=0;i<MaxRows;++i){const auto* m=i<int(filtered.size())?filtered[std::size_t(i)]:nullptr;const auto k="map."+std::to_string(i);const bool visible=map_ready&&m&&m->location;
      if(!visible)continue;
      n(k+".kind",cat==1?3:cat==0?(group==3?1:group):3);f(k+".x",m->x);f(k+".y",m->y);n(k+".tint",m->id==selected_id?0xffffe798:m->state==2?0xff64cbd8:m->state<0?0xff90989f:0xffffaf62);}
    publish_cached(h);
  }
};
}
