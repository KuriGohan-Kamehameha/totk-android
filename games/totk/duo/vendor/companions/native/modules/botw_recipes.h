// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "botw_recipes_catalog.h"
#include "botw_item_status.h"
#include "botw_msbt_reflow.h" // iteration 8 item 11 / F6: description reflow
#include "botw_text_measure.h"  // i8-layout item 10: real-font wrap/line-count estimate
#include <algorithm>
#include <map>
namespace botw {
// Read-only ingredient guide. A witness satisfies table requirements; it does
// not predict precedence, extra ingredients, effects, or execute cooking/holding.
using RecipeStock=std::map<std::string,int>;
inline RecipeStock recipe_stock(const Snapshot& pouch) {
 RecipeStock out;
 if(!pouch.valid)return out;
 for(const auto& i:pouch.items)if(i.type==7&&i.value>=0&&i.value<=999999)out[i.actor]+=i.value;
 return out;
}
inline std::vector<std::string> recipe_witness(const RecipeRule& rule,const RecipeStock& stock) {
 if(rule.kind==2||rule.groups.empty()||rule.groups.size()>5)return {};
 // Small bipartite matching: each native ingredient group uses a distinct actor
 // record. Augmenting paths avoid exponential scans when the last group is absent.
 std::vector<std::string> result(rule.groups.size());std::map<std::string,size_t> assigned;
 auto augment=[&](auto&& self,size_t group,std::set<std::string>& visited)->bool{
  for(const auto& actor:rule.groups[group]){
   const auto n=stock.find(actor);if(n==stock.end()||n->second<=0||!visited.insert(actor).second)continue;
   const auto old=assigned.find(actor);
   if(old==assigned.end()||self(self,old->second,visited)){assigned[actor]=group;result[group]=actor;return true;}
  }return false;
 };
 for(size_t i=0;i<rule.groups.size();++i){std::set<std::string> visited;if(!augment(augment,i,visited))return {};}
 return result;
}
// CookData names two actors the pouch never holds (no item name or icon exists for them): a
// world-placed monster-part variant (picked up as Item_Enemy_57) and an unused mushroom id. They
// stay valid for matching but are not offered in the alternatives pop-up.
inline bool recipe_alternative_listed(std::string_view actor){return actor!="Item_Enemy_Put_57"&&actor!="Item_Mushroom_D";}
struct RecipesView {
 int kind{};bool available{};size_t selected{};std::array<size_t,5> alternatives{};uint64_t filter_version{};
 // 2026-10-18 finishing round (owner item: Recipes alternatives grid). Tapping an ingredient row
 // that has alternatives opens a pop-up grid of every alternative for that slot instead of
 // cycling one by one. alt_open is the open slot (-1 closed). The open slot's list is published
 // bounded to AltCap entries (largest native group is 59), owned first then missing, catalog
 // order inside each half; a cell's pick payload is slot*AltPickStride + catalog index so a stale
 // tap from another slot or recipe is refused.
 static constexpr size_t AltCap=64;static constexpr int64_t AltPickStride=256;
 int alt_open=-1;
 // Iteration 8 item 9 (owner, 2026-10-18): slots whose alternatives GROUP is byte-identical
 // (confirmed real in CookData: 10/138 recipes repeat a group across 2+ slots, e.g.
 // Item_Cook_B_16's monster-part slots) merge into one pop-up with a holding-box-style tray;
 // alt_tray_chosen marks which of those merged slots this pop-up visit has already picked
 // (reset whenever the pop-up (re)opens). Single-slot (non-merged) pop-ups are unaffected:
 // picking there still commits + closes immediately, same as before this iteration.
 std::array<bool,5> alt_tray_chosen{};
 std::vector<size_t> merged_group_slots(size_t slot)const{
  std::vector<size_t> out;if(selected>=recipe_catalog().size())return out;
  const auto& r=recipe_catalog()[selected];if(r.kind==2||slot>=r.groups.size())return out;
  const auto& g=r.groups[slot];
  for(size_t i=0;i<r.groups.size();++i)if(r.groups[i]==g)out.push_back(i);
  return out;
 }
 std::vector<size_t> alternative_order(size_t slot,const RecipeStock& stock)const{
  std::vector<size_t> out;if(selected>=recipe_catalog().size())return out;
  const auto& r=recipe_catalog()[selected];if(r.kind==2||slot>=r.groups.size())return out;const auto& g=r.groups[slot];
  for(int pass=0;pass<2;++pass)for(size_t i=0;i<g.size()&&out.size()<AltCap;++i){
   if(!recipe_alternative_listed(g[i]))continue;
   const auto n=stock.find(g[i]);const bool owned=n!=stock.end()&&n->second>0;if(owned==(pass==0))out.push_back(i);}
  return out;
 }
 std::vector<size_t> visible(const Snapshot& pouch)const {
  std::vector<size_t> out;const auto stock=recipe_stock(pouch);const auto& catalog=recipe_catalog();
  for(size_t i=0;i<catalog.size();++i){const auto& r=catalog[i];if(r.kind==2||(kind==1&&r.kind!=0)||(kind==2&&r.kind!=1))continue;
   if(available&&(!pouch.valid||recipe_witness(r,stock).empty()))continue;
   out.push_back(i);
  }return out;
 }
 void select(size_t index,const RecipeStock& stock){selected=index;alternatives.fill(0);alt_open=-1;
  const auto& r=recipe_catalog()[selected];const auto witness=recipe_witness(r,stock);
  for(size_t i=0;i<witness.size();++i)alternatives[i]=size_t(std::find(r.groups[i].begin(),r.groups[i].end(),witness[i])-r.groups[i].begin());
 }
 std::vector<std::string> selected_hold_actors(const Snapshot& pouch)const{
  if(!pouch.valid||selected>=recipe_catalog().size())return {};
  const auto& r=recipe_catalog()[selected];if(r.kind==2||r.groups.empty()||r.groups.size()>5)return {};
  const auto stock=recipe_stock(pouch);std::set<std::string> used;std::vector<std::string> out;
  for(size_t i=0;i<r.groups.size();++i){const auto& group=r.groups[i];if(group.empty())return {};
   const auto& actor=group[alternatives[i]%group.size()];const auto n=stock.find(actor);
   if(n==stock.end()||n->second<1||!used.insert(actor).second)return {};
   out.push_back(actor);
  }return out;
 }
 bool action(const char* a,int64_t argument,const Snapshot& pouch){
  if(!a)return false;
  if(std::strcmp(a,"recipes_kind")==0){if(argument<0||argument>2)return false;kind=int(argument);++filter_version;alt_open=-1;return true;}
  if(std::strcmp(a,"recipes_available")==0){available=!available;++filter_version;alt_open=-1;return true;}
  if(std::strcmp(a,"recipes_alternatives")==0){
   if(argument<0||argument>=5||selected>=recipe_catalog().size())return false;
   const auto& r=recipe_catalog()[selected];
   if(r.kind==2||size_t(argument)>=r.groups.size()||r.groups[size_t(argument)].size()<2)return false;
   alt_open=int(argument);alt_tray_chosen.fill(false);return true;
  }
  if(std::strcmp(a,"recipes_alternatives_close")==0){alt_open=-1;return true;}
  if(std::strcmp(a,"recipes_alternative_pick")==0){
   if(alt_open<0||argument<0||selected>=recipe_catalog().size())return false;
   const int64_t slot=argument/AltPickStride,index=argument%AltPickStride;
   const auto& r=recipe_catalog()[selected];
   if(slot!=alt_open||size_t(slot)>=r.groups.size()||size_t(index)>=r.groups[size_t(slot)].size())return false;
   const auto order=alternative_order(size_t(slot),recipe_stock(pouch));
   if(std::find(order.begin(),order.end(),size_t(index))==order.end())return false;
   // Item 9: in a merged (mix) pop-up, a tile tap fills the first not-yet-chosen merged slot
   // instead of always writing the opened slot itself, and the pop-up stays open so the
   // remaining boxes can be filled; a true single-slot pop-up keeps the old commit+close.
   const auto merged=merged_group_slots(size_t(slot));
   if(merged.size()>1){
    size_t target=merged.size();
    for(auto m:merged)if(!alt_tray_chosen[m]){target=m;break;}
    if(target==merged.size())return false;
    alternatives[target]=size_t(index);alt_tray_chosen[target]=true;return true;
   }
   alternatives[size_t(slot)]=size_t(index);alt_open=-1;return true;
  }
  if(std::strcmp(a,"recipes_alts_mix_unpick")==0){
   if(alt_open<0||argument<0||argument>=5||selected>=recipe_catalog().size())return false;
   const auto merged=merged_group_slots(size_t(alt_open));
   if(size_t(argument)>=merged.size())return false;
   const auto target=merged[size_t(argument)];
   if(!alt_tray_chosen[target])return false;
   alt_tray_chosen[target]=false;return true;
  }
  if(std::strcmp(a,"recipes_choose")==0){const auto list=visible(pouch);if(argument<0||std::find(list.begin(),list.end(),size_t(argument))==list.end())return false;select(size_t(argument),recipe_stock(pouch));return true;}
  if(std::strcmp(a,"recipes_alternative")==0){if(argument<0||argument>=5||selected>=recipe_catalog().size())return false;const auto& r=recipe_catalog()[selected];if(size_t(argument)>=r.groups.size()||r.groups[size_t(argument)].size()<2)return false;alternatives[size_t(argument)]=(alternatives[size_t(argument)]+1)%r.groups[size_t(argument)].size();return true;}
  return false;
 }
 void publish(const EdenDsmodHostApi& h,const Snapshot& pouch,const char* page){
  if(!page||std::strcmp(page,"recipes")!=0){alt_open=-1;return;}
  auto pi=[&](const std::string& k,int64_t v){h.publish_i64(h.userdata,k.c_str(),v);};auto pt=[&](const std::string& k,const std::string& v){h.publish_text(h.userdata,k.c_str(),v.c_str());};
  const auto stock=recipe_stock(pouch);const auto list=visible(pouch);const auto& catalog=recipe_catalog();
  if(!list.empty()&&std::find(list.begin(),list.end(),selected)==list.end())select(list.front(),stock);
  pt("botw.recipes.summary",std::to_string(list.size())+(available?" available":" recipes"));pi("botw.recipes.count",list.size());pi("botw.recipes.empty",list.empty());pi("botw.recipes.filter",int64_t(filter_version));pi("botw.recipes.kind",kind);pi("botw.recipes.available",available);
  for(size_t i=0;i<list.size();++i){const auto index=list[i];const auto& r=catalog[index];const auto key="botw.recipes.row."+std::to_string(i);pt(key+".actor",r.result);pi(key+".name",actor_key(r.result));pi(key+".index",index);pi(key+".selected",index==selected);pt(key+".status",!pouch.valid?"Inventory unavailable":recipe_witness(r,stock).empty()?"Missing ingredients":"Ingredients present");}
  const RecipeRule* r=list.empty()?nullptr:&catalog[selected];pt("botw.recipes.selected.actor",r?r->result:"");pi("botw.recipes.selected.name",r?actor_key(r->result):-1);
  Recovery owned{};
  if(r&&pouch.valid)for(const auto& item:pouch.items)if(item.actor==r->result) {
    owned=recovery(h,&item);if(owned.valid)break;
  }
  std::string recovery_summary;
  if(r) {
    recovery_summary="Recovery varies by ingredients";
    if(owned.valid) {
      static constexpr const char* fractions[]={"","¼","½","¾"};
      const int q=std::clamp(owned.quarters,0,120);
      recovery_summary=owned.full?"Owned dish: Full recovery":"Owned dish: +"+std::to_string(q/4)+fractions[q%4];
    }
  }
  pt("botw.recipes.recovery.summary",recovery_summary);
  pi("botw.recipes.recovery.shown",r!=nullptr);
  pi("botw.recipes.recovery.heart_x",632+MeasureTextPx(recovery_summary,5)+12);
  // description_clean is published further down (after desc_max_lines is known), truncated to
  // what the now-content-aware layout actually has room for (layout batch 2 fix round 1) --
  // the raw-text publish that used to happen here (via a second, redundant DescriptionCache
  // instance) is removed; only `desc_cache` below is used now.
  std::set<std::string> used;bool ready=r&&pouch.valid;
  for(size_t i=0;i<5;++i){const auto key="botw.recipes.ingredient."+std::to_string(i);const bool present=r&&i<r->groups.size();const auto* group=present?&r->groups[i]:nullptr;const std::string actor=group&&!group->empty()?(*group)[alternatives[i]%group->size()]:"";
   const auto found=stock.find(actor);const int count=found==stock.end()?0:found->second;
   if(present&&(actor.empty()||count<1||!used.insert(actor).second))ready=false;
   pi(key+".present",present);pt(key+".actor",actor);pi(key+".name",actor_key(actor));pi(key+".alternatives",group&&group->size()>1);pi(key+".options",group?int64_t(group->size()):0);pt(key+".stock",present?(pouch.valid?std::to_string(count)+" / 1":"? / 1"):"");
  }
  pi("botw.recipes.hold.available",r&&!selected_hold_actors(pouch).empty());
  pt("botw.recipes.selected.status",!r?"":!pouch.valid?"Inventory unavailable":ready?"Selected ingredients present":"Ingredients missing");
  pi("botw.recipes.selected.ready",ready);
  // Alternatives pop-up (finishing round): the open slot's list, bounded to AltCap.
  if(alt_open>=0&&(!r||size_t(alt_open)>=r->groups.size()||r->groups[size_t(alt_open)].size()<2))alt_open=-1;
  const auto order=alt_open>=0?alternative_order(size_t(alt_open),stock):std::vector<size_t>{};
  pi("botw.recipes.alts.open",alt_open>=0);pi("botw.recipes.alts.slot",alt_open);pi("botw.recipes.alts.count",int64_t(order.size()));
  for(size_t i=0;i<order.size();++i){const auto& actor=r->groups[size_t(alt_open)][order[i]];const auto key="botw.recipes.alts."+std::to_string(i);
   const auto found=stock.find(actor);const int count=found==stock.end()?0:std::min(found->second,999999);
   pt(key+".actor",actor);pi(key+".name",actor_key(actor));pi(key+".count",count);pi(key+".owned",count>0);
   pi(key+".chosen",order[i]==alternatives[size_t(alt_open)]%r->groups[size_t(alt_open)].size());
   pi(key+".pick",int64_t(alt_open)*AltPickStride+int64_t(order[i]));}
  // Item 9 mix tray: when the open slot's group is shared by other slots, publish the merged
  // picker footer (one box per merged slot, in slot order) instead of leaving it to a single
  // commit-and-close pick.
  const auto merged=alt_open>=0?merged_group_slots(size_t(alt_open)):std::vector<size_t>{};
  const bool mix=merged.size()>1;
  pi("botw.recipes.alts.mix",mix);
  {unsigned filled=0;for(auto m:merged)if(alt_tray_chosen[m])++filled;
   pt("botw.recipes.alts.mix.label",mix?std::to_string(filled)+"/"+std::to_string(merged.size())+" picked":std::string());}
  for(size_t p=0;p<5;++p){const auto key="botw.recipes.alts.mix.box."+std::to_string(p);
   const bool shown=mix&&p<merged.size();const bool filled=shown&&alt_tray_chosen[merged[p]];
   const std::string actor=filled?r->groups[merged[p]][alternatives[merged[p]]%r->groups[merged[p]].size()]:std::string();
   pi(key+".shown",shown);pi(key+".filled",filled);pt(key+".actor",actor);pi(key+".name",actor_key(actor));}
  // Iteration-3 fix round 3, Recipes page finding ("~250 px empty between ingredients and
  // hints"): the ingredient rows are a fixed 5-slot block (manifest rows at y 554 + i*68,
  // each 64 tall) but most recipes use far fewer than 5 groups, leaving the footer (status +
  // "Tap ingredient..." hint, fixed at y 916/946) stranded far below the last VISIBLE row.
  // Publish the footer's own absolute y so the manifest can bind it there instead of a fixed
  // position (y_bind: ResolveBindOffset adds this as an offset to the widget's own rect[1],
  // which the manifest pass sets to 0 for these two widgets so the published value is used
  // verbatim -- mod_ui_expand.cpp).
  const int64_t ingredient_count=int64_t(r?r->groups.size():0);
  // 2026-10-18 relayout: the footer row is 112 px tall from y 944, so the ingredient block moves
  // up and the status/hint pair packs tighter.
  // Fix round A (CRITIQUE-2 Recipes L): ingredient rows grow to 72 px at a 76 px pitch from y 512
  // (25 px text, 68 px icons); five groups end at y 892 + status (to y 920). The hint
  // ("Tap ingredient to change option") is shown only while there is room above the footer.
  // Finishing round (CRITIQUE-3 F22): the rows are now the only way into the alternatives pop-up,
  // so each is an 88 px touch row. Up to four groups: rows from y 512 at a 100 px pitch (12 px
  // gaps). Five groups (3 of 138 recipes): the "Ingredients" header/rule give way
  // (botw.recipes.ingredients.compact) and the rows start at y 470 at a 94 px pitch so the
  // block ends at 934, above the 944 footer row; the status line has no room there and is not
  // shown (the per-row stock column already says what is missing). Row tops are published per
  // row (botw.recipes.ingredient.N.y) for the manifest's y_bind.
  constexpr int64_t ROW_H=88,FOOTER_GAP=8,HINT_GAP=26,FOOTER_TOP=944,HINT_CAP=20,STATUS_CAP=25;
  const bool compact=ingredient_count>4;
  // i8-layout item 10 (2026-10-18, content-aware redo) + layout batch 2 item 1 (2026-10-19,
  // moved): the title->"Ingredients" gap USED to also reserve room for the description above
  // the header (DESCRIPTION_LABEL at a fixed y=356), which produced two owner-visible bugs: (a)
  // live re-check found the description genuinely never renders up there for the common,
  // few-ingredient case (confirmed live: Item_Cook_C_17/"Elixir" -- its own `_Desc` msbt label
  // IS the empty string in the real romfs data, decoded directly, not a mapping bug -- the real
  // game simply has no generic elixir description, matching this module's existing
  // empty-description branch), so that reserved space did nothing; (b) critic REVIEW.md found a
  // ~330px DEAD band below the ingredient rows and above the fixed 944px "Hold ingredients"
  // button for any recipe with a short ingredient list (the common case) -- the description
  // never used that space at all. Fix: the description now draws BELOW the ingredient rows (in
  // exactly that otherwise-dead band) instead of above the header, so it only ever occupies
  // space nothing else needs, content-aware the same way the ingredient block itself already is
  // -- this doubles as the removal of the dead band, not a separate change: there IS no more
  // fixed-size dead band once the description is the thing filling it. The title->header gap no
  // longer depends on description height (nothing but the title sits there now), so it collapses
  // to the title's own worst-case (2-line) bottom, same TITLE_BOTTOM_MAX this file already used
  // for the "no description" branch -- always, not conditionally.
  static DescriptionCache desc_cache;
  const uint64_t name_hash=r?uint64_t(actor_key(r->result)):0;
  const std::string desc_text=(r&&h.read_romfs)?desc_cache.Lookup(h.read_romfs,h.userdata,name_hash):std::string();
  constexpr int DESC_SCALE=4,DESC_WRAP=560,DESC_MAX_LINES=4;
  constexpr int64_t TITLE_BOTTOM_MAX=338;     // TITLE_TOP + TextBlockHeightPx(6,2): worst-case 2-line title
  constexpr int64_t DIVIDER_GAP=20,HEADER_GAP=14,ROW_GAP=32; // same deltas the old static layout used
  constexpr int64_t DIVIDER_MAX=446,HEADER_MAX=460,ROW_BASE_MAX_NORMAL=492,ROW_BASE_MAX_COMPACT=450;
  const int64_t divider_y=std::min<int64_t>(TITLE_BOTTOM_MAX+DIVIDER_GAP,DIVIDER_MAX)+40;
  const int64_t header_y=std::min<int64_t>(divider_y+HEADER_GAP,HEADER_MAX);
  const int64_t row_base=std::min<int64_t>(header_y+ROW_GAP,compact?ROW_BASE_MAX_COMPACT:ROW_BASE_MAX_NORMAL);
  const int64_t row_pitch=compact?94:100;
  pi("botw.recipes.ingredients.divider_y",divider_y);
  pi("botw.recipes.ingredients.header_y",header_y);
  for(int64_t i=0;i<5;++i)pi("botw.recipes.ingredient."+std::to_string(i)+".y",row_base+i*row_pitch);
  const int64_t footer_y=(ingredient_count?row_base+ingredient_count*row_pitch-(row_pitch-ROW_H):row_base)+FOOTER_GAP;
  pi("botw.recipes.ingredients.count",ingredient_count);
  pi("botw.recipes.ingredients.compact",compact);
  pi("botw.recipes.footer.status_y",footer_y);
  const bool status_visible=footer_y+STATUS_CAP<=FOOTER_TOP-4;
  const int64_t hint_y=footer_y+HINT_GAP;
  const bool hint_visible=hint_y+HINT_CAP<=FOOTER_TOP-4;
  pi("botw.recipes.footer.status_visible",status_visible);
  pi("botw.recipes.footer.hint_y",hint_y);
  pi("botw.recipes.footer.hint_visible",hint_visible);
  // Description, below everything else on the right pane (layout batch 2 item 1): starts right
  // after whichever of hint/status/ingredient-rows is actually the lowest visible content, with
  // a 16px gap, and must leave room above the fixed 944px button row (16px clearance) for AT
  // LEAST one line -- otherwise it hides rather than overlapping the button (LEGIBILITY.md "show
  // less, never clip/overlap"). This is the only place description_clean is shown now (replaces
  // the old above-header position).
  constexpr int64_t DESC_GAP=16,BUTTON_TOP=944,BUTTON_CLEARANCE=16;
  const int64_t rows_bottom=ingredient_count?row_base+ingredient_count*row_pitch-(row_pitch-ROW_H):row_base;
  const int64_t content_end=hint_visible?(hint_y+HINT_CAP):status_visible?(footer_y+STATUS_CAP):rows_bottom;
  const int64_t desc_top=content_end+DESC_GAP;
  const int64_t desc_bottom_max=BUTTON_TOP-BUTTON_CLEARANCE;
  const int64_t one_line_h=TextBlockHeightPx(DESC_SCALE,1);
  const bool desc_fits=(desc_top+one_line_h)<=desc_bottom_max;
  const int64_t desc_available_h=desc_fits?(desc_bottom_max-desc_top):0;
  // Cap the line count to what the remaining space can hold (never 0 lines if desc_fits, since
  // that already guarantees room for >=1 line); still capped at DESC_MAX_LINES like before.
  int desc_max_lines=DESC_MAX_LINES;
  if(desc_fits&&desc_max_lines>1){
   const int64_t pitch=DESC_SCALE*5+DESC_SCALE*3;
   const int by_height=1+int((desc_available_h-one_line_h)/std::max<int64_t>(pitch,1));
   if(by_height<desc_max_lines)desc_max_lines=std::max(1,by_height);
  }
  const int desc_lines=desc_fits&&!desc_text.empty()
   ?LabelEffectiveLines(desc_text,DESC_SCALE,DESC_WRAP,DESC_SCALE,desc_max_lines):0;
  pi("botw.recipes.description.visible",desc_fits&&desc_lines>0);
  pi("botw.recipes.description.y",desc_top);
  // Truncate to EXACTLY what desc_max_lines/desc_available_h allow, with an ellipsis if cut --
  // the manifest label's own max_lines stays a generous static ceiling (see i8-layout.py), so
  // without this the widget would re-wrap the FULL text and draw past the available space (the
  // critic's live-confirmed overflow onto the "Hold ingredients" button).
  pt("botw.recipes.selected.description_clean",desc_lines>0
   ?TruncateToLines(desc_text,DESC_SCALE,DESC_WRAP,desc_max_lines):std::string());
 }
};
} // namespace botw
