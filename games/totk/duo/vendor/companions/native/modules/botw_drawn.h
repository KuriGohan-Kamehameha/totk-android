// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "botw_player_equipment.h"
namespace botw {
struct DrawnState {bool valid{},weapon{},bow{};};
// Exact1.6 native IsWeaponDrawn resolves Player+BA8. The analogous Bow slot
// +BD8 was verified separately through normal draw/sheath transitions in an
// owned1.6 fixture. The verified equipment identities prevent empty slot bytes
// or a recreated Player from being mistaken for drawn equipment.
inline DrawnState drawn_state(const EdenDsmodHostApi& h) {
  const auto a=player_equipment(h);if(!a.valid)return {};
  uint8_t weapon{},bow{},weapon2{},bow2{};
  if(!read(h,a.player+0xBA8,weapon)||!read(h,a.player+0xBD8,bow)||weapon>1||bow>1)return {};
  const auto b=player_equipment(h);
  if(!b.valid||a!=b||!read(h,a.player+0xBA8,weapon2)||!read(h,a.player+0xBD8,bow2)||weapon!=weapon2||bow!=bow2)return {};
  return {true,a.slots[0].actor!=0&&weapon==0,a.slots[2].actor!=0&&bow==0};
}
}
