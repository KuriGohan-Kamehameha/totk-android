/* GameData values for the Eden Duo page that companion_snapshot.c does not report: current stamina and whether Link is
   out of breath, and how many shrines have been found. Read-only: memory reads through the caller's fault-tolerant
   reader, nothing written or called. MIT, like the reader (LICENSE.MIT).

   The GameDataMgr layout is companion_snapshot.c's (TOTK 1.4.3, see there): *(main + 0x3ad66b0) holds the manager; one
   table per flag type at manager + base {vtable; i32 count; entries; hash index; u32 used; u32 capacity}; the index is
   open addressing over {u32 hash, u32 entry} cells (slot = hash % capacity, linear probing, a zero hash ends the probe);
   the committed value sits at a fixed offset of each entry. Names and enum values are MurmurHash3 of the full dotted name
   (companion_hash). The flags were checked by hash against GameData/GameDataList.Product.140 of the 1.4.3 romfs:
   PlayerStatus.Stamina (Float, default 1000), PlayerStatus.IsTired (Bool), DungeonState.Dungeon000..151 (Enum
   {Hidden, Appear, Open, Enter, Clear}). */
#include "totk_gamedata.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

enum { GDM_SLOT = 0x3ad66b0, MAX_TABLE = 1 << 20 };
enum { T_BOOL, T_FLOAT, T_ENUM, T_COUNT };
static const struct { uint16_t base, stride, value; } TYPES[T_COUNT] = {
    [T_BOOL] = {0x008, 0x18, 0x16},
    [T_FLOAT] = {0x188, 0x28, 0x1c},
    [T_ENUM] = {0x248, 0x38, 0x1c},
};
typedef struct { uint64_t entries, index; int32_t count; uint32_t cap; int state; } Table;
typedef struct { companion_read_fn rd; uint64_t g; Table t[T_COUNT]; } Ctx;

static int rd(Ctx *c, uint64_t a, void *out, size_t n) { return a >= 0x1000 && a <= UINT64_MAX - n && c->rd(a, out, n); }

static Table *table(Ctx *c, int type) {
    Table *t = &c->t[type];
    if (!t->state) {
        uint8_t h[0x20];
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

int totk_gamedata_read(uintptr_t main_base, companion_read_fn rdf, int with_shrines, TotkGameData *out) {
    memset(out, 0, sizeof *out);
    Ctx c = {.rd = rdf};
    if (!main_base || !rd(&c, main_base + GDM_SLOT, &c.g, 8) || !c.g) return 0;

    float stamina, max_stamina;
    if (value(&c, T_FLOAT, "PlayerStatus.Stamina", &stamina, 4) && value(&c, T_FLOAT, "PlayerStatus.MaxStamina", &max_stamina, 4) &&
        isfinite(stamina) && isfinite(max_stamina) && max_stamina >= 1 && max_stamina <= 100000 && stamina >= 0 &&
        stamina <= 4 * max_stamina) {
        out->stamina_ok = 1; out->stamina = stamina; out->max_stamina = max_stamina;
    }
    uint8_t b;
    if (value(&c, T_BOOL, "PlayerStatus.IsTired", &b, 1)) { out->tired_ok = 1; out->tired = b & 1; }

    if (!with_shrines) return 1;
    const uint32_t open = companion_hash("Open"), enter = companion_hash("Enter"), clear = companion_hash("Clear");
    int found = 0, cleared = 0, all = 1;
    for (int i = 0; i < TOTK_SHRINES && all; i++) {
        char name[32]; uint32_t h;
        snprintf(name, sizeof name, "DungeonState.Dungeon%03d", i);
        if (!value(&c, T_ENUM, name, &h, 4)) { all = 0; break; }
        found += h == open || h == enter || h == clear;
        cleared += h == clear;
    }
    if (all) { out->shrines_ok = 1; out->shrines_found = found; out->shrines_clear = cleared; }
    return 1;
}
