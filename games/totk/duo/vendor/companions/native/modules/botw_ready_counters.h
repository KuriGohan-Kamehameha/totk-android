// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "botw_tracker_flags.h"
#include <chrono>
namespace botw {
// Fix round A (2026-10-18): the inputs of the game's own Master Sword power (decomp
// ui::getWeaponStats -> dmg::DamageInfoMgr::isTrueFormMasterSword): true form = 60
// (masterSwordTrueFormAttackPower), otherwise base attack + MasterSword_Add_Power (Trial of
// the Sword upgrades, +10 per stage). The DamageInfoMgr "evil detected" glow and the Divine Beast
// map test are not readable here; GameData covers the permanent cases (Trial completed, Hyrule
// Castle area, final boss) -- recorded as a limitation.
struct MasterSwordState {
  bool valid{},true_form{};int add_power{};
  bool operator==(const MasterSwordState&)const=default;
};
// Exact1.6 native GameData integers. Count display never grants actions.
class ReadyCounters {
  TrackerFlagReader flags;
  std::chrono::steady_clock::time_point next{};
  uint64_t epoch{};
  int orbs{-1},shrines{-1};
  MasterSwordState sword{};
public:
  const MasterSwordState& master_sword() const {return sword;}
  // world: gameplay (World); menu: the native pause menu is open. Fix round A (CRITIQUE-2 Ready
  // S finding): the counts cannot change inside the pause menu, so a failed read there keeps the
  // last values instead of blanking them; any other context still clears them.
  void publish(const EdenDsmodHostApi& h,uint64_t current_epoch,bool world,bool menu=false) {
    const auto now=std::chrono::steady_clock::now();
    const bool live=world||menu;
    if(epoch!=current_epoch||!live){flags.clear();next={};epoch=current_epoch;orbs=shrines=-1;sword={};}
    if(live&&now>=next) {
      int a_out=-1,b_out=-1;MasterSwordState ms{};
      if(flags.begin(h)) {
        const auto a=flags.integer(0xF11FF7E1u),b=flags.integer(0xE605CE62u);
        const auto full=flags.observe_boolean(0x8FCFFC54u),castle=flags.observe_boolean(0x986F0046u),
                   beast=flags.observe_boolean(0x462B092Fu);
        const auto add=flags.observe_integer(0x7B248982u);
        if(flags.finish()){
          if(a&&*a>=0&&*a<=124)a_out=*a;
          if(b&&*b>=0&&*b<=136)b_out=*b;
          if(full&&castle&&beast&&add&&*add>=0&&*add<=1000){ms.valid=true;ms.true_form=*full||*castle||*beast;ms.add_power=*add;}
        }
      }
      if(world||a_out>=0)orbs=a_out;
      if(world||b_out>=0)shrines=b_out;
      if(world||ms.valid)sword=ms;
      next=now+std::chrono::seconds(1);
    }
    h.publish_i64(h.userdata,"botw.ready.spirit_orbs",orbs);
    h.publish_i64(h.userdata,"botw.ready.spirit_orbs.valid",orbs>=0);
    h.publish_i64(h.userdata,"botw.ready.shrine_count",shrines);
    h.publish_i64(h.userdata,"botw.ready.shrine_count.valid",shrines>=0);
  }
};
}
