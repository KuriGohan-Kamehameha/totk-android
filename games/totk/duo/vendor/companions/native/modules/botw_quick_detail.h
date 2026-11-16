// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "botw_ready.h"
#include "botw_ready_counters.h"
namespace botw {
struct QuickDetail {
  int stat{-1}, stat_kind{}, condition{-1}, shots{-1};
  uint32_t modifier{}, modifier_value{};
  bool yellow{};
};
// Matches the ordinary native getWeaponStats path. Fix round 4 (critique-3): a live capture
// of the native quick-select wheel showed a stat badge ("60") on a highlighted Master Sword
// tile, contradicting this function's prior assumption that the four dynamic special swords
// (Weapon_Sword_070/071/072/502) never show a static power number -- that assumption was
// wrong for the stat value specifically (only the *durability bar/percent* is legitimately
// absent for these actors, since the Master Sword recharges instead of breaking; see
// durability_percent()'s own, still-correct, name-prefix exclusion). The static base
// attack/guard + flat modifier bonus is otherwise computed the same way as any other weapon.
// Fix round A: the Master Sword (Weapon_Sword_070) shows the game's own power, read the way
// ui::getWeaponStats does: 0 while recharging (durability 0), 60 in true form, otherwise base 30 +
// MasterSword_Add_Power; the AddAtk modifier is added after, as for every weapon. Without a valid
// GameData sample the old static base value is kept (never a guess above it).
inline QuickDetail quick_detail(const Item& item, GearModifier m, const MasterSwordState* sword=nullptr) {
  QuickDetail out;
  const auto* info=weapon_info(item.actor);
  if(!info || !m.valid || (item.type!=0 && item.type!=1 && item.type!=3))return out;
  out.condition=durability_percent(item,m.flags,m.value);
  const uint32_t bits=m.flags&0x7FFFFFFFu;
  const bool numeric_bonus=bits==1 || bits==2 || bits==0x100;
  if(bits && !(bits&(bits-1)) && !(bits&~0x15Fu) && (!numeric_bonus || m.value<=100000)) {
    out.modifier=bits;out.modifier_value=m.value;out.yellow=(m.flags&0x80000000u)!=0;
  }
  const bool adds_power=(m.flags&1) || (item.type==3 && (m.flags&0x100));
  if(!adds_power || m.value<=100000) {
    out.stat_kind=item.type==3 ? 2 : 1;
    out.stat=item.type==3 ? info->guard : info->attack;
    if(item.type==0 && item.actor=="Weapon_Sword_070" && sword && sword->valid)
      out.stat=item.value<=0 ? 0 : sword->true_form ? 60 : info->attack+sword->add_power;
    if(item.type==3 && (m.flags&0x100))out.stat+=int(m.value);
    if(m.flags&1)out.stat+=int(m.value);
  }
  if(item.type==1) {
    const auto shots=(m.flags&0x10) ? int64_t(m.value) : info->shots;
    if(shots>=1 && shots<=20)out.shots=int(shots);
  }
  return out;
}
// Iteration 8 item 14: single source of truth for the modifier-bit -> semantic icon-name mapping,
// shared by the per-cell corner badge and the detail pane (previously duplicated inline in both
// 01007EF00011E000.cpp call sites). The semantic names match tools/quick_grid_layout.py's
// MODIFIER_SPRITES table and the manifest's existing top-level sprite_map entries, which already
// resolve each one to its real native texture (romfs:/Pack/Bootup.pack#Layout/Common.sblarc#timg/
// __Combined.bntx#Nt_PauseIcon*_00^t) -- found by grepping the live sprite_map, not re-researched.
// Durability Up (bit 2): the native pause inventory draws Nt_PauseIconDiamondP_00^t (gem + up
// arrow) for it, both as the cell badge and in the detail row -- confirmed side by side against a
// live native capture (iteration 11 item D). It was previously left blank
// on the assumption that no native icon existed.
inline const char* modifier_icon_for(uint32_t modifier,int shots) {
  switch(modifier) {
    case 1:return "modifier_attack";
    case 2:return "modifier_durability";
    case 4:return "modifier_critical";
    case 8:return "modifier_throw";
    case 16:return shots==3 ? "modifier_multishot3" : shots==5 ? "modifier_multishot5" : "";
    case 32:return "modifier_zoom";
    case 64:return "modifier_rapid";
    case 256:return "modifier_shield";
    default:return "";
  }
}
} // namespace botw
