/* Host test for the dual-screen companion's game-state reader (runtime/companion_snapshot.c), no game files needed.
   It lays out a fake 1.4.3 main module, GameDataMgr tables and player actor in ordinary memory, the way the game does
   (open-addressing hash index, per-type entry strides, two-partition pouch columns, string array records), and checks the
   JSON the reader produces. Reads outside the fake allocations fail, as unmapped guest memory does.
     make -C games/totk/android companiontest TARGET=linux CC=cc OUT=<scratch> && <scratch>/companiontest */
#include "companion.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- fake memory ---------- */
typedef struct { uint8_t *p; size_t n; } Region;
static Region regions[4096]; static unsigned nregions;
static void *alloc(size_t n) {
    uint8_t *p = calloc(1, n);
    if (!p || nregions == sizeof regions / sizeof *regions) { fprintf(stderr, "out of memory\n"); exit(2); }
    regions[nregions++] = (Region){p, n};
    return p;
}
/* A hook that runs after every read: the tests use it to change game memory while a snapshot is being taken. */
static void (*after_read)(uint64_t a);
static int fake_read(uint64_t a, void *out, size_t n) {
    for (unsigned i = 0; i < nregions; i++) {
        uint64_t b = (uintptr_t)regions[i].p;
        if (a >= b && a - b <= regions[i].n && n <= regions[i].n - (a - b)) {
            memcpy(out, (void *)(uintptr_t)a, n);
            if (after_read) after_read(a);
            return 1;
        }
    }
    return 0;
}
static void put64(void *at, uint64_t v) { memcpy(at, &v, 8); }
static void put32(void *at, uint32_t v) { memcpy(at, &v, 4); }
static void putf(void *at, float v) { memcpy(at, &v, 4); }

/* ---------- fake GameDataMgr ---------- */
enum { MAIN_SIZE = 0x3ae0000, GDM_SLOT = 0x3ad66b0, POUCH_SLOT = 0x3a81678, SCENE_SLOT = 0x3add238,
       PLAYER_STATE_SLOT = 0x3a82e70, EVENT_SLOT = 0x3ada558, INFO_SLOT = 0x3a6f678 };
typedef struct { uint16_t base, stride; } Type;
static const Type BOOL_ = {0x008, 0x18}, INT = {0x0c8, 0x28}, INTARR = {0x128, 0x40},
    FLOAT_ = {0x188, 0x28}, ENUM_ = {0x248, 0x38}, ENUMARR = {0x2a8, 0x50}, VEC2ARR = {0x368, 0x48}, VEC3 = {0x3c8, 0x40},
    VEC3ARR = {0x428, 0x48}, STRARR = {0x668, 0x90};
typedef struct { Type t; uint32_t hash[64]; uint8_t *entry[64]; unsigned n; } Pending;
static Pending tables[11]; static unsigned ntables;
static unsigned wrapped;   /* insertions whose probe wrapped around the end of the index */
static uint8_t *flag_hash(Type t, uint32_t hash) {
    Pending *p = NULL;
    for (unsigned i = 0; i < ntables; i++) if (tables[i].t.base == t.base) p = &tables[i];
    if (!p) { p = &tables[ntables++]; p->t = t; }
    p->hash[p->n] = hash;
    return p->entry[p->n++] = alloc(t.stride);   /* placeholder; copied into the entries array by build() */
}
static uint8_t *flag(Type t, const char *name) { return flag_hash(t, companion_hash(name)); }
static void build(uint8_t *g) {
    for (unsigned i = 0; i < ntables; i++) {
        Pending *p = &tables[i];
        uint8_t *entries = alloc((size_t)p->n * p->t.stride);
        uint32_t cap = p->n + 1;   /* nearly full, so probes collide and wrap */
        uint32_t *index = alloc(8u * cap);
        for (unsigned e = 0; e < p->n; e++) {
            memcpy(entries + (size_t)e * p->t.stride, p->entry[e], p->t.stride);
            uint32_t slot = p->hash[e] % cap;
            while (index[slot * 2]) { if (++slot == cap) { slot = 0; wrapped++; } }
            index[slot * 2] = p->hash[e]; index[slot * 2 + 1] = e;
        }
        uint8_t *t = g + p->t.base;
        put32(t + 8, p->n); put64(t + 0x10, (uintptr_t)entries); put64(t + 0x18, (uintptr_t)index);
        put32(t + 0x20, p->n); put32(t + 0x24, cap);
    }
}
static void set_int(const char *name, int32_t v) { put32(flag(INT, name) + 0x1c, (uint32_t)v); }
static void set_float(const char *name, float v) { putf(flag(FLOAT_, name) + 0x1c, v); }
static void set_bool(const char *name, int v) { flag(BOOL_, name)[0x16] = (uint8_t)v; }
static void set_enum(const char *name, const char *v) { put32(flag(ENUM_, name) + 0x1c, companion_hash(v)); }
static void set_vec3(const char *name, float x, float y, float z) { uint8_t *e = flag(VEC3, name); putf(e + 0x24, x); putf(e + 0x28, y); putf(e + 0x2c, z); }
static void *words(Type t, const char *name, uint32_t count, unsigned word_size) {
    uint8_t *e = flag(t, name), *data = alloc((size_t)count * word_size);
    unsigned co = t.base == VEC2ARR.base || t.base == VEC3ARR.base ? 0x28 : 0x20;
    put32(e + co, count); put64(e + co + 8, (uintptr_t)data);
    return data;
}
static int32_t *int_array(const char *name, uint32_t count) { int32_t *d = words(INTARR, name, count, 4); for (uint32_t i = 0; i < count; i++) d[i] = -1; return d; }
static uint32_t *enum_array(const char *name, uint32_t count, const char *fill) {
    uint32_t *d = words(ENUMARR, name, count, 4); for (uint32_t i = 0; i < count; i++) d[i] = companion_hash(fill); return d;
}
/* String array records: {char *data; i32 capacity; char inline[64]; ...} = 0x50 bytes, data -> inline buffer. */
static uint8_t *string_array(const char *name, uint32_t count) {
    uint8_t *e = flag(STRARR, name), *data = alloc((size_t)count * 0x50);
    for (uint32_t i = 0; i < count; i++) { put64(data + i * 0x50, (uintptr_t)(data + i * 0x50 + 0xc)); put32(data + i * 0x50 + 8, 64); }
    put32(e + 0x70, count); put64(e + 0x78, (uintptr_t)data);
    return data;
}
static void set_string(uint8_t *array, uint32_t i, const char *s) { strcpy((char *)array + i * 0x50 + 0xc, s); }

static int failures;
static void expect(const char *json, const char *fragment) {
    if (!strstr(json, fragment)) { fprintf(stderr, "FAIL: missing %s\n", fragment); failures++; }
}
static void reject(const char *json, const char *fragment) {
    if (strstr(json, fragment)) { fprintf(stderr, "FAIL: unexpected %s\n", fragment); failures++; }
}
/* swap_hook: the game replacing a pointer while the reader walks it. Once swap_after reads have happened, store
   swap_to[0] at swap_at; with swap_every, instead flip swap_at between swap_to[0] and [1] each time it is read. */
static uint8_t *swap_at; static uint64_t swap_to[2]; static unsigned swap_after, reads, swap_every;
static void swap_hook(uint64_t a) {
    reads++;
    if (swap_every) {
        uint64_t now; memcpy(&now, swap_at, 8);
        if (a == (uintptr_t)swap_at) put64(swap_at, now == swap_to[0] ? swap_to[1] : swap_to[0]);
    }
    else if (reads == swap_after) put64(swap_at, swap_to[0]);
}

/* ---------- fake game functions for the item actions (companion_actions.c) ---------- */
void companion_actions_test_verify(int key, unsigned mask);
void companion_actions_test_sha256(const void *m, size_t n, char out[65]);
void companion_actions_test_expect_prologues(uintptr_t main_base);
void companion_snapshot_test_expect_guard(const void *words, size_t n);
/* the functions the actions call or check, by main offset; the test fills each with its own stand-in bytes */
static const uint32_t FN_OFF[] = {0x20a1128, 0x20a9a28, 0x20ac5c4, 0x20ac080, 0x20af148, 0x20afab4, 0x20affa8, 0x20aff18,
    0x63307c, 0x711000, 0x20af2b4, 0x20af1b8, 0x20b356c, 0x20a837c, 0x20a871c, 0x20a8ad0, 0x20a7680, 0x20a6af8,
    0x89f9b0, 0xc1ee50, 0x3d01a0, 0x1cce94c, 0x1fa4e44, 0x1fa4fec};
static struct {
    uint8_t *main, *pouch, *fruit, *dish;
    int32_t *weapon_equip, *food_stock, *mat_stock;
    int aim, stuck, arrows, saving, pending, calls, equips, eats, takeouts;
    uint64_t now;
} T;
static uint64_t fake_call(uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a4; (void)a5;
    T.calls++;
    int cat = (int)a1, slot = (int)a2;
    switch ((uint32_t)(fn - (uintptr_t)T.main)) {
    case 0x20a1128: T.equips++; if (!T.stuck && cat == 0 && (int)a3 == -1) T.weapon_equip[1] = slot; return 1;   /* equip */
    case 0x20a9a28: T.equips++; if (!T.stuck && cat == 0) T.weapon_equip[1] = -1; return 1;                    /* unequip */
    case 0x20ac5c4: return cat == 0 && T.weapon_equip[1] == slot;                                               /* isEquipped */
    case 0x20ac080: return cat == 2 ? T.arrows : cat == 6 ? T.food_stock[60 + slot] >= (int)a3 : cat == 5 ? T.mat_stock[255 + slot] >= (int)a3 : 0;
    case 0x20af148: T.eats++; if (cat == 6) T.food_stock[60 + slot]--; else T.mat_stock[255 + slot]--; return 1;   /* eat */
    case 0x20afab4:                                                                                              /* take out */
        T.takeouts++;
        if (T.aim) { put32(T.pouch + 0x2f0, (uint32_t)a2); put32(T.pouch + 0x2f4, (uint32_t)a1); T.pouch[0x1fc] = 1; }
        else if ((int)a2 == 5) T.mat_stock[255 + (int)a1]--;
        return 1;
    case 0x20affa8: case 0x20aff18: return 1;                                                                    /* can hold */
    case 0x63307c: {                                                                                             /* info */
        const char *name; memcpy(&name, (void *)(uintptr_t)a1, sizeof name);
        return !strcmp(name, "Item_Fruit_A") ? (uintptr_t)T.fruit : !strncmp(name, "Item_Cook_", 10) ? (uintptr_t)T.dish : 0;
    }
    case 0x711000: return T.aim;
    case 0x3d01a0: return 0;                                                                                     /* no battery yet */
    default: fprintf(stderr, "FAIL: unexpected game call main+%#llx\n", (unsigned long long)(fn - (uintptr_t)T.main)); exit(1);
    }
}
static float fake_call_f(uint64_t fn, uint64_t a0, float s0) { (void)a0; (void)s0; fprintf(stderr, "FAIL: unexpected float call main+%#llx\n", (unsigned long long)(fn - (uintptr_t)T.main)); exit(1); }
static int fake_write(uint64_t a, const void *src, size_t n) { (void)a; (void)src; (void)n; fprintf(stderr, "FAIL: unexpected write\n"); exit(1); }
static int fake_saving(void) { return T.saving; }
static uint64_t fake_now(void) { return T.now; }
static void fake_pending(int on) { T.pending = on; }
static uint8_t *info_record(const char *use, int recover, int edible) {
    uint8_t *r = alloc(0x160); char *s = alloc(32); strcpy(s, use);
    put64(r + 0x98, (uintptr_t)s); put32(r + 0x130, (uint32_t)recover); r[0x15c] = (uint8_t)edible;
    return r;
}
static int check(int ok, const char *what) { if (!ok) { fprintf(stderr, "FAIL: %s\n", what); failures++; } return ok; }
static void result_is(int seq, const char *state, const char *reason) {
    CompanionBuf r = {0}; char want[160];
    snprintf(want, sizeof want, "{\"seq\":%d,\"state\":\"%s\",\"reason\":\"%s\"}", seq, state, reason);
    if (!companion_result(seq, &r) || strcmp(r.data, want)) { fprintf(stderr, "FAIL: result %s, want %s\n", r.data ? r.data : "(none)", want); failures++; }
    free(r.data);
}
static void tick(uintptr_t main_base, uint64_t *frame) { T.now += 150; companion_tick(main_base, (*frame)++); }   /* ~5 frames apart */
/* the world stays ok long enough (1 s and 20 frames) for actions to run */
static void settle(uintptr_t main_base, uint64_t *frame) { for (int i = 0; i < 21; i++) tick(main_base, frame); }
static long long pouch_gen(uintptr_t main_base, CompanionBuf *b) {   /* a full snapshot, as the companion takes one */
    companion_snapshot(main_base, COMPANION_FULL, fake_read, b);
    const char *g = strstr(b->data, "\"pouchGen\":");
    return g ? atoll(g + 11) : -1;
}

int main(void) {
    if (companion_hash("PlayerStatus.Life") != 0x31ab5580u || companion_hash("None") != 0xb6eede09u ||
        companion_hash("Pouch.Weapon.Content.Name") != 0x65efd0beu) { fprintf(stderr, "FAIL: murmur3\n"); return 1; }
    CompanionBuf b = {0};

    /* the code words the reader checks before trusting its offsets: stand-ins (no game code in the repository), whose
       hash the test declares the expected one; the app compares with the hash of the 1.4.3 words */
    static const uint32_t CODE_AT[] = {0x787554, 0x7875a4, 0x61fde4, 0x61fe90, 0x282964, 0x282970, 0x2097688, 0x209768c};
    uint32_t code[sizeof CODE_AT / sizeof *CODE_AT];
    for (unsigned i = 0; i < sizeof code / sizeof *code; i++) code[i] = 0x5eed0000u + i * 0x1111u;
    companion_snapshot_test_expect_guard(code, sizeof code);

    /* another build of main (the instructions the reader's offsets come from differ): nothing is read */
    uint8_t *other = alloc(0x20a0000);   /* covers every checked code word */
    if (!companion_snapshot((uintptr_t)other, COMPANION_FULL, fake_read, &b)) return 1;
    expect(b.data, "\"status\":\"unsupported\""); reject(b.data, "\"player\"");
    /* the common second-screen state (companion_state.h) says the same, in its own terms */
    CompanionState cs;
    companion_state((uintptr_t)other, fake_read, &cs);
    check(cs.status == COMPANION_STATE_UNSUPPORTED && !cs.in_game && !cs.menu_open && cs.tablet_owned == 1 && cs.life == -1 &&
          cs.max_life == -1 && cs.page == -1, "state: another build is unsupported and carries nothing");
    companion_state(0, fake_read, &cs);
    check(cs.status == COMPANION_STATE_STARTING && !cs.in_game, "state: no main yet is starting");

    uint8_t *main_ = alloc(MAIN_SIZE);
    for (unsigned i = 0; i < sizeof CODE_AT / sizeof *CODE_AT; i++) put32(main_ + CODE_AT[i], code[i]);

    /* before the game creates GameDataMgr */
    companion_snapshot((uintptr_t)main_, COMPANION_FULL, fake_read, &b);
    expect(b.data, "\"status\":\"starting\""); reject(b.data, "\"player\"");

    uint8_t *g = alloc(0xd38);
    put64(main_ + GDM_SLOT, (uintptr_t)g);
    set_int("PlayerStatus.Life", 52); set_int("PlayerStatus.MaxLife", 60); set_int("PlayerStatus.ExtraLife", 4);
    set_int("PlayerStatus.BreakLife", 8); set_int("PlayerStatus.CurrentRupee", 1234); set_int("PlayerStatus.CurrentMamo", 56);
    set_float("PlayerStatus.MaxStamina", 2000); set_float("PlayerStatus.MaxEnergy", 3000);
    set_enum("PlayerStatus.CurrentSpecialPower", "OneTouchBond"); set_enum("MapData.CurrentLayer", "Ground");
    set_bool("PlayerStatus.Companion.Wind.IsGet", 1); set_bool("PlayerStatus.Companion.Wind.IsSummon", 0);
    set_int("PlayerStatus.Companion.Wind.ForUI.RemainingMinute", 1); set_int("PlayerStatus.Companion.Wind.ForUI.RemainingSecond", 30);
    set_enum("PlayerStatus.Companion.Wind.JoiningCondition", "Joining");
    set_vec3("World_PlayerPos", 1, 2, 3);

    /* pouch, two partitions; the game is in partition 1 */
    uint8_t *weapons = string_array("Pouch.Weapon.Content.Name", 40), *fuses = string_array("Pouch.Weapon.Content.Combined.Name", 40);
    int32_t *life = int_array("Pouch.Weapon.Content.Life", 40), *fuse_life = int_array("Pouch.Weapon.Content.Combined.Life", 40);
    uint32_t *effect = enum_array("Pouch.Weapon.Content.Effect.Type", 40, "None");
    int32_t *effect_value = int_array("Pouch.Weapon.Content.Effect.Value", 40), *equip = int_array("Pouch.Weapon.EquipIndex", 2);
    int32_t *valid = int_array("Pouch.Weapon.ValidNum", 2);
    set_string(weapons, 0, "Weapon_Sword_999");                 /* partition 0: must not show */
    set_string(weapons, 20, "Weapon_Sword_001"); life[20] = 18; set_string(fuses, 20, "Item_Enemy_00"); fuse_life[20] = 3;
    effect[20] = companion_hash("AttackUp"); effect_value[20] = 5;
    set_string(weapons, 21, "Weapon_Sword_002"); life[21] = 9;
    set_string(weapons, 23, "Bad Name!");                         /* not an identifier: dropped */
    char *outside = alloc(64); strcpy(outside, "Weapon_Lsword_003");
    put64(weapons + 24 * 0x50, (uintptr_t)outside); set_string(weapons, 24, "Weapon_Ignored");   /* data pointer elsewhere */
    equip[0] = 3; equip[1] = 0; valid[0] = 8; valid[1] = 10;
    uint8_t *armor = string_array("Pouch.Armor.Content.Name", 300);
    uint32_t *color = enum_array("Pouch.Armor.Content.ColorVariation", 300, "None");
    set_string(armor, 150, "Armor_001_Head"); set_string(armor, 152, "Armor_002_Upper"); color[152] = companion_hash("Blue");
    int_array("Pouch.Armor.EquipIndexes.Head", 2)[1] = 0;
    int_array("Pouch.Armor.EquipIndexes.Upper", 2)[1] = 1;      /* slot 1 is empty: nothing marked */
    int_array("Pouch.Armor.EquipIndexes.Bottom", 2)[1] = -1;
    uint8_t *food = string_array("Pouch.Food.Content.Name", 120), *mats = string_array("Pouch.Food.Content.MaterialName", 600);
    int32_t *stock = int_array("Pouch.Food.Content.StockNum", 120), *heal = int_array("Pouch.Food.Content.LifeRecover", 120);
    uint32_t *food_effect = enum_array("Pouch.Food.Content.Effect.Type", 120, "None");
    int32_t *level = int_array("Pouch.Food.Content.Effect.Level", 120), *time = int_array("Pouch.Food.Content.Effect.Time", 120);
    int_array("Pouch.Food.Content.Price", 120);
    set_string(food, 60, "Item_Cook_C_17"); stock[60] = 2; heal[60] = 12; food_effect[60] = companion_hash("ZonauEnergyHealUp");
    level[60] = 3; time[60] = 0; set_string(mats, 300, "Item_Meat_01"); set_string(mats, 301, "Item_Fruit_A");
    uint8_t *specials = string_array("Pouch.SpecialPower.Content.Name", 20);
    set_string(specials, 10, "UltraHand"); set_string(specials, 11, "OneTouchBond");
    float *pins = words(VEC3ARR, "MapData.IconData.MapPinData.Pos", 6, 12);
    uint32_t *pin_type = enum_array("MapData.IconData.MapPinData.Type", 6, "Invalid");
    pins[3] = 10; pins[4] = 20; pins[5] = -30; pin_type[1] = companion_hash("Red");
    float *stamps = words(VEC2ARR, "MapData.IconData.StampData.Pos", 300, 8);
    uint32_t *stamp_type = enum_array("MapData.IconData.StampData.Type", 300, "Invalid");
    uint32_t *stamp_layer = enum_array("MapData.IconData.StampData.Layer", 300, "Ground");
    stamps[0] = -5; stamps[1] = 6; stamp_type[0] = companion_hash("Star"); stamp_layer[0] = companion_hash("Underground");
    /* the other pouch categories (the action rules' pouch identity covers them all) and the action-side game state */
    string_array("Pouch.Bow.Content.Name", 28); string_array("Pouch.Shield.Content.Name", 40); string_array("Pouch.Arrow.Content.Name", 2);
    string_array("Pouch.SpecialParts.Content.Name", 60); string_array("Pouch.KeyItem.Content.Name", 200);
    uint8_t *materials = string_array("Pouch.Material.Content.Name", 510);
    int32_t *mat_stock = int_array("Pouch.Material.Content.StockNum", 510);
    set_string(materials, 255, "Item_Fruit_A"); mat_stock[255] = 3;
    set_string(food, 61, "Item_Cook_C_01"); stock[61] = 1; heal[61] = 8;   /* no effect: refused at full hearts */
    flag_hash(BOOL_, 0x1c96382f)[0x16] = 1;                                  /* Zonai devices may be held */
    build(g);
    uint8_t *pouch_obj = alloc(0x1fd00); put32(pouch_obj + 0x188, 1); put64(main_ + POUCH_SLOT, (uintptr_t)pouch_obj);
    put32(pouch_obj + 0x478, 5);                                   /* the player's pouch actor */
    /* the globals the actions use, and the GOT entries the game's own code loads them through */
    uint8_t *player_state = alloc(0x700), *event_mgr = alloc(0x100), *info_owner = alloc(0x80), *info_table = alloc(0x10);
    put64(main_ + PLAYER_STATE_SLOT, (uintptr_t)player_state); put64(main_ + EVENT_SLOT, (uintptr_t)event_mgr);
    put64(main_ + INFO_SLOT, (uintptr_t)info_owner); put64(info_owner + 0x60, (uintptr_t)info_table);
    static const uint32_t GOT[][2] = {{0x39601e8, POUCH_SLOT}, {0x395ea40, PLAYER_STATE_SLOT}, {0x395dff8, EVENT_SLOT},
                                      {0x395dff0, GDM_SLOT}, {0x395f900, INFO_SLOT},
                                      {0x395dc70, 0x3add238}};
    for (unsigned i = 0; i < sizeof GOT / sizeof *GOT; i++) put64(main_ + GOT[i][0], (uintptr_t)main_ + GOT[i][1]);
    uint64_t flags = 1ull | 1ull << 33;                             /* calc (unpaused), weapon change allowed */
    memcpy(player_state + 0x658, &flags, 8);

    /* title screen: GameData exists but no player actor yet */
    companion_snapshot((uintptr_t)main_, COMPANION_FULL, fake_read, &b);
    expect(b.data, "\"status\":\"title\""); reject(b.data, "\"player\"");

    /* the player actor: scene module -> scene -> components[13] -> link -> actor "Player" */
    uint8_t *module = alloc(0x200), *scene = alloc(0x60), *components = alloc(14 * 8), *resident = alloc(0x30),
            *links = alloc(0x70), *data = alloc(0x48), *actor = alloc(0x300), *name = alloc(8);
    put64(main_ + SCENE_SLOT, (uintptr_t)module); put64(module + 0x1e8, (uintptr_t)scene);
    put32(scene + 0x50, 14); put64(scene + 0x58, (uintptr_t)components); put64(components + 13 * 8, (uintptr_t)resident);
    put32(resident + 0x20, 1); put64(resident + 0x28, (uintptr_t)links); put64(links + 8, (uintptr_t)data);
    put64(data + 0x40, (uintptr_t)actor); strcpy((char *)name, "Player"); put64(actor + 0x218, (uintptr_t)name);
    /* the title screen keeps a Player actor at the origin: still the title */
    companion_snapshot((uintptr_t)main_, COMPANION_LIVE, fake_read, &b);
    expect(b.data, "\"status\":\"title\""); reject(b.data, "\"player\"");
    companion_state((uintptr_t)main_, fake_read, &cs);
    check(cs.status == COMPANION_STATE_TITLE && !cs.in_game && cs.life == -1 && cs.max_life == -1, "state: the title is not in game");
    putf(actor + 0x2b4, -254.5f); putf(actor + 0x2b8, -900); putf(actor + 0x2bc, 1100);
    float rot[9] = {0, 0, 1, 0, 1, 0, -1, 0, 0};   /* facing +X: east */
    memcpy(actor + 0x2c0, rot, sizeof rot);

    companion_snapshot((uintptr_t)main_, COMPANION_LIVE, fake_read, &b);
    expect(b.data, "\"status\":\"ready\"");
    expect(b.data, "\"life\":52"); expect(b.data, "\"maxLife\":60"); expect(b.data, "\"extraLife\":4");
    companion_state((uintptr_t)main_, fake_read, &cs);   /* what the stick LEDs and the shell get of the same sample */
    check(cs.status == COMPANION_STATE_READY && cs.in_game == 1 && cs.menu_open == 0 && cs.tablet_owned == 1 && cs.life == 52 &&
          cs.max_life == 60 && cs.page == -1, "state: ready carries the snapshot's hearts");
    int64_t words[COMPANION_STATE_FIELDS];
    companion_state_words(&cs, 7, words);
    static const int64_t WORDS[COMPANION_STATE_FIELDS] = {COMPANION_STATE_READY, 1, 0, 1, 52, 60, -1, 7};
    check(!memcmp(words, WORDS, sizeof words), "state: the getter's word order");
    expect(b.data, "\"breakLife\":8"); expect(b.data, "\"rupees\":1234"); expect(b.data, "\"poes\":56");
    expect(b.data, "\"maxStamina\":2000"); expect(b.data, "\"maxEnergy\":3000"); expect(b.data, "\"ability\":\"OneTouchBond\"");
    expect(b.data, "\"pos\":[-254.5,-900,1100]"); expect(b.data, "\"heading\":90");
    expect(b.data, "\"layer\":\"UnderGround\"");   /* deep below the surface, whatever the map flag says */
    reject(b.data, "\"pouch\"");
    printf("live: %s\n", b.data);

    companion_snapshot((uintptr_t)main_, COMPANION_FULL, fake_read, &b);
    printf("full: %s\n", b.data);
    expect(b.data, "\"partition\":1");
    expect(b.data, "{\"name\":\"Weapon_Sword_001\",\"slot\":0,\"life\":18,\"effect\":{\"type\":\"AttackUp\",\"value\":5},"
                   "\"fuse\":\"Item_Enemy_00\",\"fuseLife\":3,\"equipped\":true,\"actions\":[");
    expect(b.data, "{\"name\":\"Weapon_Sword_002\",\"slot\":1,\"life\":9,\"equipped\":false,\"actions\":[");
    expect(b.data, "\"name\":\"Weapon_Lsword_003\",\"slot\":4");
    reject(b.data, "Weapon_Sword_999"); reject(b.data, "Bad Name"); reject(b.data, "Weapon_Ignored");
    expect(b.data, "{\"name\":\"Armor_001_Head\",\"slot\":0,\"color\":\"None\",\"equipped\":true,\"actions\":[");
    expect(b.data, "{\"name\":\"Armor_002_Upper\",\"slot\":2,\"color\":\"Blue\",\"equipped\":false,\"actions\":[");
    expect(b.data, "{\"name\":\"Item_Cook_C_17\",\"slot\":0,\"count\":2,\"heal\":12,\"effect\":\"ZonauEnergyHealUp\","
                   "\"level\":3,\"time\":0,\"price\":-1,\"materials\":[\"Item_Meat_01\",\"Item_Fruit_A\"],\"actions\":[");
    /* item actions before the actions are set up: the game thread has not ticked, nothing is offered */
    expect(b.data, "\"actionState\":{\"ok\":false,\"reason\":\"stale\"");
    expect(b.data, "\"pouchGen\":1");
    expect(b.data, "{\"id\":\"unequip\",\"enabled\":false,\"reason\":\"stale\"}");
    expect(b.data, "\"slots\":{\"weapons\":10}");
    expect(b.data, "\"abilities\":[\"UltraHand\",\"OneTouchBond\"]");
    expect(b.data, "{\"id\":\"Wind\",\"joined\":true,\"summoned\":false,\"condition\":\"Joining\",\"cooldown\":90}");
    expect(b.data, "\"pins\":[{\"x\":10,\"y\":20,\"z\":-30,\"type\":\"Red\"}]");
    expect(b.data, "\"stamps\":[{\"x\":-5,\"z\":6,\"type\":\"Star\",\"layer\":\"UnderGround\"}]");
    if (!wrapped) { fprintf(stderr, "FAIL: no hash probe wrapped; the index layout did not exercise wrap-around\n"); failures++; }

    /* the pouch partition selector missing or out of range: no inventory rather than a guessed partition */
    put64(main_ + POUCH_SLOT, 0);
    companion_snapshot((uintptr_t)main_, COMPANION_FULL, fake_read, &b);
    expect(b.data, "\"status\":\"ready\""); reject(b.data, "\"pouch\""); reject(b.data, "\"partition\""); expect(b.data, "\"sages\"");
    put32(pouch_obj + 0x188, 7); put64(main_ + POUCH_SLOT, (uintptr_t)pouch_obj);
    companion_snapshot((uintptr_t)main_, COMPANION_FULL, fake_read, &b);
    reject(b.data, "\"pouch\"");
    put32(pouch_obj + 0x188, 1);

    /* the player actor replaced while a sample is taken (a scene change): the sample is taken again */
    uint8_t *actor2 = alloc(0x300); memcpy(actor2, actor, 0x300); putf(actor2 + 0x2b8, 50);
    swap_at = data + 0x40; swap_to[0] = (uintptr_t)actor2; swap_to[1] = (uintptr_t)actor2; swap_after = 20; reads = 0;
    after_read = swap_hook;
    companion_snapshot((uintptr_t)main_, COMPANION_FULL, fake_read, &b);
    expect(b.data, "\"status\":\"ready\""); expect(b.data, "\"pos\":[-254.5,50,1100]"); expect(b.data, "\"layer\":\"Ground\"");
    /* ... and if it keeps changing, the snapshot reports the transition instead of mixed values */
    swap_to[0] = (uintptr_t)actor; swap_to[1] = (uintptr_t)actor2; swap_after = 0; reads = 0; swap_every = 1;
    companion_snapshot((uintptr_t)main_, COMPANION_FULL, fake_read, &b);
    expect(b.data, "\"status\":\"title\""); reject(b.data, "\"player\""); reject(b.data, "\"pouch\"");
    reads = 0;
    companion_state((uintptr_t)main_, fake_read, &cs);
    check(cs.status == COMPANION_STATE_TITLE && !cs.in_game && cs.life == -1, "state: a sample that keeps moving is the transition");
    after_read = NULL;

    /* ---------- item actions (companion_actions.c) ---------- */
    put64(data + 0x40, (uintptr_t)actor);
    uintptr_t M = (uintptr_t)main_; uint64_t f = 1;
    T.main = main_; T.pouch = pouch_obj; T.weapon_equip = equip; T.food_stock = stock; T.mat_stock = mat_stock; T.arrows = 1;
    T.fruit = info_record("EatFood", 4, 1); T.dish = info_record("EatCookingDish", 0, 0); T.now = 1000;
    for (unsigned i = 0; i < sizeof FN_OFF / sizeof *FN_OFF; i++)
        for (unsigned k = 0; k < 64; k++) main_[FN_OFF[i] + k] = (uint8_t)(i * 37 + k);   /* stand-in prologues */
    char hex[65]; uint8_t *msg = alloc(1000);
    companion_actions_test_sha256("abc", 3, hex);
    check(!strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "SHA-256 abc");
    for (unsigned i = 0; i < 64; i++) msg[i] = (uint8_t)i;
    companion_actions_test_sha256(msg, 64, hex);
    check(!strcmp(hex, "fdeab9acf3710362bd2658cdc9a29e8f9c757fcf9811603a8c447cd1d9151108"), "SHA-256 of 64 bytes (a prologue)");
    memset(msg, 'a', 1000); companion_actions_test_sha256(msg, 1000, hex);
    check(!strcmp(hex, "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3"), "SHA-256 multi-block");

    /* without the host environment (the app's companion switch off never sets it up): nothing is accepted */
    check(companion_request(M, "equip", "weapons", 1, "Weapon_Sword_002", 1, 1) == COMPANION_UNSUPPORTED, "no environment: unsupported");
    CompanionEnv env = {fake_read, fake_call, fake_call_f, fake_write, fake_saving, fake_now, fake_pending};
    companion_actions_init(&env);
    companion_actions_test_expect_prologues(M);
    for (int k = 0; k < COMPANION_CATS; k++) companion_actions_test_verify(k, 0);   /* the rules below start from nothing proven */
    check(companion_request(M, "juggle", "weapons", 1, "Weapon_Sword_002", 1, 1) == COMPANION_INVALID, "unknown action");
    check(companion_request(M, "equip", "pockets", 1, "Weapon_Sword_002", 1, 1) == COMPANION_INVALID, "unknown category");
    check(companion_request(M, "equip", "weapons", 255, "Weapon_Sword_002", 1, 1) == COMPANION_INVALID, "slot out of range");
    check(companion_request(M, "equip", "weapons", 1, "Bad Name!", 1, 1) == COMPANION_INVALID, "actor not an identifier");
    check(companion_request(M, "equip", "weapons", 1, "Weapon_Sword_002", 2, 1) == COMPANION_INVALID, "partition out of range");
    check(companion_request(M, "equip", "weapons", 1, "Weapon_Sword_002", 1, 1) == COMPANION_REFUSED, "refused before the game thread ran");
    companion_snapshot(M, COMPANION_FULL, fake_read, &b);
    check(T.pending, "the companion reading asks for frame-end ticks");
    tick(M, &f);
    check(!T.equips && !T.eats && !T.takeouts, "a tick alone calls nothing");
    long long gen = pouch_gen(M, &b);
    expect(b.data, "\"actionState\":{\"ok\":false,\"reason\":\"loading\"");   /* just became ok: not settled yet */
    settle(M, &f);
    gen = pouch_gen(M, &b);
    check(T.calls > 0 && !T.equips && !T.eats && !T.takeouts, "settled ticks only query");
    expect(b.data, "\"actionState\":{\"ok\":true,\"reason\":\"\",\"pauseMenu\":false,\"aiming\":false}");
    expect(b.data, "{\"name\":\"Weapon_Sword_002\",\"slot\":1,\"life\":9,\"equipped\":false,\"actions\":[{\"id\":\"equip\",\"enabled\":false,\"reason\":\"notAvailableYet\"}]}");
    check(companion_request(M, "equip", "weapons", 1, "Weapon_Sword_002", 1, gen) == COMPANION_REFUSED, "unverified action refused");
    enum { EQUIP = 1, UNEQUIP = 2, EAT = 4, DRINK = 8, USE = 16, TAKE_OUT = 64, FUSE = 128 };
    companion_actions_test_verify(0, EQUIP | UNEQUIP); companion_actions_test_verify(5, EAT | TAKE_OUT | FUSE);
    companion_actions_test_verify(6, EAT | DRINK | USE);
    tick(M, &f);   /* reads the item facts the snapshot asked for */
    gen = pouch_gen(M, &b);
    expect(b.data, "\"equipped\":true,\"actions\":[{\"id\":\"unequip\",\"enabled\":true,\"reason\":\"\"}]}");
    expect(b.data, "\"slot\":1,\"life\":9,\"equipped\":false,\"actions\":[{\"id\":\"equip\",\"enabled\":true,\"reason\":\"\"}]}");
    expect(b.data, "\"color\":\"Blue\",\"equipped\":false,\"actions\":[{\"id\":\"equip\",\"enabled\":false,\"reason\":\"notAvailableYet\"}]}");
    expect(b.data, "\"Item_Fruit_A\"],\"actions\":[{\"id\":\"eat\",\"enabled\":true,\"reason\":\"\"}]}");   /* has an effect */
    expect(b.data, "\"name\":\"Item_Cook_C_01\",\"slot\":1,\"count\":1,\"heal\":8,\"price\":-1,\"materials\":[],"
                   "\"actions\":[{\"id\":\"eat\",\"enabled\":false,\"reason\":\"fullHearts\"}]}");
    expect(b.data, "{\"name\":\"Item_Fruit_A\",\"slot\":0,\"count\":3,\"actions\":[{\"id\":\"eat\",\"enabled\":false,\"reason\":\"fullHearts\"},"
                   "{\"id\":\"takeOut\",\"enabled\":true,\"reason\":\"\"},{\"id\":\"fuseArrow\",\"enabled\":false,\"reason\":\"notAiming\"}]}");
    check(companion_request(M, "eat", "food", 1, "Item_Cook_C_01", 1, gen) == COMPANION_REFUSED, "full hearts refused like the menu");

    /* equip: queued on the poller thread, one call on the next frame end, outcome read back on a later one */
    int equips = T.equips;
    int seq = companion_request(M, "equip", "weapons", 1, "Weapon_Sword_002", 1, gen);
    check(seq > 0, "equip accepted");
    check(companion_request(M, "equip", "weapons", 1, "Weapon_Sword_002", 1, gen) == COMPANION_BUSY, "double tap dropped");
    check(companion_request(M, "eat", "food", 0, "Item_Cook_C_17", 1, gen) == COMPANION_BUSY, "any request while one is pending is dropped");
    result_is(seq, "pending", "");
    check(T.equips == equips, "no game call from the requesting thread");
    tick(M, &f);
    check(T.equips == equips + 1 && equip[1] == 1, "one equip call at the frame end");
    result_is(seq, "pending", "");
    tick(M, &f);
    result_is(seq, "done", "");
    tick(M, &f);
    check(T.equips == equips + 1, "a finished request is never repeated");
    CompanionBuf none = {0}; check(!companion_result(seq + 7, &none), "unknown seq: no result"); free(none.data);

    /* identity: a request made from an older snapshot (the pouch changed since) is refused, never run */
    seq = companion_request(M, "unequip", "weapons", 1, "Weapon_Sword_002", 1, gen);
    result_is(seq, "refused", "itemMoved");
    /* ... the pouch reordered between the request and the frame end (the actor moved to another slot) */
    gen = pouch_gen(M, &b);
    seq = companion_request(M, "unequip", "weapons", 1, "Weapon_Sword_002", 1, gen);
    char tmp[0x50]; memcpy(tmp, weapons + 20 * 0x50 + 0xc, 0x40); memcpy(weapons + 20 * 0x50 + 0xc, weapons + 21 * 0x50 + 0xc, 0x40);
    memcpy(weapons + 21 * 0x50 + 0xc, tmp, 0x40); equip[1] = 0;
    tick(M, &f); result_is(seq, "refused", "itemMoved");
    memcpy(weapons + 21 * 0x50 + 0xc, weapons + 20 * 0x50 + 0xc, 0x40); memcpy(weapons + 20 * 0x50 + 0xc, tmp, 0x40); equip[1] = 1;
    /* ... the slot replaced by another item of the same kind (its own durability) */
    gen = pouch_gen(M, &b);
    seq = companion_request(M, "unequip", "weapons", 1, "Weapon_Sword_002", 1, gen);
    life[21] = 40; tick(M, &f); result_is(seq, "refused", "itemMoved"); life[21] = 9;
    /* ... the pouch partition switched */
    gen = pouch_gen(M, &b);
    seq = companion_request(M, "unequip", "weapons", 1, "Weapon_Sword_002", 1, gen);
    put32(pouch_obj + 0x188, 0); tick(M, &f); result_is(seq, "refused", "loading");   /* a partition switch starts over */
    put32(pouch_obj + 0x188, 1); settle(M, &f);
    /* ... the game state changed between the request and the frame end (an event began; a save began) */
    gen = pouch_gen(M, &b);
    seq = companion_request(M, "unequip", "weapons", 1, "Weapon_Sword_002", 1, gen);
    put64(event_mgr + 0x58, 1); tick(M, &f); result_is(seq, "refused", "event"); put64(event_mgr + 0x58, 0);
    settle(M, &f);
    seq = companion_request(M, "unequip", "weapons", 1, "Weapon_Sword_002", 1, gen);
    T.saving = 1; tick(M, &f); result_is(seq, "refused", "saving");
    check(T.equips == equips + 1, "none of the refused requests called the game");
    /* gated: saving, weapon change locked, player not calculating (a save loading) */
    gen = pouch_gen(M, &b);
    expect(b.data, "\"actionState\":{\"ok\":false,\"reason\":\"saving\"");
    check(companion_request(M, "unequip", "weapons", 1, "Weapon_Sword_002", 1, gen) == COMPANION_REFUSED, "refused while saving");
    T.saving = 0;
    flags = 1; memcpy(player_state + 0x658, &flags, 8); settle(M, &f);
    gen = pouch_gen(M, &b);
    expect(b.data, "\"actions\":[{\"id\":\"unequip\",\"enabled\":false,\"reason\":\"weaponLocked\"}]");
    check(companion_request(M, "unequip", "weapons", 1, "Weapon_Sword_002", 1, gen) == COMPANION_REFUSED, "weapon change locked");
    flags = 1ull << 33; memcpy(player_state + 0x658, &flags, 8); tick(M, &f);
    gen = pouch_gen(M, &b);
    expect(b.data, "\"actionState\":{\"ok\":false,\"reason\":\"loading\",\"pauseMenu\":false,\"aiming\":false}");
    check(companion_request(M, "unequip", "weapons", 1, "Weapon_Sword_002", 1, gen) == COMPANION_REFUSED, "refused while the player does not calc (loading)");
    flags = 1ull | 1ull << 33; memcpy(player_state + 0x658, &flags, 8); settle(M, &f);
    /* the pause menu open (PouchMgr +0x1fcc0): reported, nothing runs; a request queued before it opened is refused */
    gen = pouch_gen(M, &b); int calls = T.equips;
    seq = companion_request(M, "unequip", "weapons", 1, "Weapon_Sword_002", 1, gen);
    pouch_obj[0x1fcc0] = 1; tick(M, &f); result_is(seq, "refused", "menu");
    gen = pouch_gen(M, &b);
    expect(b.data, "\"actionState\":{\"ok\":false,\"reason\":\"menu\",\"pauseMenu\":true,\"aiming\":false}");
    companion_state(M, fake_read, &cs);
    check(cs.status == COMPANION_STATE_READY && cs.in_game == 1 && cs.menu_open == 1, "state: the pause menu is the open tablet");
    check(companion_request(M, "unequip", "weapons", 1, "Weapon_Sword_002", 1, gen) == COMPANION_REFUSED, "refused in the pause menu");
    check(T.equips == calls, "nothing called with the pause menu open");
    /* the load transition: after any blocked state the world must stay ok for 1 s and 20 frames with the same player
       actor and partition. Rapid taps all through that gap are refused and call nothing. */
    pouch_obj[0x1fcc0] = 0; int refused = 0;
    for (int i = 0; i < 19; i++) {
        tick(M, &f); gen = pouch_gen(M, &b);
        refused += companion_request(M, "unequip", "weapons", 1, "Weapon_Sword_002", 1, gen) == COMPANION_REFUSED;
    }
    expect(b.data, "\"actionState\":{\"ok\":false,\"reason\":\"loading\"");
    check(refused == 19 && T.equips == calls, "every tap in the settling gap refused, nothing called");
    settle(M, &f);
    /* ... a request queued just before a reload (a new player actor while the state still looks ok) is refused */
    uint8_t *actor3 = alloc(0x300); memcpy(actor3, actor, 0x300);
    gen = pouch_gen(M, &b);
    seq = companion_request(M, "unequip", "weapons", 1, "Weapon_Sword_002", 1, gen);
    check(seq > 0, "accepted once settled");
    put64(data + 0x40, (uintptr_t)actor3); tick(M, &f); result_is(seq, "refused", "loading");
    check(T.equips == calls, "nothing called across the reload");
    settle(M, &f);   /* after the timer one tap works: the timeout check below makes that call */

    /* timeout: the call made, no change seen; failed after the bound and never called again */
    gen = pouch_gen(M, &b); T.stuck = 1;
    seq = companion_request(M, "unequip", "weapons", 1, "Weapon_Sword_002", 1, gen);
    tick(M, &f); check(T.equips == equips + 2, "unequip called once");
    T.now += 2500; tick(M, &f); result_is(seq, "pending", "");   /* too few frames yet */
    f += 25; tick(M, &f); result_is(seq, "failed", "timeout");
    tick(M, &f); check(T.equips == equips + 2, "timed out request not retried");
    T.stuck = 0; settle(M, &f);   /* the 2.5 s without ticks above starts the settling over */

    /* eat: one call, stock read back */
    gen = pouch_gen(M, &b);
    seq = companion_request(M, "eat", "food", 0, "Item_Cook_C_17", 1, gen);
    tick(M, &f); check(T.eats == 1 && stock[60] == 1, "one eat call, one item");
    tick(M, &f); result_is(seq, "done", "");
    /* take out a material (not aiming), then fuse it to arrows (aiming) */
    gen = pouch_gen(M, &b);
    seq = companion_request(M, "takeOut", "materials", 0, "Item_Fruit_A", 1, gen);
    tick(M, &f); tick(M, &f); result_is(seq, "done", ""); check(mat_stock[255] == 2, "taken out");
    T.aim = 1; tick(M, &f);   /* the game's isArrowAimMode (main+0x711000), not a player-state bit */
    gen = pouch_gen(M, &b);
    expect(b.data, "\"aiming\":true");
    check(companion_request(M, "takeOut", "materials", 0, "Item_Fruit_A", 1, gen) == COMPANION_REFUSED, "no take-out while aiming");
    seq = companion_request(M, "fuseArrow", "materials", 0, "Item_Fruit_A", 1, gen);
    tick(M, &f); tick(M, &f); result_is(seq, "done", ""); check(T.takeouts == 2, "fused once");
    T.aim = 0; tick(M, &f);

    /* the frame end stopped running: a queued request ends without ever being run */
    gen = pouch_gen(M, &b); equips = T.equips;
    seq = companion_request(M, "equip", "weapons", 0, "Weapon_Sword_001", 1, gen);
    T.now += 3100; result_is(seq, "refused", "stale"); tick(M, &f);
    check(T.equips == equips, "a stale request is never run later");
    settle(M, &f);
    /* the runtime relaunched the game (another main mapping): a pending request fails as teardown */
    gen = pouch_gen(M, &b);
    seq = companion_request(M, "equip", "weapons", 0, "Weapon_Sword_001", 1, gen);
    companion_tick(0, f++); result_is(seq, "failed", "teardown"); check(T.equips == equips, "torn-down request not run");
    /* more materials and food than the old 256-name facts cache held: every item's facts resolve and stay known */
    char many[32];
    for (int s = 1; s < 255; s++) { snprintf(many, sizeof many, "Item_Many_%03d", s); set_string(materials, 255 + s, many); mat_stock[255 + s] = 1; }
    for (int s = 2; s < 60; s++) { snprintf(many, sizeof many, "Item_Cook_M%02d", s); set_string(food, 60 + s, many); stock[60 + s] = 1; }
    for (int i = 0; i < 40; i++) { pouch_gen(M, &b); tick(M, &f); }
    pouch_gen(M, &b);
    expect(b.data, "\"name\":\"Item_Many_254\""); reject(b.data, "infoPending");
    companion_tick(0, f++);   /* the game relaunched once more: the next mapping is checked afresh */
    /* a called function that is not the expected one: actions unsupported, nothing called */
    main_[FN_OFF[4] + 3] ^= 1;
    tick(M, &f);
    gen = pouch_gen(M, &b);
    expect(b.data, "\"actionState\":{\"ok\":false,\"reason\":\"unsupported\"");
    check(companion_request(M, "equip", "weapons", 0, "Weapon_Sword_001", 1, gen) == COMPANION_UNSUPPORTED, "hash mismatch: unsupported");
    check(T.equips == equips && T.eats == 1, "nothing called after the mismatch");
    main_[FN_OFF[4] + 3] ^= 1;

    /* a freed manager: the reader reports the game as starting instead of reading through it */
    put64(main_ + GDM_SLOT, 0x10);
    companion_snapshot((uintptr_t)main_, COMPANION_FULL, fake_read, &b);
    expect(b.data, "\"status\":\"starting\"");

    free(b.data);
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("companion reader: all checks passed\n");
    return 0;
}
