// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "botw_msbt_reflow.h"
#include "botw_text_measure.h"
#include <algorithm>
#include <string>
#include <unordered_map>

namespace botw {
struct DetailLine {
  int icon_x{},stat_x{},modifier_x{},bonus_x{},end{};
  bool fits{};
};

// One baseline, native alpha-cropped 25px icon ink and scale5 (25px cap).
// stat is already the native effective power: never add the modifier again.
inline DetailLine detail_line(int left,int width,int stat,std::string_view label,
                              uint32_t bonus,bool numeric) {
  DetailLine p;p.icon_x=left;p.stat_x=left+32;
  const int sw=MeasureTextPx(std::to_string(std::max(0,stat)),5);
  const int lw=MeasureTextPx(label,5),bw=numeric?MeasureTextPx(std::to_string(bonus),5):0;
  // Ordinary data has 12px semantic gaps. Bounded exceptional widths may use
  // 4px gaps, preserving the font and one-line hierarchy rather than shrinking.
  int gap=12;
  if(!label.empty()){
    const int reserve=32+sw+lw+bw;
    const int gaps=numeric?2:1;
    if(reserve+gap*gaps>width)gap=std::max(4,(width-reserve)/gaps);
  }
  p.modifier_x=p.stat_x+sw+gap;
  p.bonus_x=p.modifier_x+lw+gap;
  p.end=label.empty()?p.stat_x+sw:(numeric?p.bonus_x+bw:p.modifier_x+lw);
  p.fits=p.end<=left+width;
  return p;
}

inline const char* detail_modifier_label(uint32_t message) {
  switch(message) {
    case 1:return "AddPower_Name";case 2:return "AddLife_Name";
    case 4:return "Critical_Name";case 8:return "LongThrow_Name";
    case 16:return "SpreadFire_Name";case 64:return "RapidFire_Name";
    case 256:return "AddGuard_Name";
    case 0x80000001:return "AddPowerPlus_Name";
    case 0x80000002:return "AddLifePlus_Name";
    case 0x80000100:return "AddGuardPlus_Name";
    default:return "";
  }
}
// Native pause detail: "Attack Up + 44" / "Shield Guard Up + N" carry the value, while
// "Durability Up +" / "Durability Up ++" show no number (live native capture, iteration 11 D).
inline bool detail_modifier_numeric(uint32_t message) {
  const auto bits=message&0x7fffffffu;
  return bits==1||bits==256;
}

// Resolve the exact same configured native-language table as the manifest.
// The current package aliases/font explicitly select USen/Font_US; this path
// is generated/checked by uiux-pages.py against that alias, not translated
// text embedded in code. A future language config must supply its own table.
class DetailModifierText {
 public:
  const std::string& lookup(DescriptionCache::ReadRomfs read,void* userdata,uint32_t message) {
    static const std::string empty;
    if(!*detail_modifier_label(message))return empty;
    if(!loaded_){
      loaded_=true;
      constexpr const char* path="romfs:/Pack/Bootup_USen.pack#Message/Msg_USen.product.ssarc#StaticMsg/SpecialStatus.msbt";
      const size_t size=read?read(userdata,path,0,nullptr,0):0;
      if(size>0&&size<(4u<<20)){
        std::vector<uint8_t> data(size);
        const auto got=read(userdata,path,0,data.data(),size);
        if(got==size)for(const auto& [key,raw]:msbt_detail::ParseLabels(data))text_[key]=msbt_detail::ToUtf8(raw);
      }
    }
    const auto it=text_.find(detail_modifier_label(message));
    return it==text_.end()?empty:it->second;
  }
 private:
  bool loaded_{};std::unordered_map<std::string,std::string> text_;
};

template<class Host> void publish_detail_line(const Host& h,const char* prefix,int stat,
                                             int quantity,uint32_t message,uint32_t value,
                                             bool inventory) {
  static DetailModifierText cache;
  const auto& label=stat>=0?cache.lookup(h.read_romfs,h.userdata,message):std::string();
  const auto p=detail_line(inventory?798:90,inventory?434:560,stat>=0?stat:quantity,
                           label,value,detail_modifier_numeric(message));
  const std::string root(prefix);
  auto n=[&](const char* suffix,int64_t v){h.publish_i64(h.userdata,(root+suffix).c_str(),v);};
  h.publish_text(h.userdata,(root+".modifier_text").c_str(),label.c_str());
  n(".line.stat_x",p.stat_x);n(".line.modifier_x",p.modifier_x);n(".line.bonus_x",p.bonus_x);
  n(".line.modifier_valid",!label.empty());n(".line.fits",p.fits);
}
} // namespace botw
