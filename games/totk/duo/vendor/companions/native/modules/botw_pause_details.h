// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "botw_item_status.h"
#include "botw_detail_line.h"
#include "botw_msbt_reflow.h"
#include "botw_detail_flow.h"
#include <cstdio>
namespace botw {
// Identifiers only: all displayed names/effect prose come from the configured native messages.
inline const char* food_effect_key(int effect) {
 switch(effect){case 2:return "LifeMaxUp";case 4:return "ResistHot";case 5:return "ResistCold";
 case 6:return "ResistElectric";case 10:return "AttackUp";case 11:return "DefenseUp";
 case 12:return "Quietness";case 13:return "AllSpeed";case 14:return "GutsRecover";
 case 15:return "ExGutsMaxUp";case 16:return "Fireproof";default:return "";}
}
inline std::string armor_effect_key(std::string_view effect) {
 if(effect=="Quietness")return "Quietness";
 if(effect=="SwimSpeed"||effect=="SwimSpeedAndResistAncient")return "SwimSpeedUp";
 if(effect=="ClimbSpeed"||effect=="ClimbSpeedAndBeamPowerUp")return "ClimbSpeedUp";
 if(effect=="ResistBurn"||effect=="ResistBurnAndResistAncient")return "Fireproof";
 if(effect=="ResistAncient")return "ReduceAncientEnemyDamge";
 if(effect=="ResistElectricAndResistAncient")return "ResistElectric";
 if(effect=="ResistColdAndResistAncient")return "ResistCold";
 if(effect=="ResistHotAndWakeWind")return "ResistHot";
 if(effect=="SnowMove")return "SnowMovingSpeed";
 if(effect=="SandMove")return "SandMoveSpeedUp";
 return effect=="None"?"":std::string(effect);
}
class PauseMessages {
 std::unordered_map<std::string,std::unordered_map<std::string,std::string>> tables_;
 public:
 std::string get(const EdenDsmodHostApi& h,std::string_view file,const std::string& key) {
  if(key.empty())return {};
  const std::string name(file);auto it=tables_.find(name);
  if(it==tables_.end()){
   std::unordered_map<std::string,std::string> labels;
   const std::string path="romfs:/Pack/Bootup_USen.pack#Message/Msg_USen.product.ssarc#"+name+".msbt";
   const auto n=h.read_romfs?h.read_romfs(h.userdata,path.c_str(),0,nullptr,0):0;
   if(n&&n<(4u<<20)){std::vector<uint8_t>b(n);if(h.read_romfs(h.userdata,path.c_str(),0,b.data(),n)==n)
    for(const auto& [k,v]:msbt_detail::ParseLabels(b))labels[k]=text_reflow::Reflow(msbt_detail::ToUtf8(v),0);}
   it=tables_.emplace(name,std::move(labels)).first;
  }
  const auto label=it->second.find(key);return label==it->second.end()?std::string():label->second;
 }
};
struct OutfitDefense {bool valid{};int before{},after{};};
inline OutfitDefense outfit_defense(const Snapshot& pouch,const Item* selected) {
 if(!pouch.valid||!selected||selected->type<4||selected->type>6)return {};
 const auto* target=armor_status(selected->actor);if(!target)return {};
 std::array<bool,3> seen{};int total{},replaced{};
 for(const auto& i:pouch.items)if(i.equipped&&i.type>=4&&i.type<=6){
  const auto* a=armor_status(i.actor);if(!a||seen[size_t(i.type-4)])return {};
  seen[size_t(i.type-4)]=true;total+=a->defense;if(i.type==selected->type)replaced=a->defense;
 }
 return {true,total,total-replaced+target->defense};
}
inline void publish_pause_item(const EdenDsmodHostApi& h,const Item* item,const Snapshot& pouch) {
 static PauseMessages messages;static DescriptionCache descriptions;
 const std::string p="botw.inventory.selected.pause";
 auto n=[&](const char* k,int64_t v){h.publish_i64(h.userdata,(p+k).c_str(),v);};
 auto t=[&](const char* k,const std::string& v){h.publish_text(h.userdata,(p+k).c_str(),v.c_str());};
 std::string name,desc,effect,icon,timer;int defense=-1;bool cooked=false;
 const auto food=item?consumable(h,*item):Consumable{};
 if(item){
  const auto* ref=description_ref(actor_key(item->actor));
  if(ref){const auto path=BuildMsbtPath(ref->alias);const auto marker=path.rfind('#');
   const auto file=marker==std::string::npos?std::string():path.substr(marker+1,path.size()-marker-6);
   if(!file.empty())name=messages.get(h,file,item->actor+"_Name");}
  desc=descriptions.Lookup(h.read_romfs,h.userdata,actor_key(item->actor));
  if(item->type>=4&&item->type<=6){if(const auto* a=armor_status(item->actor)){
   defense=a->defense;effect=messages.get(h,"StaticMsg/SpecialStatus",armor_effect_key(a->effect)+"_Name");icon=a->icon.empty()?std::string():"status-detail:"+std::string(a->icon);
  }}
  if(food.valid&&item->type==8){
   cooked=true;const std::string key=food_effect_key(food.effect);
   if(!key.empty()){
    auto prefix=messages.get(h,"StaticMsg/CookEffect",key+"_Name");
    while(!prefix.empty()&&prefix.back()==' ')prefix.pop_back();
    if(!name.empty()&&!prefix.empty())name=prefix+" "+name;
    const auto suffix=food.level>=2&&food.level<=3?"_0"+std::to_string(food.level):std::string();
    const bool elixir=item->actor=="Item_Cook_C_17";
    auto intro=messages.get(h,"StaticMsg/CookEffect",key+(elixir?"_MedicineDesc":"_Desc")+suffix);
    if(intro.empty())intro=messages.get(h,"StaticMsg/CookEffect",key+(elixir?"_MedicineDesc":"_Desc"));
    if(!intro.empty())desc=elixir?intro:intro+" "+desc;
    effect=messages.get(h,"StaticMsg/SpecialStatus",key+"_Name");icon="status-detail:"+std::string(food_badge(food.effect));
    if(food.frames>0){char b[24];std::snprintf(b,sizeof b,"%02d:%02d",food.frames/1800,(food.frames/30)%60);timer=b;}
    // Native prose carries exact potency; no guessed numeric conversion of stamina units.
   }
  }
 }
 const auto outfit=outfit_defense(pouch,item);
 t(".name",name);t(".description",desc);t(".effect",effect);t(".effect_icon",icon);t(".timer",timer);
 n(".effect_valid",!effect.empty());n(".timer_valid",!timer.empty());n(".defense",defense);n(".cooked",cooked);
 n(".outfit_known",outfit.valid);n(".outfit_valid",outfit.valid&&outfit.before!=outfit.after);n(".outfit_before",outfit.before);n(".outfit_after",outfit.after);
 // Iteration 13 (items 2/3): every well uses the shared box model (botw_detail_flow.h): exact
 // quantised width, content centred, 8 px inset; heart rows 49 px apart, the food effect box
 // flows after the hearts and never overlaps them.
 namespace fl=botw::flow;
 // Armor effect: 28 px icon, 8 px gap, label; one row below the defense comparison.
 const int an=fl::kPad+28+8+MeasureTextPx(effect,5)+fl::kPad,aw=fl::quantise(an);
 n(".armor_effect_width",aw);n(".armor_effect_content_x",816+fl::kPad+fl::centre_offset(an,aw));
 // Defense before/after values centred in their 80 px wells (860 and 966).
 n(".outfit_before_x",860+std::max(0,(80-MeasureTextPx(std::to_string(outfit.before),5))/2));
 n(".outfit_after_x",966+std::max(0,(80-MeasureTextPx(std::to_string(outfit.after),5))/2));
 // Food effect: icon, then clock + timer, or the effect name for instant effects.
 const int en=timer.empty()?fl::kPad+28+8+MeasureTextPx(effect,5)+fl::kPad:fl::kPad+28+8+25+9+MeasureTextPx(timer,5)+fl::kPad;
 const int ew=fl::quantise(en);
 const auto rec=recovery(h,item);
 const int heart_count=rec.valid&&!rec.full?(rec.quarters+3)/4:0;
 const int heart_rows=rec.full?(rec.extra>0?2:1):(heart_count+9)/10;
 fl::Flow flow(816,1208,816);
 // One well per heart row (= botw_item_status.h row_width<r>); the effect flows after the last.
 for(int r=0;r<heart_rows&&heart_count;++r){
  if(r)flow.newline();
  flow.place(std::clamp(heart_count-r*10,0,10)*34+12);
 }
 const bool show_effect=!effect.empty()&&!rec.full;
 const auto box=show_effect?flow.place(ew):fl::Box{816,0,ew};
 const int rows=std::max(1,show_effect?flow.rows():heart_rows);
 const int content_x=box.x+fl::kPad+fl::centre_offset(en,ew);
 n(".food_effect_x",box.x);n(".food_effect_dy",box.row*fl::kPitch);n(".recovery_rows",rows);
 n(".food_content_x",content_x);n(".timer_x",content_x+28+8+25+9);n(".food_effect_width",ew);
}
// Iteration 13 (items 2-4): the native order is value (+ counter), modifier, durability. The
// stat icon stands bare at x; the value well follows; the modifier well flows after it and wraps
// to the next row inside the Inventory pane (816..1208); the durability bar/percent sit on the
// row after the last well (dur_dy), never between the value and the modifier. Pure layout over
// measured text widths so it is testable without romfs text.
struct GearLayout {int well_x{},well_w{},power_x{},effect_x{},effect_w{},icon_x{},modifier_x{},bonus_x{},effect_dy{},rows{1},dur_dy{};bool fits{true};};
inline GearLayout layout_gear(int x,int power_w,bool icon,int label_w,int value_w,bool inventory) {
 namespace fl=botw::flow;GearLayout g;
 g.well_x=x+36;g.well_w=fl::quantise(power_w+2*fl::kPad);g.power_x=g.well_x+fl::centre_offset(power_w,g.well_w);
 const bool has_label=label_w>0;
 const int mn=fl::kPad+(icon?32:0)+label_w+(value_w?6+value_w:0)+fl::kPad;g.effect_w=has_label?fl::quantise(mn):0;
 const int right=inventory?1208:1160;
 fl::Flow flow(x-8,right,g.well_x);flow.place(g.well_w);
 // Pop-ups are wide enough for one row; only the Inventory pane wraps.
 auto box=has_label?flow.place(g.effect_w):fl::Box{g.well_x+g.well_w+fl::kGap,0,0};
 if(!inventory&&box.row){box.row=0;box.x=g.well_x+g.well_w+fl::kGap;}
 g.effect_x=box.x;g.icon_x=box.x+fl::kPad+(has_label?fl::centre_offset(mn,g.effect_w):0);
 g.modifier_x=g.icon_x+(icon?32:0);g.bonus_x=g.modifier_x+label_w+6;
 g.effect_dy=box.row*fl::kPitch;g.rows=has_label?box.row+1:1;g.dur_dy=(g.rows-1)*fl::kPitch;
 g.fits=!has_label||box.x+g.effect_w<=right;return g;
}
inline void publish_pause_gear(const EdenDsmodHostApi& h,const char* prefix,int stat,int shots,
 uint32_t message,uint32_t value,const char* modifier_icon,bool inventory) {
 static DetailModifierText text;const auto label=stat>=0?text.lookup(h.read_romfs,h.userdata,message):std::string();
 const std::string p=std::string(prefix)+".pause";const int x=inventory?824:100;
 const std::string power=stat<0?"":std::to_string(stat)+(shots>1?" ×"+std::to_string(shots):"");
 const bool icon_valid=modifier_icon&&*modifier_icon;
 const int lw=label.empty()?0:MeasureTextPx(label,5);
 const int vw=detail_modifier_numeric(message)?MeasureTextPx(std::to_string(value),5):0;
 const auto g=layout_gear(x,MeasureTextPx(power,5),icon_valid,lw,vw,inventory);
 auto n=[&](const char* k,int v){h.publish_i64(h.userdata,(p+k).c_str(),v);};
 h.publish_text(h.userdata,(p+".power").c_str(),power.c_str());
 n(".stat_valid",stat>=0);n(".well_x",g.well_x);n(".well_width",g.well_w);n(".power_x",g.power_x);
 n(".effect_x",g.effect_x);n(".effect_width",g.effect_w);
 n(".icon_x",g.icon_x);n(".modifier_x",g.modifier_x);n(".bonus_x",g.bonus_x);n(".icon_valid",icon_valid&&!label.empty());
 n(".effect_dy",g.effect_dy);n(".rows",g.rows);n(".dur_dy",g.dur_dy);n(".fits",g.fits);
}
} // namespace botw
