// SPDX-License-Identifier: GPL-3.0-or-later
// Action dispatch for 01007EF00011E000 (companion button/tap handlers): heal_action (the Heal
// Advisor card's "eat" rows) and handle_action (everything else -- recipes/warp/map/cards/
// quick-picker/inventory/quest/secondary-weapon/touch dispatch). Textual split only (included
// into the module's single translation unit, same as before) -- extracted 2026-10-19 refactor
// round from the former monolithic 01007EF00011E000.cpp; State and all botw::* helpers come
// from the headers the including .cpp already pulled in. No behaviour change.
#pragma once

bool heal_action(State& s,const char* action,int64_t argument) {
  if(std::strcmp(action,"heal_eat")!=0 || s.cards.active!=botw::HealAdvisor::CardId)return false;
  if(!s.bridge.scope() || botw::context(s.bridge.host)!=botw::UiContext::World)return false;
  const auto* row=s.heal.row(argument);
  if(!row || s.bridge.pending || !s.data.valid || !botw::HealAdvisor::still_matches(s.bridge.host,s.data,*row))return false;
  const auto attached=botw::player_equipment(s.bridge.host);
  const auto health=attached.valid ? botw::player_health(s.bridge.host,attached.info,attached.player) : botw::PlayerHealth{};
  if(health.missing()<=0){s.bridge.status="Hearts full";return false;}
  if(!s.bridge.request_consume(row->address,s.data,true)){if(!s.bridge.pending)s.bridge.status="Eat unavailable";return false;}
  s.eat_life_before=health.life;s.eat_expect_heal=row->heal>0||row->full;s.eat_at=std::chrono::steady_clock::now();
  s.next={};return true;
}
bool handle_action(State& s,const char* action,int64_t argument) {
  if(!action)return false;
  const char* active_page=s.bridge.host.get_text?s.bridge.host.get_text(s.bridge.host.userdata,"__page"):nullptr;
  if(std::strcmp(action,"ready_radial_tap")==0) {
    if(!active_page||std::strcmp(active_page,"ready")!=0||!s.bridge.scope()||
       botw::context(s.bridge.host)!=botw::UiContext::World)return false;
    const auto hit=botw::ready_radial_hit(s.cards.active,argument);
    if(hit.kind==botw::ReadyRadialHit::Close){s.cards.close(s.cards.active);return true;}
    if(hit.kind==botw::ReadyRadialHit::Food)return heal_action(s,"heal_eat",hit.row);
    if(hit.kind==botw::ReadyRadialHit::Clothing)
      return handle_action(s,hit.row==3?"temperature_card_eat":"temperature_card_equip",hit.row);
    return false; // runtime already consumed this surface; never fall through to map/gear.
  }
  if(std::strcmp(action,"recipes_hold")==0) {
    if(!active_page||std::strcmp(active_page,"recipes")!=0||!s.bridge.healthy||!s.bridge.hold_supported||s.bridge.pending||!s.recipe_hold.empty()||botw::context(s.bridge.host)!=botw::UiContext::World)return false;
    const auto tray=botw::held_tray(s.bridge.host);const auto carry=botw::held_carry(s.bridge.host);
    if(!tray.valid||!carry.valid||tray.count||carry.active())return false;
    auto plan=s.recipes.selected_hold_actors(s.data);if(plan.empty()||plan.size()>5)return false;
    s.recipe_hold=std::move(plan);s.recipe_hold_index=0;s.recipe_hold_epoch=s.bridge.epoch;s.recipe_hold_at=std::chrono::steady_clock::now();s.inventory.category=4;return true;
  }
  if(std::strncmp(action,"recipes_",8)==0)return s.recipes.action(action,argument,s.data);
  const bool map_ui_allowed=active_page&&std::strcmp(active_page,"map")==0&&s.bridge.healthy&&botw::context(s.bridge.host)==botw::UiContext::World;
  if(std::strncmp(action,"warp_",5)==0)return s.travel.action(action,s.bridge.host,active_page,s.map_ui,s.bridge,std::chrono::steady_clock::now());
  if(s.map_ui.action(action,argument,map_ui_allowed))return true;
  // Minimap helper cards. card_toggle 1/2/3 opens/closes that card;
  // compass (card 1) always opens (the dial's own proximity reading stays unconditional);
  // temperature (card 2) and food (card 3) only open while they actually have something to
  // show (a glow, or an offered low-health page) -- a tap with neither does nothing.
  if(std::strcmp(action,"card_toggle")==0) {
    if(argument<1 || argument>3 || !s.bridge.scope())return false;
    // card 2 (temperature) only opens with an active glow (tempcard NOTES.md: "card_toggle 2
    // refuses to open the card without a glow"). Card 1 (compass) always opens -- the dial's
    // own proximity reading stays unconditional. Card 3 (food) also always opens -- its content
    // gate lives in heal_eat (food.page.available), matching the right-pane page's own tap rule.
    if(argument==2 && s.cards.active!=2 && s.temp_card.shown==botw::TempDanger::None)return false;
    // Owner bug 2026-10-23: the shrine compass (card 1) follows the game's own Sheikah Sensor --
    // it opens only once the sensor is obtained and switched on (native map+0x38AC, the same
    // state the Ready sensor dial greys out on); before that the game offers no shrine guidance.
    if(argument==1 && s.cards.active!=1 && !botw::sensor_available(s.sensor))return false;
    return s.cards.toggle(argument);
  }
  if(std::strcmp(action,"card_close_any")==0){if(!s.cards.active)return false;s.cards.close(s.cards.active);return true;}
  if(std::strcmp(action,"temperature_card_equip")==0 || std::strcmp(action,"temperature_card_eat")==0) {
    if(s.cards.active!=2 || !s.bridge.scope() || s.context!=botw::UiContext::World)return false;
    const int row=std::strcmp(action,"temperature_card_eat")==0 ? 3 : int(argument);
    if(row<1 || row>3)return false;
    const bool ok=s.temp_card.request(row,s.bridge.epoch);
    if(ok)s.next={};
    return ok;
  }
  if(std::strncmp(action,"heal_",5)==0)return heal_action(s,action,argument);
  if(std::strncmp(action,"tracker_",8)==0)return s.trackers.action(action,argument);
  if(std::strcmp(action,"ready_left_pane")==0 || std::strcmp(action,"ready_right_pane")==0) {
    if(!s.bridge.scope() || (argument!=1 && argument!=-1))return false;
    if(std::strcmp(action,"ready_right_pane")==0)return s.heal.page.step(s.ready_panes,int(argument));
    return s.ready_panes.step(false,int(argument));
  }
  if(std::strcmp(action,"map_tool")==0 || std::strcmp(action,"map_tap")==0 || std::strcmp(action,"map_marker")==0 || std::strcmp(action,"map_delete")==0) {
    if(!s.bridge.map_scope() || !s.bridge.healthy || !s.bridge.map_supported || s.bridge.pending || botw::context(s.bridge.host)!=botw::UiContext::World)return false;
    if(std::strcmp(action,"map_tool")==0) {
      if(argument< -1 || argument>13)return false;
      // Choosing the armed symbol again stops placing (the old Pan button's job).
      s.map_palette=argument>=0&&argument==s.map_palette?-1:int(argument);s.map_selected=-1;s.map_tap_deferred=false;s.map_marker_deferred=false;s.map_delete_deferred=false;return true;
    }
    if(std::strcmp(action,"map_delete")==0) {
      const auto fresh=botw::map_edit_snapshot(s.bridge.host);
      if(!fresh.valid || s.map_selected<0 || fresh.markers[size_t(s.map_selected)]!=s.map_selected_marker)return false;
      s.map_action_epoch=s.bridge.epoch;s.map_action_at=std::chrono::steady_clock::now();
      s.map_delete_deferred=true;s.map_tap_deferred=false;s.map_marker_deferred=false;return true;
    }
    s.map_action_epoch=s.bridge.epoch;s.map_action_at=std::chrono::steady_clock::now();s.map_delete_deferred=false;
    if(std::strcmp(action,"map_tap")==0) {
      if(s.map_palette<0){s.map_selected=-1;s.map_marker_deferred=false;s.map_tap_deferred=false;return true;}
      s.map_deferred_palette=s.map_palette;
      s.map_tap_before=s.map_last_tap_seq;s.map_tap_deferred=true;return true;
    }
    if(!s.saved_icons.valid || argument<0 || argument>=200)return false;
    s.map_clicked_icon=s.saved_icons.icons[size_t(argument)];
    if(!((s.map_clicked_icon.kind>=27 && s.map_clicked_icon.kind<=35) || (s.map_clicked_icon.kind>=37 && s.map_clicked_icon.kind<=41)))return false;
    s.map_palette=-1;s.map_tap_deferred=false;
    if(s.map_selected>=0 && s.map_selected_marker.kind==s.map_clicked_icon.kind && s.map_selected_marker.xyz==s.map_clicked_icon.xyz){s.map_selected=-1;return true;}
    s.map_marker_deferred=true;return true;
  }
  // 2026-10-18 drag shortcuts: a grid tile dropped on the Hold tray carries its grid index; resolve
  // it to the same fresh hold token the long-press path uses, then begin (empty tray) or add.
  const bool drag_hold=std::strcmp(action,"inventory_drag_hold")==0;
  if(drag_hold) {
    if(argument<0||uint64_t(argument)>=s.hold_targets.cells.size()){s.hold_decline=0x1000;return false;}
    argument=s.hold_targets.cells[size_t(argument)].token;
  }
  if(drag_hold || std::strcmp(action,"inventory_hold_begin")==0 || std::strcmp(action,"inventory_hold_add")==0) {
    // Fix round A: one immediate re-read when the double-captured tray is torn by a native update.
    auto tray=botw::held_tray(s.bridge.host);if(!tray.valid)tray=botw::held_tray(s.bridge.host);
    const bool beginning=drag_hold ? (tray.valid&&tray.count==0) : std::strcmp(action,"inventory_hold_begin")==0;
    if(tray.valid&&tray.count>=5) {
      // Cap feedback: the tray flashes for a moment (botw.inventory.hold.refused).
      s.hold_refused_until=std::chrono::steady_clock::now()+std::chrono::milliseconds(900);
      s.hold_decline=0x8000;return false;
    }
    const auto* target=s.hold_targets.target(argument,s.bridge.epoch,std::chrono::steady_clock::now());
    s.hold_decline=!target?0x2000:!s.bridge.inventory_scope()?0x4000:!tray.valid?0x6000:(beginning?tray.count!=0:tray.count==0)?0x7000:0;
    if(s.hold_decline)return false;
    if(!s.bridge.request_hold(target->address,s.data)){s.hold_decline=0x10000000u|s.bridge.hold_refusal;return false;}
    s.inventory.selected=target->address;s.hold_targets.clear();s.next={};s.hold_next={};return true;
  }
  // 2026-10-18 drag shortcut: a grid tile dropped on the action slot selects that item and runs
  // the request the slot shows for it (Equip / Eat / Remove) -- the same as tap-select + tap.
  if(std::strcmp(action,"inventory_drag_use")==0) {
    if(!s.inventory.choose(s.data,argument))return false;
    const botw::Item* item{};for(const auto& i:s.data.items)if(i.address==s.inventory.selected)item=&i;
    if(!item)return false;
    switch(inventory_slot(s,s.bridge.host,*item)) {
      case InventorySlot::Consume:
        if(!s.bridge.request_consume(s.inventory.selected,s.data)){if(!s.bridge.pending)s.bridge.status="Eat unavailable";return false;}
        s.next={};return true;
      case InventorySlot::Remove:
      case InventorySlot::Equip: {
        const bool remove=inventory_slot(s,s.bridge.host,*item)==InventorySlot::Remove;
        if(!s.bridge.request_inventory(s.inventory.selected,s.data,remove)){if(!s.bridge.pending)s.bridge.status=remove?"Remove unavailable":"Equip unavailable";return false;}
        s.next={};return true;
      }
      default: return false;
    }
  }
  if(std::strcmp(action,"inventory_put_away")==0) {
    if(!s.bridge.request_hold(0,s.data,true)){s.hold_decline=0x20000000u|s.bridge.hold_refusal;return false;}
    s.hold_targets.clear();s.next={};s.hold_next={};return true;
  }
  // Item 8d, iteration 8 round 2 (2026-10-18): tap a single occupied tray box -> remove just
  // that one unit. No native per-slot release exists (RE this round); compose it from the two
  // existing native calls -- a bulk put-away immediately, then re-hold every OTHER occupied
  // slot's item (in native slot order, botw::hold_unpick_keep) one at a time as the tick-driven
  // sequence above confirms each step. Refuses (same as every other hold action) while a
  // request or another sequence is already in flight -- never stacks two sequences.
  if(std::strcmp(action,"inventory_hold_unpick")==0) {
    if(argument<0||argument>=5||!s.hold_unpick.empty())return false;
    const auto tray=botw::held_tray(s.bridge.host);
    const auto keep=botw::hold_unpick_keep(tray,unsigned(argument));
    if(!tray.valid||!tray.slots[size_t(argument)].address)return false;
    if(!s.bridge.request_hold(0,s.data,true)){s.hold_decline=0x30000000u|s.bridge.hold_refusal;return false;}
    s.hold_unpick=keep;s.hold_unpick_index=0;s.hold_unpick_put_away_done=false;
    s.hold_unpick_epoch=s.bridge.epoch;s.hold_unpick_at=std::chrono::steady_clock::now();
    s.hold_targets.clear();s.next={};s.hold_next={};return true;
  }
  if(std::strcmp(action,"inventory_consume")==0) {
    if(!s.bridge.request_consume(s.inventory.selected,s.data)) {
      if(!s.bridge.pending)s.bridge.status="Eat unavailable";
      return false;
    }
    s.next={};return true;
  }
  if(std::strcmp(action,"inventory_sort")==0) {
    if(!s.data.valid || botw::inventory_items(s.data,s.inventory.category).empty() || !s.bridge.request_sort(s.inventory.category))return false;
    s.next={};return true;
  }
  if(std::strcmp(action,"inventory_equip")==0 || std::strcmp(action,"inventory_remove")==0) {
    const bool remove=std::strcmp(action,"inventory_remove")==0;
    if(!s.bridge.request_inventory(s.inventory.selected,s.data,remove)) {
      if(!s.bridge.pending)s.bridge.status=remove ? "Remove unavailable" : "Equip unavailable";
      return false;
    }
    s.next={};return true;
  }
  if(std::strcmp(action,"quest_activate")==0) {
    if(s.quest_deferred)return false;
    if(!s.bridge.request_quest(s.quest_selected,s.quests)) {
      if(s.bridge.quest_refusal && !(s.bridge.quest_refusal&~384u) &&
         std::chrono::steady_clock::now()-s.bridge.sampled<std::chrono::milliseconds(250) &&
         std::chrono::steady_clock::now()-s.bridge.heartbeat_at<s.bridge.health_window()) {
        const botw::Quest* target{};for(const auto& q:s.quests.entries)if(q.address==s.quest_selected)target=&q;
        if(target && target->state==1 && !target->active && !(target->dlc&6u)) {
          s.quest_target=s.quest_selected;s.quest_display=s.quests;s.quest_epoch=s.bridge.epoch;
          s.quest_action_at=std::chrono::steady_clock::now();s.quest_deferred=true;s.bridge.status="Quest selection queued";return true;
        }
      }
      if(!s.bridge.pending)s.bridge.status="Quest selection unavailable";
      return false;
    }
    s.quest_next={};return true;
  }
  if(std::strcmp(action,"quest_category")==0) {
    if(!s.quests.valid||argument<0||argument>2)return false;
    s.quest_deferred=false;s.quest_category=int32_t(argument);s.quest_selected=0;return true;
  }
  if(std::strcmp(action,"quest_choose")==0) {
    if(!s.quests.valid||argument<0)return false;
    const auto journal=quest_journal(s);
    if(uint64_t(argument)>=journal.size())return false;
    s.quest_deferred=false;s.quest_selected=journal[size_t(argument)]->address;return true;
  }
  if(std::strcmp(action,"quest_pane")==0) {
    if(argument!=-1&&argument!=1)return false;
    s.quest_pane=(s.quest_pane+int32_t(argument)+4)%4;return true;
  }
  if(std::strcmp(action,"inventory_category")==0) {
    if(argument<0 || argument>=7 || !s.data.valid) return false;
    s.hold_targets.clear();s.inventory.category=int(argument);s.inventory.selected=0;s.inventory.update(s.data);s.status_at={};return true;
  }
  if(std::strcmp(action,"inventory_choose")==0)
    return s.inventory.choose(s.data,argument);
  if(std::strcmp(action,"secondary_confirm")==0) {
    // Iteration 8 item 1: the grid's None cell (items[0], inserted whenever touch_gear.has_none)
    // is selectable through the same generic "select" action as any real item, so secondary_focus
    // can legitimately already equal the None sentinel {address 0, empty actor} here. Committing
    // that selection has no native write -- defer to the existing secondary_none action instead of
    // calling pin(), which would correctly fail to find address 0 in the fresh pouch.
    if(s.secondary_focus.address==0 && s.secondary_focus.actor.empty())return handle_action(s,"secondary_none",0);
    const auto native=botw::native_quick(s.bridge.host);
    if(!s.secondary_picker || s.touch_kind!=botw::QuickKind::Weapon || s.touch_epoch!=s.bridge.epoch || !s.bridge.host.get_i64 || s.bridge.host.get_i64(s.bridge.host.userdata,"__mailbox_epoch",0)!=s.touch_epoch ||
       !s.bridge.scope() || !s.bridge.healthy || s.bridge.pending || botw::context(s.bridge.host)!=botw::UiContext::World ||
       !native.valid || native.kind!=botw::QuickKind::None)return false;
    const auto fresh=botw::pouch(s.bridge.host);
    if(botw::context(s.bridge.host)!=botw::UiContext::World || !s.recent[0].pin(fresh,s.secondary_focus,0))return false;
    s.secondary_none=false;
    s.touch_kind=botw::QuickKind::None;s.touch_gear={};s.secondary_picker=false;s.secondary_focus={};s.quick={};s.gear={};
    s.bridge.status="Secondary weapon set";return true;
  }
  if(std::strcmp(action,"secondary_none")==0) {
    // Clears the secondary choice (teardrop goes to the native empty icon). No native
    // write happens here -- this only edits companion-side state, so it needs none of
    // secondary_confirm's pin() freshness, only the same "picker genuinely open right
    // now" guard.
    const auto none_native=botw::native_quick(s.bridge.host);
    if(!s.secondary_picker || s.touch_kind!=botw::QuickKind::Weapon || s.touch_epoch!=s.bridge.epoch || !s.bridge.host.get_i64 || s.bridge.host.get_i64(s.bridge.host.userdata,"__mailbox_epoch",0)!=s.touch_epoch ||
       !s.bridge.scope() || !s.bridge.healthy || s.bridge.pending || botw::context(s.bridge.host)!=botw::UiContext::World ||
       !none_native.valid || none_native.kind!=botw::QuickKind::None)return false;
    s.secondary_none=true;s.recent[0].pinned={};
    s.touch_kind=botw::QuickKind::None;s.touch_gear={};s.secondary_picker=false;s.secondary_focus={};s.quick={};s.gear={};
    s.bridge.status="No secondary weapon";return true;
  }
  if(std::strcmp(action,"quick_recent")==0) {
    if(argument<0 || argument>1 || s.recent_deferred || s.touch_select_deferred || s.touch_kind!=botw::QuickKind::None || s.bridge.pending)return false;
    const auto native=botw::native_quick(s.bridge.host);
    // Iteration 12 item 3 (owner 2026-10-22), weapon side only. An empty secondary (None, or no
    // valid backup) no longer blocks the flick: the primary goes to the secondary through the
    // native Remove (primary becomes empty hand). Swapping a secondary into an EMPTY primary leaves
    // the secondary empty. Both reuse the existing native Equip/Remove requests; the secondary
    // bookkeeping is applied only when the native result is observed (see swap_promote/demote).
    const auto* item=(argument==0 && s.secondary_none) ? nullptr : s.recent[size_t(argument)].target(s.data,int(argument));
    const botw::Item* primary{};
    if(argument==0 && !item)if(const auto* p=s.recent[0].find(s.data,s.recent[0].current,0);p && p->equipped)primary=p;
    if((!item && !primary) || (primary && !s.bridge.remove_supported) || !s.recent_actor_valid[size_t(argument)] || !s.bridge.scope() || !s.bridge.healthy || !s.bridge.inventory_supported || botw::context(s.bridge.host)!=botw::UiContext::World || !native.valid || native.kind!=botw::QuickKind::None)return false;
    s.swap_promote={};s.swap_demote={};s.swap_at=std::chrono::steady_clock::now();
    if(primary){s.swap_demote={primary->address,primary->actor};defer_recent(s,*primary,true);return true;}
    if(argument==0) {
      bool empty_primary=true;
      for(const auto& i:s.data.items)if(i.type==0 && i.equipped)empty_primary=false;
      if(empty_primary)s.swap_promote={item->address,item->actor};
    }
    defer_recent(s,*item);return true;
  }
  if(std::strcmp(action,"arrow_cycle")==0) {
    // Ready arrow circle: next/previous owned arrow stack through the verified native arrow equip (slot 6).
    if((argument!=1 && argument!=-1) || s.recent_deferred || s.touch_select_deferred || s.touch_kind!=botw::QuickKind::None || s.bridge.pending || !s.bridge.arrows_supported)return false;
    const auto native=botw::native_quick(s.bridge.host);
    const auto* item=botw::arrow_step(s.data,int(argument));
    if(!item || !s.bridge.scope() || !s.bridge.healthy || !s.bridge.inventory_supported || botw::context(s.bridge.host)!=botw::UiContext::World || !native.valid || native.kind!=botw::QuickKind::None)return false;
    defer_recent(s,*item);return true;
  }
  if(std::strcmp(action,"quick_close")==0) {
    if(s.touch_kind==botw::QuickKind::None)return false;
    s.touch_kind=botw::QuickKind::None;s.touch_gear={};s.touch_select_deferred=false;s.gear={};s.quick={};return true;
  }
  const bool opening_secondary=std::strcmp(action,"secondary_open")==0;
  if (opening_secondary || std::strcmp(action,"quick_help")==0) {
    if(opening_secondary)argument=0;
    constexpr const char* hints[]={"Hold right to open Weapons", "Hold left to open Shields",
      "Hold right while aiming to open Bows", "Hold up to open Runes"};
    if (argument<0 || argument>4 || s.recent_deferred || (argument==4 && !s.bridge.arrows_supported)) return false;
    const auto native=botw::native_quick(s.bridge.host);
    if(!s.bridge.scope() || !s.bridge.healthy || !s.bridge.inventory_supported || s.bridge.pending ||
       botw::context(s.bridge.host)!=botw::UiContext::World || !native.valid || native.kind!=botw::QuickKind::None || (!s.data.valid && argument!=3))return false;
    if(argument==3) {
      if(!s.bridge.world_rune_supported){s.ready_hint=hints[argument];return true;}
      s.touch_runes=botw::world_runes(s.bridge.host);if(!s.touch_runes.valid)return false;
    }
    s.touch_kind=argument==0 ? botw::QuickKind::Weapon : argument==1 ? botw::QuickKind::Shield : argument==2 ? botw::QuickKind::Bow : argument==3 ? botw::QuickKind::Rune : botw::QuickKind::Arrow;
    // Iteration 8 item 1 (coordinator follow-up, found live): a default-constructed Identity
    // ({address:0, actor:""}) is indistinguishable from the synthetic None entry's own identity
    // (also address 0, empty actor) -- opening a has_none picker fresh made the "already have a
    // focus" scan below match None immediately (coincidentally "finding" it at index 0) instead
    // of falling through to the "no focus yet, default to the equipped item" branch, so the
    // picker opened with None highlighted (and the button reading "Unequip") instead of the
    // actually-equipped item. UINT64_MAX can never be a real pouch address or the None
    // sentinel's address (0), so it never accidentally matches either.
    s.secondary_picker=opening_secondary;s.secondary_focus={};s.touch_focus={UINT64_MAX,""};s.touch_rune_focus=-1;
    if(opening_secondary)if(const auto* backup=s.recent[0].target(s.data,0))s.secondary_focus={backup->address,backup->actor};
    s.touch_epoch=s.bridge.epoch;s.touch_select_deferred=false;s.quick={};s.ready_hint.clear();return true;
  }
  // 2026-10-18 drag shortcut: a picker tile dropped on the Equip bar inspects that tile and then
  // confirms exactly as tapping the bar would (Equip / Equip Rune / Set secondary).
  if(std::strcmp(action,"quick_drag_confirm")==0) {
    if(s.touch_kind==botw::QuickKind::None||!handle_action(s,"select",argument))return false;
    return handle_action(s,s.secondary_picker?"secondary_confirm":"touch_confirm",0);
  }
  if(std::strcmp(action,"select")==0 && s.touch_kind!=botw::QuickKind::None) {
    const auto native=botw::native_quick(s.bridge.host);
    if(s.touch_epoch!=s.bridge.epoch||s.bridge.pending||s.touch_select_deferred||!s.bridge.scope()||
      botw::context(s.bridge.host)!=botw::UiContext::World||!native.valid||native.kind!=botw::QuickKind::None)return false;
    if(s.touch_kind==botw::QuickKind::Rune) {
      if(!s.touch_runes.valid||argument<0||argument>=s.touch_runes.list.count)return false;
      s.touch_rune_focus=s.touch_runes.list.ids[size_t(argument)];return true;
    }
    if(!s.touch_gear.valid||argument<0||size_t(argument)>=s.touch_gear.items.size())return false;
    const auto& item=s.touch_gear.items[size_t(argument)];
    if(s.secondary_picker)s.secondary_focus={item.address,item.actor};else s.touch_focus={item.address,item.actor};
    return true;
  }
  if(std::strcmp(action,"touch_confirm")==0 && s.touch_kind!=botw::QuickKind::None) {
    uint32_t guest_epoch{};
    if(s.secondary_picker||s.touch_epoch!=s.bridge.epoch||s.bridge.session_epoch()!=s.bridge.epoch||
      !s.bridge.load(4092,guest_epoch)||guest_epoch!=s.bridge.epoch)return false;
    argument=-1;
    if(s.touch_kind==botw::QuickKind::Rune) {
      for(int i=0;i<s.touch_runes.list.count;++i)if(s.touch_runes.list.ids[size_t(i)]==s.touch_rune_focus)argument=i;
    } else for(size_t i=0;i<s.touch_gear.items.size();++i) {
      const auto& item=s.touch_gear.items[i];if(item.address==s.touch_focus.address&&item.actor==s.touch_focus.actor)argument=int64_t(i);
    }
    if(s.touch_kind==botw::QuickKind::Rune) {
      const auto native=botw::native_quick(s.bridge.host);
      if(!s.touch_runes.valid || s.touch_epoch!=s.bridge.epoch || argument<0 || argument>=s.touch_runes.list.count ||
         !s.bridge.scope() || botw::context(s.bridge.host)!=botw::UiContext::World || !native.valid || native.kind!=botw::QuickKind::None || s.bridge.pending || s.touch_select_deferred)return false;
      const int32_t id=s.touch_runes.list.ids[size_t(argument)];
      if(id==s.touch_runes.current){s.touch_kind=botw::QuickKind::None;s.quick={};return true;}
      s.touch_rune_id=id;s.touch_rune_display=s.touch_runes;s.touch_action_at=std::chrono::steady_clock::now();s.touch_select_deferred=true;return true;
    }
    if(!s.touch_gear.valid || s.touch_epoch!=s.bridge.epoch || argument<0 || size_t(argument)>=s.touch_gear.items.size())return false;
    const auto& item=s.touch_gear.items[size_t(argument)];
    const auto native=botw::native_quick(s.bridge.host);
    if(!s.bridge.scope() || botw::context(s.bridge.host)!=botw::UiContext::World || !native.valid || native.kind!=botw::QuickKind::None)return false;
    if(s.secondary_picker) {
      if(s.bridge.pending || s.touch_select_deferred || item.type!=0 || item.value<=0)return false;
      s.secondary_focus={item.address,item.actor};return true;
    }
    // Iteration 8 item 1: argument==0 of a has_none grid is the synthetic None entry (empty
    // actor/address inserted above) -- confirming it means "unequip", reusing the same native
    // route Inventory's own Remove button uses (INVENTORY-REMOVE.md's DUO9 kind7), never a new
    // guest action. Secondary's None is handled separately by secondary_confirm, not here (this
    // whole handler already refuses when s.secondary_picker is set, above).
    if(s.touch_gear.has_none && argument==0) {
      if(s.bridge.pending || s.touch_select_deferred)return false;
      const botw::Item* equipped=nullptr;
      for(size_t i=1;i<s.touch_gear.items.size();++i)if(s.touch_gear.items[i].equipped){equipped=&s.touch_gear.items[i];break;}
      if(!equipped) {
        // Nothing of this kind is equipped already -- same no-request close as tapping an
        // already-equipped item.
        s.touch_kind=botw::QuickKind::None;s.touch_gear={};s.touch_select_deferred=false;s.quick={};s.gear={};return true;
      }
      s.touch_target=equipped->address;s.touch_display=s.data;s.touch_remove=true;
      s.touch_action_at=std::chrono::steady_clock::now();s.touch_select_deferred=true;return true;
    }
    if(!item.equipped) {
      if(s.bridge.pending || s.touch_select_deferred)return false;
      s.touch_remove=false;
      s.touch_target=item.address;s.touch_display=s.data;s.touch_action_at=std::chrono::steady_clock::now();s.touch_select_deferred=true;return true;
    }
    s.touch_kind=botw::QuickKind::None;s.touch_gear={};s.touch_select_deferred=false;s.quick={};s.gear={};return true;
  }
  if (std::strcmp(action,"select") || !s.quick.armed) return false;
  // Owner decision 2026-10-19 (held None cell): s.gear carries a display-only synthetic None at
  // items[0] when has_none (inserted above, held branch only) -- s.bridge.request()'s own
  // anti-replay identity check re-reads the native wheel's array fresh and compares it item-for-
  // item against whatever snapshot is passed in, so that snapshot must be the EXACT pre-
  // insertion shape, never the None-augmented one, or every real selection would start failing.
  if(s.gear.has_none) {
    s.held_focus_none=false;
    botw::GearSnapshot raw=s.gear;raw.items.erase(raw.items.begin());raw.has_none=false;
    --raw.highlighted;
    --raw.initial;
    if (!s.bridge.request(argument-1,s.quick.shown,raw,s.runes)) return false;
    return true;
  }
  if (!s.bridge.request(argument,s.quick.shown,s.gear,s.runes)) return false;
  // A held tap moves the game's cursor only. Keep following it until the game
  // handles the physical direction's release and commits the selected item.
  return true;
}
