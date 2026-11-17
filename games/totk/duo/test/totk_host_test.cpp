// SPDX-License-Identifier: GPL-3.0-or-later
// Desktop test of the Tears of the Kingdom Eden Duo package, no game running and no device (build: CMakeLists.txt
// with -DDUO_HOST_TEST=ON -DDUO_TITLE=0100F2C0115B6000; README.md here):
//
//   duo_host_test <package_root> <romfs_dir> <main.bin> <out_prefix> [scene] [otherbuild]
//
// "main" is the real 1.4.3 main image (so the reader's code guard sees the real words), mapped
// privately with its GOT entries relocated; the game state behind it (GameDataMgr tables, pouch,
// player actor, pause-menu and event flags) is laid out in ordinary memory the way TotK Native's
// reader test does (games/totk/tests/companion/test_companion.c), filled with that app's "ready"
// fixture. The RomFS is a folder of the player's own files. Pages are reached by tapping the
// command bar, and each is written as <out_prefix>-<page>.png. The PNGs show art decoded from the player's own files:
// they stay on the machine that ran the test.
//   scene: ready (default), menu (pause menu open), event (cutscene), sky, depths, title, food (low hearts), loading,
//          overlays (the pause-menu band on Inventory and Map),
//          actions (item actions through a fake runtime: DuoPort ABI 3 functions standing in for companion_actions.c),
//          actions-off (the same taps with no action functions, as in Eden Duo: every action shown disabled),
//          actions-errors (the fake runtime answers busy, refused now, unsupported, then a request that ends refused
//          "itemMoved" and one that ends failed "timeout": the page's result line and reason for each)
// Besides the pages, the common state the host hands to the port (duo_state.h, Tears of the Kingdom's table) is
// checked against the fixture: status, in game, menu and hearts.
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <stb_image_write.h>

#include "duo_host.h"
#include "duo_state.h"

extern "C" uint32_t companion_hash(const char* s);   // the reader's flag-name hash (totk_reader/companion_snapshot.c)

namespace {
std::string g_romfs;
uint64_t g_frames;
CompanionState g_state;   // the last common state the host handed to the port (tick thread; read after Stop)
uint64_t g_states;

int SafeRead(uint64_t a, void* out, size_t n) {
    static thread_local int fds[2] = {-1, -1};
    if (fds[0] < 0 && pipe(fds) != 0)
        return 0;
    auto* dst = static_cast<uint8_t*>(out);
    while (n != 0) {
        const size_t chunk = n > 4096 ? 4096 : n;
        const ssize_t w = write(fds[1], reinterpret_cast<const void*>(static_cast<uintptr_t>(a)), chunk);
        if (w <= 0 || read(fds[0], dst, static_cast<size_t>(w)) != w)
            return 0;
        a += static_cast<uint64_t>(w);
        dst += w;
        n -= static_cast<size_t>(w);
    }
    return 1;
}
int64_t RomfsRead(const char* path, uint64_t offset, void* out, uint64_t n) {
    const std::string full = g_romfs + "/" + path;
    struct stat st;
    if (stat(full.c_str(), &st) != 0)
        return -1;
    if (*path == 0)
        return S_ISDIR(st.st_mode) ? 0 : -1;
    if (!S_ISREG(st.st_mode))
        return -1;
    if (out == nullptr)
        return st.st_size;
    const int fd = open(full.c_str(), O_RDONLY);
    if (fd < 0)
        return -1;
    const ssize_t got = pread(fd, out, n, static_cast<off_t>(offset));
    close(fd);
    return got;
}
int64_t NoAoc(const char*, uint64_t, void*, uint64_t) { return -1; }

// ---------- a fake runtime's item actions (DuoPort ABI 3; the app's are runtime/companion_actions.c) ----------
// Rules: gear equips (unequips the equipped one), food eats, materials eat / take out (fuse disabled: not aiming).
// A request is pending for its first PendingPolls result calls, then done; one at a time, as the runtime's.
// g_answer (actions-errors): what the next request gets instead: a refusal from request() (-1 busy, -2 refused now,
// -3 unsupported), or a seq whose result ends in another state ("refused itemMoved", "failed timeout").
struct FakeRequest { std::string action, category, actor; int slot, partition; long long gen; };
struct FakeAnswer { int code = 0; std::string state = "done", reason; };
std::mutex g_fake_mutex;
std::vector<FakeRequest> g_requests;
FakeAnswer g_answer, g_running;
int g_seq = 0, g_polls = 0;
bool g_pending = false;
void Answer(FakeAnswer a) { std::scoped_lock lock{g_fake_mutex}; g_answer = std::move(a); }
constexpr int PendingPolls = 40;
constexpr long long FakeGen = 7;
unsigned FakeItemActions(int key, int slot, DuoPortAction out[3]) {
    switch (key) {
    case 0: case 1: case 2: case 4:
        out[0] = {slot == 0 ? "unequip" : "equip", 1, ""};
        return 1;
    case 6:
        out[0] = {"eat", 1, ""};
        return 1;
    case 5:
        out[0] = {"eat", 1, ""};
        out[1] = {"takeOut", 1, ""};
        out[2] = {"fuseArrow", 0, "notAiming"};
        return 3;
    default:
        return 0;
    }
}
void FakeActionState(int* ok, const char** reason) { *ok = 1; *reason = ""; }
long long FakePouchGen() { return FakeGen; }
int FakeRequestOne(const char* action, const char* category, int slot, const char* actor, int partition, long long gen) {
    std::scoped_lock lock{g_fake_mutex};
    if (g_pending) return -1;
    g_requests.push_back({action, category, actor, slot, partition, gen});
    if (g_answer.code < 0) return g_answer.code;   // refused before anything is queued
    g_running = g_answer;
    g_pending = true;
    g_polls = 0;
    return ++g_seq;
}
size_t FakeResult(int seq, char* out, size_t cap) {
    std::scoped_lock lock{g_fake_mutex};
    if (seq != g_seq || seq <= 0) return 0;
    const bool done = !g_pending || ++g_polls > PendingPolls;
    if (done) g_pending = false;
    const int n = std::snprintf(out, cap, "{\"seq\":%d,\"state\":\"%s\",\"reason\":\"%s\"}", seq,
                                done ? g_running.state.c_str() : "pending", done ? g_running.reason.c_str() : "");
    return n > 0 && static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : 0;
}
size_t Requests() { std::scoped_lock lock{g_fake_mutex}; return g_requests.size(); }
FakeRequest RequestAt(size_t i) { std::scoped_lock lock{g_fake_mutex}; return g_requests.at(i); }
void Log(int level, const char* line) {
    if (level >= 2 || std::strstr(line, "TotK") || std::strstr(line, "DSMod"))
        std::fprintf(stderr, "[duo:%d] %s\n", level, line);
}

// ---------- the fake game state (layout as test_companion.c) ----------
void* Alloc(size_t n) { return std::calloc(1, n); }
void Put64(void* at, uint64_t v) { std::memcpy(at, &v, 8); }
void Put32(void* at, uint32_t v) { std::memcpy(at, &v, 4); }
void PutF(void* at, float v) { std::memcpy(at, &v, 4); }
struct Type { uint16_t base, stride; };
constexpr Type BOOL_{0x008, 0x18}, INT{0x0c8, 0x28}, INTARR{0x128, 0x40}, FLOAT_{0x188, 0x28}, ENUM_{0x248, 0x38},
    ENUMARR{0x2a8, 0x50}, VEC3{0x3c8, 0x40}, STRARR{0x668, 0x90};
struct Pending { Type t; std::vector<uint32_t> hash; std::vector<uint8_t*> entry; };
std::vector<Pending> tables;
uint8_t* FlagHash(Type t, uint32_t hash) {
    Pending* p = nullptr;
    for (auto& x : tables) if (x.t.base == t.base) p = &x;
    if (!p) { tables.push_back({t, {}, {}}); p = &tables.back(); }
    p->hash.push_back(hash);
    p->entry.push_back(static_cast<uint8_t*>(Alloc(t.stride)));
    return p->entry.back();
}
uint8_t* Flag(Type t, const char* name) { return FlagHash(t, companion_hash(name)); }
void Build(uint8_t* g) {
    for (auto& p : tables) {
        const uint32_t n = static_cast<uint32_t>(p.hash.size()), cap = n * 2 + 1;
        auto* entries = static_cast<uint8_t*>(Alloc(size_t{n} * p.t.stride));
        auto* index = static_cast<uint32_t*>(Alloc(8u * cap));
        for (uint32_t e = 0; e < n; ++e) {
            std::memcpy(entries + size_t{e} * p.t.stride, p.entry[e], p.t.stride);
            uint32_t slot = p.hash[e] % cap;
            while (index[slot * 2]) slot = (slot + 1) % cap;
            index[slot * 2] = p.hash[e];
            index[slot * 2 + 1] = e;
        }
        uint8_t* t = g + p.t.base;
        Put32(t + 8, n); Put64(t + 0x10, reinterpret_cast<uintptr_t>(entries));
        Put64(t + 0x18, reinterpret_cast<uintptr_t>(index)); Put32(t + 0x20, n); Put32(t + 0x24, cap);
    }
}
void SetInt(const char* n, int32_t v) { Put32(Flag(INT, n) + 0x1c, static_cast<uint32_t>(v)); }
void SetFloat(const char* n, float v) { PutF(Flag(FLOAT_, n) + 0x1c, v); }
void SetBool(const char* n, int v) { Flag(BOOL_, n)[0x16] = static_cast<uint8_t>(v); }
void SetEnum(const char* n, const char* v) { Put32(Flag(ENUM_, n) + 0x1c, companion_hash(v)); }
void* Words(Type t, const char* name, uint32_t count, unsigned size) {
    uint8_t* e = Flag(t, name);
    void* data = Alloc(size_t{count} * size);
    Put32(e + 0x20, count); Put64(e + 0x28, reinterpret_cast<uintptr_t>(data));
    return data;
}
int32_t* IntArray(const char* n, uint32_t count) {
    auto* d = static_cast<int32_t*>(Words(INTARR, n, count, 4));
    for (uint32_t i = 0; i < count; ++i) d[i] = -1;
    return d;
}
uint32_t* EnumArray(const char* n, uint32_t count, const char* fill) {
    auto* d = static_cast<uint32_t*>(Words(ENUMARR, n, count, 4));
    for (uint32_t i = 0; i < count; ++i) d[i] = companion_hash(fill);
    return d;
}
uint8_t* StringArray(const char* name, uint32_t count) {
    uint8_t* e = Flag(STRARR, name);
    auto* data = static_cast<uint8_t*>(Alloc(size_t{count} * 0x50));
    for (uint32_t i = 0; i < count; ++i) {
        Put64(data + i * 0x50, reinterpret_cast<uintptr_t>(data + i * 0x50 + 0xc));
        Put32(data + i * 0x50 + 8, 64);
    }
    Put32(e + 0x70, count); Put64(e + 0x78, reinterpret_cast<uintptr_t>(data));
    return data;
}
void SetString(uint8_t* a, uint32_t i, const char* s) { std::strcpy(reinterpret_cast<char*>(a) + i * 0x50 + 0xc, s); }

struct Entry { const char* name; int life_or_count; const char* fuse; int fuse_life; bool equipped; };
void Category(const char* cat, uint32_t slots, bool life, bool fuse, bool equip_index, std::initializer_list<Entry> items) {
    char f[96];
    std::snprintf(f, sizeof f, "Pouch.%s.Content.Name", cat);
    uint8_t* names = StringArray(f, slots * 2);
    std::snprintf(f, sizeof f, life ? "Pouch.%s.Content.Life" : "Pouch.%s.Content.StockNum", cat);
    int32_t* values = IntArray(f, slots * 2);
    uint8_t* fuses = nullptr; int32_t* fuse_life = nullptr;
    if (fuse) {
        std::snprintf(f, sizeof f, "Pouch.%s.Content.Combined.Name", cat); fuses = StringArray(f, slots * 2);
        std::snprintf(f, sizeof f, "Pouch.%s.Content.Combined.Life", cat); fuse_life = IntArray(f, slots * 2);
    }
    int32_t* eq = nullptr;
    if (equip_index) { std::snprintf(f, sizeof f, "Pouch.%s.EquipIndex", cat); eq = IntArray(f, 2); }
    uint32_t i = 0;
    for (const auto& it : items) {
        SetString(names, i, it.name);
        values[i] = it.life_or_count;
        if (fuses && it.fuse) { SetString(fuses, i, it.fuse); fuse_life[i] = it.fuse_life; }
        if (eq && it.equipped) eq[0] = static_cast<int32_t>(i);
        ++i;
    }
}

struct Scene { std::string name; bool menu = false, event = false, title = false; const char* layer = "Ground"; float y = 120;
               int life = 47; bool loading = false; };

uint8_t* g_pouch = nullptr;   // the pause-menu byte lives at +0x1fcc0 (the overlay scenes flip it)
void BuildGame(uint8_t* main, const Scene& sc) {
    constexpr uint32_t GDM_SLOT = 0x3ad66b0, POUCH_SLOT = 0x3a81678, SCENE_SLOT = 0x3add238,
                       PLAYER_STATE_SLOT = 0x3a82e70, EVENT_SLOT = 0x3ada558;
    // GOT entries as rtld leaves them: main + target
    static const uint32_t GOT[][2] = {{0x39601e8, POUCH_SLOT}, {0x395ea40, PLAYER_STATE_SLOT},
                                      {0x395dff8, EVENT_SLOT}, {0x395dff0, GDM_SLOT}};
    for (const auto& g : GOT) Put64(main + g[0], reinterpret_cast<uintptr_t>(main) + g[1]);
    auto* g = static_cast<uint8_t*>(Alloc(0xd38));
    Put64(main + GDM_SLOT, reinterpret_cast<uintptr_t>(g));
    SetInt("PlayerStatus.Life", sc.life); SetInt("PlayerStatus.MaxLife", 80); SetInt("PlayerStatus.ExtraLife", 8);
    SetInt("PlayerStatus.BreakLife", 12); SetInt("PlayerStatus.CurrentRupee", 12845); SetInt("PlayerStatus.CurrentMamo", 238);
    SetFloat("PlayerStatus.MaxStamina", 2500); SetFloat("PlayerStatus.ExtraStamina", 500);
    SetFloat("PlayerStatus.Stamina", 1400); SetBool("PlayerStatus.IsTired", 0);   // 56 %: the ring shows
    for (int i = 0; i < 152; ++i) {   // 27 shrines found: 20 cleared, 4 entered, 3 open; the rest hidden or seen
        char f[64];
        std::snprintf(f, sizeof f, "DungeonState.Dungeon%03d", i);
        SetEnum(f, i < 20 ? "Clear" : i < 24 ? "Enter" : i < 27 ? "Open" : i < 40 ? "Appear" : "Hidden");
    }
    SetFloat("PlayerStatus.MaxEnergy", 1800); SetFloat("PlayerStatus.ExtraEnergy", 300);
    SetEnum("PlayerStatus.CurrentSpecialPower", "UltraHand"); SetEnum("MapData.CurrentLayer", sc.layer);
    SetInt("PlayerStatus.CookBuff", 3); SetInt("PlayerStatus.CookBuffLv", 2); SetFloat("PlayerStatus.CookBuffTime", 150);
    static const char* Sages[] = {"Wind", "Fire", "Water", "Electric", "Soul"};
    for (int i = 0; i < 5; ++i) {
        char f[96];
        std::snprintf(f, sizeof f, "PlayerStatus.Companion.%s.IsGet", Sages[i]); SetBool(f, i < 4);
        std::snprintf(f, sizeof f, "PlayerStatus.Companion.%s.IsSummon", Sages[i]); SetBool(f, i == 0 || i == 2);
        std::snprintf(f, sizeof f, "PlayerStatus.Companion.%s.ForUI.RemainingMinute", Sages[i]); SetInt(f, 0);
        std::snprintf(f, sizeof f, "PlayerStatus.Companion.%s.ForUI.RemainingSecond", Sages[i]); SetInt(f, i == 1 ? 12 : 0);
        std::snprintf(f, sizeof f, "PlayerStatus.Companion.%s.JoiningCondition", Sages[i]); SetEnum(f, "Joining");
    }
    Category("Weapon", 20, true, true, true,
             {{"Weapon_Sword_001", 18, "Item_Enemy_77", 12, true}, {"Weapon_Sword_002", 8, "Item_Ore_A", 5, false},
              {"Weapon_Lsword_001", 23, nullptr, 0, false}, {"Weapon_Spear_001", 3, "Item_Enemy_01", 4, false},
              {"Weapon_Sword_003", 24, nullptr, 0, false}});
    Category("Bow", 14, true, false, true, {{"Weapon_Bow_001", 20, nullptr, 0, true}, {"Weapon_Bow_002", 8, nullptr, 0, false}});
    Category("Shield", 20, true, true, true,
             {{"Weapon_Shield_001", 12, "SpObj_FlameThrower_A_01", 30, true}, {"Weapon_Shield_002", 21, nullptr, 0, false}});
    Category("Arrow", 1, false, false, true, {{"NormalArrow", 86, nullptr, 0, true}});
    {
        uint8_t* armor = StringArray("Pouch.Armor.Content.Name", 300);
        EnumArray("Pouch.Armor.Content.ColorVariation", 300, "None");
        SetString(armor, 0, "Armor_001_Head"); SetString(armor, 1, "Armor_001_Upper");
        SetString(armor, 2, "Armor_001_Lower"); SetString(armor, 3, "Armor_002_Head");
        IntArray("Pouch.Armor.EquipIndexes.Head", 2)[0] = 0;
        IntArray("Pouch.Armor.EquipIndexes.Upper", 2)[0] = 1;
        IntArray("Pouch.Armor.EquipIndexes.Bottom", 2)[0] = 2;
    }
    Category("Material", 255, false, false, false,
             {{"Item_Fruit_A", 12}, {"Item_Mushroom_A", 28}, {"Item_Meat_01", 8}, {"Item_Ore_A", 2},
              {"Item_Enemy_77", 37}, {"Item_Enemy_01", 18}, {"Item_PlantGet_A", 24}});
    Category("Food", 60, false, false, false,
             {{"Item_Cook_C_17", 1}, {"Item_Cook_A_01", 3}, {"Item_Cook_A_02", 2}, {"Item_Cook_B_02", 5},
              {"Item_Cook_D_01", 1}});
    {   // recovery in quarter hearts; the first meal is an attack-up dish
        int32_t* heal = IntArray("Pouch.Food.Content.LifeRecover", 120);
        const int32_t h[] = {24, 12, 40, 8, 64};
        for (int i = 0; i < 5; ++i) heal[i] = h[i];
        uint32_t* effect = EnumArray("Pouch.Food.Content.Effect.Type", 120, "None");
        effect[0] = companion_hash("AttackUp");
        int32_t* level = IntArray("Pouch.Food.Content.Effect.Level", 120); level[0] = 2;
        int32_t* time = IntArray("Pouch.Food.Content.Effect.Time", 120); time[0] = 150;
    }
    Category("SpecialParts", 30, false, false, false,
             {{"SpObj_FlameThrower_Capsule_A_01", 3}, {"SpObj_WindGenerator_Capsule_A_01", 14}});
    Category("KeyItem", 100, false, false, false,
             {{"Obj_DRStone_Get", 1}, {"Obj_HeartUtuwa_A_01", 2}, {"Obj_StaminaUtuwa_A_01", 1},
              {"Obj_DungeonClearSeal", 3}});
    Build(g);
    auto* pouch = static_cast<uint8_t*>(Alloc(0x1fd00));
    g_pouch = pouch;
    Put32(pouch + 0x188, 0); Put32(pouch + 0x478, 5); pouch[0x1fcc0] = sc.menu ? 1 : 0;
    Put64(main + POUCH_SLOT, reinterpret_cast<uintptr_t>(pouch));
    auto* player_state = static_cast<uint8_t*>(Alloc(0x700));
    auto* event_mgr = static_cast<uint8_t*>(Alloc(0x100));
    if (sc.event) Put64(event_mgr + 0x58, reinterpret_cast<uintptr_t>(Alloc(16)));
    Put64(main + PLAYER_STATE_SLOT, reinterpret_cast<uintptr_t>(player_state));
    Put64(main + EVENT_SLOT, reinterpret_cast<uintptr_t>(event_mgr));
    const uint64_t flags = (sc.loading ? 0ull : 1ull) | 1ull << 33;   // bit 0 clear: a save is loading
    std::memcpy(player_state + 0x658, &flags, 8);
    if (sc.title) return; // GameData, but no player actor: the title screen
    auto *module = static_cast<uint8_t*>(Alloc(0x200)), *scene = static_cast<uint8_t*>(Alloc(0x60)),
         *components = static_cast<uint8_t*>(Alloc(30 * 8)), *resident = static_cast<uint8_t*>(Alloc(0x30)),
         *links = static_cast<uint8_t*>(Alloc(0x70)), *data = static_cast<uint8_t*>(Alloc(0x48)),
         *actor = static_cast<uint8_t*>(Alloc(0x300)), *name = static_cast<uint8_t*>(Alloc(8));
    Put64(main + SCENE_SLOT, reinterpret_cast<uintptr_t>(module)); Put64(module + 0x1e8, reinterpret_cast<uintptr_t>(scene));
    Put32(scene + 0x50, 14); Put64(scene + 0x58, reinterpret_cast<uintptr_t>(components));
    Put64(components + 13 * 8, reinterpret_cast<uintptr_t>(resident));
    Put32(resident + 0x20, 1); Put64(resident + 0x28, reinterpret_cast<uintptr_t>(links));
    Put64(links + 8, reinterpret_cast<uintptr_t>(data)); Put64(data + 0x40, reinterpret_cast<uintptr_t>(actor));
    std::strcpy(reinterpret_cast<char*>(name), "Player"); Put64(actor + 0x218, reinterpret_cast<uintptr_t>(name));
    PutF(actor + 0x2b4, -254); PutF(actor + 0x2b8, sc.y); PutF(actor + 0x2bc, -1102);
    const float h = 135 * static_cast<float>(M_PI) / 180;   // heading 135 degrees: south-east
    const float rot[9] = {std::cos(h), 0, std::sin(h), 0, 1, 0, -std::sin(h), 0, -std::cos(h)};
    std::memcpy(actor + 0x2c0, rot, sizeof rot);
}

void Run(int ticks, std::vector<uint32_t>& latest, uint32_t& w, uint32_t& h) {
    std::vector<uint32_t> pixels;
    uint64_t serial = 0;
    for (int i = 0; i < ticks; ++i) {
        ++g_frames;
        duo_host_frame();
        std::this_thread::sleep_for(std::chrono::milliseconds{16});
        uint32_t fw, fh;
        if (Duo::TakeFrame(pixels, fw, fh, serial)) { latest = pixels; w = fw; h = fh; }
    }
}
void Tap(uint32_t x, uint32_t y, std::vector<uint32_t>& latest, uint32_t& w, uint32_t& h) {
    Duo::TestTouch(x, y, true); Run(4, latest, w, h);
    Duo::TestTouch(x, y, false); Run(4, latest, w, h);
}
void Swipe(uint32_t x, uint32_t y0, uint32_t y1, std::vector<uint32_t>& latest, uint32_t& w, uint32_t& h) {
    for (int k = 0; k <= 8; ++k) {   // the finger travels in 9 steps, about 130 ms
        Duo::TestTouch(x, static_cast<uint32_t>(y0 + (static_cast<int>(y1) - static_cast<int>(y0)) * k / 8), true);
        Run(1, latest, w, h);
    }
    Duo::TestTouch(x, y1, false); Run(4, latest, w, h);
}
bool Expect(const char* name, const std::string& want) {
    const std::string got = Duo::TestValue(name);
    if (got == want) return true;
    std::fprintf(stderr, "test: FAIL, %s is \"%s\", wanted \"%s\"\n", name, got.c_str(), want.c_str());
    return false;
}
bool ExpectRequest(size_t i, const char* action, const char* category, int slot, const char* actor) {
    if (Requests() <= i) {
        std::fprintf(stderr, "test: FAIL, request %zu was not made\n", i);
        return false;
    }
    const FakeRequest r = RequestAt(i);
    const bool ok = r.action == action && r.category == category && r.slot == slot && r.actor == actor &&
                    r.partition == 0 && r.gen == FakeGen;
    std::fprintf(stderr, "test: request %zu: %s %s/%d %s partition %d gen %lld%s\n", i, r.action.c_str(), r.category.c_str(),
                 r.slot, r.actor.c_str(), r.partition, r.gen, ok ? "" : " (FAIL: not the item tapped)");
    return ok;
}
bool Save(const std::string& path, const std::vector<uint32_t>& px, uint32_t w, uint32_t h) {
    if (px.empty()) return false;
    std::vector<uint32_t> surface(size_t{w} * h);
    Duo::BlitFrame(surface.data(), w, h, w, px, w, h);
    stbi_write_png(path.c_str(), static_cast<int>(w), static_cast<int>(h), 4, surface.data(), static_cast<int>(w) * 4);
    std::fprintf(stderr, "test: wrote %s\n", path.c_str());
    return true;
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 5) {
        std::fprintf(stderr, "usage: %s <package_root> <romfs_dir> <main.bin> <out_prefix> [scene]\n", argv[0]);
        return 2;
    }
    g_romfs = argv[2];
    Scene sc;
    sc.name = argc > 5 ? argv[5] : "ready";
    sc.menu = sc.name == "menu"; sc.event = sc.name == "event"; sc.title = sc.name == "title";
    if (sc.name == "sky") { sc.layer = "Sky"; sc.y = 1500; }
    if (sc.name == "depths") { sc.layer = "UnderGround"; sc.y = -900; }
    if (sc.name == "food") sc.life = 14;   // low hearts: the Food page and card
    sc.loading = sc.name == "loading";
    const size_t main_size = 0x3b00000;
    auto* const main_image = static_cast<uint8_t*>(mmap(nullptr, main_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
    if (main_image == MAP_FAILED) return 2;
    std::FILE* f = std::fopen(argv[3], "rb");
    if (!f) return 2;
    const size_t got = std::fread(main_image, 1, main_size, f);
    std::fclose(f);
    std::fprintf(stderr, "test: main %zu bytes\n", got);
    BuildGame(main_image, sc);

    const std::string data_dir = std::string{argv[4]} + ".data";
    DuoPort port{};
    port.abi = DUO_PORT_ABI;
    port.size = sizeof port;
    port.main_base = reinterpret_cast<uintptr_t>(main_image);
    port.main_size = main_size;
    port.title_id = 0x0100F2C0115B6000ull;
    static uint8_t build[20] = {0x27, 0x71, 0x78, 0xB7, 0xDB, 0xA1, 0xB6, 0xD4, 0x94, 0x9A,
                                      0x84, 0x77, 0x8D, 0x4A, 0xBC, 0x58, 0xB3, 0x1B, 0x34, 0xF5};
    if (argc > 6) build[0] ^= 1; // "otherbuild": another version of the game
    std::memcpy(port.build_id, build, sizeof build);
    port.safe_read = SafeRead;
    port.romfs_read = RomfsRead;
    port.aoc_read = NoAoc;
    port.log = Log;
    port.package_root = argv[1];
    port.data_dir = data_dir.c_str();
    port.frames = [] { return g_frames; };
    port.state = [](const CompanionState* state) {
        g_state = *state;
        ++g_states;
    };
    const bool errors = sc.name == "actions-errors";
    const bool actions = sc.name == "actions" || errors, actions_off = sc.name == "actions-off";
    if (actions) {   // ABI 3: the runtime's item actions (here the fake above); actions-off leaves them NULL
        port.item_actions = FakeItemActions;
        port.action_state = FakeActionState;
        port.pouch_gen = FakePouchGen;
        port.request = FakeRequestOne;
        port.result = FakeResult;
    }
    if (actions || actions_off) Duo::TestWatch("totk.");
    if (!duo_host_start(&port)) return 1;
    Duo::SetSurfaceSize(1240, 1080, true);
    std::vector<uint32_t> latest;
    uint32_t w = 0, h = 0;
    const std::string out = argv[4];
    Run(300, latest, w, h);
    bool ok = Save(out + "-" + sc.name + ".png", latest, w, h);
    if (sc.name == "ready") {   // Breath of the Wild's Ready layout (gen_manifest.py): command bar at y 952-1064
        Tap(493, 1008, latest, w, h); Run(90, latest, w, h);  // command bar: Inventory
        ok &= Save(out + "-inventory.png", latest, w, h);
        Tap(114, 200, latest, w, h); Run(60, latest, w, h);   // first item: its detail strip
        ok &= Save(out + "-inventory-selected.png", latest, w, h);
        Tap(1164, 60, latest, w, h); Run(30, latest, w, h);   // Back to the main page
        Tap(747, 1008, latest, w, h); Run(150, latest, w, h); // command bar: Map
        ok &= Save(out + "-map.png", latest, w, h);
    }
    if (sc.name == "overlays") {   // the pause-menu band on the Inventory and Map pages (BotW's overlay block)
        Tap(493, 1008, latest, w, h); Run(90, latest, w, h);
        g_pouch[0x1fcc0] = 1; Run(60, latest, w, h);
        ok &= Save(out + "-inventory-menu.png", latest, w, h);
        g_pouch[0x1fcc0] = 0; Run(60, latest, w, h);
        Tap(1164, 60, latest, w, h); Run(30, latest, w, h);
        Tap(747, 1008, latest, w, h); Run(150, latest, w, h);
        g_pouch[0x1fcc0] = 1; Run(60, latest, w, h);
        ok &= Save(out + "-map-menu.png", latest, w, h);
        Tap(120, 60, latest, w, h); Run(30, latest, w, h);   // Back is blocked under the band: still the Map
        ok &= Save(out + "-map-menu-tap.png", latest, w, h);
    }
    if (actions || actions_off) {
        // Inventory, Weapons, the second weapon (not equipped): its action is Equip
        Tap(493, 1008, latest, w, h); Run(90, latest, w, h);
        Tap(314, 200, latest, w, h); Run(40, latest, w, h);
        ok &= Expect("totk.inv.sel.has", "1") && Expect("totk.inv.act.count", "1") && Expect("totk.inv.act.0.label", "Equip");
        if (errors) {
            // Each answer of the runtime as the page shows it: the refusals from request() at once, the others when
            // their result ends. A refused or failed request does not block the next tap.
            struct Case { FakeAnswer answer; const char *state, *reason, *line; size_t requests; };
            const Case cases[] = {
                {{-1, "", ""}, "3", "busy", "Not done: Another action is still running", 1},
                {{-2, "", ""}, "3", "notNow", "Not done: Not possible right now", 2},
                {{-3, "", ""}, "3", "unsupported", "Not done: Not supported on this game build", 3},
                {{0, "refused", "itemMoved"}, "3", "itemMoved", "Not done: The item moved; try again", 4},
                {{0, "failed", "timeout"}, "4", "timeout", "Failed: No response from the game", 5},
            };
            for (const Case& c : cases) {
                Answer(c.answer);
                Tap(1112, 1000, latest, w, h);
                Run(c.answer.code < 0 ? 10 : 80, latest, w, h);
                const bool case_ok = Expect("totk.act.state", c.state) && Expect("totk.act.reason", c.reason) &&
                                     Expect("totk.act.line", c.line) && Requests() == c.requests &&
                                     Expect(c.answer.code < 0 || c.answer.state == "refused" ? "totk.act.refused" : "totk.act.failed", "1");
                std::fprintf(stderr, "test: answer %d %s %s: \"%s\"%s\n", c.answer.code, c.answer.state.c_str(),
                             c.answer.reason.c_str(), Duo::TestValue("totk.act.line").c_str(), case_ok ? "" : " (FAIL)");
                ok &= case_ok;
            }
            ok &= ExpectRequest(4, "equip", "weapons", 1, "Weapon_Sword_002");
            ok &= Save(out + "-inventory-failed.png", latest, w, h);
        } else if (actions) {
            ok &= Expect("totk.inv.act.0.enabled", "1") && Expect("totk.inv.act.0.disabled", "0");
            Tap(1112, 1000, latest, w, h);   // the action button: request accepted, pending
            ok &= Expect("totk.act.pending", "1");
            ok &= Save(out + "-inventory-pending.png", latest, w, h);
            Tap(1112, 1000, latest, w, h);   // a second tap while it runs is dropped, never queued
            Run(80, latest, w, h);
            ok &= ExpectRequest(0, "equip", "weapons", 1, "Weapon_Sword_002");
            ok &= Requests() == 1;
            ok &= Expect("totk.act.done", "1") && Expect("totk.act.state", "2");
            const std::string line = Duo::TestValue("totk.act.line");
            std::fprintf(stderr, "test: result line \"%s\"\n", line.c_str());
            ok &= line.starts_with("Equipped: ");
            ok &= Save(out + "-inventory-done.png", latest, w, h);
            // Materials: three actions, the third disabled with the runtime's reason
            Tap(16 + 4 * 134 + 63, 56, latest, w, h); Run(40, latest, w, h);
            Tap(114, 200, latest, w, h); Run(40, latest, w, h);
            ok &= Expect("totk.inv.act.count", "3") && Expect("totk.inv.act.2.label", "Fuse to arrows") &&
                  Expect("totk.inv.act.2.disabled", "1") && Expect("totk.inv.act.2.reason", "notAiming") &&
                  Expect("totk.inv.act.2.reason_text", "Aim your bow to fuse");
            ok &= Save(out + "-inventory-materials.png", latest, w, h);
            // The quick picker: the weapon corner opens it with the equipped weapon selected (Unequip); another cell,
            // then the confirm bar, equips that one
            Tap(1164, 60, latest, w, h); Run(30, latest, w, h);
            Tap(1132, 968, latest, w, h); Run(40, latest, w, h);
            ok &= Expect("totk.quick.open", "1") && Expect("totk.quick.title", "Weapons") && Expect("totk.quick.count", "5") &&
                  Expect("totk.quick.0.selected", "1") && Expect("totk.quick.act.0.label", "Unequip");
            ok &= Save(out + "-quick.png", latest, w, h);
            Tap(470, 340, latest, w, h); Run(30, latest, w, h);
            ok &= Expect("totk.quick.2.selected", "1") && Expect("totk.quick.act.0.label", "Equip");
            Tap(892, 936, latest, w, h); Run(80, latest, w, h);
            ok &= ExpectRequest(1, "equip", "weapons", 2, "Weapon_Lsword_001") && Expect("totk.act.done", "1");
            ok &= Save(out + "-quick-done.png", latest, w, h);
            Tap(348, 936, latest, w, h); Run(30, latest, w, h);   // Back closes the picker
            ok &= Expect("totk.quick.open", "0");
        } else {
            // No runtime actions (Eden Duo): shown, disabled, with the reason; a tap requests nothing
            ok &= Expect("totk.inv.act.0.enabled", "0") && Expect("totk.inv.act.0.disabled", "1") &&
                  Expect("totk.inv.act.0.reason", "notAvailableYet") &&
                  Expect("totk.inv.act.0.reason_text", "Item actions need the TotK Native app");
            Tap(1112, 1000, latest, w, h); Run(10, latest, w, h);
            ok &= Expect("totk.act.refused", "1") && Expect("totk.act.reason", "notAvailableYet") &&
                  Expect("totk.act.line", "Not done: Item actions need the TotK Native app");
            ok &= Requests() == 0;
            ok &= Save(out + "-inventory-off.png", latest, w, h);
            Tap(1164, 60, latest, w, h); Run(30, latest, w, h);
            Tap(1132, 968, latest, w, h); Run(40, latest, w, h);
            ok &= Expect("totk.quick.open", "1") && Expect("totk.quick.act.0.disabled", "1") &&
                  Expect("totk.quick.act.0.label", "Unequip");
            ok &= Save(out + "-quick-off.png", latest, w, h);
        }
    }
    if (sc.name == "food") {    // low hearts: swipe the right pager to Food, then open its card
        Swipe(1128, 620, 520, latest, w, h); Run(30, latest, w, h);
        ok &= Save(out + "-food-page.png", latest, w, h);
        Tap(1128, 570, latest, w, h); Run(40, latest, w, h);
        ok &= Save(out + "-food-card.png", latest, w, h);
        Tap(620, 600, latest, w, h); Run(30, latest, w, h);  // a tap on the card closes it
        ok &= Save(out + "-food-closed.png", latest, w, h);
    }
    Duo::Stop();
    // The common state of the scene's last sample, from the module's totk.status / totk.state.* (duo_state.h).
    std::fprintf(stderr, "test: %llu common states, last status %d in_game %d menu %d tablet %d life %d/%d\n",
                 static_cast<unsigned long long>(g_states), g_state.status, g_state.in_game, g_state.menu_open,
                 g_state.tablet_owned, g_state.life, g_state.max_life);
    // Checked where the fixture decides it alone: not for a save that is loading (the reader's gate decides when
    // the world counts as running) nor for another build (the package refuses it before the module runs).
    if (!sc.loading && argc <= 6) {
        const int want_status = sc.title ? COMPANION_STATE_TITLE : COMPANION_STATE_READY;
        bool state_ok = g_states > 0 && g_state.status == want_status && g_state.tablet_owned == 1;
        if (want_status == COMPANION_STATE_READY) {
            state_ok &= g_state.in_game == 1 && g_state.menu_open == (sc.menu || sc.name == "overlays" ? 1 : 0) && g_state.max_life == 80 &&
                        g_state.life == sc.life;
        } else {
            state_ok &= g_state.in_game == 0 && g_state.life == -1 && g_state.max_life == -1;
        }
        if (!state_ok) {
            std::fprintf(stderr, "test: FAIL, the common state is not the fixture's (status %d wanted)\n", want_status);
        }
        ok &= state_ok;
    }
    std::fprintf(stderr, "test: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
