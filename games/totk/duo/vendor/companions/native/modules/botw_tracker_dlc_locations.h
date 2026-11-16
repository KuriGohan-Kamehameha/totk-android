// SPDX-License-Identifier: GPL-3.0-or-later
// Generated from the game's DLC location data.
// Private coordinate supplement: maintained public metadata, not local DLC extraction.
// Source: https://objmap.zeldamods.org/game_files/map_summary/MainField/static.json
// SHA256 db50e3f1b36f7b6ad701f666c8266dd1d9a30f4dc1a5d66f1c4ca766cc12be02
// Client provenance: zeldamods/objmap commit 9a937fa2888aaf4f0b40d021d2f8d87057f3c8d9.
// All 120 base XYZ/IDs match local 1.6 exactly; names are local USen Dungeon.msbt.
#pragma once
#include <array>
#include <string_view>
namespace botw {
struct TrackerDlcShrineLocation { const char* id; float x,y,z; const char* name; };
inline constexpr std::array<TrackerDlcShrineLocation,16> tracker_dlc_shrine_locations{{
    {"Dungeon120",-941.8468017578125f,196.6999969482422f,1671.74169921875f,"Yowaka Ita Shrine"},
    {"Dungeon121",-3392.15625f,450.978515625f,1347.4669189453125f,"Kihiro Moh Shrine"},
    {"Dungeon122",-1215.9742431640625f,349.5f,2110.884521484375f,"Etsu Korima Shrine"},
    {"Dungeon123",-517.9490356445312f,182.8000030517578f,1799.02392578125f,"Rohta Chigah Shrine"},
    {"Dungeon124",3103.6083984375f,551.9120483398438f,-833.9618530273438f,"Sato Koda Shrine"},
    {"Dungeon125",4934.23291015625f,115.80000305175781f,-1002.050537109375f,"Kee Dafunia Shrine"},
    {"Dungeon126",3761.8720703125f,556.78271484375f,-645.5045166015625f,"Mah Eliya Shrine"},
    {"Dungeon127",-3821.00244140625f,98.7734375f,-2310.520751953125f,"Noe Rajee Shrine"},
    {"Dungeon128",-2156.432373046875f,459.3999938964844f,-3188.149658203125f,"Kiah Toza Shrine"},
    {"Dungeon129",-2377.081298828125f,273.9705810546875f,-1383.91943359375f,"Shira Gomar Shrine"},
    {"Dungeon130",2129.3486328125f,518.9150390625f,-3228.66064453125f,"Kamia Omuna Shrine"},
    {"Dungeon131",2242.218505859375f,550.82275390625f,-1996.1873779296875f,"Rinu Honika Shrine"},
    {"Dungeon132",2931.935546875f,639.5069580078125f,-2418.7333984375f,"Sharo Lun Shrine"},
    {"Dungeon133",-2340.23095703125f,138.5f,3900.926513671875f,"Keive Tala Shrine"},
    {"Dungeon134",-4894.4912109375f,150.984375f,2155.50146484375f,"Takama Shiri Shrine"},
    {"Dungeon135",-833.5947265625f,198.8000030517578f,2144.810546875f,"Ruvo Korbah Shrine"},
}};
static_assert(tracker_dlc_shrine_locations.size() == 16);
static_assert([] {
    for (std::size_t i=0; i<tracker_dlc_shrine_locations.size(); ++i)
        for (std::size_t j=i+1; j<tracker_dlc_shrine_locations.size(); ++j)
            if (std::string_view(tracker_dlc_shrine_locations[i].id) == tracker_dlc_shrine_locations[j].id) return false;
    return true;
}());
} // namespace botw
