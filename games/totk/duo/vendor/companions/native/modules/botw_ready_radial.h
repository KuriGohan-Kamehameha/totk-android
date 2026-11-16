// SPDX-License-Identifier: GPL-3.0-or-later
// Runtime22 $tap gives packed widget-local signed x/y. The manifest surface is
// [220,64,800,800]; reject its corners instead of exposing rectangular sectors.
#pragma once
#include <cmath>
#include <cstdint>
#include <initializer_list>
namespace botw {
struct ReadyRadialHit {
  enum Kind { Blocked, Close, Food, Clothing } kind{Blocked};
  int row{-1};
};
inline ReadyRadialHit ready_radial_hit(int card,int64_t payload) {
  if(card!=2 && card!=3)return {};
  const auto bits=static_cast<uint64_t>(payload);
  const int64_t x=static_cast<int32_t>(bits>>32),y=static_cast<int32_t>(bits&0xffffffffu);
  if(x<0||y<0||x>=800||y>=800)return {};
  const int64_t dx=x-400,dy=y-400,r2=dx*dx+dy*dy;
  if(r2>400*400)return {};
  if(r2<=104*104)return {ReadyRadialHit::Close,-1};
  if(card==3) {
    if(std::abs(dx)<=2 || std::abs(dy)<=2)return {};
    return {ReadyRadialHit::Food,dy<0?(dx<0?0:1):(dx<0?2:3)};
  }
  // Thermal boundaries are rays at30,150,270deg; guard the2px drawn seam.
  constexpr double pi=3.14159265358979323846;
  for(double a:{pi/6,5*pi/6,3*pi/2}) {
    const double ux=std::cos(a),uy=std::sin(a);
    if(dx*ux+dy*uy>=0 && std::abs(dx*uy-dy*ux)<=2.0)return {};
  }
  const double a=std::atan2(double(dy),double(dx))*180/pi;
  return {ReadyRadialHit::Clothing,a>=30 && a<=150?3:(a>150 || a< -90?1:2)};
}
} // namespace botw
