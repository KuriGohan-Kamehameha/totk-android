/* The dual-screen companion's view of the game: GameData flags and the player actor, read from memory into JSON.
   Read-only: nothing here writes game memory or calls game code. Every guest read goes through the caller's
   fault-tolerant reader (an unmapped address fails the read instead of faulting), all counts and indexes are bounded and
   actor names must be plain identifiers. The reads are not synchronized with the game: a sample taken while it frees or
   rebuilds its state (title return, save load) can see stale or mixed memory, so companion_snapshot re-checks the state
   it sampled and reports a transition instead (see there); rejected fields are left out.

   TOTK 1.4.3 layout (static analysis of main, see android/APP.md "Dual-screen companion"):
   - gmd::GameDataMgr *g = *(main + 0x3ad66b0) (installed at main+0x7875a4, cleared at main+0x2a98284).
   - One table per flag type at g + base: {vtable; i32 count; entries; hash index; u32 used; u32 capacity}. The index is
     open addressing over {u32 hash, u32 entry} cells: slot = hash % capacity, linear probing with wrap, a zero hash ends
     the probe. Committed values sit at a fixed offset of each entry (getInt main+0x61fdd0); queued writes that the game
     has not committed yet are not visible here.
   - Flag and enum value hashes are MurmurHash3 of the full dotted name (e.g. "Pouch.Weapon.Content.Name").
   - Pouch columns are arrays of two partitions (index = partition * slots + slot); the current partition is a u32 at
     +0x188 of the object at *(main + 0x3a81678) (main+0x28295c).
   - The player actor is the scene's resident actor named "Player" (world_survey.c's chain); its position and rotation
     are live, unlike PlayerStatus.SavePos. */
#include "companion.h"
#include "game_version.h"
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t companion_hash(const char *s) {
    const uint8_t *p = (const uint8_t *)s; size_t n = strlen(s); uint32_t h = 0;
    const uint32_t c1 = 0xcc9e2d51u, c2 = 0x1b873593u;
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        uint32_t k = (uint32_t)p[i] | (uint32_t)p[i + 1] << 8 | (uint32_t)p[i + 2] << 16 | (uint32_t)p[i + 3] << 24;
        k *= c1; k = k << 15 | k >> 17; k *= c2;
        h ^= k; h = h << 13 | h >> 19; h = h * 5 + 0xe6546b64u;
    }
    uint32_t k = 0;
    switch (n & 3) {
        case 3: k ^= (uint32_t)p[i + 2] << 16; /* fallthrough */
        case 2: k ^= (uint32_t)p[i + 1] << 8;  /* fallthrough */
        case 1: k ^= p[i]; k *= c1; k = k << 15 | k >> 17; k *= c2; h ^= k;
    }
    h ^= (uint32_t)n; h ^= h >> 16; h *= 0x85ebca6bu; h ^= h >> 13; h *= 0xc2b2ae35u; h ^= h >> 16;
    return h;
}

/* ---------- JSON writer ---------- */
typedef struct { CompanionBuf *b; int depth, after_key; unsigned char comma[16]; } Json;
static void put(Json *j, const char *s, size_t n) {
    CompanionBuf *b = j->b;
    if (b->failed) return;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->len + n + 1) cap *= 2;
        char *d = realloc(b->data, cap);
        if (!d) { b->failed = 1; return; }
        b->data = d; b->cap = cap;
    }
    memcpy(b->data + b->len, s, n); b->len += n; b->data[b->len] = 0;
}
static void putf(Json *j, const char *fmt, ...) {
    char t[64]; va_list ap; va_start(ap, fmt); int n = vsnprintf(t, sizeof t, fmt, ap); va_end(ap);
    if (n > 0) put(j, t, (size_t)n < sizeof t ? (size_t)n : sizeof t - 1);
}
static void pre(Json *j) {
    if (j->after_key) { j->after_key = 0; return; }
    if (j->comma[j->depth]) put(j, ",", 1);
    j->comma[j->depth] = 1;
}
static void quoted(Json *j, const char *s) {
    put(j, "\"", 1);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { char e[2] = {'\\', (char)c}; put(j, e, 2); }
        else if (c < 0x20 || c >= 0x7f) putf(j, "\\u%04x", c);   /* names are ASCII identifiers; anything else is escaped */
        else put(j, (const char *)&c, 1);
    }
    put(j, "\"", 1);
}
static void key(Json *j, const char *k) { pre(j); quoted(j, k); put(j, ":", 1); j->after_key = 1; }
static void open_(Json *j, char c) { pre(j); put(j, &c, 1); if (j->depth < 15) j->depth++; j->comma[j->depth] = 0; }
static void close_(Json *j, char c) { if (j->depth) j->depth--; put(j, &c, 1); }
static void obj(Json *j, const char *k) { if (k) key(j, k); open_(j, '{'); }
static void arr(Json *j, const char *k) { if (k) key(j, k); open_(j, '['); }
static void end_obj(Json *j) { close_(j, '}'); }
static void end_arr(Json *j) { close_(j, ']'); }
static void val(Json *j, const char *k) {   /* start a value: under key k in an object, or the next array element */
    if (k) key(j, k);
    pre(j);
}
static void jint(Json *j, const char *k, long long v) { val(j, k); putf(j, "%lld", v); }
static void jnum(Json *j, const char *k, double v) {
    val(j, k);
    if (isfinite(v)) putf(j, "%.6g", v); else put(j, "null", 4);
}
static void jbool(Json *j, const char *k, int v) { val(j, k); put(j, v ? "true" : "false", v ? 4 : 5); }
static void jstr(Json *j, const char *k, const char *v) { val(j, k); quoted(j, v); }

/* ---------- enum value names (the game stores the hash of the value's name) ---------- */
typedef struct { const char *const *names; unsigned n; uint32_t *hashes; } EnumSet;
static const char *const EFFECTS[] = {"None", "AttackUp", "AttackUpPlus", "DurabilityUp", "DurabilityUpPlus", "FinishBlow",
    "LongThrow", "RapidFire", "ThreeWayZoom", "FiveWay", "GuardUp", "GuardUpPlus"};
static const char *const FOOD_EFFECTS[] = {"None", "ResistHot", "ResistBurn", "ResistCold", "ResistElectric",
    "ResitLightning", "ResistFreeze", "ResistAncient", "SwimSpeedUp", "DecreaseSwimStamina", "SpinAttack",
    "ClimbWaterfall", "ClimbSpeedUp", "ClimbSpeedUpOnlyHorizontaly", "AttackUp", "AttackUpCold", "AttackUpHot",
    "AttackUpThunderstorm", "AttackUpDark", "AttackUpBone", "QuietnessUp", "SandMoveUp", "SnowMoveUp", "WakeWind",
    "TwiceJump", "EmergencyAvoid", "DefenseUp", "AllSpeed", "MiasmaGuard", "MaskBokoblin", "MaskMoriblin",
    "MaskLizalfos", "MaskLynel", "YigaDisguise", "StalDisguise", "LifeRecover", "LifeMaxUp", "StaminaRecover",
    "ExStaminaMaxUp", "LifeRepair", "DivingMobilityUp", "NotSlippy", "Moisturizing", "LightEmission", "RupeeGuard",
    "FallResist", "SwordBeamUp", "VisualizeLife", "NightMoveSpeedUp", "NightGlow", "DecreaseWallJumpStamina",
    "DecreaseChargeAttackStamina", "EmitTerror", "NoBurning", "NoFallDamage", "NoSlip", "RupeeGuardRate", "MaskAll",
    "DecreaseZonauEnergy", "ZonauEnergyHealUp", "MaskHorablin", "MiasmaDefenseUp", "ChargePowerUpCold",
    "ChargePowerUpHot", "ChargePowerUpThunderstorm", "LightFootprint", "SoulPowerUpLightning", "SoulPowerUpWater",
    "SoulPowerUpWind", "SoulPowerUpFire", "SoulPowerUpSpirit", "EnableUseSwordBeam"};
static const char *const COLORS[] = {"None", "Blue", "Red", "Yellow", "White", "Black", "Purple", "Green", "LightBlue",
    "Navy", "Orange", "Pink", "Crimson", "LightYellow", "Brown", "Gray"};
static const char *const POWERS[] = {"None", "UltraHand", "OneTouchBond", "CeilingClipper", "ReverseRecorder",
    "AutoBuilder", "SheikahCamera", "Map", "Amiibo"};
static const char *const LAYERS[] = {"Ground", "Sky", "UnderGround", "Underground"};   /* stamps spell it Underground */
static const char *const PINS[] = {"Invalid", "Red", "Blue", "Yellow", "Green", "Purple", "LightBlue"};
static const char *const STAMPS[] = {"Invalid", "Sword", "Pot", "Human", "Rhombus", "Heart", "Star", "TreasureBox",
    "Skull", "Leaf", "Ore"};
static const char *const JOINING[] = {"Joining", "BreakawayNotice", "Breakaway"};
#define ENUM_SET(a) {a, sizeof a / sizeof *a, NULL}
static EnumSet E_EFFECT = ENUM_SET(EFFECTS), E_FOOD = ENUM_SET(FOOD_EFFECTS), E_COLOR = ENUM_SET(COLORS),
    E_POWER = ENUM_SET(POWERS), E_LAYER = ENUM_SET(LAYERS), E_PIN = ENUM_SET(PINS), E_STAMP = ENUM_SET(STAMPS),
    E_JOIN = ENUM_SET(JOINING);
static pthread_once_t enum_once = PTHREAD_ONCE_INIT;
static void enum_init(void) {
    EnumSet *sets[] = {&E_EFFECT, &E_FOOD, &E_COLOR, &E_POWER, &E_LAYER, &E_PIN, &E_STAMP, &E_JOIN};
    for (unsigned s = 0; s < sizeof sets / sizeof *sets; s++) {
        uint32_t *h = calloc(sets[s]->n, sizeof *h);
        if (h) for (unsigned i = 0; i < sets[s]->n; i++) h[i] = companion_hash(sets[s]->names[i]);
        sets[s]->hashes = h;
    }
}
static const char *enum_name(const EnumSet *e, uint32_t hash) {
    if (e->hashes) for (unsigned i = 0; i < e->n; i++) if (e->hashes[i] == hash) return e->names[i];
    return NULL;
}
#define NONE_HASH 0xb6eede09u   /* companion_hash("None") */

#if defined(TOTK_GAME_V143)
#define COMPANION_SUPPORTED 1
enum {
    GDM_SLOT = 0x3ad66b0,                  /* gmd::GameDataMgr * */
    POUCH_SLOT = 0x3a81678, POUCH_PART = 0x188, POUCH_RECEIVER = 0x478,
    POUCH_MENU = 0x1fcc0,                  /* byte, non-zero while the pause menu is open (reader main+0x2097648) */
    SCENE_SLOT = GAME_ADDR(0x4663f08),
    PLAYER_STATE_SLOT = 0x3a82e70, PLAYER_FLAGS = 0x658,   /* the flag word the UI's SeqQueryIsPlayer* nodes read */
    EVENT_SLOT = 0x3ada558, EVENT_CONTEXT = 0x58,          /* non-null while an event flow runs (main+0xd779e4) */
};
/* GOT entries the game's code loads those globals through; after relocation each holds main + the global's offset. */
static const struct { uint32_t got, target; } ACTION_GOT[] = {
    {0x39601e8, POUCH_SLOT}, {0x395ea40, PLAYER_STATE_SLOT}, {0x395dff8, EVENT_SLOT}, {0x395dff0, GDM_SLOT},
};
#else
#define COMPANION_SUPPORTED 0
#endif

/* ---------- GameData tables ---------- */
enum { T_BOOL, T_BOOLARR, T_INT, T_INTARR, T_FLOAT, T_ENUM, T_ENUMARR, T_VEC2ARR, T_VEC3, T_VEC3ARR, T_STRARR, T_COUNT };
static const struct { uint16_t base, stride, value, count, data; } TYPES[T_COUNT] = {
    /*                 table  entry  value  array count/data */
    [T_BOOL]    = {0x008, 0x18, 0x16, 0, 0},
    [T_BOOLARR] = {0x068, 0x40, 0, 0x20, 0x28},
    [T_INT]     = {0x0c8, 0x28, 0x1c, 0, 0},
    [T_INTARR]  = {0x128, 0x40, 0, 0x20, 0x28},
    [T_FLOAT]   = {0x188, 0x28, 0x1c, 0, 0},
    [T_ENUM]    = {0x248, 0x38, 0x1c, 0, 0},
    [T_ENUMARR] = {0x2a8, 0x50, 0, 0x20, 0x28},
    [T_VEC2ARR] = {0x368, 0x48, 0, 0x28, 0x30},
    [T_VEC3]    = {0x3c8, 0x40, 0x24, 0, 0},
    [T_VEC3ARR] = {0x428, 0x48, 0, 0x28, 0x30},
    [T_STRARR]  = {0x668, 0x90, 0, 0x70, 0x78},
};
enum { MAX_TABLE = 1 << 20, MAX_ARRAY = 4096, STR_ELEM = 0x50, NAME_MAX_LEN = 64 };
typedef struct { uint64_t entries, index; int32_t count; uint32_t cap; int state; } Table;   /* state: 0 unread, 1 ok, -1 bad */
typedef struct { companion_read_fn rd; uint64_t main, g; Table t[T_COUNT]; unsigned part; } Ctx;
typedef struct { uint64_t data; uint32_t count; } Arr;

static int rd(Ctx *c, uint64_t a, void *out, size_t n) { return a >= 0x1000 && a <= UINT64_MAX - n && c->rd(a, out, n); }
static uint64_t rd64(Ctx *c, uint64_t a) { uint64_t v = 0; return rd(c, a, &v, 8) ? v : 0; }

static Table *table(Ctx *c, int type) {
    Table *t = &c->t[type];
    if (!t->state) {
        uint8_t h[0x20];   /* count, pad, entries, index, used, capacity */
        t->state = -1;
        if (rd(c, c->g + TYPES[type].base + 8, h, sizeof h)) {
            memcpy(&t->count, h, 4); memcpy(&t->entries, h + 8, 8); memcpy(&t->index, h + 0x10, 8); memcpy(&t->cap, h + 0x1c, 4);
            if (t->count > 0 && t->count <= MAX_TABLE && t->cap && t->cap <= 2 * MAX_TABLE && t->entries && t->index) t->state = 1;
        }
    }
    return t->state > 0 ? t : NULL;
}
static uint64_t lookup(Ctx *c, int type, uint32_t hash) {
    Table *t = table(c, type);
    if (!t || !hash) return 0;
    uint32_t slot = hash % t->cap;
    for (uint32_t probe = 0; probe < t->cap; probe++) {
        uint32_t cell[2];
        if (!rd(c, t->index + 8ull * slot, cell, sizeof cell) || !cell[0]) return 0;
        if (cell[0] == hash) return cell[1] < (uint32_t)t->count ? t->entries + (uint64_t)cell[1] * TYPES[type].stride : 0;
        if (++slot == t->cap) slot = 0;
    }
    return 0;
}
static int value(Ctx *c, int type, const char *name, void *out, size_t n) {
    uint64_t e = lookup(c, type, companion_hash(name));
    return e && rd(c, e + TYPES[type].value, out, n);
}
static int gd_int(Ctx *c, const char *name, int32_t *v) { return value(c, T_INT, name, v, 4); }
static int gd_float(Ctx *c, const char *name, float *v) { return value(c, T_FLOAT, name, v, 4) && isfinite(*v); }
static int gd_bool(Ctx *c, const char *name, int *v) { uint8_t b; if (!value(c, T_BOOL, name, &b, 1)) return 0; *v = b & 1; return 1; }
static const char *gd_enum(Ctx *c, const char *name, const EnumSet *e) { uint32_t h; return value(c, T_ENUM, name, &h, 4) ? enum_name(e, h) : NULL; }
static int gd_vec3(Ctx *c, const char *name, float v[3]) {
    return value(c, T_VEC3, name, v, 12) && isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2]);
}
static int gd_arr(Ctx *c, int type, const char *name, Arr *a) {
    uint64_t e = lookup(c, type, companion_hash(name));
    if (!e || !rd(c, e + TYPES[type].count, &a->count, 4) || !rd(c, e + TYPES[type].data, &a->data, 8)) return 0;
    return a->data && a->count <= MAX_ARRAY;
}
/* n 32-bit elements (ints, floats, enum hashes, packed bool words) from index first */
static int arr_words(Ctx *c, const Arr *a, uint32_t first, uint32_t n, void *out) {
    return first <= a->count && n <= a->count - first && rd(c, a->data + 4ull * first, out, 4ull * n);
}
static int name_ok(const char *s) {
    if (!*s) return 0;
    for (; *s; s++) if (!((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '_')) return 0;
    return 1;
}
/* n strings from index first into out[i] (64 bytes each). A string array element is {char *data; i32 capacity;
   char inline[64]; ...}, 0x50 bytes; data normally points at its own inline buffer. Anything that is not an identifier
   (unused slots, torn or freed storage) comes out empty. */
static int arr_names(Ctx *c, const Arr *a, uint32_t first, uint32_t n, char (*out)[NAME_MAX_LEN]) {
    if (first > a->count || n > a->count - first) return 0;
    uint8_t *raw = malloc((size_t)n * STR_ELEM + 1);
    if (!raw) return 0;
    int ok = rd(c, a->data + (uint64_t)first * STR_ELEM, raw, (size_t)n * STR_ELEM);
    for (uint32_t i = 0; i < n; i++) {
        out[i][0] = 0;
        if (!ok) continue;
        const uint8_t *e = raw + (size_t)i * STR_ELEM;
        uint64_t data; int32_t cap; memcpy(&data, e, 8); memcpy(&cap, e + 8, 4);
        size_t len = cap > 0 && cap < NAME_MAX_LEN ? (size_t)cap : NAME_MAX_LEN;
        char s[NAME_MAX_LEN + 1] = {0};
        uint64_t self = a->data + (uint64_t)(first + i) * STR_ELEM;
        if (data == self + 0xc) memcpy(s, e + 0xc, len);
        else if (!data || !rd(c, data, s, len)) continue;
        s[len < NAME_MAX_LEN ? len : NAME_MAX_LEN - 1] = 0;
        if (memchr(s, 0, len) && name_ok(s)) memcpy(out[i], s, strlen(s) + 1);
    }
    free(raw);
    return ok;
}

/* ---------- player actor (as world_survey.c) ---------- */
#if COMPANION_SUPPORTED
static int pos_ok(const float p[3]) {
    return isfinite(p[0]) && isfinite(p[1]) && isfinite(p[2]) && fabsf(p[0]) <= 8000 && fabsf(p[1]) <= 8000 && fabsf(p[2]) <= 8000;
}
static uint64_t player_actor(Ctx *c) {
    uint64_t module = rd64(c, c->main + SCENE_SLOT), scene = module ? rd64(c, module + 0x1e8) : 0;
    int32_t count = 0;
    if (!scene || !rd(c, scene + 0x50, &count, 4) || count < 14 || count > 256) return 0;
    uint64_t components = rd64(c, scene + 0x58), resident = components ? rd64(c, components + 13 * 8) : 0;
    if (!resident || !rd(c, resident + 0x20, &count, 4) || count < 1 || count > 64) return 0;
    uint64_t links = rd64(c, resident + 0x28);
    if (!links) return 0;
    for (int i = 0; i < count; i++) {
        uint64_t data = rd64(c, links + (uint64_t)i * 0x70 + 8), actor = data ? rd64(c, data + 0x40) : 0;
        uint64_t name = actor ? rd64(c, actor + GAME_ACTOR_NAME) : 0;
        char text[7]; float p[3];
        /* the title screen keeps a Player actor at the origin */
        if (name && rd(c, name, text, sizeof text) && !memcmp(text, "Player", 7) && rd(c, actor + GAME_ACTOR_POS, p, sizeof p) && pos_ok(p) &&
            (p[0] != 0 || p[1] != 0 || p[2] != 0))
            return actor;
    }
    return 0;
}
#endif

/* ---------- snapshot sections ---------- */
static void player(Ctx *c, Json *j, uint64_t actor) {
    int32_t i; float f;
    obj(j, "player");
    static const char *const INTS[][2] = {{"life", "PlayerStatus.Life"}, {"maxLife", "PlayerStatus.MaxLife"},
        {"extraLife", "PlayerStatus.ExtraLife"}, {"breakLife", "PlayerStatus.BreakLife"},
        {"temporaryLife", "PlayerStatus.TemporaryLife"}, {"rupees", "PlayerStatus.CurrentRupee"},
        {"poes", "PlayerStatus.CurrentMamo"}};
    for (unsigned k = 0; k < sizeof INTS / sizeof *INTS; k++) if (gd_int(c, INTS[k][1], &i)) jint(j, INTS[k][0], i);
    static const char *const FLOATS[][2] = {{"maxStamina", "PlayerStatus.MaxStamina"},
        {"extraStamina", "PlayerStatus.ExtraStamina"}, {"maxEnergy", "PlayerStatus.MaxEnergy"},
        {"extraEnergy", "PlayerStatus.ExtraEnergy"}};
    for (unsigned k = 0; k < sizeof FLOATS / sizeof *FLOATS; k++) if (gd_float(c, FLOATS[k][1], &f)) jnum(j, FLOATS[k][0], f);
    const char *s = gd_enum(c, "PlayerStatus.CurrentSpecialPower", &E_POWER);
    if (s) jstr(j, "ability", s);
    int32_t buff, level; float time;
    if (gd_int(c, "PlayerStatus.CookBuff", &buff) && buff > 0 && gd_int(c, "PlayerStatus.CookBuffLv", &level) &&
        gd_float(c, "PlayerStatus.CookBuffTime", &time)) {
        obj(j, "cookBuff"); jint(j, "id", buff); jint(j, "level", level); jnum(j, "time", time); end_obj(j);
    }
    float p[3], rot[9];
#if COMPANION_SUPPORTED
    int live = actor && rd(c, actor + GAME_ACTOR_POS, p, sizeof p) && pos_ok(p);
#else
    int live = 0; (void)actor;
#endif
    if (!live) live = gd_vec3(c, "World_PlayerPos", p);   /* the game's own published position, if no actor */
    if (live) { arr(j, "pos"); jnum(j, NULL, p[0]); jnum(j, NULL, p[1]); jnum(j, NULL, p[2]); end_arr(j); }
#if COMPANION_SUPPORTED
    if (actor && rd(c, actor + GAME_ACTOR_ROT, rot, sizeof rot) && isfinite(rot[2]) && isfinite(rot[8]) &&
        fabsf(rot[2]) <= 2 && fabsf(rot[8]) <= 2 && (rot[2] != 0 || rot[8] != 0)) {
        /* local +Z (column 2 of the row-major rotation) is the facing; 0 degrees = north (-Z), clockwise */
        double h = atan2(rot[2], -rot[8]) * 180.0 / M_PI;
        jnum(j, "heading", h < 0 ? h + 360 : h);
    }
#else
    (void)rot;
#endif
    /* The map layer the game shows (MapData.CurrentLayer), corrected by height for the Depths, which lie far below
       any surface point. */
    const char *layer = gd_enum(c, "MapData.CurrentLayer", &E_LAYER);
    if (live && p[1] < -400) layer = "UnderGround";
    else if (live && layer && !strcmp(layer, "UnderGround") && p[1] > -100) layer = "Ground";
    if (layer) jstr(j, "layer", layer);
    end_obj(j);
}

enum { COL_LIFE = 1, COL_EFFECT = 2, COL_FUSE = 4, COL_STOCK = 8, COL_COLOR = 16, COL_FOOD = 32 };
typedef struct { const char *key, *cat; uint32_t slots; unsigned cols; const char *equip; int game; } Category;
static const Category CATEGORIES[COMPANION_CATS] = {
    {"weapons", "Weapon", 20, COL_LIFE | COL_EFFECT | COL_FUSE, "EquipIndex", 0},
    {"bows", "Bow", 14, COL_LIFE | COL_EFFECT, "EquipIndex", 1},
    {"shields", "Shield", 20, COL_LIFE | COL_EFFECT | COL_FUSE, "EquipIndex", 3},
    {"arrows", "Arrow", 1, COL_STOCK, "EquipIndex", 2},
    {"armor", "Armor", 150, COL_COLOR, NULL, 4},
    {"materials", "Material", 255, COL_STOCK, NULL, 5},
    {"food", "Food", 60, COL_STOCK | COL_FOOD, NULL, 6},
    {"zonai", "SpecialParts", 30, COL_STOCK, NULL, 7},
    {"keyItems", "KeyItem", 100, COL_STOCK, NULL, 8},
};
int companion_category(const char *key) {
    for (int k = 0; k < COMPANION_CATS; k++) if (!strcmp(CATEGORIES[k].key, key)) return k;
    return -1;
}
int companion_category_game(int key) { return CATEGORIES[key].game; }
const char *companion_category_key(int key) { return CATEGORIES[key].key; }
enum { MAX_SLOTS = 255 };
/* One column of the current partition, or NULL when the flag is missing or not the expected shape. */
static const int32_t *column(Ctx *c, int type, const char *cat, const char *field, uint32_t slots, uint32_t per, int32_t *buf) {
    char name[96]; Arr a; snprintf(name, sizeof name, "Pouch.%s.%s", cat, field);
    if (!gd_arr(c, type, name, &a) || a.count < 2 * slots * per) return NULL;
    return arr_words(c, &a, c->part * slots * per, slots * per, buf) ? buf : NULL;
}
static int equip_index(Ctx *c, const char *flag) {
    Arr a; int32_t v[2];
    return gd_arr(c, T_INTARR, flag, &a) && a.count >= 2 && arr_words(c, &a, 0, 2, v) ? v[c->part] : -1;
}
static void pouch(Ctx *c, Json *j, const CompanionView *base) {
    static char names[MAX_SLOTS][NAME_MAX_LEN], fuse[MAX_SLOTS][NAME_MAX_LEN], mats[60 * 5][NAME_MAX_LEN];
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;   /* the name buffers are large: shared, one snapshot at a time */
    static int32_t life[MAX_SLOTS], ev[MAX_SLOTS], et[MAX_SLOTS], fl[MAX_SLOTS], stock[MAX_SLOTS], color[MAX_SLOTS],
        heal[60], flevel[60], ftime[60], price[60];
    pthread_mutex_lock(&mu);
    int armor_eq[3] = {equip_index(c, "Pouch.Armor.EquipIndexes.Head"), equip_index(c, "Pouch.Armor.EquipIndexes.Upper"),
        equip_index(c, "Pouch.Armor.EquipIndexes.Bottom")};
    obj(j, "pouch");
    for (unsigned k = 0; k < sizeof CATEGORIES / sizeof *CATEGORIES; k++) {
        const Category *cat = &CATEGORIES[k];
        char flag[96]; Arr a; snprintf(flag, sizeof flag, "Pouch.%s.Content.Name", cat->cat);
        if (!gd_arr(c, T_STRARR, flag, &a) || a.count < 2 * cat->slots || !arr_names(c, &a, c->part * cat->slots, cat->slots, names)) continue;
        const int32_t *L = cat->cols & COL_LIFE ? column(c, T_INTARR, cat->cat, "Content.Life", cat->slots, 1, life) : NULL;
        const int32_t *ET = cat->cols & COL_EFFECT ? column(c, T_ENUMARR, cat->cat, "Content.Effect.Type", cat->slots, 1, et) : NULL;
        const int32_t *EV = cat->cols & COL_EFFECT ? column(c, T_INTARR, cat->cat, "Content.Effect.Value", cat->slots, 1, ev) : NULL;
        const int32_t *FL = cat->cols & COL_FUSE ? column(c, T_INTARR, cat->cat, "Content.Combined.Life", cat->slots, 1, fl) : NULL;
        const int32_t *S = cat->cols & COL_STOCK ? column(c, T_INTARR, cat->cat, "Content.StockNum", cat->slots, 1, stock) : NULL;
        const int32_t *CV = cat->cols & COL_COLOR ? column(c, T_ENUMARR, cat->cat, "Content.ColorVariation", cat->slots, 1, color) : NULL;
        const int32_t *H = NULL, *FLV = NULL, *FT = NULL, *PR = NULL, *FE = NULL;
        int fused = 0, food_mats = 0;
        if (cat->cols & COL_FUSE) {
            snprintf(flag, sizeof flag, "Pouch.%s.Content.Combined.Name", cat->cat);
            fused = gd_arr(c, T_STRARR, flag, &a) && a.count >= 2 * cat->slots && arr_names(c, &a, c->part * cat->slots, cat->slots, fuse);
        }
        if (cat->cols & COL_FOOD) {
            H = column(c, T_INTARR, cat->cat, "Content.LifeRecover", cat->slots, 1, heal);
            FE = column(c, T_ENUMARR, cat->cat, "Content.Effect.Type", cat->slots, 1, et);
            FLV = column(c, T_INTARR, cat->cat, "Content.Effect.Level", cat->slots, 1, flevel);
            FT = column(c, T_INTARR, cat->cat, "Content.Effect.Time", cat->slots, 1, ftime);
            PR = column(c, T_INTARR, cat->cat, "Content.Price", cat->slots, 1, price);
            food_mats = gd_arr(c, T_STRARR, "Pouch.Food.Content.MaterialName", &a) && a.count >= 2 * cat->slots * 5 &&
                        arr_names(c, &a, c->part * cat->slots * 5, cat->slots * 5, mats);
        }
        int equipped = -1;
        if (cat->equip) { snprintf(flag, sizeof flag, "Pouch.%s.%s", cat->cat, cat->equip); equipped = equip_index(c, flag); }
        arr(j, cat->key);
        for (uint32_t s = 0; s < cat->slots; s++) {
            if (!names[s][0]) continue;
            obj(j, NULL);
            jstr(j, "name", names[s]); jint(j, "slot", s);
            if (L) jint(j, "life", L[s]);
            if (S) jint(j, "count", S[s]);
            if (ET) {
                const char *e = enum_name(&E_EFFECT, (uint32_t)ET[s]);
                if (e && strcmp(e, "None")) { obj(j, "effect"); jstr(j, "type", e); if (EV) jint(j, "value", EV[s]); end_obj(j); }
            }
            if (fused && fuse[s][0]) { jstr(j, "fuse", fuse[s]); if (FL) jint(j, "fuseLife", FL[s]); }
            if (CV) { const char *col = enum_name(&E_COLOR, (uint32_t)CV[s]); if (col) jstr(j, "color", col); }
            if (cat->cols & COL_FOOD) {
                if (H) jint(j, "heal", H[s]);
                const char *e = FE ? enum_name(&E_FOOD, (uint32_t)FE[s]) : NULL;
                if (e && strcmp(e, "None")) {
                    jstr(j, "effect", e);
                    if (FLV) jint(j, "level", FLV[s]);
                    if (FT) jint(j, "time", FT[s]);
                }
                if (PR) jint(j, "price", PR[s]);
                if (food_mats) {
                    arr(j, "materials");
                    for (unsigned m = 0; m < 5; m++) if (mats[s * 5 + m][0]) jstr(j, NULL, mats[s * 5 + m]);
                    end_arr(j);
                }
            }
            if (cat->equip) jbool(j, "equipped", (int)s == equipped);
            else if (cat->cols & COL_COLOR) jbool(j, "equipped", (int)s == armor_eq[0] || (int)s == armor_eq[1] || (int)s == armor_eq[2]);
            /* what the game's own menus offer for this item now (companion_actions.c) */
            CompanionView v = *base;
            snprintf(v.name, sizeof v.name, "%s", names[s]);
            snprintf(v.fuse, sizeof v.fuse, "%s", fused ? fuse[s] : "");
            v.life_item = L ? L[s] : INT32_MIN; v.stock = S ? S[s] : INT32_MIN; v.equip_index = equipped;
            v.equipped = cat->equip ? (int)s == equipped : (cat->cols & COL_COLOR) && ((int)s == armor_eq[0] || (int)s == armor_eq[1] || (int)s == armor_eq[2]);
            v.has_effect = FE && (uint32_t)FE[s] != NONE_HASH;
            CompanionAction act[3]; unsigned n = companion_item_actions((int)k, &v, act);
            arr(j, "actions");
            for (unsigned i = 0; i < n; i++) {
                obj(j, NULL); jstr(j, "id", act[i].id); jbool(j, "enabled", act[i].enabled); jstr(j, "reason", act[i].reason); end_obj(j);
            }
            end_arr(j);
            end_obj(j);
        }
        end_arr(j);
    }
    end_obj(j);
    obj(j, "slots");
    static const char *const VALID[][2] = {{"weapons", "Pouch.Weapon.ValidNum"}, {"bows", "Pouch.Bow.ValidNum"}, {"shields", "Pouch.Shield.ValidNum"}};
    for (unsigned k = 0; k < 3; k++) { int v = equip_index(c, VALID[k][1]); if (v > 0) jint(j, VALID[k][0], v); }
    end_obj(j);
    Arr a;
    if (gd_arr(c, T_STRARR, "Pouch.SpecialPower.Content.Name", &a) && a.count >= 20 && arr_names(c, &a, c->part * 10, 10, names)) {
        arr(j, "abilities");
        for (unsigned s = 0; s < 10; s++) if (names[s][0]) jstr(j, NULL, names[s]);
        end_arr(j);
    }
    pthread_mutex_unlock(&mu);
}
static void sages(Ctx *c, Json *j) {
    static const char *const SAGES[] = {"Wind", "Fire", "Water", "Electric", "Soul"};
    arr(j, "sages");
    for (unsigned k = 0; k < 5; k++) {
        char f[96]; int got = 0, summon = 0; int32_t mn = 0, sec = 0; float energy;
        snprintf(f, sizeof f, "PlayerStatus.Companion.%s.IsGet", SAGES[k]);
        if (!gd_bool(c, f, &got)) continue;
        obj(j, NULL); jstr(j, "id", SAGES[k]); jbool(j, "joined", got);
        snprintf(f, sizeof f, "PlayerStatus.Companion.%s.IsSummon", SAGES[k]);
        if (gd_bool(c, f, &summon)) jbool(j, "summoned", summon);
        snprintf(f, sizeof f, "PlayerStatus.Companion.%s.JoiningCondition", SAGES[k]);
        const char *cond = gd_enum(c, f, &E_JOIN); if (cond) jstr(j, "condition", cond);
        snprintf(f, sizeof f, "PlayerStatus.Companion.%s.ForUI.RemainingMinute", SAGES[k]);
        int have = gd_int(c, f, &mn);
        snprintf(f, sizeof f, "PlayerStatus.Companion.%s.ForUI.RemainingSecond", SAGES[k]);
        if (have && gd_int(c, f, &sec) && mn >= 0 && sec >= 0) jint(j, "cooldown", (long long)mn * 60 + sec);
        snprintf(f, sizeof f, "PlayerStatus.Companion.%s.RemainingEnergy", SAGES[k]);
        if (gd_float(c, f, &energy)) jnum(j, "energy", energy);
        end_obj(j);
    }
    end_arr(j);
}
static void map_marks(Ctx *c, Json *j) {
    enum { MAX_MARKS = 300 };
    static float pos[MAX_MARKS * 3]; static uint32_t type[MAX_MARKS], layer[MAX_MARKS];
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu);
    obj(j, "map");
    Arr p, t, l;
    arr(j, "pins");
    if (gd_arr(c, T_VEC3ARR, "MapData.IconData.MapPinData.Pos", &p) && gd_arr(c, T_ENUMARR, "MapData.IconData.MapPinData.Type", &t)) {
        uint32_t n = p.count < t.count ? p.count : t.count; if (n > MAX_MARKS) n = MAX_MARKS;
        if (rd(c, p.data, pos, 12ull * n) && arr_words(c, &t, 0, n, type))
            for (uint32_t i = 0; i < n; i++) {
                const char *ty = enum_name(&E_PIN, type[i]);
                if (!ty || !strcmp(ty, "Invalid") || !isfinite(pos[i * 3]) || !isfinite(pos[i * 3 + 1]) || !isfinite(pos[i * 3 + 2])) continue;
                obj(j, NULL); jnum(j, "x", pos[i * 3]); jnum(j, "y", pos[i * 3 + 1]); jnum(j, "z", pos[i * 3 + 2]); jstr(j, "type", ty); end_obj(j);
            }
    }
    end_arr(j);
    arr(j, "stamps");
    if (gd_arr(c, T_VEC2ARR, "MapData.IconData.StampData.Pos", &p) && gd_arr(c, T_ENUMARR, "MapData.IconData.StampData.Type", &t) &&
        gd_arr(c, T_ENUMARR, "MapData.IconData.StampData.Layer", &l)) {
        uint32_t n = p.count < t.count ? p.count : t.count; if (l.count < n) n = l.count; if (n > MAX_MARKS) n = MAX_MARKS;
        if (rd(c, p.data, pos, 8ull * n) && arr_words(c, &t, 0, n, type) && arr_words(c, &l, 0, n, layer))
            for (uint32_t i = 0; i < n; i++) {
                const char *ty = enum_name(&E_STAMP, type[i]), *ly = enum_name(&E_LAYER, layer[i]);
                if (!ty || !strcmp(ty, "Invalid") || !isfinite(pos[i * 2]) || !isfinite(pos[i * 2 + 1])) continue;
                obj(j, NULL); jnum(j, "x", pos[i * 2]); jnum(j, "z", pos[i * 2 + 1]); jstr(j, "type", ty);
                if (ly) jstr(j, "layer", !strcmp(ly, "Underground") ? "UnderGround" : ly);
                end_obj(j);
            }
    }
    end_arr(j);
    end_obj(j);
    pthread_mutex_unlock(&mu);
}

/* ---------- item actions: the game state around an item (companion_actions.c decides with it) ---------- */
static void mix(uint64_t *h, const void *p, size_t n) {   /* FNV-1a */
    for (const uint8_t *b = p; n--; b++) *h = (*h ^ *b) * 0x100000001b3ull;
}
/* The identity of the current partition's whole pouch: every category's names, durability, fuse, stock and equipment.
   A reordered, replaced (a new sword of the same kind has its own durability), used up or re-equipped item changes it.
   0 when the pouch cannot be read. */
static uint64_t pouch_ident(Ctx *c) {
    char (*names)[NAME_MAX_LEN] = malloc(sizeof(char[MAX_SLOTS][NAME_MAX_LEN]));
    int32_t *w = malloc(sizeof(int32_t[MAX_SLOTS]));
    uint64_t h = 0xcbf29ce484222325ull;
    int ok = names && w;
    mix(&h, &c->part, sizeof c->part);
    for (unsigned k = 0; ok && k < COMPANION_CATS; k++) {
        const Category *cat = &CATEGORIES[k];
        char flag[96]; Arr a; snprintf(flag, sizeof flag, "Pouch.%s.Content.Name", cat->cat);
        if (!gd_arr(c, T_STRARR, flag, &a) || a.count < 2 * cat->slots || !arr_names(c, &a, c->part * cat->slots, cat->slots, names)) { ok = 0; break; }
        for (uint32_t s = 0; s < cat->slots; s++) mix(&h, names[s], strlen(names[s]) + 1);
        static const struct { unsigned col; const char *field; } INTS[] = {{COL_LIFE, "Content.Life"}, {COL_STOCK, "Content.StockNum"}};
        for (unsigned i = 0; i < 2; i++) {
            if (!(cat->cols & INTS[i].col)) continue;
            const int32_t *v = column(c, T_INTARR, cat->cat, INTS[i].field, cat->slots, 1, w);
            if (v) mix(&h, v, 4ull * cat->slots); else mix(&h, "?", 1);
        }
        if (cat->cols & COL_FUSE) {
            snprintf(flag, sizeof flag, "Pouch.%s.Content.Combined.Name", cat->cat);
            if (gd_arr(c, T_STRARR, flag, &a) && a.count >= 2 * cat->slots && arr_names(c, &a, c->part * cat->slots, cat->slots, names))
                for (uint32_t s = 0; s < cat->slots; s++) mix(&h, names[s], strlen(names[s]) + 1);
            else mix(&h, "?", 1);
        }
        if (cat->equip) { snprintf(flag, sizeof flag, "Pouch.%s.%s", cat->cat, cat->equip); int e = equip_index(c, flag); mix(&h, &e, 4); }
    }
    static const char *const ARMOR[] = {"Pouch.Armor.EquipIndexes.Head", "Pouch.Armor.EquipIndexes.Upper", "Pouch.Armor.EquipIndexes.Bottom"};
    for (unsigned i = 0; ok && i < 3; i++) { int e = equip_index(c, ARMOR[i]); mix(&h, &e, 4); }
    free(names); free(w);
    return ok ? (h ? h : 1) : 0;
}
/* One value of the current partition's column at slot, INT32_MIN when it cannot be read. */
static int32_t slot_int(Ctx *c, int type, const char *cat, const char *field, uint32_t slots, uint32_t slot) {
    char name[96]; Arr a; int32_t v;
    snprintf(name, sizeof name, "Pouch.%s.%s", cat, field);
    return gd_arr(c, type, name, &a) && a.count >= 2 * slots && arr_words(c, &a, c->part * slots + slot, 1, &v) ? v : INT32_MIN;
}
static void view_state(Ctx *c, const char *status, int part_ok, CompanionView *v) {
    memset(v, 0, sizeof *v);
    v->status = status; v->receiver = -1; v->life_item = v->stock = INT32_MIN; v->equip_index = -1;
#if COMPANION_SUPPORTED
    if (strcmp(status, "ready")) return;
    if (!part_ok) { v->status = "starting"; return; }
    static _Atomic uint64_t checked_main; static _Atomic int checked_ok;
    if (atomic_load(&checked_main) != c->main) {
        int ok = 1;
        for (unsigned i = 0; i < sizeof ACTION_GOT / sizeof *ACTION_GOT; i++) if (rd64(c, c->main + ACTION_GOT[i].got) != c->main + ACTION_GOT[i].target) ok = 0;
        atomic_store(&checked_ok, ok); atomic_store(&checked_main, c->main);
    }
    if (!atomic_load(&checked_ok)) { v->status = "unsupported"; return; }
    v->part = c->part;
    v->pouch = rd64(c, c->main + POUCH_SLOT);
    if (!v->pouch || !rd(c, v->pouch + POUCH_RECEIVER, &v->receiver, 4)) v->receiver = -1;
    uint8_t menu = 0;   /* unreadable counts as open */
    v->pause_menu = !v->pouch || !rd(c, v->pouch + POUCH_MENU, &menu, 1) || menu != 0;
    uint64_t ps = rd64(c, c->main + PLAYER_STATE_SLOT), ev = rd64(c, c->main + EVENT_SLOT);
    if (!ps || !rd(c, ps + PLAYER_FLAGS, &v->player_flags, 8)) { v->status = "noPlayer"; return; }
    v->event = !ev || rd64(c, ev + EVENT_CONTEXT) != 0;
    /* SeqQueryIsLoadingPause (main+0x20134b8): bit 1 of +0x78 in the scene's component 29 */
    uint64_t module = rd64(c, c->main + SCENE_SLOT), scene = module ? rd64(c, module + 0x1e8) : 0, comp = 0;
    int32_t count = 0; uint32_t mode = 0;
    if (scene && rd(c, scene + 0x50, &count, 4) && count > 29 && (comp = rd64(c, rd64(c, scene + 0x58) + 29 * 8)) &&
        rd(c, comp + 0x78, &mode, 4)) v->loading = (mode & 2) != 0;
    if (!gd_int(c, "PlayerStatus.Life", &v->life) || !gd_int(c, "PlayerStatus.MaxLife", &v->max_life) ||
        !gd_int(c, "PlayerStatus.BreakLife", &v->break_life)) v->life = v->max_life = v->break_life = INT32_MIN;
    uint64_t e = lookup(c, T_BOOL, 0x1c96382f); uint8_t b = 0;
    v->zonai_unlocked = e && rd(c, e + TYPES[T_BOOL].value, &b, 1) && (b & 1);
    if (!(v->ident = pouch_ident(c))) v->status = "starting";
#else
    (void)c; (void)part_ok;
#endif
}
static void view_item(Ctx *c, int key, uint32_t slot, CompanionView *v) {
    const Category *cat = &CATEGORIES[key];
    if (slot >= cat->slots) return;
    char flag[96], name[1][NAME_MAX_LEN]; Arr a;
    snprintf(flag, sizeof flag, "Pouch.%s.Content.Name", cat->cat);
    if (gd_arr(c, T_STRARR, flag, &a) && a.count >= 2 * cat->slots && arr_names(c, &a, c->part * cat->slots + slot, 1, name))
        memcpy(v->name, name[0], sizeof v->name);
    if (cat->cols & COL_FUSE) {
        snprintf(flag, sizeof flag, "Pouch.%s.Content.Combined.Name", cat->cat);
        if (gd_arr(c, T_STRARR, flag, &a) && a.count >= 2 * cat->slots && arr_names(c, &a, c->part * cat->slots + slot, 1, name))
            memcpy(v->fuse, name[0], sizeof v->fuse);
    }
    if (cat->cols & COL_LIFE) v->life_item = slot_int(c, T_INTARR, cat->cat, "Content.Life", cat->slots, slot);
    if (cat->cols & COL_STOCK) v->stock = slot_int(c, T_INTARR, cat->cat, "Content.StockNum", cat->slots, slot);
    if (cat->cols & COL_FOOD) {
        int32_t e = slot_int(c, T_ENUMARR, cat->cat, "Content.Effect.Type", cat->slots, slot);
        v->has_effect = e != INT32_MIN && (uint32_t)e != NONE_HASH;
    }
    if (cat->equip) {
        snprintf(flag, sizeof flag, "Pouch.%s.%s", cat->cat, cat->equip);
        v->equip_index = equip_index(c, flag); v->equipped = v->equip_index == (int)slot;
    } else if (cat->cols & COL_COLOR) {
        static const char *const ARMOR[] = {"Pouch.Armor.EquipIndexes.Head", "Pouch.Armor.EquipIndexes.Upper", "Pouch.Armor.EquipIndexes.Bottom"};
        for (unsigned i = 0; i < 3; i++) if (equip_index(c, ARMOR[i]) == (int)slot) v->equipped = 1;
    }
}

/* Which state the game is in, from what this sample can see. "ready" needs the manager, a plausible MaxLife and the
   player actor; part_ok says whether the pouch partition selector was read (without it the inventory is left out
   rather than shown from a partition that may not be the current one). */
#if COMPANION_SUPPORTED
/* The host test lays out its own stand-in words and declares their hash the expected one; the app always compares with
   the 1.4.3 hash. */
static const char GUARD_SHA256[] = "0a2d05de9f11934c5e775c1d61b344fe609fdb9336a8e64ebd38fd706ec6b8cb";
#ifdef COMPANION_ACTIONS_TEST
static char test_guard[65];
void companion_snapshot_test_expect_guard(const void *words, size_t n) { companion_sha256_hex(words, n, test_guard); }
static const char *guard_expected(void) { return *test_guard ? test_guard : GUARD_SHA256; }
#else
static const char *guard_expected(void) { return GUARD_SHA256; }
#endif
#endif
static const char *gate(Ctx *c, uint64_t *actor, int *part_ok) {
    *actor = 0; *part_ok = 0;
#if COMPANION_SUPPORTED
    /* The offsets above belong to one build of main: before trusting them, check the instructions that use them (once
       per mapping), by the SHA-256 of their words in this order. Another build fails closed as "unsupported". */
    static const uint32_t GUARD[] = {
        0x787554,    /* GameDataMgr getter: ldr x19, [x22, #0x6b0] */
        0x7875a4,    /* ... install: str x19, [x22, #0x6b0] */
        0x61fde4,    /* getInt: ldr x8, [x0, #0xe0] (Int hash index) */
        0x61fe90,    /* getInt: ldr x10, [x0, #0xd8] (Int entries) */
        0x282964,    /* pouch selector: ldr x8, [x8, #0x1e8] (GOT -> main+0x3a81678) */
        0x282970,    /* ... ldr w8, [x8, #0x188] */
        0x2097688,   /* pause-menu flag reader: mov w9, #0xfcc0 */
        0x209768c,   /* ... movk w9, #1, lsl #16 (PouchMgr +0x1fcc0) */
    };
    static _Atomic uint64_t checked_main; static _Atomic int checked_ok;
    if (!c->main) return "starting";
    if (atomic_load(&checked_main) != c->main) {
        uint32_t words[sizeof GUARD / sizeof *GUARD]; char hex[65];
        for (unsigned i = 0; i < sizeof GUARD / sizeof *GUARD; i++)
            if (!rd(c, c->main + GUARD[i], &words[i], 4)) return "starting";   /* not mapped yet */
        companion_sha256_hex(words, sizeof words, hex);
        atomic_store(&checked_ok, !strcmp(hex, guard_expected())); atomic_store(&checked_main, c->main);
    }
    if (!atomic_load(&checked_ok)) return "unsupported";
    int32_t max_life = 0;
    if (!(c->g = rd64(c, c->main + GDM_SLOT)) || !gd_int(c, "PlayerStatus.MaxLife", &max_life) || max_life < 4 ||
        max_life > 400) return "starting";
    uint64_t p = rd64(c, c->main + POUCH_SLOT); uint32_t part = 0;
    if (p && rd(c, p + POUCH_PART, &part, 4) && part < 2) { c->part = part; *part_ok = 1; }
    *actor = player_actor(c);
    return *actor ? "ready" : "title";
#else
    (void)c; return "unsupported";
#endif
}
int companion_view(uintptr_t main_base, companion_read_fn rdf, int key, int slot, CompanionView *v) {
    pthread_once(&enum_once, enum_init);
    Ctx c = {.rd = rdf, .main = main_base};
    uint64_t actor; int part_ok;
    const char *status = gate(&c, &actor, &part_ok);
    view_state(&c, status, part_ok, v);
    v->actor = actor;
    if (strcmp(v->status, "ready")) return 0;
    if (key >= 0 && key < COMPANION_CATS && slot >= 0) view_item(&c, key, (uint32_t)slot, v);
    return 1;
}
static void emit(Ctx *c, Json *j, const char *status, unsigned long long seq, uint64_t actor, int part_ok, int detail) {
    obj(j, NULL);
    jint(j, "schema", 1);
    jstr(j, "status", status);
    jint(j, "seq", (long long)seq);
#ifdef TOTK_GAME_VERSION
    jstr(j, "gameVersion", TOTK_GAME_VERSION);
#endif
    if (!strcmp(status, "ready")) {
        player(c, j, actor);
        /* actions: the game-side state of the last frame-end tick (live too, so aiming or an event shows at once); a
           full snapshot also checks the pouch the rules would use */
        CompanionView base; int full = detail == COMPANION_FULL && part_ok;
        if (full) view_state(c, status, part_ok, &base);
        int ok, pause_menu, aiming; const char *reason;
        companion_action_state(&ok, &reason, &pause_menu, &aiming);
        if (full && strcmp(base.status, "ready")) {
            ok = 0; reason = !strcmp(base.status, "unsupported") || !strcmp(base.status, "noPlayer") ? base.status : "loading";
        }
        obj(j, "actionState"); jbool(j, "ok", ok); jstr(j, "reason", reason); jbool(j, "pauseMenu", pause_menu);
        jbool(j, "aiming", aiming); end_obj(j);
        if (full) {
            jint(j, "partition", c->part);
            if (base.ident) jint(j, "pouchGen", companion_pouch_gen(base.ident));
            pouch(c, j, &base);
        }
        if (detail == COMPANION_FULL) {
            sages(c, j);
            map_marks(c, j);
        }
    }
    end_obj(j);
}

/* The values of a "ready" sample come from many separate reads. A sample taken while the game tore down or rebuilt its
   state (title return, save load) shows up as a different manager, Int table, pouch partition or player actor
   afterwards: 1 when a second look at these agrees with the first. Changes that keep all of them (an item picked up
   mid-sample) only make individual fields one sample old or new. */
static int settled(Ctx *c, uint64_t actor, int part_ok) {
    Ctx d = {.rd = c->rd, .main = c->main};
    uint64_t actor2; int part_ok2;
    return !strcmp(gate(&d, &actor2, &part_ok2), "ready") && d.g == c->g && actor2 == actor && part_ok2 == part_ok &&
           d.part == c->part && table(&d, T_INT) && c->t[T_INT].entries == d.t[T_INT].entries;
}

void companion_state(uintptr_t main_base, companion_read_fn rdf, CompanionState *out) {
    pthread_once(&enum_once, enum_init);
    for (int attempt = 0; attempt < 2; attempt++) {
        companion_state_clear(out);
        Ctx c = {.rd = rdf, .main = main_base};
        uint64_t actor; int part_ok;
        const char *status = gate(&c, &actor, &part_ok);
        out->status = !strcmp(status, "ready") ? COMPANION_STATE_READY : !strcmp(status, "title") ? COMPANION_STATE_TITLE
                    : !strcmp(status, "unsupported") ? COMPANION_STATE_UNSUPPORTED : COMPANION_STATE_STARTING;
        if (out->status != COMPANION_STATE_READY) return;
        int32_t life, max_life; int ok, pause_menu, aiming; const char *reason;
        if (gd_int(&c, "PlayerStatus.Life", &life)) out->life = life;
        if (gd_int(&c, "PlayerStatus.MaxLife", &max_life)) out->max_life = max_life;
        companion_action_state(&ok, &reason, &pause_menu, &aiming);
        out->in_game = 1; out->menu_open = pause_menu != 0;
        if (settled(&c, actor, part_ok)) return;
    }
    companion_state_clear(out); out->status = COMPANION_STATE_TITLE;   /* moved twice: the transition, not mixed values */
}

size_t companion_snapshot(uintptr_t main_base, int detail, companion_read_fn rdf, CompanionBuf *out) {
    static _Atomic unsigned long long counter;
    pthread_once(&enum_once, enum_init);
    unsigned long long seq = ++counter;
    for (int attempt = 0; attempt < 2; attempt++) {
        out->len = 0; out->failed = 0;
        Json j = {.b = out};
        Ctx c = {.rd = rdf, .main = main_base};
        uint64_t actor; int part_ok;
        const char *status = gate(&c, &actor, &part_ok);
        emit(&c, &j, status, seq, actor, part_ok, detail);
        if (strcmp(status, "ready")) break;
        /* A sample that moved while it was taken (settled): take it again, and if it moves again report the transition
           instead of mixed values. */
        if (settled(&c, actor, part_ok)) break;
        if (attempt) { out->len = 0; out->failed = 0; Json t = {.b = out}; emit(&c, &t, "title", seq, 0, 0, detail); }
    }
    return out->failed ? 0 : out->len;
}
