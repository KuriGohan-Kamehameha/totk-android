// SPDX-FileCopyrightText: Copyright 2026 P1R4N351
// SPDX-License-Identifier: GPL-3.0-or-later

// The Legend of Zelda: Tears of the Kingdom 1.4.3 (0100F2C0115B6000). Reads the game; writes and calls nothing.
//
// Game state comes from TotK Native's companion reader (totk_reader/, MIT, compiled unchanged):
// GameData flag tables and the player actor, read through host->read_memory with code guards
// that fail closed on any other build, plus totk_reader/totk_gamedata.c (MIT) for the values the
// snapshot does not report (current stamina, shrines found). The module turns them into page
// values (totk.*); nothing in the game is written or called. The Ready page is Breath of the
// Wild's (tools/totk/gen_manifest.py derives it), so its values carry Breath of the Wild's names
// with totk. in front: botw.ready.shield -> totk.ready.shield, and so on.
//
// Item actions (equip, eat, take out, ...): the Inventory detail and the Ready page's quick picker show the actions the
// game's menus offer for the selected item, and a tap requests one. Only TotK Native's embedded port can carry them
// out: it hands the module a table (totk_native_actions.h) whose rules, state and requests are its runtime's own
// (runtime/companion_actions.c, which calls the game's pouch functions on its frame-end thread). In Eden Duo there is
// no table: every action shows disabled, "Item actions need the TotK Native app", and nothing is requested. This
// module never writes guest memory and never calls guest code, with or without the table.
//
// Art and text are decoded from the player's romfs on request (module:totk:<key>, see
// LoadImage): item icons, layout textures from the Common layout archive, the map tiles, and the
// game's Rodin font (Super Mario Bros. Wonder's font code, wonder_font.cpp). Every TotK file is a
// zstd frame with a ZsDic dictionary (totk_romfs.cpp). Nothing is shipped or written to disk.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"
#include "core/mods/modules/dsmod_module_sdk.h"
extern "C" {
#include "totk_reader/companion.h" // TotK Native's C header, kept as it is there
}
#include "totk_reader/totk_gamedata.h"
#include "totk_reader/totk_reader_ro.h"
#include "totk_native_actions.h"
#include "totk_layer.h"
#include "totk_romfs.h"
#include "wonder_assets.h"
#include "wonder_font.h"

namespace {

using namespace dsmod_sdk::int_types;
using json = nlohmann::json;

constexpr u64 TitleId = 0x0100F2C0115B6000ull;
constexpr std::string_view BuildId = "277178B7DBA1B6D4949A84778D4ABC58"; // 1.4.3 main
constexpr std::string_view Prefix = "module:totk:";
constexpr int LiveEvery = 6;   // ticks between live snapshots (10 Hz)
constexpr int FullEvery = 30;  // ticks between full snapshots (inventory, sages, map marks: 2 Hz)
constexpr int MaxRows = 300;
constexpr int Cols = 12, Rows = 10;
// The Ready page's right-pane header: the time of day on the Surface and in the Sky once a clock read exists
// (ClockText), the layer name otherwise and always in the Depths. Set to true when ClockText returns the time.
constexpr bool HeaderShowsClock = false;
std::optional<std::string> ClockText() { return std::nullopt; } // no time-of-day read yet ("03:05 PM" when there is)

// companion_snapshot reads guest memory through a plain function pointer: the host table of the
// callback that is running it.
thread_local const EdenDsmodHostApi* t_host = nullptr;
int GuestRead(uint64_t address, void* out, size_t size) {
    return t_host && t_host->read_memory &&
           t_host->read_memory(t_host->userdata, address, out, size) != EDEN_DSMOD_FALSE;
}

std::string BuildHex(const EdenDsmodHostApi& h) {
    static constexpr char Hex[] = "0123456789ABCDEF";
    std::string s;
    for (const u8 b : h.build_id) {
        s += Hex[b >> 4];
        s += Hex[b & 15];
    }
    return s;
}

WonderAssets::RomfsReader RawRomfs(const EdenDsmodHostApi& h) {
    return [read = h.read_romfs, userdata = h.userdata](const std::string& path)
               -> std::optional<std::vector<u8>> {
        if (!read)
            return std::nullopt;
        const size_t size = read(userdata, path.c_str(), 0, nullptr, 0);
        if (size == 0 || size > 64u * 1024 * 1024)
            return std::nullopt;
        std::vector<u8> out(size);
        if (read(userdata, path.c_str(), 0, out.data(), size) != size)
            return std::nullopt;
        return out;
    };
}

// ---------- snapshot model ----------

enum Cat { Weapons, Bows, Shields, Arrows, Armor, Materials, Food, Zonai, KeyItems, CatCount };
constexpr std::array<const char*, CatCount> CatKeys{"weapons", "bows",  "shields", "arrows",  "armor",
                                                    "materials", "food", "zonai",   "keyItems"};
// The inventory page's eight tabs (bows and arrows share one, as in the game).
constexpr int TabCount = 8;
constexpr std::array<std::array<int, 2>, TabCount> Tabs{{{Weapons, -1}, {Bows, Arrows}, {Shields, -1},
                                                         {Armor, -1}, {Materials, -1}, {Food, -1},
                                                         {Zonai, -1}, {KeyItems, -1}}};
constexpr std::array<const char*, 5> SageIds{"Wind", "Fire", "Water", "Electric", "Soul"};

struct Item {
    std::string actor, fuse, effect;
    int slot = -1; // in the current pouch partition (the snapshot's "slot")
    int life = INT32_MIN, count = INT32_MIN, fuse_life = INT32_MIN, heal = INT32_MIN;
    int level = INT32_MIN, time = INT32_MIN;
    bool equipped = false;
};
struct Sage {
    bool joined = false, summoned = false;
    double cooldown = 0;
};
struct Model {
    std::string status = "starting";
    std::string reason;
    bool pause_menu = false;
    json player = json::object();
    std::array<std::vector<Item>, CatCount> pouch;
    bool have_pouch = false;
    int partition = -1;               // the pouch partition the items are from
    long long pouch_gen = 0;          // the action host's generation of that pouch (0: none)
    bool act_ok = false;              // item actions may run now (the action host's state)
    std::string act_reason = "notAvailableYet";
    std::array<Sage, 5> sages;
    bool have_sages = false;
    CompanionState common{};          // life, max life (quarter hearts, -1 unknown), from companion_state
    TotkGameData gamedata{};          // stamina (every read)
    bool shrines_ok = false;          // shrines found (full reads only)
    int shrines_found = 0;
};

int Int(const json& o, const char* key, int fallback = INT32_MIN) {
    const auto it = o.find(key);
    return it != o.end() && it->is_number() ? static_cast<int>(std::lround(it->get<double>())) : fallback;
}
double Num(const json& o, const char* key, double fallback = NAN) {
    const auto it = o.find(key);
    return it != o.end() && it->is_number() ? it->get<double>() : fallback;
}
std::string Str(const json& o, const char* key) {
    const auto it = o.find(key);
    return it != o.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

// ---------- text helpers (the same wording as TotK Native's page) ----------

std::string Hearts(int quarters) {
    static constexpr std::array<const char*, 4> Parts{"", "\xC2\xBC", "\xC2\xBD", "\xC2\xBE"}; // ¼ ½ ¾
    const int q = std::max(0, quarters);
    if (q / 4 == 0 && q % 4 != 0)
        return Parts[q % 4];
    return std::to_string(q / 4) + Parts[q % 4];
}
std::string Duration(int seconds) {
    if (seconds < 0)
        return "";
    char b[32];
    std::snprintf(b, sizeof b, "%d:%02d", seconds / 60, seconds % 60);
    return b;
}
std::string Humanize(std::string_view actor) {
    std::string s{actor};
    std::replace(s.begin(), s.end(), '_', ' ');
    return s;
}
bool LowHearts(int life, int max) { // the stick-LED rule of TotK Native (BifrostLooks.lowOn)
    return max > 0 && life >= 0 && (life <= 4 || life * 4 <= max);
}

// A BYML (v7) document's root dictionary: the int value under `key` (the actor packs' parameter files).
std::optional<int> BymlRootInt(const std::vector<u8>& d, std::string_view key) {
    const auto at = [&](size_t o) -> std::optional<u32> {
        if (o + 4 > d.size())
            return std::nullopt;
        u32 v;
        std::memcpy(&v, d.data() + o, 4);
        return v;
    };
    if (d.size() < 16 || d[0] != 'Y' || d[1] != 'B')
        return std::nullopt;
    const auto keys = at(4), root = at(12);
    if (!keys || !root || *keys == 0 || *root == 0 || *keys >= d.size() || *root >= d.size() || d[*root] != 0xC1)
        return std::nullopt;
    const auto nkeys = at(*keys);
    std::optional<u32> want;
    for (u32 i = 0; nkeys && i < (*nkeys >> 8) && !want; ++i) {
        const auto off = at(*keys + 4 + 4 * size_t{i});
        if (!off || *keys + *off >= d.size())
            return std::nullopt;
        const char* str = reinterpret_cast<const char*>(d.data() + *keys + *off);
        const size_t room = d.size() - (*keys + *off);
        if (strnlen(str, room) == key.size() && std::memcmp(str, key.data(), key.size()) == 0)
            want = i;
    }
    const auto n = at(*root);
    for (u32 i = 0; want && n && i < (*n >> 8); ++i) {
        const size_t e = *root + 4 + 8 * size_t{i};
        const auto k = at(e), v = at(e + 4);
        if (!k || !v)
            return std::nullopt;
        if ((*k & 0xFFFFFF) == *want)
            return d[e + 3] == 0xD1 ? std::optional<int>{static_cast<int>(*v)} : std::nullopt;
    }
    return std::nullopt;
}

// ---------- item actions (the words of TotK Native's own companion page) ----------

struct Act {
    std::string id, reason; // reason: "" when enabled
    bool enabled = false;
};
constexpr std::array<std::array<const char*, 3>, 8> ActionWords{{{"equip", "Equip", "Equipped"},
                                                                 {"unequip", "Unequip", "Unequipped"},
                                                                 {"eat", "Eat", "Eaten"},
                                                                 {"drink", "Drink", "Drunk"},
                                                                 {"use", "Use", "Used"},
                                                                 {"hold", "Hold", "Now holding it"},
                                                                 {"takeOut", "Take out", "Taken out"},
                                                                 {"fuseArrow", "Fuse to arrows", "Fused to arrows"}}};
std::string ActionWord(std::string_view id, int which) { // 1: the button, 2: the outcome
    for (const auto& w : ActionWords)
        if (id == w[0])
            return w[which];
    return Humanize(id);
}
// A reason code (per action, of the action state, or of a request's result) in plain words. "notAvailableYet" without
// the TotK Native port is this module's own: Eden Duo cannot carry item actions out.
std::string ReasonText(std::string_view code, bool native) {
    static const std::map<std::string_view, const char*> Texts{
        {"", "Not possible right now"}, {"unsupported", "Not supported on this game build"},
        {"title", "Load a save first"}, {"loading", "Wait for loading to finish"},
        {"noPlayer", "Link isn't in the world right now"}, {"event", "In a cutscene"},
        {"menu", "Close the game menu first"}, {"saving", "The game is saving"},
        {"stale", "The game isn't running right now"}, {"notAvailableYet", "Not available from the companion yet"},
        {"weaponLocked", "Weapons can't be changed right now"}, {"notAiming", "Aim your bow to fuse"},
        {"aiming", "Not while aiming"}, {"noArrows", "No arrows left"},
        {"fullHearts", "Your hearts are already full"}, {"notEdible", "This can't be eaten"},
        {"batteryFull", "Your battery is already full"}, {"noBattery", "You don't have the battery yet"},
        {"noStock", "None left"}, {"cannotHold", "This can't be held"},
        {"infoPending", "Reading the item's details"}, {"itemMoved", "The item moved; try again"},
        {"gameRefused", "The game didn't allow it"}, {"timeout", "No response from the game"},
        {"noAnswer", "No response from the game"}, {"teardown", "The game closed"},
        {"busy", "Another action is still running"}, {"notNow", "Not possible right now"},
        {"invalid", "The request was not valid"}, {"notOffered", "Not possible right now"}};
    if (code == "notAvailableYet" && !native)
        return "Item actions need the TotK Native app";
    const auto it = Texts.find(code);
    return it != Texts.end() ? it->second : Humanize(code);
}
// The actions shown without the TotK Native port: the ones the game's menus would offer for an item of that kind (eat,
// drink and use of a material depend on facts only the game's info lookup has, so a material shows take out and
// fuse), all disabled.
std::vector<Act> OfflineActions(int cat, const Item& it) {
    std::vector<Act> out;
    const auto add = [&](const char* id) { out.push_back({id, "notAvailableYet", false}); };
    switch (cat) {
    case Weapons: case Bows: case Shields: case Armor:
        add(it.equipped ? "unequip" : "equip");
        break;
    case Food:
        add("eat");
        break;
    case Materials: case Zonai:
        add("takeOut");
        add("fuseArrow");
        break;
    default:
        break;
    }
    return out;
}

// ---------- the module ----------

class Module {
public:
    explicit Module(const EdenDsmodHostApi& api)
        : host{api}, supported{BuildHex(api).starts_with(BuildId)},
          romfs{std::make_shared<TotkRomfs::Reader>(RawRomfs(api))} {
        text_thread = std::thread([this] { LoadText(); });
    }
    ~Module() {
        if (text_thread.joinable())
            text_thread.join();
    }
    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;

    void Configure(const EdenDsmodHostExtensions* ext) {
        if (ext && ext->version == EDEN_DSMOD_EXT_VERSION && ext->abi_hash == EDEN_DSMOD_EXT_HASH &&
            ext->struct_size >= sizeof(*ext)) {
            std::scoped_lock lock{image_mutex};
            astc = {ext->userdata, ext->decode_astc};
        }
    }

    void Sample(const EdenDsmodHostApi& api) {
        host = api;
        t_host = &host;
        if (supported && tick % LiveEvery == 0)
            Read(tick % FullEvery == 0 || !model.have_pouch || std::exchange(full_soon, false));
        PollRequest();
        ++tick;
        Publish();
        t_host = nullptr;
    }

    EdenDsmodBool DecodeFont(const u8* bytes, size_t size, void* receiver, EdenDsmodFontSink sink) {
        const auto spec = WonderFont::ParseFontSpec({bytes, size});
        const auto font = spec ? WonderFont::BuildFontAtlas(romfs->AsWonderReader(), *spec) : nullptr;
        if (!font || font->glyphs.empty())
            return EDEN_DSMOD_FALSE;
        sink(receiver, font->line_height, font->first_codepoint, font->glyphs.data(),
             static_cast<u32>(font->glyphs.size()));
        return EDEN_DSMOD_TRUE;
    }

    EdenDsmodBool LoadImage(std::string_view key, void* receiver, EdenDsmodImageSink sink);

    // The TotK Native port's action table (totk_native_actions.h); null unbinds. Any thread.
    void BindNative(const TotkNativeActions* table) {
        std::scoped_lock lock{act_mutex};
        if (table && table->version == TOTK_NATIVE_ACTIONS_VERSION && table->struct_size >= sizeof(TotkNativeActions) &&
            table->item_actions && table->action_state && table->pouch_gen && table->request && table->result)
            native = *table;
        else
            native.reset();
    }
    // "totk.act": argument = source * 8 + button (source 0 the Inventory detail, 1 the quick picker); "totk.select":
    // the quick picker's cell. Runs outside the tick (MODULE_GUIDE); false refuses (the page plays the refused haptic).
    EdenDsmodBool Action(std::string_view name, s64 argument);

private:
    enum Source { SrcInventory, SrcQuick, SourceCount };
    // What a source showed at its last sample: the item and its actions, for on_action (guarded by act_mutex).
    struct Chosen {
        bool valid = false;
        int cat = -1, slot = -1, partition = -1;
        long long gen = 0;
        std::string actor, name;
        std::vector<Act> acts;
    };
    // The latest request and its outcome (guarded by act_mutex).
    struct Request {
        int seq = 0;
        std::string action, name, state, reason;
        bool final = true;
        std::chrono::steady_clock::time_point at{}, ended{};
        std::string logged; // the state last logged
    };
    std::optional<TotkNativeActions> Native() const {
        std::scoped_lock lock{act_mutex};
        return native;
    }
    const std::vector<Act>& ActionsFor(Source src, int cat, const Item& it, const std::optional<TotkNativeActions>& n);
    void PublishActions(const std::string& prefix, Source src, int cat, const Item* it);
    void PollRequest();
    void PublishRequest();
    void PublishQuick(bool shown);

    void Read(bool full);
    void Publish();
    void LoadText();
    std::string ItemName(const std::string& actor) const;
    std::string ItemCaption(const std::string& actor) const;
    void I64(const std::string& name, s64 v) const { host.publish_i64(host.userdata, name.c_str(), v); }
    void F64(const std::string& name, double v) const { host.publish_f64(host.userdata, name.c_str(), v); }
    void Text(const std::string& name, const std::string& v) const {
        host.publish_text(host.userdata, name.c_str(), v.c_str());
    }
    s64 Get(const char* name, s64 fallback) const {
        return host.get_i64 ? host.get_i64(host.userdata, name, fallback) : fallback;
    }
    void PublishReady();
    void PublishGear(const char* slot, Cat cat);
    int MaxLife(const std::string& actor);
    void PublishHeal();
    void PublishFx();
    void PublishMap();
    void PublishInventory();

    std::optional<WonderAssets::Image> Layout(std::string_view texture);
    std::optional<WonderAssets::Image> Icon(std::string_view actor);
    std::optional<WonderAssets::Image> MapTile(std::string_view key);
    std::optional<WonderAssets::Image> Generated(std::string_view key);

    EdenDsmodHostApi host;
    bool supported;
    std::shared_ptr<TotkRomfs::Reader> romfs;
    u64 tick = 0;
    TotkReaderState state{};
    Model model;
    CompanionBuf buf{};
    int sel_tab = -1;
    s64 sel_seen = -1;

    // text: PouchContent names and captions, loaded once on a thread
    std::thread text_thread;
    mutable std::mutex text_mutex;
    std::unordered_map<std::string, std::string> names;
    std::atomic<bool> text_ready{false};

    // Link's map layer, decided as the game decides it (totk_layer.h); never MapData.CurrentLayer
    void UpdateLayer(uintptr_t main);
    TotkLayer::Params layer_grids;            // live grids, kept while the area-height object stays the same
    std::uint64_t layer_grids_key = 0;
    std::optional<TotkLayer::Params> layer_romfs;   // MainField.beco from the romfs (text thread; under text_mutex)
    int layer_last = TotkLayer::Ground;
    bool layer_live_seen = false;
    int layer_logged = -1;

    // images (asset worker)
    std::mutex image_mutex;
    WonderAssets::AstcDecoder astc{};
    std::optional<std::vector<u8>> combined; // Common layout archive's timg/__Combined.bntx
    bool combined_tried = false;
    std::optional<std::map<std::string, std::string>> tiles; // "<L>/<lod>/<c>_<r>" -> romfs path

    // an item's maximum durability (the actor pack's MaxLife), -1 when it has none; sample thread only
    std::unordered_map<std::string, int> max_life;

    // item actions
    mutable std::mutex act_mutex;
    std::optional<TotkNativeActions> native; // guarded
    std::array<Chosen, SourceCount> chosen;  // guarded
    Request request;                         // guarded
    struct Cached {                          // sample thread: each source's actions, refreshed at the live rate
        int cat = -1, slot = -1;
        std::string actor;
        bool equipped = false;
        u64 at = 0;
        std::vector<Act> acts;
    };
    std::array<Cached, SourceCount> cached;
    bool full_soon = false;     // sample thread: an action ended; read the pouch again at the next read
    int native_logged = -1;     // sample thread: the action route last logged (0 none, 1 TotK Native)
    int quick_open = 0;         // sample thread: the quick picker's last category (0 closed)
    std::atomic<s64> quick_tap{-1}; // the quick picker's tapped cell ("totk.select"); -1: the equipped item
};

// The item's maximum durability: MaxLife in its actor pack's Life/LifeParameters/<actor> file (weapons, bows,
// shields). Read once per actor, on the sample thread (a few KiB each); -1 when the pack has none.
int Module::MaxLife(const std::string& actor) {
    if (actor.empty())
        return -1;
    if (const auto it = max_life.find(actor); it != max_life.end())
        return it->second;
    int v = -1;
    if (const auto pack = romfs->Read("Pack/Actor/" + actor + ".pack.zs")) {
        auto file = WonderAssets::SarcMember(*pack, "Life/LifeParameters/" + actor + ".game__life__LifeParameters.bgyml");
        if (!file) { // a shared parameter file: used only when it is the pack's one LifeParameters file
            int found = 0;
            for (const auto& name : WonderAssets::SarcNames(*pack))
                if (name.starts_with("Life/LifeParameters/") && ++found == 1)
                    file = WonderAssets::SarcMember(*pack, name);
            if (found != 1)
                file.reset();
        }
        if (file)
            if (const auto m = BymlRootInt(*file, "MaxLife"); m && *m > 0)
                v = *m;
    }
    max_life.emplace(actor, v);
    return v;
}

void Module::LoadText() {
    // The English text archive; its name carries a data version (Mals/USen.Product.142.sarc.zs on
    // 1.4.3), so the newest one present is used.
    for (int v = 199; v >= 100; --v) {
        const std::string path = "Mals/USen.Product." + std::to_string(v) + ".sarc.zs";
        const auto sarc = romfs->Read(path);
        if (!sarc)
            continue;
        std::unordered_map<std::string, std::string> all;
        if (const auto msbt = WonderAssets::SarcMember(*sarc, "ActorMsg/PouchContent.msbt"))
            all = TotkRomfs::ParseMsbt(*msbt);
        std::scoped_lock lock{text_mutex};
        names = std::move(all);
        break;
    }
    // The main field's area-height grids, for the map layer while the live ones cannot be read
    if (const auto pack = romfs->Read("Pack/Bootup.Nin_NX_NVN.pack.zs"))
        if (const auto beco = WonderAssets::SarcMember(*pack, "WorldMgr/AreaHeight/MainField.beco")) {
            TotkLayer::Params p;
            if (TotkLayer::ParseBeco(*beco, p)) {
                std::scoped_lock lock{text_mutex};
                layer_romfs = std::move(p);
            }
        }
    text_ready = true;
}

// Replaces the snapshot's "layer" (MapData.CurrentLayer, a saved UI value: "Sky on the ground", 2026-11-17) with the
// game's own rule over Link's position. Every user of the layer reads this one value: the minimap and Map page tiles,
// the header and the Shrine/Lightroot dial. In a world that is not the main field (shrines, interiors) the last
// main-field layer stays; before any live read the romfs grids stand in.
void Module::UpdateLayer(uintptr_t main) {
    const auto pos = model.player.find("pos");
    if (pos == model.player.end() || !pos->is_array() || pos->size() != 3)
        return;   // no position: keep the snapshot's value
    const float x = (*pos)[0].get<float>(), y = (*pos)[1].get<float>(), z = (*pos)[2].get<float>();
    int layer = layer_last;
    const char* source = "kept";
    if (const auto live = TotkLayer::ReadLive(main, GuestRead, layer_grids, layer_grids_key)) {
        layer = TotkLayer::Calc(*live, x, y, z);
        layer_live_seen = true;
        source = "live";
    } else if (!layer_live_seen) {
        std::optional<TotkLayer::Params> rom;
        {
            std::scoped_lock lock{text_mutex};
            rom = layer_romfs;
        }
        if (rom) {
            layer = TotkLayer::Calc(*rom, x, y, z);
            source = "romfs";
        } else {
            TotkLayer::Params plain;   // no grids yet: the main field's Depths line only, never Sky
            plain.have_area_height = false;
            layer = TotkLayer::Calc(plain, x, y, z);
            source = "plain";
        }
    }
    layer_last = layer;
    static constexpr const char* Names[] = {"Ground", "Sky", "UnderGround"};
    model.player["layer"] = Names[layer];
    const int logged = layer * 4 + (source[0] == 'l' ? 0 : source[0] == 'r' ? 1 : source[0] == 'p' ? 2 : 3);
    if (logged != layer_logged && host.log) {
        layer_logged = logged;
        const std::string line = std::string{"TotK map layer: "} + Names[layer] + " (" + source + ")";
        host.log(host.userdata, EDEN_DSMOD_LOG_INFO, line.c_str());
    }
}

std::string Module::ItemName(const std::string& actor) const {
    if (actor.empty())
        return {};
    std::scoped_lock lock{text_mutex};
    const auto it = names.find(actor + "_Name");
    return it != names.end() && !it->second.empty() ? it->second : Humanize(actor);
}
std::string Module::ItemCaption(const std::string& actor) const {
    std::scoped_lock lock{text_mutex};
    const auto it = names.find(actor + "_Caption");
    return it != names.end() ? it->second : std::string{};
}

void Module::Read(bool full) {
    const uintptr_t main = static_cast<uintptr_t>(host.main_base);
    totk_reader_refresh(main, GuestRead, &state);
    companion_state(main, GuestRead, &model.common);
    if (companion_snapshot(main, full ? COMPANION_FULL : COMPANION_LIVE, GuestRead, &buf) == 0)
        return;
    const json root = json::parse(std::string_view{buf.data, buf.len}, nullptr, false);
    if (!root.is_object())
        return;
    model.status = Str(root, "status");
    if (model.status != "ready") {
        model.player = json::object();
        model.gamedata = {};
        model.shrines_ok = false;
        if (model.status != "starting") { // keep the pouch across a moment of "starting" only
            model.have_pouch = model.have_sages = false;
            for (auto& c : model.pouch)
                c.clear();
        }
        model.reason = model.status;
        model.pause_menu = false;
        return;
    }
    model.player = root.value("player", json::object());
    UpdateLayer(main);
    const auto n = Native();
    if (static_cast<int>(n.has_value()) != native_logged) {   // which route item actions take, once per change
        native_logged = n.has_value();
        if (host.log)
            host.log(host.userdata, EDEN_DSMOD_LOG_INFO,
                     n ? "TotK item actions: through TotK Native's runtime (its own rules and frame-end calls)"
                       : "TotK item actions: none in this host; shown disabled (Item actions need the TotK Native app)");
    }
    // after a "ready" snapshot: the code guard has passed for this main
    totk_gamedata_read(main, GuestRead, full, &model.gamedata);
    if (full) {
        model.shrines_ok = model.gamedata.shrines_ok;
        model.shrines_found = model.gamedata.shrines_found;
    }
    const json action = root.value("actionState", json::object());
    model.reason = Str(action, "reason");
    model.pause_menu = action.value("pauseMenu", false);
    // the action route's own state (TotK Native: its frame-end gate, which also covers saving and the settle after a
    // menu or a load), asked only while a selection shows actions: asking keeps the frame-end refresh running
    bool showing = false;
    {
        std::scoped_lock lock{act_mutex};
        for (const auto& c : chosen)
            showing |= c.valid;
    }
    model.act_ok = false;
    model.act_reason = n ? "stale" : "notAvailableYet";
    if (n && showing) {
        s32 ok = 0;
        const char* why = nullptr;
        n->action_state(n->userdata, &ok, &why);
        model.act_ok = ok != 0;
        model.act_reason = why ? why : "";
    }
    if (!full)
        return;
    model.partition = Int(root, "partition", -1);
    // the generation a request names: the action host's own (its pouch state), read right after the pouch was
    model.pouch_gen = n ? n->pouch_gen(n->userdata) : Int(root, "pouchGen", 0);
    if (const auto pouch = root.find("pouch"); pouch != root.end() && pouch->is_object()) {
        for (int c = 0; c < CatCount; ++c) {
            auto& list = model.pouch[c];
            list.clear();
            const auto items = pouch->find(CatKeys[c]);
            if (items == pouch->end() || !items->is_array())
                continue;
            for (const auto& o : *items) {
                Item it;
                it.actor = Str(o, "name");
                it.slot = Int(o, "slot", -1);
                it.fuse = Str(o, "fuse");
                it.life = Int(o, "life");
                it.count = Int(o, "count");
                it.fuse_life = Int(o, "fuseLife");
                it.heal = Int(o, "heal");
                it.level = Int(o, "level");
                it.time = Int(o, "time");
                it.effect = Str(o, "effect");
                if (it.effect.empty())
                    if (const auto e = o.find("effect"); e != o.end() && e->is_object())
                        it.effect = Str(*e, "type");
                it.equipped = o.value("equipped", false);
                list.push_back(std::move(it));
            }
        }
        model.have_pouch = true;
    }
    if (const auto sages = root.find("sages"); sages != root.end() && sages->is_array()) {
        model.sages = {};
        for (const auto& o : *sages)
            for (size_t i = 0; i < SageIds.size(); ++i)
                if (Str(o, "id") == SageIds[i])
                    model.sages[i] = {o.value("joined", false), o.value("summoned", false),
                                      Num(o, "cooldown", 0)};
        model.have_sages = true;
    }
}

int Equipped(const std::vector<Item>& list) {
    for (size_t i = 0; i < list.size(); ++i)
        if (list[i].equipped)
            return static_cast<int>(i);
    return -1;
}

void Module::Publish() {
    // status: TotK Native's common state numbers (companion_state.h)
    const std::string& s = model.status;
    const int status = !supported ? 2
                       : s == "ready"        ? 5
                       : s == "title"        ? 3
                       : s == "unsupported"  ? 2
                                             : 1;
    I64("totk.status", status);
    I64("totk.ready", status == 5);
    I64("totk.not_ready", status != 5);
    Text("totk.status_text", status == 3   ? "Title screen"
                             : status == 2 ? "This companion needs Tears of the Kingdom 1.4.3"
                                           : "Waiting for the game");
    const bool event = status == 5 && model.reason == "event";
    const bool menu = status == 5 && !event && model.pause_menu;
    const bool loading = status == 1 || (status == 5 && (model.reason == "loading" || model.reason == "noPlayer"));
    I64("totk.event", event);
    I64("totk.menu", menu);
    I64("totk.dim", event || menu);
    I64("totk.text_ready", text_ready.load());

    // The page the second screen shows, as Breath of the Wild's package numbers them (0 Ready, 1 the title's
    // main-menu page, 2 Loading); another build keeps the Ready page, whose cover says so.
    I64("totk.state.page", status == 3 ? 1 : loading ? 2 : 0);
    I64("totk.state.title", status == 3);
    I64("totk.state.loading", loading);
    I64("totk.state.unsupported", status == 2);
    I64("totk.state.dim", event);
    // The common second-screen state words (companion_state.h): -1 while they are not known.
    I64("totk.state.menu_open", status == 5 ? (menu ? 1 : 0) : -1);
    I64("totk.state.life", status == 5 && model.common.life >= 0 ? model.common.life : -1);
    I64("totk.state.max_life", status == 5 && model.common.max_life >= 0 ? model.common.max_life : -1);
    PublishFx();
    PublishRequest();
    if (status != 5) {
        std::scoped_lock lock{act_mutex};
        for (auto& c : chosen)
            c.valid = false;
        return;
    }
    PublishReady();
    PublishMap();
    PublishInventory();
    PublishQuick(!event && !menu && !loading); // the picker gives way to the game's menu, an event and loading
}

// The loading page's twelve-dot spinner: Breath of the Wild's native loop shape (dots 0.5 -> 1 -> 0.5 every 20 UI
// frames at 30 fps, odd arms half a cycle later; the faint halos the same plus 1.0), as scale x 1000.
void Module::PublishFx() {
    const double f = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count() * 30;
    for (int k = 0; k < 12; ++k) {
        const double ph = std::fmod(f + (k % 2 ? 10.0 : 0.0), 20.0) / 20.0;
        const double v = 0.75 - 0.25 * std::cos(2 * M_PI * ph) + (k >= 6 ? 1.0 : 0.0);
        I64("totk.fx.spin." + std::to_string(k), std::lround(v * 1000));
    }
}

namespace {
std::string Grouped(long long v) { // 12,845
    std::string d = std::to_string(v < 0 ? -v : v), out;
    for (size_t i = 0; i < d.size(); ++i) {
        if (i && (d.size() - i) % 3 == 0)
            out += ',';
        out += d[i];
    }
    return v < 0 ? "-" + out : out;
}
std::string Clock(int seconds) { // m:ss, as the buff times on Breath of the Wild's page
    if (seconds < 0)
        seconds = 0;
    char b[32];
    std::snprintf(b, sizeof b, "%d:%02d", seconds / 60, seconds % 60);
    return b;
}
// A food effect's short label on the food card, for the effects whose name says what they do.
const char* FoodEffectLabel(const std::string& e) {
    static const std::map<std::string, const char*> Labels{
        {"AttackUp", "Attack up"},         {"DefenseUp", "Defense up"},     {"AllSpeed", "Speed up"},
        {"QuietnessUp", "Stealth up"},     {"ResistHot", "Heat resistance"}, {"ResistCold", "Cold resistance"},
        {"ResistBurn", "Flame guard"},     {"ResistElectric", "Shock resistance"},
        {"StaminaRecover", "Stamina"},     {"ExStaminaMaxUp", "Extra stamina"}, {"LightEmission", "Glow"},
        {"NotSlippy", "Slip resistance"},  {"MiasmaGuard", "Gloom resistance"}, {"LifeRepair", "Gloom recovery"}};
    const auto it = Labels.find(e);
    return it != Labels.end() ? it->second : nullptr;
}
} // namespace

// The Ready page's gear corners and wells, named as Breath of the Wild's (botw.ready.* -> totk.ready.*).
void Module::PublishGear(const char* slot, Cat cat) {
    const auto& list = model.pouch[cat];
    const int i = Equipped(list);
    const std::string p = std::string{"totk.ready."} + slot;
    I64(p + ".valid", i >= 0);
    if (i < 0) {
        I64(p + ".durability_pct", -1);
        I64(p + ".low", 0);
        return;
    }
    const Item& it = list[i];
    // the bar is the item's durability over its own maximum (spec C14); with no maximum read, no bar and no red
    const int max = MaxLife(it.actor);
    Text(p, std::string{Prefix} + "icon/" + it.actor);
    const bool known = it.life != INT32_MIN && max > 0;
    const int pct = known ? std::clamp(static_cast<int>(std::lround(100.0 * std::max(0, it.life) / max)), 0, 100) : -1;
    I64(p + ".durability_pct", pct);
    I64(p + ".low", known && pct <= 20);
}

void Module::PublishReady() {
    const json& pl = model.player;
    const auto has = [&](const char* k) { return pl.contains(k); };

    // rupees, bottom left
    I64("totk.ready.rupees.valid", has("rupees"));
    Text("totk.ready.rupees.text", has("rupees") ? Grouped(Int(pl, "rupees", 0)) : "");

    // the three gear corners, the ability corner, the arrow well and the Fuse well
    PublishGear("weapon", Weapons);
    PublishGear("bow", Bows);
    PublishGear("shield", Shields);
    static const std::map<std::string, std::string> AbilityActor{
        {"UltraHand", "Obj_UltraHand"},       {"OneTouchBond", "Obj_OneTouchBond"},
        {"CeilingClipper", "Obj_Tooreroof"},  {"ReverseRecorder", "Obj_ReverseRecorder"},
        {"AutoBuilder", "Obj_AutoBuilder"}};
    const auto ab = AbilityActor.find(Str(pl, "ability"));
    I64("totk.ready.rune.valid", ab != AbilityActor.end());
    if (ab != AbilityActor.end())
        Text("totk.ready.rune", std::string{Prefix} + "icon/" + ab->second);

    const auto& arrows = model.pouch[Arrows];
    int a = Equipped(arrows);
    if (a < 0 && !arrows.empty())
        a = 0;
    I64("totk.ready.arrow.valid", a >= 0);
    I64("totk.ready.arrow.count.valid", a >= 0 && arrows[a].count != INT32_MIN);
    if (a >= 0) {
        Text("totk.ready.arrow", std::string{Prefix} + "icon/" + arrows[a].actor);
        I64("totk.ready.arrow.count", std::max(0, arrows[a].count));
    }
    // Fuse (owner: in Breath of the Wild's secondary-weapon well and arc): the material fused to the equipped weapon
    // and its durability points. The material's maximum fuse durability is not read (its actor pack has no
    // LifeParameters), so the page draws the arc's empty track only.
    const auto& weapons = model.pouch[Weapons];
    const int w = Equipped(weapons);
    const bool fused = w >= 0 && !weapons[w].fuse.empty();
    I64("totk.ready.fuse.valid", fused);
    I64("totk.ready.fuse.life", fused && weapons[w].fuse_life != INT32_MIN ? std::max(0, weapons[w].fuse_life) : -1);
    if (fused)
        Text("totk.ready.fuse", std::string{Prefix} + "icon/" + weapons[w].fuse);

    // dials: Lights of Blessing (top left), shrines found (top right), the sensor (bottom left); temperature
    // (bottom right) has no read yet and shows its empty state
    int lights = 0;
    for (const auto& it : model.pouch[KeyItems])
        if (it.actor == "Obj_DungeonClearSeal")
            lights += std::max(0, it.count == INT32_MIN ? 1 : it.count);
    I64("totk.ready.lights.valid", model.have_pouch);
    I64("totk.ready.lights", lights);
    I64("totk.ready.shrine_count.valid", model.shrines_ok);
    I64("totk.ready.shrine_count", model.shrines_found);

    const std::string layer = Str(pl, "layer");
    const int li = layer == "Sky" ? 1 : layer == "UnderGround" ? 2 : 0;
    I64("totk.ready.sensor.ring_palette.0", 0);
    Text("totk.ready.sensor.art.0", std::string{Prefix} + "lyt/SeekSensor_00^s");
    Text("totk.ready.sensor.text", li == 2 ? "Lightroot" : "Shrine");

    // The right pane's header (owner 2026-11-17): the layer name while Link is in the Depths; on the Surface and
    // in the Sky the time of day, like Breath of the Wild's clock. There is no time-of-day read yet, so until one
    // exists HeaderShowsClock stays false and the header names the layer everywhere.
    static constexpr const char* LayerNames[] = {"Surface", "Sky", "Depths"};
    const std::optional<std::string> clock = HeaderShowsClock && li != 2 ? ClockText() : std::nullopt;
    I64("totk.ready.header.valid", 1);
    Text("totk.ready.header.text", clock ? *clock : LayerNames[li]);
    I64("totk.layer", li);

    // the stamina ring round the disc: current stamina while it is not full or Link is out of breath
    const bool st = model.gamedata.stamina_ok;
    const double pct = st ? 100.0 * model.gamedata.stamina / model.gamedata.max_stamina : 0;
    I64("totk.ready.stamina.valid", st);
    I64("totk.ready.stamina.percent", st ? std::clamp(static_cast<int>(pct), 0, 100) : -1);
    I64("totk.ready.stamina.depleted",
        st && (model.gamedata.stamina < model.gamedata.max_stamina - 0.5f || (model.gamedata.tired_ok && model.gamedata.tired)));

    // Buffs: TotK has one meal effect at a time. Which effect its id stands for is not read yet, so the row
    // shows a plain effect mark and the time left.
    const json buff = pl.value("cookBuff", json::object());
    const bool meal = buff.contains("id") && Int(buff, "time", 0) > 0;
    I64("totk.ready.effects.count", meal ? 1 : 0);
    I64("totk.ready.effect.0.kind", meal ? 0 : -1);
    if (meal) {
        Text("totk.ready.effect.0.icon", std::string{Prefix} + "lyt/BonusIcon72_NoMark_00^s");
        Text("totk.ready.effect.0.time", Clock(Int(buff, "time", 0)));
    }

    // Sages in Breath of the Wild's Champions rows: joined sages only, in the game's order. display_kind 1: ready
    // (with the glow, "On"), 2: recharging (the time left), 4: not with Link ("Off").
    static constexpr const char* SageIcons[] = {"Wind", "Fire", "Water", "Thunder", "Soul"};
    int rows = 0;
    for (size_t i = 0; i < SageIds.size(); ++i) {
        const Sage& sg = model.sages[i];
        if (!sg.joined)
            continue;
        const std::string p = "totk.ready.champion_slot." + std::to_string(rows);
        const int cd = static_cast<int>(std::lround(sg.cooldown));
        const int kind = cd > 0 ? 2 : sg.summoned ? 1 : 4;
        Text(p + ".icon", std::string{Prefix} + "lyt/SageSoulIcon_" + SageIcons[i] + "^s");
        I64(p + ".tint", kind == 4 ? 1 : 0);
        I64(p + ".display_kind", kind);
        Text(p + ".display", kind == 2 ? Clock(cd) : kind == 1 ? "On" : "Off");
        ++rows;
    }
    I64("totk.ready.champions.count", model.have_sages ? rows : 0);
    I64("totk.ready.champions.available", model.have_sages && rows > 0);

    PublishHeal();
}

// The food page and card (Breath of the Wild's heal advisor, read-only): offered while the hearts are low, with the
// four pouch foods closest to what is missing (exact first, then one heart over, one under, ...), hearty food last.
void Module::PublishHeal() {
    const int life = model.common.life, max = model.common.max_life;
    const bool valid = life >= 0 && max > 0;
    const bool low = valid && LowHearts(life, max);
    const int missing = valid ? std::max(0, max - life) : 0;
    struct Row { const Item* it; bool full; int rank, waste; };
    std::vector<Row> rows;
    if (low && missing > 0)
        for (const auto& it : model.pouch[Food]) {
            if (it.actor.empty() || it.count == 0)
                continue;
            const bool full = it.effect == "LifeMaxUp";
            if (!full && (it.heal == INT32_MIN || it.heal <= 0))
                continue;
            Row r{&it, full, INT32_MAX, 0};
            if (!full) {
                const int d = it.heal - missing, k = (std::abs(d) + 3) / 4;
                r.rank = d == 0 ? 0 : d > 0 ? 2 * k - 1 : 2 * k;
                r.waste = std::abs(d);
            }
            rows.push_back(r);
        }
    std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        if (a.rank != b.rank)
            return a.rank < b.rank;
        if (a.waste != b.waste)
            return a.waste < b.waste;
        return a.it->count > b.it->count;
    });
    if (rows.size() > 4)
        rows.resize(4);
    I64("totk.ready.heal.available", low);
    Text("totk.ready.heal.need_text", missing > 0 ? "Need " + Hearts(missing) : std::string{"Full"});
    Text("totk.ready.heal.need_amount_text", missing > 0 ? Hearts(missing) : std::string{"Full"});
    I64("totk.ready.heal.empty", low && rows.empty() && missing > 0);
    for (size_t i = 0; i < 4; ++i) {
        const std::string p = "totk.ready.heal.row." + std::to_string(i);
        const Row* r = low && i < rows.size() ? &rows[i] : nullptr;
        I64(p + ".valid", r != nullptr);
        if (!r) {
            I64(p + ".full", 0);
            I64(p + ".has_effect", 0);
            continue;
        }
        const Item& it = *r->it;
        Text(p + ".actor", std::string{Prefix} + "icon/" + it.actor + (it.effect.empty() ? "" : "_" + it.effect));
        Text(p + ".name", ItemName(it.actor));
        I64(p + ".count", std::max(1, it.count == INT32_MIN ? 1 : it.count));
        I64(p + ".full", r->full);
        Text(p + ".heal_text", r->full ? std::string{"Full"} : "+" + Hearts(it.heal));
        const char* label = r->full ? nullptr : FoodEffectLabel(it.effect);
        I64(p + ".has_effect", label != nullptr);
        Text(p + ".effect_text", label ? label : "");
        Text(p + ".bonus_text", "");
    }
}

void Module::PublishMap() {
    const json& pl = model.player;
    const auto pos = pl.find("pos");
    const bool have = pos != pl.end() && pos->is_array() && pos->size() == 3;
    I64("totk.map.valid", have);
    I64("totk.map.minimap.valid", have);
    if (have) {
        F64("totk.map.x", (*pos)[0].get<double>());
        F64("totk.map.y", -(*pos)[2].get<double>()); // map y is north-up: -Z
        // the Map page's coordinates (spec C48): whole metres, signed, in the game's axis order (east, height, south)
        char b[96];
        std::snprintf(b, sizeof b, "X %+.0f  Y %+.0f  Z %+.0f", (*pos)[0].get<double>(), (*pos)[1].get<double>(),
                      (*pos)[2].get<double>());
        Text("totk.coords", b);
    } else {
        Text("totk.coords", "");
    }
    // 0 = north, clockwise, as Breath of the Wild's botw.map.view.heading; the minimap stays north up
    F64("totk.map.heading", Num(pl, "heading", 0));
    F64("totk.map.view.heading", Num(pl, "heading", 0));
    I64("totk.map.view.rotation", 0);
    static constexpr std::array<char, 3> Layers{'G', 'S', 'U'};
    const std::string layer = Str(pl, "layer");
    const int follow = layer == "Sky" ? 1 : layer == "UnderGround" ? 2 : 0;
    // The Map page's layer: the one the player picked (flag map_layer 1..3), else Link's.
    const s64 picked = Get("@flag:map_layer", 0);
    const int page_layer = picked >= 1 && picked <= 3 ? static_cast<int>(picked - 1) : follow;
    I64("totk.map.page_layer", page_layer);
    for (int k = 0; k < 3; ++k)
        I64("totk.map.page_layer_is." + std::to_string(k), page_layer == k);
    // Finer tiles once the Map page is zoomed past the coarse tiles' size.
    const double ppw = host.get_f64 ? host.get_f64(host.userdata, "@map_view_ppw:TotkFull", 0) : 0;
    const int page_lod = ppw > 0.45 ? 1 : 2;
    for (int r = 0; r < Rows; ++r)
        for (int c = 0; c < Cols; ++c) {
            const int k = r * Cols + c;
            char tail[32];
            std::snprintf(tail, sizeof tail, "/%d_%d", c, r);
            // the minimap shows a 900 m window (Breath of the Wild's follow_window): the finer tiles
            Text("totk.mini.tile." + std::to_string(k),
                 std::string{Prefix} + "map/" + Layers[follow] + "/1" + tail);
            Text("totk.map.tile." + std::to_string(k), std::string{Prefix} + "map/" + Layers[page_layer] +
                                                           "/" + std::to_string(page_lod) + tail);
        }
}

void Module::PublishInventory() {
    const int tab = static_cast<int>(std::clamp<s64>(Get("@flag:inv_tab", 0), 0, TabCount - 1));
    for (int t = 0; t < TabCount; ++t)
        I64("totk.inv.tab." + std::to_string(t), t == tab);
    std::vector<std::pair<int, const Item*>> rows;
    for (const int cat : Tabs[tab])
        if (cat >= 0)
            for (const auto& it : model.pouch[cat])
                if (!it.actor.empty() && rows.size() < MaxRows)
                    rows.emplace_back(cat, &it);
    I64("totk.inv.count", static_cast<s64>(rows.size()));
    I64("totk.inv.empty", rows.empty());
    for (size_t i = 0; i < rows.size(); ++i) {
        const Item* it = rows[i].second;
        const std::string p = "totk.inv." + std::to_string(i);
        I64(p + ".on", 1);
        Text(p + ".icon", std::string{Prefix} + "icon/" + it->actor);
        Text(p + ".name", ItemName(it->actor));
        I64(p + ".equipped", it->equipped);
        // durability over the item's own maximum; no bar when the maximum is not known
        const int max = it->life != INT32_MIN && it->count == INT32_MIN ? MaxLife(it->actor) : -1;
        const bool life = max > 0;
        I64(p + ".has_life", life);
        I64(p + ".life", life ? std::min(std::max(0, it->life), max) : 0);
        I64(p + ".life_max", life ? max : 1);
        I64(p + ".life_low", life && it->life * 5 <= max);
        Text(p + ".qty", it->count != INT32_MIN && it->count > 1 ? "x" + std::to_string(it->count) : ""); // C41
        I64(p + ".fused", !it->fuse.empty());
        if (!it->fuse.empty())
            Text(p + ".fuse_icon", std::string{Prefix} + "icon/" + it->fuse);
    }
    // the selected item's detail strip (tap selects: @sel:inv carries the row index)
    const s64 sel_flag = Get("@flag:inv_sel", -1);
    if (tab != sel_tab) { // a new tab starts with nothing selected, until the next tap
        sel_tab = tab;
        sel_seen = sel_flag;
    }
    const s64 sel = sel_flag != sel_seen ? sel_flag : -1;
    if (sel >= 0)
        sel_seen = INT64_MIN; // a tap on this tab: keep it until the tab changes
    for (size_t i = 0; i < rows.size(); ++i)
        I64("totk.inv." + std::to_string(i) + ".selected", static_cast<s64>(i) == sel);
    const bool has_sel = sel >= 0 && sel < static_cast<s64>(rows.size());
    I64("totk.inv.sel.has", has_sel);
    I64("totk.inv.sel.none", !has_sel);
    if (has_sel) {
        const Item& it = *rows[static_cast<size_t>(sel)].second;
        Text("totk.inv.sel.name", ItemName(it.actor));
        Text("totk.inv.sel.caption", ItemCaption(it.actor));
        std::string facts;
        if (it.life != INT32_MIN && it.count == INT32_MIN) { // C43: as a % of the item's maximum when it is known
            const int max = MaxLife(it.actor);
            facts += max > 0 ? "Durability " +
                                   std::to_string(std::clamp(static_cast<int>(std::lround(100.0 * std::max(0, it.life) / max)), 0, 100)) + "%"
                             : "Durability " + std::to_string(it.life);
        }
        if (!it.fuse.empty())
            facts += (facts.empty() ? "" : "   ") + std::string{"Fused: "} + ItemName(it.fuse) +
                     (it.fuse_life != INT32_MIN ? " (" + std::to_string(it.fuse_life) + ")" : "");
        if (it.count != INT32_MIN)
            facts += (facts.empty() ? "" : "   ") + std::string{"x"} + std::to_string(it.count);
        if (it.heal != INT32_MIN && it.heal > 0)
            facts += (facts.empty() ? "" : "   ") + std::string{"Hearts "} + Hearts(it.heal);
        if (it.time != INT32_MIN && it.time > 0)
            facts += (facts.empty() ? "" : "   ") + std::string{"Lv "} + std::to_string(std::max(0, it.level)) +
                     " \xC2\xB7 " + Duration(it.time);
        if (it.equipped)
            facts += (facts.empty() ? "" : "   ") + std::string{"Equipped"};
        Text("totk.inv.sel.facts", facts);
    }
    PublishActions("totk.inv.act", SrcInventory, has_sel ? rows[static_cast<size_t>(sel)].first : -1,
                   has_sel ? rows[static_cast<size_t>(sel)].second : nullptr);
}

// The Ready page's quick picker (Breath of the Wild's, for the three gear corners): flag quick 1 weapons, 2 bows,
// 3 shields, 0 closed. The equipped item is selected until a cell is tapped; its action (equip / unequip) is the
// picker's confirm button.
void Module::PublishQuick(bool shown) {
    const int q = shown ? static_cast<int>(std::clamp<s64>(Get("@flag:quick", 0), 0, 3)) : 0;
    if (q != quick_open) { // opened, closed or switched: the equipped item is selected again
        quick_open = q;
        quick_tap = -1;
    }
    I64("totk.quick.open", q > 0);
    const int cat = q - 1; // Weapons, Bows, Shields
    std::vector<const Item*> items;
    if (q > 0)
        for (const auto& it : model.pouch[cat])
            if (!it.actor.empty() && items.size() < 21)
                items.push_back(&it);
    static constexpr const char* Titles[] = {"", "Weapons", "Bows", "Shields"};
    Text("totk.quick.title", Titles[q]);
    I64("totk.quick.count", static_cast<s64>(items.size()));
    I64("totk.quick.empty", q > 0 && items.empty());
    s64 sel = quick_tap.load();
    if (sel < 0 || sel >= static_cast<s64>(items.size()))
        for (size_t i = 0; i < items.size() && sel < 0; ++i)
            if (items[i]->equipped)
                sel = static_cast<s64>(i);
    if (sel >= static_cast<s64>(items.size()))
        sel = -1;
    for (size_t i = 0; i < items.size(); ++i) {
        const Item& it = *items[i];
        const std::string p = "totk.quick." + std::to_string(i);
        I64(p + ".on", 1);
        Text(p + ".icon", std::string{Prefix} + "icon/" + it.actor);
        I64(p + ".equipped", it.equipped);
        I64(p + ".selected", static_cast<s64>(i) == sel);
        const int max = it.life != INT32_MIN ? MaxLife(it.actor) : -1;
        const int pct = max > 0 ? std::clamp(static_cast<int>(std::lround(100.0 * std::max(0, it.life) / max)), 0, 100) : -1;
        I64(p + ".durability_pct", pct);
        I64(p + ".has_life", pct >= 0);
        I64(p + ".low", pct >= 0 && pct <= 20);
        I64(p + ".fused", !it.fuse.empty());
        if (!it.fuse.empty())
            Text(p + ".fuse_icon", std::string{Prefix} + "icon/" + it.fuse);
    }
    const Item* picked = sel >= 0 ? items[static_cast<size_t>(sel)] : nullptr;
    I64("totk.quick.sel.has", picked != nullptr);
    if (picked) {
        Text("totk.quick.sel.name", ItemName(picked->actor));
        std::string facts;
        if (picked->life != INT32_MIN) {
            const int max = MaxLife(picked->actor);
            facts = max > 0 ? "Durability " + std::to_string(std::clamp(static_cast<int>(std::lround(100.0 * std::max(0, picked->life) / max)), 0, 100)) + "%"
                            : "Durability " + std::to_string(picked->life);
        }
        if (!picked->fuse.empty())
            facts += (facts.empty() ? "" : "   ") + std::string{"Fused: "} + ItemName(picked->fuse);
        if (picked->equipped)
            facts += (facts.empty() ? "" : "   ") + std::string{"Equipped"};
        Text("totk.quick.sel.facts", facts);
    }
    PublishActions("totk.quick.act", SrcQuick, cat, picked);
}

// One source's actions for its selected item (it null: none): refreshed at the live rate, or at once for another item.
const std::vector<Act>& Module::ActionsFor(Source src, int cat, const Item& it, const std::optional<TotkNativeActions>& n) {
    Cached& c = cached[src];
    if (c.cat == cat && c.slot == it.slot && c.actor == it.actor && c.equipped == it.equipped && tick - c.at < LiveEvery)
        return c.acts;
    c = {cat, it.slot, it.actor, it.equipped, tick, {}};
    if (!n) {
        c.acts = OfflineActions(cat, it);
        return c.acts;
    }
    if (it.slot < 0)
        return c.acts;
    TotkNativeAction out[3]{};
    const u32 count = std::min<u32>(n->item_actions(n->userdata, cat, it.slot, out), 3);
    for (u32 k = 0; k < count; ++k)
        c.acts.push_back({out[k].id ? out[k].id : "", out[k].reason ? out[k].reason : "", out[k].enabled != 0});
    return c.acts;
}

// <prefix>.count, and per button k (0..2) <prefix>.k.on / .enabled / .disabled / .label / .reason_text / .reason.
void Module::PublishActions(const std::string& prefix, Source src, int cat, const Item* it) {
    const auto n = Native();
    static const std::vector<Act> None;
    const std::vector<Act>& acts = it ? ActionsFor(src, cat, *it, n) : None;
    if (!it)
        cached[src] = {};
    I64(prefix + ".count", static_cast<s64>(acts.size()));
    for (size_t k = 0; k < 3; ++k) {
        const std::string p = prefix + "." + std::to_string(k);
        const Act* a = k < acts.size() ? &acts[k] : nullptr;
        I64(p + ".on", a != nullptr);
        I64(p + ".enabled", a && a->enabled);
        I64(p + ".disabled", a && !a->enabled);
        if (!a)
            continue;
        Text(p + ".label", ActionWord(a->id, 1));
        Text(p + ".reason", a->reason);
        Text(p + ".reason_text", a->enabled ? std::string{} : ReasonText(a->reason, n.has_value()));
    }
    std::scoped_lock lock{act_mutex};
    Chosen& c = chosen[src];
    c.valid = it != nullptr && !acts.empty();
    if (!c.valid)
        return;
    c.cat = cat;
    c.slot = it->slot;
    c.partition = model.partition;
    c.gen = model.pouch_gen;
    c.actor = it->actor;
    c.name = ItemName(it->actor);
    c.acts = acts;
}

EdenDsmodBool Module::Action(std::string_view name, s64 argument) {
    if (name == "totk.select") {
        if (argument < 0 || argument > 20)
            return EDEN_DSMOD_FALSE;
        quick_tap = argument;
        return EDEN_DSMOD_TRUE;
    }
    if (name != "totk.act" || argument < 0 || argument >= 8 * SourceCount)
        return EDEN_DSMOD_FALSE;
    const auto src = static_cast<size_t>(argument / 8), k = static_cast<size_t>(argument % 8);
    std::scoped_lock lock{act_mutex};
    const Chosen& c = chosen[src];
    if (!c.valid || k >= c.acts.size())
        return EDEN_DSMOD_FALSE;
    const Act& a = c.acts[k];
    const auto refuse = [&](const std::string& reason) {
        request = {0, a.id, c.name, "refused", reason, true, std::chrono::steady_clock::now(),
                   std::chrono::steady_clock::now(), {}};
        return EDEN_DSMOD_FALSE;
    };
    if (!native)
        return refuse("notAvailableYet");   // Eden Duo: nothing is ever requested
    if (!request.final)
        return EDEN_DSMOD_FALSE;            // one at a time: a tap while one runs is dropped, as in TotK Native
    if (!a.enabled)
        return refuse(a.reason);
    if (c.gen <= 0 || c.partition < 0)
        return refuse("itemMoved");
    const s32 seq = native->request(native->userdata, a.id.c_str(), CatKeys[static_cast<size_t>(c.cat)], c.slot,
                                    c.actor.c_str(), c.partition, c.gen);
    if (seq <= 0)
        return refuse(seq == -1 ? "busy" : seq == -2 ? "notNow" : seq == -3 ? "unsupported" : "invalid");
    request = {seq, a.id, c.name, "pending", "", false, std::chrono::steady_clock::now(), {}, {}};
    return EDEN_DSMOD_TRUE;
}

// Sample thread: the pending request's outcome, from the action host (which ends it itself when the game's frame end
// stops: "stale", "timeout").
void Module::PollRequest() {
    std::string line;
    {
        std::scoped_lock lock{act_mutex};
        if (!request.final && request.seq > 0 && native) {
            char buf[256];
            const size_t len = native->result(native->userdata, request.seq, buf, sizeof buf);
            const json r = len > 0 && len < sizeof buf ? json::parse(std::string_view{buf, len}, nullptr, false) : json{};
            const std::string state = r.is_object() ? Str(r, "state") : std::string{};
            const auto now = std::chrono::steady_clock::now();
            if (!state.empty() && state != "pending") {
                request.state = state == "done" || state == "refused" ? state : "failed";
                request.reason = Str(r, "reason");
                if (request.state == "failed" && state != "failed" && request.reason.empty())
                    request.reason = state;
                request.final = true;
                request.ended = now;
                full_soon = true; // the pouch changed (or may have): read it again now
            } else if (state.empty() && now - request.at > std::chrono::seconds{8}) {
                request.state = "failed";
                request.reason = "noAnswer";
                request.final = true;
                request.ended = now;
            }
        }
        if (request.logged != request.state && !request.action.empty()) {
            request.logged = request.state;
            line = "TotK item action " + std::to_string(request.seq) + " " + request.action + " " + request.name + ": " +
                   request.state + (request.reason.empty() ? "" : " " + request.reason);
        }
    }
    if (!line.empty() && host.log)
        host.log(host.userdata, EDEN_DSMOD_LOG_INFO, line.c_str());
}

// totk.act.*: the latest request's line, shown while it runs and for five seconds after it ends.
void Module::PublishRequest() {
    Request r;
    {
        std::scoped_lock lock{act_mutex};
        r = request;
    }
    const bool shown = !r.action.empty() && (!r.final || std::chrono::steady_clock::now() - r.ended < std::chrono::seconds{5});
    const int state = !shown ? 0 : r.state == "pending" ? 1 : r.state == "done" ? 2 : r.state == "refused" ? 3 : 4;
    I64("totk.act.state", state);
    I64("totk.act.seq", shown ? r.seq : 0);
    I64("totk.act.pending", state == 1);
    I64("totk.act.done", state == 2);
    I64("totk.act.refused", state == 3);
    I64("totk.act.failed", state == 4);
    Text("totk.act.reason", shown ? r.reason : std::string{});
    const bool with_native = Native().has_value();
    std::string line;
    if (state == 1)
        line = ActionWord(r.action, 1) + ": " + r.name + "\xE2\x80\xA6";
    else if (state == 2)
        line = ActionWord(r.action, 2) + ": " + r.name;
    else if (state == 3)
        line = "Not done: " + ReasonText(r.reason, with_native);
    else if (state == 4)
        line = "Failed: " + ReasonText(r.reason, with_native);
    Text("totk.act.line", line);
    I64("totk.act.ready", model.act_ok);
    Text("totk.act.ready_reason", model.act_reason);
}

// ---------- images ----------

using Image = WonderAssets::Image;

std::optional<Image> Module::Layout(std::string_view texture) {
    std::scoped_lock lock{image_mutex};
    if (!combined_tried) {
        combined_tried = true;
        if (const auto blarc = romfs->Read("UI/LayoutArchive/Common.Product.140.Nin_NX_NVN.blarc.zs"))
            combined = WonderAssets::SarcMember(*blarc, "timg/__Combined.bntx");
    }
    if (!combined)
        return std::nullopt;
    return WonderAssets::DecodeBntx(*combined, texture, astc);
}

std::optional<Image> Module::Icon(std::string_view actor) {
    if (actor.empty() || actor.find('/') != std::string_view::npos || actor.find("..") != std::string_view::npos)
        return std::nullopt;
    auto bntx = romfs->Read("UI/Tex/Icon/" + std::string{actor} + ".bntx.zs");
    // icon/<actor>_<effect>: a cooked meal's picture named after its effect (Item_Cook_C_17_AttackUp), else the
    // actor's own
    if (!bntx && actor.rfind('_') != std::string_view::npos)
        bntx = romfs->Read("UI/Tex/Icon/" + std::string{actor.substr(0, actor.rfind('_'))} + ".bntx.zs");
    if (!bntx)
        return std::nullopt;
    WonderAssets::AstcDecoder a;
    {
        std::scoped_lock lock{image_mutex};
        a = astc;
    }
    return WonderAssets::DecodeBntx(*bntx, "", a);
}

// map/<G|S|U>/<1|2>/<col>_<row>: the tile TotK Native's art stage picks (most revealed variant),
// named in the package's map_tiles.txt (generated from a romfs listing; names only).
std::optional<Image> Module::MapTile(std::string_view key) {
    std::string path;
    {
        std::scoped_lock lock{image_mutex};
        if (!tiles) {
            tiles.emplace();
            const auto list = romfs->Read("file:map_tiles.txt");
            std::string_view all = list ? std::string_view{reinterpret_cast<const char*>(list->data()), list->size()}
                                        : std::string_view{};
            while (!all.empty()) {
                const size_t nl = all.find('\n');
                std::string_view line = all.substr(0, nl);
                all = nl == std::string_view::npos ? std::string_view{} : all.substr(nl + 1);
                // UI/Map/MainField/G_05-03_01_TT.bntx.zs
                const size_t at = line.rfind('/');
                if (at == std::string_view::npos || line.size() < at + 11)
                    continue;
                const std::string_view n = line.substr(at + 1);
                const int col = std::atoi(std::string{n.substr(2, 2)}.c_str());
                const int row = std::atoi(std::string{n.substr(5, 2)}.c_str());
                const int lod = n[9] - '0';
                char k[32];
                std::snprintf(k, sizeof k, "%c/%d/%d_%d", n[0], lod, col, row);
                (*tiles)[k] = std::string{line};
            }
        }
        const auto it = tiles->find(std::string{key});
        if (it == tiles->end())
            return std::nullopt;
        path = it->second;
    }
    const auto bntx = romfs->Read(path);
    if (!bntx)
        return std::nullopt;
    WonderAssets::AstcDecoder a;
    {
        std::scoped_lock lock{image_mutex};
        a = astc;
    }
    return WonderAssets::DecodeBntx(*bntx, "", a);
}

// gen/clear: a transparent pixel (the map areas' base picture: the tiles are overlays).
// gen/disc/<size>/<AARRGGBB>: the colour outside a centred disc, clear inside, with a thin ring:
// rounds the square minimap.
std::optional<Image> Module::Generated(std::string_view key) {
    if (key == "clear")
        return Image{1, 1, std::vector<u8>(4, 0)};
    if (key.starts_with("circle/")) { // circle/<size>/<AARRGGBB>: a filled disc, antialiased
        const auto rest = key.substr(7);
        const size_t slash = rest.find('/');
        if (slash == std::string_view::npos)
            return std::nullopt;
        const int size = std::atoi(std::string{rest.substr(0, slash)}.c_str());
        const u32 argb = static_cast<u32>(std::strtoul(std::string{rest.substr(slash + 1)}.c_str(), nullptr, 16));
        if (size < 2 || size > 2048)
            return std::nullopt;
        Image im{static_cast<u32>(size), static_cast<u32>(size), std::vector<u8>(static_cast<size_t>(size) * size * 4)};
        const double c = size / 2.0;
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x) {
                const double cover = std::clamp(c - std::hypot(x + 0.5 - c, y + 0.5 - c) + 0.5, 0.0, 1.0);
                u8* p = &im.rgba[(static_cast<size_t>(y) * size + x) * 4];
                p[0] = argb >> 16 & 255;
                p[1] = argb >> 8 & 255;
                p[2] = argb & 255;
                p[3] = static_cast<u8>(std::lround(cover * (argb >> 24 & 255)));
            }
        return im;
    }
    if (key.starts_with("disc/")) {
        const auto rest = key.substr(5);
        const size_t slash = rest.find('/');
        if (slash == std::string_view::npos)
            return std::nullopt;
        const int size = std::atoi(std::string{rest.substr(0, slash)}.c_str());
        const u32 argb = static_cast<u32>(std::strtoul(std::string{rest.substr(slash + 1)}.c_str(), nullptr, 16));
        if (size < 16 || size > 2048)
            return std::nullopt;
        Image im{static_cast<u32>(size), static_cast<u32>(size), std::vector<u8>(static_cast<size_t>(size) * size * 4)};
        const double c = size / 2.0, r = size / 2.0 - 4;
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x) {
                const double d = std::hypot(x + 0.5 - c, y + 0.5 - r - 4);
                u8* p = &im.rgba[(static_cast<size_t>(y) * size + x) * 4];
                // outside: the page colour (antialiased over one pixel)
                const double out = std::clamp(d - r + 0.5, 0.0, 1.0);
                // the ring: cream, 3 px, just inside the edge (TotK Native draws INK at 0xc0)
                const double ring = std::clamp(1.5 - std::abs(d - (r - 3)), 0.0, 1.0) * (0xc0 / 255.0);
                const double a_out = out * (argb >> 24 & 255) / 255.0;
                const double a = a_out + ring * (1 - a_out);
                if (a <= 0)
                    continue;
                const auto mix = [&](int shift, double ink) {
                    const double page = (argb >> shift & 255);
                    return static_cast<u8>(std::lround((page * a_out + ink * ring * (1 - a_out)) / a));
                };
                p[0] = mix(16, 0xe9);
                p[1] = mix(8, 0xe5);
                p[2] = mix(0, 0xd5);
                p[3] = static_cast<u8>(std::lround(a * 255));
            }
        return im;
    }
    return std::nullopt;
}

EdenDsmodBool Module::LoadImage(std::string_view key, void* receiver, EdenDsmodImageSink sink) {
    if (!key.starts_with(Prefix))
        return EDEN_DSMOD_FALSE;
    const auto k = key.substr(Prefix.size());
    std::optional<Image> im;
    std::shared_ptr<const WonderFont::FontAtlas> font;
    if (k.starts_with("icon/"))
        im = Icon(k.substr(5));
    else if (k.starts_with("lyt/"))
        im = Layout(k.substr(4));
    else if (k.starts_with("map/"))
        im = MapTile(k.substr(4));
    else if (k.starts_with("gen/"))
        im = Generated(k.substr(4));
    else if (k.starts_with("font/")) {
        font = WonderFont::BuildFontAtlas(romfs->AsWonderReader(), k.substr(5));
        if (!font || font->atlas.rgba.empty())
            return EDEN_DSMOD_FALSE;
        sink(receiver, font->atlas.width, font->atlas.height, font->atlas.rgba.data(), font->atlas.rgba.size());
        return EDEN_DSMOD_TRUE;
    }
    if (!im || im->rgba.empty())
        return EDEN_DSMOD_FALSE;
    sink(receiver, im->width, im->height, im->rgba.data(), im->rgba.size());
    return EDEN_DSMOD_TRUE;
}

// ---------- ABI ----------

EdenDsmodBool SupportsBuild(const char* build_id) {
    // Other builds load too: the page then says which version it needs (no memory is read).
    return build_id ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
}
void* Create(const EdenDsmodHostApi* host, const char*) {
    try {
        if (!dsmod_sdk::HostAbiMatches(host) || host->title_id != TitleId || !host->read_memory)
            return nullptr;
        return new Module{*host};
    } catch (...) {
        return nullptr;
    }
}
void Destroy(void* p) {
    try {
        delete static_cast<Module*>(p);
    } catch (...) {
    }
}
void SampleCallback(void* p, const EdenDsmodHostApi* host) {
    static std::atomic_flag reported = ATOMIC_FLAG_INIT;
    try {
        if (p && host)
            static_cast<Module*>(p)->Sample(*host);
    } catch (...) {
        t_host = nullptr;
        if (!reported.test_and_set() && host && host->log)
            host->log(host->userdata, EDEN_DSMOD_LOG_ERROR, "TotK DSMod sample failed");
    }
}
void TickCallback(void*, const EdenDsmodHostApi*) {}
void ConfigureCallback(void* p, const EdenDsmodHostExtensions* ext) {
    try {
        if (p)
            static_cast<Module*>(p)->Configure(ext);
    } catch (...) {
    }
}
EdenDsmodBool ActionCallback(void* p, const char* name, std::int64_t argument) {
    try {
        if (p && name)
            return static_cast<Module*>(p)->Action(name, argument);
    } catch (...) {
    }
    return EDEN_DSMOD_FALSE;
}
EdenDsmodBool LoadImageCallback(void* p, const EdenDsmodHostApi*, const char* key, void* receiver,
                                EdenDsmodImageSink sink) {
    try {
        if (p && key && sink)
            return static_cast<Module*>(p)->LoadImage(key, receiver, sink);
    } catch (...) {
    }
    return EDEN_DSMOD_FALSE;
}
EdenDsmodBool DecodeFontCallback(void* p, const std::uint8_t* bytes, size_t size, void* receiver,
                                 EdenDsmodFontSink sink) {
    try {
        if (p && bytes && sink)
            return static_cast<Module*>(p)->DecodeFont(bytes, size, receiver, sink);
    } catch (...) {
    }
    return EDEN_DSMOD_FALSE;
}

const EdenDsmodModuleApi ModuleApi{EDEN_DSMOD_MODULE_ABI_VERSION,
                                   sizeof(EdenDsmodModuleApi),
                                   0,
                                   EDEN_DSMOD_MODULE_ABI_HASH,
                                   TitleId,
                                   "Tears of the Kingdom DSMod",
                                   EDEN_DSMOD_CAP_EXTENSIONS | EDEN_DSMOD_CAP_ROMFS_READ,
                                   &SupportsBuild,
                                   &Create,
                                   &Destroy,
                                   &SampleCallback,
                                   &TickCallback};
const EdenDsmodModuleExtensions ModuleExtensions{EDEN_DSMOD_EXT_VERSION, sizeof(EdenDsmodModuleExtensions),
                                                 EDEN_DSMOD_EXT_HASH,    &ConfigureCallback,
                                                 &ActionCallback,        &LoadImageCallback};
const EdenDsmodFontExtensions FontExtensions{EDEN_DSMOD_FONT_EXT_VERSION, sizeof(EdenDsmodFontExtensions),
                                             EDEN_DSMOD_FONT_EXT_HASH, &DecodeFontCallback};
} // namespace

// The TotK Native port's action table (totk_native_actions.h). Not exported from Eden's build of this module: only the
// port, which links the module in, calls it.
extern "C" void totk_native_port_bind_actions(void* instance, const TotkNativeActions* actions) {
    try {
        if (instance)
            static_cast<Module*>(instance)->BindNative(actions);
    } catch (...) {
    }
}

DSMOD_SDK_EXPORT_MODULE(ModuleApi)
DSMOD_SDK_EXPORT_EXTENSIONS(ModuleExtensions)
DSMOD_SDK_EXPORT_FONT_EXTENSIONS(FontExtensions)
