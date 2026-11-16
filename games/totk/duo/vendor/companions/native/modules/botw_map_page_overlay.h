// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "botw_key.h"
#include "botw_map.h"
#include "botw_map_markers.h"
#include <cmath>
#include <limits>
#include <string>
namespace botw {
// Critique item E (fix round 2): the Map page's built-in map-widget marker draws under the
// native stamps/pins (same limitation as the Ready circle's finding 1), and there is no
// off-view indicator for a quest target panned out of frame. The map widget publishes its
// last-drawn view per area as host floats "@map_view_{cx,cy,ppw,x,y,w,h}:<area>" (runtime 21+,
// mod_input.cpp's PublishMapState: "screen = rect centre + (world - c) * ppw, y up") -- read
// those instead of filing a runtime request, and draw a dedicated overlay widget the same way
// the Ready circle does.
struct MapViewRect {bool valid{};double cx{},cy{},ppw{},x{},y{},w{},h{};};
inline MapViewRect map_view_rect(const EdenDsmodHostApi& h,const std::string& area) {
  MapViewRect r;
  if(!h.get_f64)return r;
  constexpr double nan=std::numeric_limits<double>::quiet_NaN();
  r.cx=h.get_f64(h.userdata,("@map_view_cx:"+area).c_str(),nan);
  r.cy=h.get_f64(h.userdata,("@map_view_cy:"+area).c_str(),nan);
  r.ppw=h.get_f64(h.userdata,("@map_view_ppw:"+area).c_str(),nan);
  r.x=h.get_f64(h.userdata,("@map_view_x:"+area).c_str(),nan);
  r.y=h.get_f64(h.userdata,("@map_view_y:"+area).c_str(),nan);
  r.w=h.get_f64(h.userdata,("@map_view_w:"+area).c_str(),nan);
  r.h=h.get_f64(h.userdata,("@map_view_h:"+area).c_str(),nan);
  for(double v:{r.cx,r.cy,r.ppw,r.x,r.y,r.w,r.h})if(!std::isfinite(v))return {};
  if(r.ppw<=0.||r.w<=0.||r.h<=0.)return {};
  r.valid=true;return r;
}
inline void project(const MapViewRect& v,double wx,double wy,double& px,double& py) {
  px=v.x+v.w/2.+(wx-v.cx)*v.ppw;
  py=v.y+v.h/2.-(wy-v.cy)*v.ppw; // "y up" per mod_input.cpp
}
struct MapPageOverlay {
  bool player_valid{},player_on_view{};int player_x{},player_y{};
  bool quest_valid{},quest_on_view{};int quest_x{},quest_y{};double quest_bearing{};
};
// heading: companion degrees (view_heading already turns with the map on Ready; the Map page's
// own built-in marker_rotate_bind already reads botw.map.heading the same way, reused here).
inline MapPageOverlay map_page_overlay(const EdenDsmodHostApi& h,const std::string& area,
                                        const MapPosition& pos,const QuestMapMarkers& quest) {
  MapPageOverlay out;
  if(!pos.valid)return out;
  const MapViewRect v=map_view_rect(h,area);
  if(!v.valid)return out;
  double px{},py{};project(v,pos.x,pos.y,px,py);
  out.player_valid=true;out.player_x=int(std::lround(px));out.player_y=int(std::lround(py));
  out.player_on_view=px>=v.x&&px<=v.x+v.w&&py>=v.y&&py<=v.y+v.h;
  if(quest.valid&&quest.count>0) {
    const double qwx=quest.positions[0][0],qwy=-quest.positions[0][2];
    double qx{},qy{};project(v,qwx,qwy,qx,qy);
    out.quest_valid=true;
    out.quest_on_view=qx>=v.x&&qx<=v.x+v.w&&qy>=v.y&&qy<=v.y+v.h;
    if(out.quest_on_view) {
      out.quest_x=int(std::lround(qx));out.quest_y=int(std::lround(qy));
    } else {
      // Clamp to the view rect's edge (4 px margin) along the line from the view centre to the
      // target, and point an edge arrow there -- the off-view indicator the critique asked for.
      const double ccx=v.x+v.w/2.,ccy=v.y+v.h/2.;
      const double dx=qx-ccx,dy=qy-ccy;
      double scale=1e18;
      if(dx!=0.)scale=std::min(scale,(v.w/2.-4.)/std::abs(dx));
      if(dy!=0.)scale=std::min(scale,(v.h/2.-4.)/std::abs(dy));
      out.quest_x=int(std::lround(ccx+dx*scale));out.quest_y=int(std::lround(ccy+dy*scale));
      out.quest_bearing=std::atan2(dx,-dy)*180./3.14159265358979323846; // 0 = up, clockwise
    }
  }
  return out;
}
// Owner 2026-10-23: Map-page icon labels sat far from their icons when zoomed in. Labels are
// placed at a fixed WORLD offset below the icon (runtime map labels have no pixel offset), so the
// pixel gap grew with the zoom. The module publishes a zoom tier from the map widget's own scale
// (@map_view_ppw:FullHyrule); tools/mapfix2.py gives every icon label one copy per tier with the
// world offset that yields the same ~pixel gap at that tier's scale, and shows only the current
// tier's copy (group hide_bind/hide_in). Tiers are sqrt(2) apart, so the gap stays within ~19 %.
inline constexpr double MapLabelTierBasePpw=0.05;
inline constexpr int MapLabelTiers=11,MapLabelTierDefault=4;
inline int map_label_tier(double ppw) {
  if(!std::isfinite(ppw)||ppw<=0.)return MapLabelTierDefault;
  const long t=std::lround(2.0*std::log2(ppw/MapLabelTierBasePpw));
  return int(t<0?0:t>MapLabelTiers-1?MapLabelTiers-1:t);
}
inline void publish_map_label_tier(const EdenDsmodHostApi& h,const MapViewRect& v) {
  h.publish_i64(h.userdata,"botw.map.label_tier",v.valid?map_label_tier(v.ppw):MapLabelTierDefault);
}
inline void publish_map_page_overlay(const EdenDsmodHostApi& h,const char* prefix,const MapPageOverlay& o) {
  const std::string p=prefix;
  h.publish_i64(h.userdata,(p+".player.valid").c_str(),o.player_valid);
  h.publish_i64(h.userdata,(p+".player.on_view").c_str(),o.player_valid&&o.player_on_view);
  h.publish_i64(h.userdata,(p+".player.x").c_str(),o.player_x);
  h.publish_i64(h.userdata,(p+".player.y").c_str(),o.player_y);
  h.publish_i64(h.userdata,(p+".quest.valid").c_str(),o.quest_valid);
  h.publish_i64(h.userdata,(p+".quest.edge").c_str(),o.quest_valid&&!o.quest_on_view);
  h.publish_i64(h.userdata,(p+".quest.x").c_str(),o.quest_x);
  h.publish_i64(h.userdata,(p+".quest.y").c_str(),o.quest_y);
  if(h.publish_f64)h.publish_f64(h.userdata,(p+".quest.bearing").c_str(),o.quest_bearing);
  else h.publish_i64(h.userdata,(p+".quest.bearing").c_str(),int64_t(std::lround(o.quest_bearing)));
}
} // namespace botw
