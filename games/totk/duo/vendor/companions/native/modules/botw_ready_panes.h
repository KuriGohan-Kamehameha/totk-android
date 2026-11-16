// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "botw_champions.h"
#include "botw_key.h"
#include "botw_pouch.h"
#include <string>
namespace botw {
// Independent normal and drawn-weapon views: a draw edge selects combat defaults;
// swiping while drawn changes that temporary view; sheathing restores normal pages.
// The production drawn-state reader is not verified yet. Do not substitute D-pad
// quick-selector visibility, equipment membership, or button state for that signal.
struct ReadyPanes {
  int normal_left{},normal_right{},drawn_left{2},drawn_right{1};bool drawn{};
  // Owner feedback item 11 (2026-10-17): the Champions page (normal_left==2) must not
  // be reachable in the normal-left cycle when no champion ability is owned yet, since
  // there is nothing to show there. Caller publishes ownership first, then calls this
  // every frame before reading left()/step() so the clamp applies immediately.
  bool champions_available{true};
  void observe_drawn(bool valid,bool value) {
    const bool next=valid && value;
    if(next && !drawn){drawn_left=2;drawn_right=1;}
    drawn=next;
  }
  // Iteration 12 item 9 (owner 2026-10-22): no champion ability owned -> no Champions page in
  // EITHER view; the drawn-weapon default (drawn_left=2) falls back to the quest page instead of
  // an empty pane.
  void set_champions_available(bool available) {
    champions_available=available;
    if(!available && normal_left==2)normal_left=0;
    if(!available && drawn_left==2)drawn_left=0;
  }
  int left()const{return drawn?drawn_left:normal_left;}
  int right()const{return drawn?drawn_right:normal_right;}
  bool step(bool right_side,int direction) {
    if(direction!=1 && direction!=-1)return false;
    auto& page=right_side?(drawn?drawn_right:normal_right):(drawn?drawn_left:normal_left);
    const int count=right_side?2:(!champions_available?2:3);
    page=(page+direction+count)%count;return true;
  }
};
// Publishes per-champion ownership/state, plus a compacted 0..count-1 slot list
// (botw.ready.champion_slot.N.icon/.status) holding all four champions in
// canonical order; unowned powers display a dash (owner 2026-10-21). Rows are vertically
// centred. Returns the number of OWNED champion abilities (iteration 12 item 9: the row count
// is always 4 once the pouch is valid, so it cannot gate the pane), so the caller can gate
// ReadyPanes::set_champions_available and the page bind; botw.ready.champions.available
// mirrors it for the manifest (pager dots).
inline int publish_champion_ownership(const EdenDsmodHostApi& h,const Snapshot& pouch,const ChampionAbilities& abilities={}) {
  constexpr const char* actors[]={"Obj_HeroSoul_Zora","Obj_HeroSoul_Rito","Obj_HeroSoul_Goron","Obj_HeroSoul_Gerudo"};
  constexpr const char* upgraded[]={"Obj_DLC_HeroSoul_Zora","Obj_DLC_HeroSoul_Rito","Obj_DLC_HeroSoul_Goron","Obj_DLC_HeroSoul_Gerudo"};
  // Native HUD champion-ability icons (main-screen, recoloured per owner item 11 in
  // the manifest pass, not here: this module only names the source art).
  constexpr const char* icons[]={
    "romfs:/Pack/Bootup.pack#Layout/Common.sblarc#timg/__Combined.bntx#Nt_IconSinJuNew_01^d",
    "romfs:/Pack/Bootup.pack#Layout/Common.sblarc#timg/__Combined.bntx#Nt_IconSinJuNew_02^d",
    "romfs:/Pack/Bootup.pack#Layout/Common.sblarc#timg/__Combined.bntx#Nt_IconSinJuNew_00^d",
    "romfs:/Pack/Bootup.pack#Layout/Common.sblarc#timg/__Combined.bntx#Nt_IconSinJuNew_03^d"};
  h.publish_i64(h.userdata,"botw.ready.champions.ownership_valid",pouch.valid);
  h.publish_i64(h.userdata,"botw.ready.champions.timers_supported",1);
  const char* slot_icon[4]{};std::string slot_status[4];int slot_tint[4]{};int slot_tint_level[4]{};bool slot_enabled[4]{},slot_owned[4]{},slot_valid[4]{};ChampionAbility slot_ability[4]{};const int count=pouch.valid?4:0;
  for(unsigned i=0;i<4;++i) {
    const Item* owned{};unsigned n{};
    if(pouch.valid)for(const auto& item:pouch.items)if(item.type==9 && (item.actor==actors[i] || item.actor==upgraded[i])){owned=&item;++n;}
    Key key("botw.ready.champion.",i);
    const bool valid=pouch.valid && n<=1;
    const bool is_owned=valid && owned;
    const bool enabled=valid&&owned&&owned->equipped;
    h.publish_i64(h.userdata,key(".owned"),is_owned);
    h.publish_i64(h.userdata,key(".valid"),valid);
    h.publish_i64(h.userdata,key(".enabled"),enabled);
    const auto ability=is_owned?abilities[i]:ChampionAbility{};
    h.publish_i64(h.userdata,key(".state_valid"),ability.valid);
    h.publish_i64(h.userdata,key(".charges"),ability.valid?ability.charges:-1);
    h.publish_i64(h.userdata,key(".seconds"),ability.valid?ability.seconds:-1);
    h.publish_i64(h.userdata,key(".recharging"),ability.valid&&ability.recharging);
    const char* status=!valid?"Unavailable":!owned?"Locked":owned->equipped?"Enabled":"Disabled";
    h.publish_text(h.userdata,key(".status"),status);
    // Canonical champion color identity remains separate from enabled and charge
    // state. Legacy tint_level is retained for existing manifest compatibility;
    // the current pane uses .tint plus the validated live .display_kind/.display.
    slot_icon[i]=icons[i];slot_status[i]=status;slot_tint[i]=int(i);slot_tint_level[i]=int(i)*2+(enabled?1:0);slot_enabled[i]=enabled;slot_owned[i]=is_owned;slot_valid[i]=valid;slot_ability[i]=ability;
  }
  h.publish_i64(h.userdata,"botw.ready.champions.count",count);
  int owned_count=0;for(bool o:slot_owned)owned_count+=o;
  h.publish_i64(h.userdata,"botw.ready.champions.owned",owned_count);
  h.publish_i64(h.userdata,"botw.ready.champions.available",owned_count>0);
  for(int s=0;s<count;++s) {
    Key out("botw.ready.champion_slot.",s);
    h.publish_text(h.userdata,out(".icon"),slot_icon[s]);
    h.publish_text(h.userdata,out(".status"),slot_status[s].c_str());
    h.publish_i64(h.userdata,out(".tint"),slot_tint[s]);
    h.publish_i64(h.userdata,out(".tint_level"),slot_tint_level[s]);
    h.publish_i64(h.userdata,out(".enabled"),slot_enabled[s]);
    const auto& ability=slot_ability[s];
    // Iteration 14 item 12: the key item's pouch "equipped" bit is NOT an on/off switch (the native
    // HUD showed Daruk's Protection available while that bit was clear, and the game has no ability
    // toggle text), so an obtained power always shows its live Player state; not obtained = "-".
    // display_kind 3 ("Off") is retired.
    const int kind=!slot_valid[s]?0:!slot_owned[s]?4:!ability.valid?0:ability.recharging?2:1;
    const std::string display=kind==4?"-":kind==0?"--":kind==2?champion_countdown(ability.seconds):"x"+std::to_string(ability.charges);
    h.publish_i64(h.userdata,out(".state_valid"),ability.valid);
    h.publish_i64(h.userdata,out(".display_kind"),kind);
    h.publish_i64(h.userdata,out(".charges"),ability.valid?ability.charges:-1);
    h.publish_i64(h.userdata,out(".seconds"),ability.valid?ability.seconds:-1);
    h.publish_text(h.userdata,out(".display"),display.c_str());
  }
  return owned_count;
}
} // namespace botw
