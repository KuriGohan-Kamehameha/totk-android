/* IPS patches of main that a mod ships in exefs/ (a mod manager's merge puts them beside romfs/, runtime/romfs.c):
   <mods>/exefs/<build id>.ips, in the IPS32 or the classic PATCH form, as the console's loader takes them. A record's
   offset counts the 0x100-byte NSO header, so it patches main + offset - 0x100. Shared by the two game-patch files and
   the game-data-free regression (tests/test_mod_ips.c).

   One file is applied, whole or not at all, after the relocations and before this runtime's own patches (whose checks
   of the original words then refuse a file that overlaps them) and before main is made executable. It is applied only
   when every record passes:
   - the file name is main's build id, complete: the one GNU build-id note in main's read-only data, zero padded to
     32 bytes on both sides, so no id is compiled in. Two files that both name it are refused;
   - a record lies inside one of main's text, read-only data or data, records do not overlap, and none overwrites a
     pointer the loader relocated;
   - in text, a record is whole instructions, and none of them is one this port changed or took out of use:
       a rewritten site (a branch into the patch area behind the image);
       the call of a prepared call (blr x30 behind a rewritten site: it was blr x18, call_window.py);
       a parameter gate (mrs tpidrro_el0 and the load behind it; none is registered for this game);
       an instruction within MOD_IPS_WINDOW instructions behind a rewritten site that covers more than itself: an
       exclusive window (at most 16 instructions from the load to its store, and a cbz / cbnz: patch_nso.py
       find_windows, rewrite.c). Its remaining instructions no longer run, so a patch there would silently do
       nothing. The code of a rewritten site leaves only to the instruction behind the site, to the end of its span
       or window, or to a branch target outside it (structck.py, check 10), so a site whose first branch back into
       text goes to site + 4 covers one instruction; every other site is taken for a window of the full length;
   - each new instruction is one of the forms this check decodes, names no x18 (the platform register here: every use
     in the game was rewritten, and a patch cannot be), is no exception, system or hint instruction other than nop, and,
     when it branches, lands in text on an instruction that still runs.
   This is about what the port changed, not about whether the patch is sensible code. Anything else stops the start
   with the file, the address and the reason; TOTK_MOD_IPS=0 leaves the files unread. */
#pragma once
#include "runtime.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define MOD_IPS_RECORDS 4096          /* per file */
#define MOD_IPS_FILE_MAX (1u << 20)   /* bytes */
#define MOD_IPS_WINDOW 17             /* instructions a rewritten window can span: 16 from the exclusive load to its store, one cbz / cbnz */
#define MOD_IPS_STUB_WORDS 64         /* instructions of a rewritten site's code that are read */
#define MOD_IPS_BLR_X30 0xd63f03c0u

typedef struct { uint64_t addr; uint32_t len; const uint8_t *data; uint8_t fill, rle; } ModIpsRecord;

static inline uint32_t mod_ips_word(const Module *m, uint64_t a) { uint32_t w; memcpy(&w, m->base + a, 4); return w; }

/* The target of the b or bl at a, as an offset in the module; UINT64_MAX when a holds something else. */
static inline uint64_t mod_ips_branch(const Module *m, uint64_t a, int *link) {
    uint32_t w = mod_ips_word(m, a);
    if ((w & 0x7c000000) != 0x14000000) return UINT64_MAX;
    int64_t off = (int64_t)(w & 0x03ffffff);
    if (off & 0x02000000) off -= 0x04000000;
    if (link) *link = (int)(w >> 31);
    return a + (uint64_t)(off * 4);
}

/* Is the instruction at text offset a one this port rewrote: a branch into the patch area behind the image? */
static inline int mod_ips_rewritten(const Module *m, uint64_t a) {
    uint64_t t = mod_ips_branch(m, a, NULL);
    return t != UINT64_MAX && t >= m->patch_off && t < m->file_size;
}

/* Does the rewritten site at `site` cover only itself? Yes when the first plain b back into text in its code goes to
   site + 4: a window's code never returns into the window, and site + 4 is inside any window longer than one. */
static inline int mod_ips_single(const Module *m, uint64_t site) {
    uint64_t p = mod_ips_branch(m, site, NULL);
    for (unsigned k = 0; k < MOD_IPS_STUB_WORDS && p + 4 <= m->file_size; k++, p += 4) {
        int link = 0;
        uint64_t t = mod_ips_branch(m, p, &link);
        if (t != UINT64_MAX && !link && t < m->text_end) return t == site + 4;
    }
    return 0;
}

/* 0: the instruction at text offset a runs as the game's own; 1: it is a rewritten site; 2: the port changed it in
   place, or it lies within MOD_IPS_WINDOW instructions of a rewritten site that covers more than itself. */
static inline int mod_ips_covered(const Module *m, uint64_t a) {
    if (mod_ips_rewritten(m, a)) return 1;
    uint32_t w = mod_ips_word(m, a);
    if (a >= 4 && w == MOD_IPS_BLR_X30 && mod_ips_rewritten(m, a - 4)) return 2;                   /* the call of a prepared call */
    if ((w & 0xffffffe0) == 0xd53bd060 || (a >= 4 && (mod_ips_word(m, a - 4) & 0xffffffe0) == 0xd53bd060)) return 2;   /* a parameter gate */
    for (uint64_t k = 1; k < MOD_IPS_WINDOW && a >= 4 * k; k++)
        if (mod_ips_rewritten(m, a - 4 * k) && !mod_ips_single(m, a - 4 * k)) return 2;
    return 0;
}

/* 0: a decoded instruction that names no x18 and is no system instruction; 1: names x18 (a conditional compare with the
   immediate 18 and a prefetch with operation 18 are taken for one); 2: an exception, system or hint instruction other
   than nop; 3: not one of the forms decoded here. */
static inline int mod_ips_insn(uint32_t w) {
    unsigned rd = w & 31, rn = w >> 5 & 31, ra = w >> 10 & 31, rm = w >> 16 & 31, op = w >> 25 & 15;
    if ((op & 14) == 8) {                                   /* data processing, immediate */
        unsigned kind = w >> 23 & 7;                        /* 0,1 adr(p); 2 add/sub; 4 logical; 5 move wide; 6 bitfield; 7 extract */
        if (kind == 3) return 3;                            /* add/sub with tags */
        return rd == 18 || (kind != 0 && kind != 1 && kind != 5 && rn == 18) || (kind == 7 && rm == 18);
    }
    if ((op & 14) == 10) {                                  /* branches, exceptions, system */
        if ((w & 0x7c000000) == 0x14000000) return 0;       /* b, bl */
        if ((w & 0xff000010) == 0x54000000) return 0;       /* b.cond */
        if ((w & 0x7e000000) == 0x34000000 || (w & 0x7e000000) == 0x36000000) return rd == 18;   /* cbz, cbnz, tbz, tbnz */
        if ((w & 0xff9ffc1f) == 0xd61f0000 && (w >> 21 & 3) != 3) return rn == 18;   /* br, blr, ret: not eret, drps or the authenticated forms */
        if (w == 0xd503201f) return 0;                      /* nop */
        return w >> 24 == 0xd4 || w >> 24 == 0xd5 ? 2 : 3;
    }
    if ((op & 5) == 4) {                                    /* loads and stores */
        unsigned vec = w >> 26 & 1, group = w >> 28 & 3;    /* 0 exclusive / vector structures; 1 literal; 2 pair; 3 single */
        if (group == 1) return w >> 24 & 1 ? 3 : !vec && rd == 18;   /* literal: no base register; bit 24: memory tags, memory copies */
        if (group == 0 && !vec) {                           /* exclusive, ordered, compare and swap */
            if (w >> 24 & 1) return 3;
            if ((w >> 21 & 1) && ra == 31 && !(w >> 23 & 1)) return 3;   /* casp: pairs of registers, the second of each implied */
            return rn == 18 || rd == 18 || rm == 18 || ra == 18;
        }
        if (group == 0) return rn == 18 || rm == 18;        /* vector structures: base and post-index register */
        if (group == 2) return rn == 18 || (!vec && (rd == 18 || ra == 18));
        if (w >> 24 & 1) return rn == 18 || (!vec && rd == 18);                              /* unsigned offset */
        if (!(w >> 21 & 1)) return (w >> 10 & 3) == 2 ? 3 : rn == 18 || (!vec && rd == 18);  /* unscaled, post- and pre-index; not unprivileged */
        if ((w >> 10 & 3) != 2) return 3;                   /* atomic memory operations, ld64b / st64b (which write x16..x23), authenticated loads */
        return rn == 18 || rm == 18 || (!vec && rd == 18);  /* register offset */
    }
    if ((op & 7) == 5) {                                    /* data processing, register */
        if (rd == 18 || rn == 18 || rm == 18) return 1;
        return (w & 0x1f000000) == 0x1b000000 && ra == 18;  /* madd, msub and the long forms */
    }
    if ((op & 7) == 7) {                                    /* floating point and vector: v18 is an ordinary register */
        if ((w & 0x5f000000) == 0x1e000000 && (!(w >> 21 & 1) || !(w & 0xfc00))) {   /* scalar <-> integer, with or without fraction bits */
            unsigned code = w >> 16 & 7;                    /* scvtf, ucvtf and fmov from a general register read it; the others write one */
            return (code == 2 || code == 3 || code == 7 ? rn : rd) == 18;
        }
        if ((w & 0xbfe08400) == 0x0e000400 && (w >> 11 & 1))   /* dup and ins from a general register (read), smov and umov (written) */
            return (w >> 13 & 1 ? rd : rn) == 18;
        return 0;
    }
    return 3;                                               /* reserved, SVE, SME */
}

/* The text address a new instruction at a branches to, or UINT64_MAX when it is not a direct branch. */
static inline uint64_t mod_ips_target(uint32_t w, uint64_t a) {
    int64_t off;
    if ((w & 0x7c000000) == 0x14000000) { off = (int64_t)(w & 0x03ffffff); if (off & 0x02000000) off -= 0x04000000; }
    else if ((w & 0xff000010) == 0x54000000 || (w & 0x7e000000) == 0x34000000) { off = (int64_t)(w >> 5 & 0x7ffff); if (off & 0x40000) off -= 0x80000; }
    else if ((w & 0x7e000000) == 0x36000000) { off = (int64_t)(w >> 5 & 0x3fff); if (off & 0x2000) off -= 0x4000; }
    else return UINT64_MAX;
    return a + (uint64_t)(off * 4);
}

/* Parse an IPS32 or PATCH file. NULL and the records (pointing into f), or what is wrong with it. */
static inline const char *mod_ips_parse(const uint8_t *f, size_t size, ModIpsRecord *r, unsigned cap, unsigned *count) {
    *count = 0;
    if (size < 5) return "not an IPS file";
    int wide = !memcmp(f, "IPS32", 5);
    if (!wide && memcmp(f, "PATCH", 5)) return "not an IPS file (no IPS32 or PATCH header)";
    size_t on = wide ? 4 : 3, p = 5;
    for (;;) {
        if (size - p >= on && !memcmp(f + p, wide ? "EEOF" : "EOF", on)) { p += on; break; }
        if (size - p < on + 2) return "the file ends inside a record (or has no end marker)";
        uint64_t off = 0;
        for (size_t k = 0; k < on; k++) off = off << 8 | f[p + k];
        unsigned len = (unsigned)f[p + on] << 8 | f[p + on + 1];
        p += on + 2;
        ModIpsRecord rec = {0};
        if (!len) {                                         /* a run: count, byte */
            if (size - p < 3) return "the file ends inside a record (or has no end marker)";
            rec.len = (unsigned)f[p] << 8 | f[p + 1]; rec.fill = f[p + 2]; rec.rle = 1; p += 3;
            if (!rec.len) return "a run of no bytes";
        } else {
            if (size - p < len) return "the file ends inside a record (or has no end marker)";
            rec.len = len; rec.data = f + p; p += len;
        }
        if (off < 0x100) return "a record inside the NSO header (offsets count its 0x100 bytes)";
        rec.addr = off - 0x100;
        if (*count == cap) return "too many records";
        r[(*count)++] = rec;
    }
    return p == size ? NULL : "bytes after the end marker";
}

/* The build id a file name gives ("<hex>.ips", 16 to 32 bytes), zero padded to 32 bytes. 1, or 0 if the name is not one. */
static inline int mod_ips_name_id(const char *name, uint8_t id[32]) {
    size_t n = strlen(name);
    if (n < 5 || strcasecmp(name + n - 4, ".ips")) return 0;
    n -= 4;
    if (n % 2 || n < 32 || n > 64) return 0;
    memset(id, 0, 32);
    for (size_t i = 0; i < n / 2; i++) {
        unsigned v;
        char pair[3] = {name[2 * i], name[2 * i + 1], 0};
        if (strspn(pair, "0123456789abcdefABCDEF") != 2 || sscanf(pair, "%2x", &v) != 1) return 0;
        id[i] = (uint8_t)v;
    }
    return 1;
}

/* main's build id, zero padded to 32 bytes: the description of the GNU build-id note in its read-only data. 1 when
   there is exactly one such note; a module with none or several has no id to match. */
static inline int mod_ips_build_id(const Module *m, uint8_t id[32]) {
    static const uint8_t tag[8] = {3, 0, 0, 0, 'G', 'N', 'U', 0};   /* type NT_GNU_BUILD_ID, name "GNU" */
    unsigned found = 0;
    for (uint64_t p = (m->ro_off + 3) & ~3ULL; p + 16 <= m->data_off; p += 4) {
        uint32_t namesz, descsz;
        if (memcmp(m->base + p + 8, tag, 8)) continue;
        memcpy(&namesz, m->base + p, 4); memcpy(&descsz, m->base + p + 4, 4);
        if (namesz != 4 || descsz < 16 || descsz > 32 || p + 16 + descsz > m->data_off) continue;
        if (found++) return 0;
        memset(id, 0, 32); memcpy(id, m->base + p + 16, descsz);
    }
    return found == 1;
}

static inline int mod_ips_order(const void *a, const void *b) {
    uint64_t x = ((const ModIpsRecord *)a)->addr, y = ((const ModIpsRecord *)b)->addr;
    return (x > y) - (x < y);
}

/* Check every record against main (sorts them by address). NULL when all may be applied; otherwise the reason, with
   *where the address of the offending record or instruction. */
static inline const char *mod_ips_check(const Module *m, ModIpsRecord *r, unsigned n, uint64_t *where) {
    qsort(r, n, sizeof *r, mod_ips_order);
    for (unsigned i = 0; i < n; i++) {
        uint64_t a = r[i].addr, e = a + r[i].len;
        *where = a;
        if (i && a < r[i - 1].addr + r[i - 1].len) return "two records overlap";
        if (e <= m->text_end) {
            if (a % 4 || r[i].len % 4) return "a text record is not whole instructions";
            for (uint64_t p = a; p < e; p += 4) {
                uint32_t w;
                if (r[i].rle) w = r[i].fill * 0x01010101u; else memcpy(&w, r[i].data + (p - a), 4);
                *where = p;
                switch (mod_ips_insn(w)) {
                case 1: return "the new instruction names x18, the platform register here: this port rewrote every use of it and cannot rewrite a patch";
                case 2: return "the new instruction is an exception, system or hint instruction; only nop is taken";
                case 3: return "the new instruction is not one of the forms this check decodes";
                }
                uint64_t target = mod_ips_target(w, p);
                if (target != UINT64_MAX && (target >= m->text_end || mod_ips_covered(m, target) == 2))
                    return "the new instruction branches out of main's text or onto an instruction this port took out of use";
                switch (mod_ips_covered(m, p)) {
                case 1: return "the patched instruction is one this port rewrote";
                case 2: return "the patched instruction was changed by this port, or lies behind a rewritten one and no longer runs";
                }
            }
        } else if (!(a >= m->ro_off && e <= m->data_off) && !(a >= m->data_off && e <= m->key_off))
            return "a record is outside main's text, read-only data and data, or runs across two of them";
    }
    for (unsigned j = 0; n && j < m->nreloc; j++) {         /* a relocated pointer is 8 bytes at relocs[j].where, in data */
        uint64_t w = m->relocs[j].where;
        unsigned lo = 0, hi = n;
        while (lo < hi) { unsigned mid = lo + (hi - lo) / 2; if (r[mid].addr + r[mid].len <= w) lo = mid + 1; else hi = mid; }
        if (lo < n && r[lo].addr < w + 8) { *where = r[lo].addr; return "a record overwrites a pointer the loader relocated"; }
    }
    return NULL;
}

static inline void mod_ips_apply(Module *m, const ModIpsRecord *r, unsigned n) {
    for (unsigned i = 0; i < n; i++) {
        if (r[i].rle) memset(m->base + r[i].addr, r[i].fill, r[i].len);
        else memcpy(m->base + r[i].addr, r[i].data, r[i].len);
    }
}

/* The exefs/ folder that goes with a mods folder: inside it, or beside it when the mods folder is a merge's romfs/
   itself (TOTK_ROMFS_MODS=<merge>/romfs with a manifest.tsv in it). 1 and the path, or 0. */
static inline int mod_exefs_dir(const char *mods, char *out, size_t n) {
    struct stat st;
    size_t len = strlen(mods);
    if (snprintf(out, n, "%s/exefs", mods) < (int)n && !stat(out, &st) && S_ISDIR(st.st_mode)) return 1;
    return len > 6 && !strcmp(mods + len - 6, "/romfs") && snprintf(out, n, "%.*s/exefs", (int)(len - 6), mods) < (int)n
           && !stat(out, &st) && S_ISDIR(st.st_mode);
}

/* Apply the mods' IPS file that is for this main (runtime_game_patches, first). */
static inline void mod_ips_load(Module *m) {
    const char *mods = getenv("TOTK_ROMFS_MODS"), *enabled = getenv("TOTK_MOD_IPS");
    char dir[4200], path[4600] = "";
    if (!mods || !*mods || !strcmp(mods, "0") || !mod_exefs_dir(mods, dir, sizeof dir)) return;
    DIR *d = opendir(dir);
    if (!d) return;
    uint8_t build[32], id[32];                              /* a copy: the note itself lies in bytes a record may patch */
    int have_build = mod_ips_build_id(m, build);
    unsigned matching = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n < 5 || strcasecmp(e->d_name + n - 4, ".ips")) continue;
        if (enabled && !strcmp(enabled, "0")) { fprintf(stderr, "[totk] mod IPS: %s/%s not read (TOTK_MOD_IPS=0)\n", dir, e->d_name); continue; }
        if (!have_build || !mod_ips_name_id(e->d_name, id) || memcmp(id, build, 32)) {
            fprintf(stderr, "[totk] mod IPS: %s/%s is not named after this main's build id (another module, another game version, or not a build id); not applied\n", dir, e->d_name);
            continue;
        }
        if (matching++) {
            fprintf(stderr, "[totk] mod IPS: %s and %s/%s are both named after main's build id; keep one\n", path, dir, e->d_name);
            runtime_fail("mod IPS: two patch files for main (see the log; remove one or set TOTK_MOD_IPS=0)");
        }
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
    }
    closedir(d);
    if (!matching) return;
    FILE *f = fopen(path, "rb");
    uint8_t *bytes = malloc(MOD_IPS_FILE_MAX + 1);
    ModIpsRecord *records = malloc(MOD_IPS_RECORDS * sizeof *records);
    size_t size = f && bytes ? fread(bytes, 1, MOD_IPS_FILE_MAX + 1, f) : 0;
    if (!f || !bytes || !records || ferror(f)) { fprintf(stderr, "[totk] mod IPS: %s cannot be read\n", path); runtime_fail("mod IPS patch could not be read (see the log)"); }
    fclose(f);
    unsigned count = 0;
    uint64_t where = 0;
    const char *why = size > MOD_IPS_FILE_MAX ? "the file is larger than 1 MB" : mod_ips_parse(bytes, size, records, MOD_IPS_RECORDS, &count);
    if (why) { fprintf(stderr, "[totk] mod IPS: %s refused: %s\n", path, why); runtime_fail("mod IPS patch refused (see the log; remove the file or set TOTK_MOD_IPS=0)"); }
    why = mod_ips_check(m, records, count, &where);
    if (why) {
        fprintf(stderr, "[totk] mod IPS: %s refused at main+%#llx: %s\n", path, (unsigned long long)where, why);
        runtime_fail("mod IPS patch refused (see the log; remove the file or set TOTK_MOD_IPS=0)");
    }
    unsigned long total = 0;
    for (unsigned i = 0; i < count; i++) {
        const ModIpsRecord *r = &records[i];
        total += r->len;
        if (i < 16) fprintf(stderr, "[totk] mod IPS: main+%#llx, %u bytes of %s\n", (unsigned long long)r->addr, r->len,
                            r->addr < m->text_end ? "code" : r->addr < m->data_off ? "read-only data" : "data");
    }
    mod_ips_apply(m, records, count);
    fprintf(stderr, "[totk] mod IPS: %s applied, %u records, %lu bytes in main%s\n", path, count, total, count > 16 ? " (the first 16 listed)" : "");
    free(records); free(bytes);
}
