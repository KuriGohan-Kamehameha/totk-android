// SPDX-License-Identifier: GPL-3.0-or-later
// Map-icon name labels (owner-approved 2026-10-18, internal notes bottom item).
// This header does NOT read guest memory itself: it matches the module's own
// already-sampled MapPlaces/MapLandmarks/MapMoving entries (computed this frame
// by botw_map_places.h/botw_map_landmarks.h/botw_map_moving.h) against the
// static catalog of native MessageID keys in botw_map_label_catalog.h, by
// position -- the same "match by xyz" technique botw_map_ui.h::name_key()
// already uses for the Map inspect popup. Spoiler gate: a catalog entry is
// only reported "shown" when the matching live entry's own `visible` flag
// (the same flag its icon uses) is true; an unmatched/undiscovered position
// publishes nothing, so the label never appears before the icon does.
//
// Per-kind label policy (owner-approved):
//   LABELLED  : place kinds 0 (town/village), 1 (stable), 4 (travel/start
//               point), 5 (misc named place), 6 (Hyrule Castle), 16 (Tech
//               Lab); shrines/towers (handled separately: see
//               botw_map_ui.h::name_key(), already covers kind 2/3);
//               Divine Beasts (moving kind 7-10, `beast` entries here).
//   ICON ONLY : shops (kind 11-15, excluded from the catalog -- verified
//               finding, see NOTES.md "what kind 5/shops turned out to be");
//               DLC Travel Medallion (kind 42, excluded -- no static
//               LocationMarker entry in base 1.6.0 data, open RE item).
#pragma once
#include "botw_key.h"
#include "botw_map_label_catalog.h"
#include "botw_map_places.h"
#include "botw_map_landmarks.h"
#include "botw_map_moving.h"
#include "botw_map_ui.h"
#include "botw_map.h"
#include "botw_inventory.h"
#include "botw_map_special.h"
namespace botw {
struct MapLabels {
  bool valid{};
  // One slot per catalog entry (botw_map_label_catalog.h order). `xyz` is the
  // MATCHED LIVE record's own position (not the static catalog position) --
  // for beasts this is where the Divine Beast currently stands, not its
  // catalog spawn reference, so the Ready-caption cone/distance below tracks
  // a moving marker correctly.
  std::array<bool,map_label_catalog.size()> shown{};
  std::array<std::array<float,3>,map_label_catalog.size()> xyz{};
};
// Horizontal (X/Z) identity only: the catalog's Y (height) comes from the already-extracted
// static dump (runs/native-Static.json), which is not guaranteed bit-identical to the live
// record's Y on this exact build/version (unlike X/Z, Y has no bearing on map-icon identity --
// two distinct LocationMarker entries never share an X/Z pair, so X/Z alone is unambiguous,
// same as the existing map_name_info matcher in botw_map_names.h, which also only compares all
// three axes with a tight epsilon for AREA names specifically because those were captured from
// the exact same live build; LocationMarker identity here instead relies on X/Z only to tolerate
// a Y mismatch between sources. Live-debugged 2026-10-18: Hyrule Castle's icon rendered
// correctly (proving the live record's X/Z matched) but its label never showed with a 3-axis
// eps=1.0 match -- root cause was exactly this Y discrepancy).
inline bool xyz_match(float ax,float /*ay*/,float az,const MapLabelEntry& e,float eps=1.0f) {
  return std::abs(ax-e.x)<eps && std::abs(az-e.z)<eps;
}
inline MapLabels map_labels(const MapPlaces& places,const MapLandmarks& landmarks,const MapMoving& moving) {
  MapLabels out{};
  if(!places.valid && !landmarks.valid && !moving.valid)return out;
  for(size_t i=0;i<map_label_catalog.size();++i){
    const auto& e=map_label_catalog[i];
    bool found=false;
    std::array<float,3> matched{e.x,e.y,e.z};
    if(e.beast){
      if(moving.valid)for(const auto& m:moving.entries)
        if(m.visible && xyz_match(m.xyz[0],m.xyz[1],m.xyz[2],e)){found=true;matched=m.xyz;break;}
    } else if(e.kind==2||e.kind==3){
      if(landmarks.valid)for(unsigned j=0;j<landmarks.count && j<landmarks.entries.size();++j){
        const auto& l=landmarks.entries[j];
        if(l.visible && l.kind==e.kind && xyz_match(l.xyz[0],l.xyz[1],l.xyz[2],e)){found=true;matched=l.xyz;break;}
      }
    } else {
      if(places.valid)for(unsigned j=0;j<places.count && j<places.entries.size();++j){
        const auto& p=places.entries[j];
        if(p.visible && p.kind==e.kind && xyz_match(p.xyz[0],p.xyz[1],p.xyz[2],e)){found=true;matched=p.xyz;break;}
      }
    }
    out.shown[i]=found;out.xyz[i]=matched;
  }
  out.valid=true;return out;
}
inline void publish_map_labels(const EdenDsmodHostApi& h,const MapLabels& labels,bool full=true,const MapViewCull* vcull=nullptr) {
  h.publish_i64(h.userdata,"botw.map.labels.valid",labels.valid);
  // Perf: full=false skips the per-entry loop off the Map page.
  // Perf round 3: vcull (Map only) additionally hides a label whose
  // own xyz (the matched live entry's position, or the catalog's static position as a fallback --
  // map_labels() above always fills one or the other) falls outside the current view+margin --
  // same "show=false" shape the runtime already treats as not-shown for an unmatched label.
  // Perf round 4: skip the publish_i64 entirely for a hidden label instead
  // of writing an explicit show=0 -- verified from the runtime source (mod_ui_widget_state.cpp's
  // GateOpen()/SnapshotNumber()): a missing show_bind key reads as "nonzero=false" -> gate closed,
  // the same outcome as an explicit 0. Each label's own show_bind is its only consumer in the
  // manifest (one show_bind per catalog entry, manifest-checked), so there is nothing else to
  // keep in sync.
  if(full)for(size_t i=0;i<labels.shown.size();++i){
    bool show=labels.valid&&labels.shown[i];
    if(show&&vcull&&!WithinMapView(labels.xyz[i][0],labels.xyz[i][2],*vcull))show=false;
    if(!show)continue;
    Key key("botw.map.labels.",unsigned(i));
    h.publish_i64(h.userdata,key(".show"),show);
  }
}

// Ready circle caption (owner-approved 2026-10-18, ITER5-BRIEF.md): "no persistent
// labels... the marker the player arrow points at (nearest labelled marker within
// a small forward cone/distance; justify the choice) or the tapped marker shows its
// name in the existing caption strip". Covers Divine Beasts too (their only name
// source, since an in-map floating label isn't possible -- see NOTES.md section 7 /
// RUNTIME-CHANGE-REQUEST.md, withdrawn in favour of this caption).
//
// Forward cone / distance: reused, not invented. `tools/apply-ready-map-rotation.py`
// already draws the Ready circle's own native-POV cone wedge at a measured ~90 deg
// total (CONE_HALF_ANGLE = 45 deg) reaching exactly to the circle's rim; the minimap
// widget (`tools/add-map-view.py`'s `ready_map`, `follow_window=900`) shows a 900
// world-unit span across its diameter, i.e. a 450-unit radius at the rim. This
// caption uses the SAME two numbers (45 deg half-angle, 450-unit radius) so "the
// marker the arrow points at" is exactly the marker inside the cone the player can
// already see highlighted on their own circle -- never a marker that's off-screen
// or behind them.
//
// Tapped override: `MapUi::selected`/`observed` (set by tapping a marker on the Map
// page's own inspect popup) persists in `State` across page navigation, so a tap
// there keeps naming that marker on the Ready page too, exactly like the Map page's
// own inspect popup already does, until cleared or a different marker is tapped.
// This only covers landmarks (shrine/tower, kind 2/3) since that's the only family
// `MapUi` tracks a tap selection for; places/beasts fall through to the automatic
// cone pick (a places/beasts tap-to-caption would need `MapUi` to track more than
// landmark selection -- out of scope here, noted as an open item).
struct ReadyCaption { bool valid{}; int64_t name_key{-1}; };
// Iteration 12: a placed Travel Medallion (MapSpecial slot 3, native kind 42, shown only while
// the native record's own activation bit is set) joins the same cone pick; its caption key is
// actor_key("Obj_WarpDLC"), mapped in the manifest to the native item name
// (ActorType/Item.msbt Obj_WarpDLC_Name, "Travel Medallion"). Default nullptr = old behaviour.
inline constexpr unsigned MedallionSlot=3;
// Approximate arrival radii (metres) for the Ready caption: inside this distance of a shown
// marker, the caption names it (like the game's own location banner on arrival); outside, the
// native area name stands. Villages/castle/beasts are large, shrines/towers/travel points small.
inline constexpr float ArrivalRadiusMedallion=15.f;
inline float arrival_radius(const MapLabelEntry& e){
  if(e.beast)return 200.f;
  switch(e.kind){case 0:return 120.f;case 6:return 250.f;case 1:case 16:return 50.f;case 2:return 40.f;case 3:case 4:return 30.f;default:return 60.f;}
}
inline ReadyCaption ready_caption(const MapLabels& labels,const MapPosition& pos,const MapUi& map_ui,const MapSpecial* specials=nullptr) {
  ReadyCaption out{};
  // Tapped landmark wins outright (same identity key the Map inspect popup itself
  // publishes via `botw.map.ui.inspect.name` -- not a new key space).
  if(map_ui.selected>=0 && map_ui.selected<int(map_ui.observed.count) && map_ui.observed.valid){
    const auto& e=map_ui.observed.entries[size_t(map_ui.selected)];
    if(e.visible){const uint32_t key=MapUi::name_key(e);if(key){out.valid=true;out.name_key=int64_t(key);return out;}}
  }
  const bool medallion=specials&&specials->valid&&specials->entries[MedallionSlot].visible&&specials->entries[MedallionSlot].kind==42;
  if((!labels.valid&&!medallion) || !pos.valid)return out;
  // Owner bug 2026-10-23 (Thor, early save on the Great Plateau): "in an empty field the area name
  // below the map is a shrine". The old rule named the nearest labelled marker up to 450 units
  // ahead in a 45-degree cone, which hid the native area name across open ground. The game itself
  // names a place only when Link ARRIVES there (its location banner); otherwise the region/area
  // name stands. So a marker now wins only inside its arrival radius (approximate per kind, world
  // units = metres), nearest first, with no heading cone; otherwise the area name shows.
  int best=-1;float best_dist=1e9f;
  const size_t candidates=labels.shown.size()+1; // last candidate = the placed Travel Medallion
  for(size_t i=0;i<candidates;++i){
    const bool is_medallion=i==labels.shown.size();
    if(is_medallion?!medallion:!(labels.valid&&labels.shown[i]))continue;
    const auto& at=is_medallion?specials->entries[MedallionSlot].xyz:labels.xyz[i];
    const float dx=at[0]-pos.x,dy=-at[2]-pos.y; // screen convention: y = -world z
    const float dist=std::sqrt(dx*dx+dy*dy);
    const float radius=is_medallion?ArrivalRadiusMedallion:arrival_radius(map_label_catalog[i]);
    if(dist>radius||dist>=best_dist)continue;
    best=int(i);best_dist=dist;
  }
  if(best>=0){out.valid=true;out.name_key=size_t(best)==labels.shown.size()?actor_key("Obj_WarpDLC"):actor_key(map_label_catalog[size_t(best)].id);}
  return out;
}
inline void publish_ready_caption(const EdenDsmodHostApi& h,const ReadyCaption& c) {
  h.publish_i64(h.userdata,"botw.ready.marker.valid",c.valid);
  h.publish_i64(h.userdata,"botw.ready.marker.name_key",c.valid?c.name_key:-1);
}
}
