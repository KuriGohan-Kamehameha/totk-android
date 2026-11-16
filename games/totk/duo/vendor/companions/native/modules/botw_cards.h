// SPDX-License-Identifier: GPL-3.0-or-later
// Shared minimap helper-card frame (iteration-6 integration). One card at a
// time inside the Ready circle: 1 shrine compass, 2 temperature clothing, 3 low-health food. The
// map dims behind whichever card is open; tapping the dim (or the same trigger again) closes it.
// Superseded stand-ins: an earlier temperature-card frame (CardFrame, same shape -- this
// file is that one, promoted) and an earlier food-card stand-in (CardStandin,
// dropped). Card-owning features publish only their own content keys; this header owns
// botw.card.active, botw.card.any_open and the card_toggle / card_close_any actions' state.
#pragma once
#include "core/mods/dsmod_module_abi.h"
#include <cstdint>
namespace botw {
struct CardFrame {
  int active{};
  // card_toggle <n>, n in 1..3: opens if closed/another card is open, closes if n is already open.
  bool toggle(int64_t n){if(n<1||n>3)return false;active=active==int(n)?0:int(n);return true;}
  // A card's own close_request (compass target discovered/all found, temperature danger gone,
  // low health recovered) -- closes only that card, a no-op if another card is the one showing.
  void close(int n){if(active==n)active=0;}
  // Shared gate, applied once per sample: Ready page only, no event/menu dim, not inside a shrine.
  void gate(bool allowed){if(!allowed)active=0;}
  void publish(const EdenDsmodHostApi& h) const {
    h.publish_i64(h.userdata,"botw.card.active",active);
    h.publish_i64(h.userdata,"botw.card.any_open",active!=0);
  }
};
} // namespace botw
