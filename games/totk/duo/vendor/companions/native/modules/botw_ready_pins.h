// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "botw_key.h"
#include "botw_map_pins.h"
#include "botw_map_markers.h"
#include "botw_fx_curves.h"
#include <algorithm>
#include <chrono>
#include <cmath>
namespace botw {
// Ready circle map geometry (manifest ready_map: rect 220,64 800x800, follow_window 900 world units;
// ready-native-circle-frame rim line y 65..67 = radius 398 about the centre 620,464).
inline constexpr double ReadyCentreX=620.,ReadyCentreY=464.,ReadyRim=398.,ReadyPxPerUnit=800./900.;
inline constexpr double ReadyQuestRingHalf=86.; // half-width of the ready_mark_quest_N_ring rect (tools/i12-markers.py)
// One marker the Ready page draws itself (pins, quest destinations): canvas px of its centre.
// Inside the circle it sits at its true map position; past the rim it is pinned ON the rim at its
// true bearing, as the game's minimap does (the marker centre on the rim line).
// clamped_distance: the mark's final (post-clamp) distance from the circle centre -- always
// <= ReadyRim, and exactly ReadyRim when rim is true. Used to shrink anything drawn around the
// mark (e.g. the quest ring) so it never paints past the rim, instead of filing a runtime clip.
struct ReadyMark {bool visible{},rim{};int x{},y{};double clamped_distance{};};
// rotation: the Ready map's clockwise view turn in degrees (botw.map.view.rotation); world x grows
// east, native z grows south (the map's y is -z).
inline ReadyMark ready_mark(float player_x,float player_y,float world_x,float world_z,double rotation=0.) {
  const double ux=(double(world_x)-player_x)*ReadyPxPerUnit;
  const double uy=(double(world_z)+player_y)*ReadyPxPerUnit;
  const double r=rotation*3.14159265358979323846/180.,c=std::cos(r),sn=std::sin(r);
  const double dx=ux*c-uy*sn,dy=ux*sn+uy*c;
  const double distance=std::hypot(dx,dy);
  if(!std::isfinite(distance))return {};
  if(distance<=ReadyRim)return {true,false,int(std::lround(ReadyCentreX+dx)),int(std::lround(ReadyCentreY+dy)),distance};
  return {true,true,int(std::lround(ReadyCentreX+dx*ReadyRim/distance)),int(std::lround(ReadyCentreY+dy*ReadyRim/distance)),ReadyRim};
}
// Companion animation steps for the Ready marks, from the native layout animations
// (Layout/Common.sblarc, frames at 60 per second):
// - PaMapIconBtn_00 N_MapPin_xx: GradationFire scrolls Abstract04_1 one texture height per 120 frames
//   under the nishikawa_Alpha_007 flame mask (Loop/LoopRandom FLTS) -> 16 pre-composed flame frames;
//   P_Flash (white FL over the pin) breathes alpha 150 -> 78 -> 150 every 60 frames.
// - PaMapIconChallenge_00_Loop: P_Effect_00 (Test_G_BeamS ring) every 40 frames grows scale
//   0.25 -> 0.7 (frame 10) -> 0.8 (frame 30), alpha 0 -> 255 (frame 1), held to 20, gone at 30;
//   P_Icon_00 blue 100 -> 0 (frames 2..20) -> 100 (frame 40).
// Values only change on a 15 Hz step (a redraw of the few small mark rects, never the map).
struct ReadyAnim {
  int flame{},flash{},ring_scale{1000},ring_alpha{},quest_tint{};
  bool operator==(const ReadyAnim&) const = default;
};
inline constexpr int ReadyFlameFrames=16,ReadyAlphaLevels=8,ReadyQuestTints=4;
inline constexpr double ReadyAnimHz=15.;
inline ReadyAnim ready_anim(double seconds) {
  if(!std::isfinite(seconds) || seconds<0.)seconds=0.;
  ReadyAnim a;
  const double step=std::floor(seconds*ReadyAnimHz)/ReadyAnimHz;
  a.flame=int(std::fmod(std::floor(step*8.+1e-9),double(ReadyFlameFrames)));        // 16 frames / 2 s
  const double pulse=0.5+0.5*std::cos(2.*3.14159265358979323846*std::fmod(step,1.)); // 1 = alpha 150, 0 = 78
  a.flash=int(std::lround(pulse*(ReadyAlphaLevels-1)));
  const double f=std::fmod(step*60.+1e-9,40.);
  // Ring scale relative to its 0.8 maximum (the widget rect); an ease-out like the native hermite keys.
  const double s=f<10.?0.25+0.45*(1.-(1.-f/10.)*(1.-f/10.)):f<30.?0.7+0.1*(f-10.)/20.:0.25;
  a.ring_scale=int(std::lround(s/0.8*1000.));
  const double alpha=f<1.?f:f<20.?1.:f<30.?(30.-f)/10.:0.;
  a.ring_alpha=int(std::lround(alpha*(ReadyAlphaLevels-1)));
  const double blue=f<2.?1.:f<20.?(20.-f)/18.:(f-20.)/20.;                         // 100 -> 0 -> 100
  a.quest_tint=int(std::lround(std::clamp(blue,0.,1.)*(ReadyQuestTints-1)));
  return a;
}
// The same values from the player's own native loops (botw_fx_curves.h) at the game's 30 fps UI
// rate, on the same 15 Hz steps; any curve that is missing keeps ready_anim's value for its field.
inline ReadyAnim native_ready_anim(const fx::NativeCurves* n,double seconds) {
  ReadyAnim a=ready_anim(seconds);
  if(!n)return a;
  if(!std::isfinite(seconds) || seconds<0.)seconds=0.;
  const double f=std::floor(seconds*ReadyAnimHz)/ReadyAnimHz*fx::kUiFps;
  if(n->pins) {
    const double v=n->flame.At(f);
    a.flame=int(std::floor((v-std::floor(v))*ReadyFlameFrames+1e-6))%ReadyFlameFrames;
    a.flash=int(std::lround(std::clamp((n->flash.At(f)-78.)/72.,0.,1.)*(ReadyAlphaLevels-1)));
  }
  if(n->quest) {
    a.ring_scale=int(std::lround(n->ring_scale.At(f)/0.8*1000.));
    a.ring_alpha=int(std::lround(std::clamp(n->ring_alpha.At(f)/255.,0.,1.)*(ReadyAlphaLevels-1)));
    a.quest_tint=int(std::lround(std::clamp(n->quest_blue.At(f)/100.,0.,1.)*(ReadyQuestTints-1)));
  }
  return a;
}
// Pins (5 native colours) and quest destinations (up to 5) for the Ready circle. Animation values
// are held still while nothing is shown, so hidden marks never cause a redraw.
inline void publish_ready_marks(const EdenDsmodHostApi& h,const MapPins& pins,const QuestMapMarkers& quests,
                                const MapPosition& position,bool valid,double rotation=0.,
                                std::chrono::steady_clock::time_point now=std::chrono::steady_clock::now()) {
  bool any=false;
  auto put=[&](Key k,const ReadyMark& m) {
    any=any||m.visible;
    h.publish_i64(h.userdata,k(".visible"),m.visible);
    h.publish_i64(h.userdata,k(".rim"),m.visible&&m.rim);
    h.publish_i64(h.userdata,k(".x"),m.visible?m.x:0);h.publish_i64(h.userdata,k(".y"),m.visible?m.y:0);
  };
  const bool live=valid&&position.valid;
  for(unsigned i=0;i<5;++i)
    put(Key("botw.ready.pin.",i),live&&pins.valid&&pins.active[i]?
        ready_mark(position.x,position.y,pins.positions[i][0],pins.positions[i][2],rotation):ReadyMark{});
  std::array<ReadyMark,5> quest_marks{};
  for(unsigned i=0;i<5;++i) {
    quest_marks[i]=live&&quests.valid&&i<quests.count?
        ready_mark(position.x,position.y,quests.positions[i][0],quests.positions[i][2],rotation):ReadyMark{};
    put(Key("botw.ready.quest.mark.",i),quest_marks[i]);
  }
  const ReadyAnim a=any?native_ready_anim(fx::Curves(),std::chrono::duration<double>(now.time_since_epoch()).count()):ReadyAnim{};
  h.publish_i64(h.userdata,"botw.ready.mark.flame",a.flame);
  h.publish_i64(h.userdata,"botw.ready.mark.flash",a.flash);
  h.publish_i64(h.userdata,"botw.ready.mark.ring.scale",a.ring_scale);
  h.publish_i64(h.userdata,"botw.ready.mark.ring.alpha",a.ring_alpha);
  h.publish_i64(h.userdata,"botw.ready.mark.quest.tint",a.quest_tint);
  // Finding 3 (quest beam escapes the circle): no per-widget clip exists in this runtime, so
  // shrink the ring geometrically instead. The ring widget's own rect half-width is 72 px (144 px
  // asset, already reduced from 192 in the previous fix round); cap each mark's ring scale so
  // its outer edge never passes ReadyRim, using the mark's own clamped_distance (0 budget, i.e.
  // ring fully collapsed, exactly when the mark itself is pinned on the rim).
  for(unsigned i=0;i<5;++i) {
    const double budget_px=std::max(0.,ReadyRim-quest_marks[i].clamped_distance);
    // Iteration 12 item 6: the quest icon grew to the player arrow's 36 px (icon rect 96) and the
    // ring keeps its native ratio to it (rect 172, half-width 86), so the cap uses 86.
    const double budget_scale=std::clamp(budget_px/ReadyQuestRingHalf,0.,1.);
    const int capped=quest_marks[i].visible?int(std::lround(std::min(double(a.ring_scale),budget_scale*1000.))):a.ring_scale;
    h.publish_i64(h.userdata,Key("botw.ready.quest.mark.",i)(".ring_scale"),capped);
  }
}
}
