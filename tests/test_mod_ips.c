/* The mod IPS loader (runtime/mod_ips.h) against a made-up module: no game files. Without arguments it runs the parser,
   name, build-id, instruction and record checks and prints the number of cases; "load" runs mod_ips_load with the
   environment tests/test_mod_ips.py sets and prints the bytes it left at the three places a patch may touch. */
#include "../games/totk/runtime/mod_ips.h"
#include "test_mod_ips_forms.h"

_Noreturn void runtime_fail(const char *message) { printf("FAIL\t%s\n", message); exit(3); }

/* text 0..0x1000 | gap | read-only data 0x2000..0x3000 | data 0x3000..0x5000 (key page) | patch area 0x10000..0x10400 */
enum { TEXT_END = 0x1000, RO = 0x2000, DATA = 0x3000, KEY = 0x5000, PATCH = 0x10000, SIZE = 0x10400 };
#define OFF(address) ((uint32_t)(address) + 0x100)   /* a record's offset counts the NSO header */
#define NOP 0xd503201fu
static uint8_t image[SIZE];
static const uint8_t build[20] = {0x27, 0x71, 0x78, 0xb7, 0xdb, 0xa1, 0xb6, 0xd4, 0x94, 0x9a, 0x84, 0x77, 0x8d, 0x4a, 0xbc, 0x58, 0xb3, 0x1b, 0x34, 0xf5};
static const Reloc relocs[2] = {{.where = 0x3100, .kind = 1027}, {.where = 0x4000, .kind = 1027}};
static Module module;

static void put(uint64_t at, uint32_t word) { memcpy(image + at, &word, 4); }
static uint32_t branch(uint64_t from, uint64_t to) { return 0x14000000u | (uint32_t)(((int64_t)to - (int64_t)from) / 4 & 0x03ffffff); }
static void build_module(void) {
    memset(image, 0, sizeof image);
    for (uint64_t a = 0; a < TEXT_END; a += 4) put(a, NOP);
    for (uint64_t a = PATCH; a < SIZE; a += 4) put(a, NOP);
    put(0x40, branch(0x40, PATCH));                 /* one rewritten instruction: its code returns to 0x44 */
    put(PATCH + 8, branch(PATCH + 8, 0x44));
    put(0x80, branch(0x80, PATCH + 0x40));          /* a rewritten window: its code leaves to a branch target first, then behind the window */
    put(PATCH + 0x44, branch(PATCH + 0x44, 0x300));
    put(PATCH + 0x4c, branch(PATCH + 0x4c, 0x90));
    put(0x400, branch(0x400, PATCH + 0x80));        /* a prepared call: the instruction before the call is replayed, the call became blr x30 */
    put(0x404, MOD_IPS_BLR_X30);
    put(PATCH + 0x88, branch(PATCH + 0x88, 0x404));
    put(0x500, MOD_IPS_BLR_X30);                    /* a blr x30 of the game itself */
    put(0x600, 0xd53bd069);                         /* a parameter gate: mrs x9, tpidrro_el0 and the load behind it */
    put(0xa00, 0x94000000u | (branch(0xa00, PATCH + 0x3f0) & 0x03ffffff));   /* rewritten as bl; its code runs off the image: no return found */
    put(0x200, branch(0x200, 0x300));               /* an ordinary branch inside text is not a rewritten site */
    static const uint8_t note[16] = {4, 0, 0, 0, 20, 0, 0, 0, 3, 0, 0, 0, 'G', 'N', 'U', 0};
    memcpy(image + 0x2800, note, 16); memcpy(image + 0x2810, build, 20);
    module = (Module){.name = "main", .file_size = SIZE, .text_end = TEXT_END, .ro_off = RO, .data_off = DATA, .patch_off = PATCH,
                      .key_off = KEY, .relocs = relocs, .nreloc = 2, .base = image};
}

static unsigned cases, failed;
#define CHECK(c, what) do { cases++; if (!(c)) { failed++; printf("failed: %s (line %d)\n", what, __LINE__); } } while (0)

/* An IPS32 file from records {offset, length, bytes}; a NULL bytes pointer makes a run of the byte in `fill`. */
typedef struct { uint32_t off; uint16_t len; const void *bytes; uint8_t fill; } Rec;
static size_t make(uint8_t *out, const Rec *r, unsigned n) {
    size_t p = 0;
    memcpy(out, "IPS32", 5); p = 5;
    for (unsigned i = 0; i < n; i++) {
        out[p++] = (uint8_t)(r[i].off >> 24); out[p++] = (uint8_t)(r[i].off >> 16); out[p++] = (uint8_t)(r[i].off >> 8); out[p++] = (uint8_t)r[i].off;
        if (r[i].bytes) { out[p++] = (uint8_t)(r[i].len >> 8); out[p++] = (uint8_t)r[i].len; memcpy(out + p, r[i].bytes, r[i].len); p += r[i].len; }
        else { out[p++] = 0; out[p++] = 0; out[p++] = (uint8_t)(r[i].len >> 8); out[p++] = (uint8_t)r[i].len; out[p++] = r[i].fill; }
    }
    memcpy(out + p, "EEOF", 4);
    return p + 4;
}
/* Parse and check one IPS32 file; NULL when it may be applied. */
static const char *verdict(const Rec *r, unsigned n, uint64_t *where) {
    static uint8_t file[4096];
    static ModIpsRecord rec[16];
    unsigned count;
    size_t size = make(file, r, n);
    const char *why = mod_ips_parse(file, size, rec, 16, &count);
    *where = 0;
    return why ? why : mod_ips_check(&module, rec, count, where);
}
static int refused(const Rec *r, unsigned n, const char *part, uint64_t at) {
    uint64_t where;
    const char *why = verdict(r, n, &where);
    return why && strstr(why, part) && where == at;
}

static void parser(void) {
    uint8_t f[256];
    ModIpsRecord r[4];
    unsigned n;
    const uint32_t word = 0x52804008;
    Rec two[2] = {{0x200, 4, &word, 0}, {0x2300, 0x20, NULL, 0xab}};
    size_t size = make(f, two, 2);
    CHECK(!mod_ips_parse(f, size, r, 4, &n) && n == 2, "IPS32 with a record and a run");
    CHECK(r[0].addr == 0x100 && r[0].len == 4 && !r[0].rle && !memcmp(r[0].data, &word, 4), "record: address without the NSO header");
    CHECK(r[1].addr == 0x2200 && r[1].len == 0x20 && r[1].rle && r[1].fill == 0xab, "run: count and byte");
    static const uint8_t classic[] = {'P', 'A', 'T', 'C', 'H', 0x00, 0x02, 0x00, 0x00, 0x02, 0xaa, 0xbb, 'E', 'O', 'F'};
    CHECK(!mod_ips_parse(classic, sizeof classic, r, 4, &n) && n == 1 && r[0].addr == 0x100 && r[0].len == 2, "classic PATCH with 3-byte offsets");
    CHECK(mod_ips_parse(f, size - 1, r, 4, &n) != NULL, "end marker cut short");
    CHECK(mod_ips_parse(f, 9, r, 4, &n) != NULL, "cut inside the first record");
    CHECK(mod_ips_parse(f, 3, r, 4, &n) != NULL, "shorter than a header");
    CHECK(mod_ips_parse((const uint8_t *)"IPS33EEOF", 9, r, 4, &n) != NULL, "another header");
    f[size] = 0; CHECK(mod_ips_parse(f, size + 1, r, 4, &n) != NULL, "a byte after the end marker");
    CHECK(mod_ips_parse(f, size, r, 1, &n) != NULL, "more records than the table holds");
    Rec header[1] = {{0xfc, 4, &word, 0}};
    CHECK(mod_ips_parse(f, make(f, header, 1), r, 4, &n) != NULL, "record inside the NSO header");
    Rec empty[1] = {{0x200, 0, NULL, 0}};
    CHECK(mod_ips_parse(f, make(f, empty, 1), r, 4, &n) != NULL, "a run of no bytes");
    CHECK(!mod_ips_parse((const uint8_t *)"IPS32EEOF", 9, r, 4, &n) && n == 0, "a file with no record is valid and empty");
}

static void names(void) {
    uint8_t id[32], padded[32] = {0}, got[32];
    memcpy(padded, build, 20);
    CHECK(mod_ips_name_id("277178B7DBA1B6D4949A84778D4ABC58B31B34F5.ips", id) && !memcmp(id, padded, 32), "40 hex digits, padded with zeros");
    CHECK(mod_ips_name_id("277178b7dba1b6d4949a84778d4abc58b31b34f5000000000000000000000000.IPS", id) && !memcmp(id, padded, 32), "64 digits, zero padded, lower case");
    CHECK(mod_ips_name_id("277178B7DBA1B6D4949A84778D4ABC58.ips", id) && memcmp(id, padded, 32), "a 16-byte prefix is a name, but another id");
    CHECK(mod_ips_name_id("277178B7DBA1B6D4949A84778D4ABC58B31B34F5AA.ips", id) && memcmp(id, padded, 32), "a longer id is another id");
    CHECK(!mod_ips_name_id("277178B7DBA1B6D4949A84778D4ABC.ips", id), "15 bytes is too short");
    CHECK(!mod_ips_name_id("277178B7DBA1B6D4949A84778D4ABC58B31B34F.ips", id), "odd number of digits");
    CHECK(!mod_ips_name_id("277178B7DBA1B6D4949A84778D4ABC58B31B34G5.ips", id), "not hex");
    CHECK(!mod_ips_name_id("277178B7DBA1B6D4949A84778D4ABC58B31B34F5.pchtxt", id), "another extension");
    CHECK(!mod_ips_name_id("+77178B7DBA1B6D4949A84778D4ABC58B31B34F5.ips", id), "a sign is not a digit");
    CHECK(mod_ips_build_id(&module, got) && !memcmp(got, padded, 32), "the GNU build-id note of the module, padded");
    Module bare = module; bare.ro_off = 0x2900;
    CHECK(!mod_ips_build_id(&bare, got), "no note, no build id");
    memcpy(image + 0x2a00, image + 0x2800, 36);
    CHECK(!mod_ips_build_id(&module, got), "two notes, no build id");
    build_module();
}

static void instructions(void) {
    static const struct { uint32_t word; int kind; const char *what; } t[] = {
        {NOP, 0, "nop"}, {0x52804008, 0, "mov w8, #0x200"}, {0xd2808009, 0, "mov x9, #0x400"}, {0x91004820, 0, "add x0, x1, #18"},
        {0x14000001, 0, "b"}, {0x94000001, 0, "bl"}, {0x54000040, 0, "b.eq"}, {0xd65f03c0, 0, "ret"}, {0xb4000040, 0, "cbz x0"},
        {0xa9bf7bfd, 0, "stp x29, x30, [sp, #-16]!"}, {0xf9400020, 0, "ldr x0, [x1]"}, {0x18000040, 0, "ldr w0, literal"},
        {0x3dc00012, 0, "ldr q18, [x0]"}, {0x9e660020, 0, "fmov x0, d1"}, {0x1e202820, 0, "fadd s0, s1, s0"}, {0xaa0103e0, 0, "mov x0, x1"},
        {0xd2800032, 1, "mov x18, #1"}, {0x91000640, 1, "add x0, x18, #1"}, {0xaa1203e0, 1, "mov x0, x18"}, {0xf9400240, 1, "ldr x0, [x18]"},
        {0xf9400012, 1, "ldr x18, [x0]"}, {0xa9004be0, 1, "stp x0, x18, [sp]"}, {0xf8726820, 1, "ldr x0, [x1, x18]"},
        {0x9b024820, 1, "madd x0, x1, x2, x18"}, {0xd61f0240, 1, "br x18"}, {0xb4000052, 1, "cbz x18"}, {0x58000052, 1, "ldr x18, literal"},
        {0xd4000001, 2, "svc #0"}, {0xd53bd040, 2, "mrs x0, tpidr_el0"}, {0xd51bd040, 2, "msr tpidr_el0, x0"}, {0xd53be020, 2, "mrs x0, cntpct_el0"},
        {0x9e670012, 0, "fmov d18, x0"}, {0x00000000, 3, "udf"}, {0x05200000, 3, "an SVE encoding"}, {0x91800000, 3, "add with tags"},
        {0xd9200000, 3, "stg (memory tags)"}, {0x54000050, 3, "bc.eq"}, {0xd503233f, 2, "paciasp (a hint)"}, {0xd503203f, 2, "yield"}, {0xd4200000, 2, "brk"},
        {0xf83fd010, 3, "ld64b x16, [x0] (writes x16..x23)"}, {0xf83f9010, 3, "st64b"}, {0xd69f03e0, 3, "eret"}, {0xd6bf03e0, 3, "drps"}, {0xd61f081f, 3, "braaz x0"},
        {0xd63f0020, 0, "blr x1"}, {0xd61f0200, 0, "br x16"}, {0x48207c82, 3, "casp x0, x1, x2, x3, [x4]"}, {0xc8a07c41, 0, "cas x0, x1, [x2]"}, {0xc8b27c41, 1, "cas x18, x1, [x2]"},
        {0xf8200041, 3, "ldadd x0, x1, [x2]"}, {0xf8400841, 3, "ldtr x1, [x2]"}, {0xf8408441, 0, "ldr x1, [x2], #8"}, {0x1d000400, 3, "a memory copy encoding"},
    };
    for (unsigned i = 0; i < sizeof t / sizeof *t; i++) CHECK(mod_ips_insn(t[i].word) == t[i].kind, t[i].what);
    for (unsigned i = 0; i < sizeof ips_forms / sizeof *ips_forms; i++) CHECK(mod_ips_insn(ips_forms[i].word) == ips_forms[i].kind, ips_forms[i].what);
}

static void sites(void) {
    CHECK(mod_ips_rewritten(&module, 0x40) && mod_ips_rewritten(&module, 0x80) && mod_ips_rewritten(&module, 0xa00), "branches into the patch area are rewritten sites");
    CHECK(!mod_ips_rewritten(&module, 0x44) && !mod_ips_rewritten(&module, 0x200), "a nop and a branch inside text are not");
    CHECK(mod_ips_single(&module, 0x40) && mod_ips_single(&module, 0x400), "one rewritten instruction, and a prepared call, return behind themselves");
    CHECK(!mod_ips_single(&module, 0x80), "a window's code leaves elsewhere first");
    CHECK(!mod_ips_single(&module, 0xa00), "no return found: not known to be single");
    CHECK(mod_ips_covered(&module, 0x40) == 1 && mod_ips_covered(&module, 0x44) == 0 && mod_ips_covered(&module, 0x7c) == 0, "around one rewritten instruction");
    CHECK(mod_ips_covered(&module, 0x84) == 2 && mod_ips_covered(&module, 0x80 + 4 * (MOD_IPS_WINDOW - 1)) == 2
          && mod_ips_covered(&module, 0x80 + 4 * MOD_IPS_WINDOW) == 0, "behind a window's head, for the longest a window can be");
    CHECK(mod_ips_covered(&module, 0xa04) == 2 && mod_ips_covered(&module, 0xa00 + 4 * MOD_IPS_WINDOW) == 0 && mod_ips_covered(&module, 0x9fc) == 0, "behind a site whose return is unknown");
    CHECK(mod_ips_covered(&module, 0x400) == 1 && mod_ips_covered(&module, 0x404) == 2 && mod_ips_covered(&module, 0x408) == 0, "a prepared call: the replayed instruction and the call");
    CHECK(mod_ips_covered(&module, 0x500) == 0, "a blr x30 that is not behind a rewritten site");
    CHECK(mod_ips_covered(&module, 0x600) == 2 && mod_ips_covered(&module, 0x604) == 2 && mod_ips_covered(&module, 0x608) == 0 && mod_ips_covered(&module, 0x5fc) == 0, "a parameter gate's two instructions");
    CHECK(mod_ips_covered(&module, 0) == 0 && mod_ips_covered(&module, 4) == 0, "the first instructions of text");
    CHECK(mod_ips_target(0x14000010, 0x100) == 0x140 && mod_ips_target(0x97fffff0, 0x100) == 0xc0, "b and bl targets");
    CHECK(mod_ips_target(0x54000040, 0x100) == 0x108 && mod_ips_target(0xb4ffffe0, 0x100) == 0xfc && mod_ips_target(0x36180040, 0x100) == 0x108, "b.cond, cbz and tbz targets");
    CHECK(mod_ips_target(0x52804008, 0x100) == UINT64_MAX && mod_ips_target(0xd65f03c0, 0x100) == UINT64_MAX, "no target for other instructions");
}

static void records(void) {
    const uint32_t mov = 0x52804008, x18 = 0xd2800032, svc = 0xd4000001, udf = 0;
    const uint32_t pair[2] = {0x52804008, 0xd2800032};
    const uint8_t bytes[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint64_t where;
    Rec good[4] = {{OFF(0x3200), 8, bytes, 0}, {OFF(0x100), 4, &mov, 0}, {OFF(0x2100), 3, bytes, 0}, {OFF(0x3300), 0x10, NULL, 0xee}};
    CHECK(!verdict(good, 4, &where), "code, read-only data, data and a run, given out of order");
    Rec at40[1] = {{OFF(0x40), 4, &mov, 0}}, at44[1] = {{OFF(0x44), 4, &mov, 0}}, at84[1] = {{OFF(0x84), 4, &mov, 0}}, at90[1] = {{OFF(0x80 + 4 * MOD_IPS_WINDOW), 4, &mov, 0}}, ata08[1] = {{OFF(0xa08), 4, &mov, 0}};
    CHECK(refused(at40, 1, "this port rewrote", 0x40), "a rewritten instruction");
    CHECK(!verdict(at44, 1, &where), "the instruction behind it");
    CHECK(refused(at84, 1, "no longer runs", 0x84), "inside a rewritten window");
    CHECK(!verdict(at90, 1, &where), "the instruction behind the window");
    CHECK(refused(ata08, 1, "no longer runs", 0xa08), "behind a rewritten site whose return is unknown");
    Rec call[1] = {{OFF(0x404), 4, &mov, 0}}, gate[1] = {{OFF(0x604), 4, &mov, 0}}, own[1] = {{OFF(0x500), 4, &mov, 0}};
    CHECK(refused(call, 1, "changed by this port", 0x404) && refused(gate, 1, "changed by this port", 0x604), "the call of a prepared call, and a parameter gate");
    CHECK(!verdict(own, 1, &where), "the game's own blr x30");
    const uint32_t to_window = 0x17ffffe1 /* b 0x84 from 0x100 */, to_site = 0x17ffffd0 /* b 0x40 */, out = 0x14001000 /* b 0x4100 */, cb = 0xb4fffc20 /* cbz x0, 0x84 */;
    Rec bw[1] = {{OFF(0x100), 4, &to_window, 0}}, bs[1] = {{OFF(0x100), 4, &to_site, 0}}, bo[1] = {{OFF(0x100), 4, &out, 0}}, bc[1] = {{OFF(0x100), 4, &cb, 0}};
    CHECK(refused(bw, 1, "branches out of", 0x100) && refused(bc, 1, "branches out of", 0x100), "a new branch into a window's tail");
    CHECK(refused(bo, 1, "branches out of", 0x100), "a new branch out of text");
    CHECK(!verdict(bs, 1, &where), "a new branch onto a rewritten site is a branch onto its code");
    Rec span[1] = {{OFF(0x3c), 8, pair, 0}}, into[1] = {{OFF(0x7c), 8, &pair[0], 0}};
    CHECK(refused(span, 1, "names x18", 0x40), "the second instruction of a record is checked, and named");
    const uint32_t movs[2] = {0x52804008, 0x52804008};
    into[0].bytes = movs;
    CHECK(refused(into, 1, "this port rewrote", 0x80), "a record that runs into a rewritten instruction");
    Rec odd[1] = {{OFF(0x102), 4, &mov, 0}}, part[1] = {{OFF(0x100), 3, &mov, 0}};
    CHECK(refused(odd, 1, "whole instructions", 0x102) && refused(part, 1, "whole instructions", 0x100), "text records are whole instructions");
    Rec r18[1] = {{OFF(0x100), 4, &x18, 0}}, rsvc[1] = {{OFF(0x100), 4, &svc, 0}}, rudf[1] = {{OFF(0x100), 4, &udf, 0}}, run[1] = {{OFF(0x100), 4, NULL, 0}};
    CHECK(refused(r18, 1, "names x18", 0x100) && refused(rsvc, 1, "system", 0x100) && refused(rudf, 1, "not one of the forms", 0x100), "instructions the port would have rewritten");
    CHECK(refused(run, 1, "not one of the forms", 0x100), "a run of zero bytes in code is not an instruction");
    Rec edge[1] = {{OFF(TEXT_END - 4), 8, movs, 0}}, gap[1] = {{OFF(0x1800), 4, bytes, 0}}, last[1] = {{OFF(TEXT_END - 4), 4, &mov, 0}};
    CHECK(refused(edge, 1, "outside main's", TEXT_END - 4) && refused(gap, 1, "outside main's", 0x1800), "across the end of text, and between text and data");
    CHECK(!verdict(last, 1, &where), "the last instruction of text");
    Rec cross[1] = {{OFF(DATA - 4), 8, bytes, 0}}, key[1] = {{OFF(KEY - 4), 8, bytes, 0}}, area[1] = {{OFF(PATCH), 4, &mov, 0}}, far[1] = {{0x7fffff00, 4, bytes, 0}};
    CHECK(refused(cross, 1, "outside main's", DATA - 4) && refused(key, 1, "outside main's", KEY - 4), "across read-only data and data, and into the key page");
    CHECK(refused(area, 1, "outside main's", PATCH) && refused(far, 1, "outside main's", 0x7ffffe00), "the patch area, and far outside the image");
    Rec ends[2] = {{OFF(RO), 4, bytes, 0}, {OFF(KEY - 8), 8, bytes, 0}};
    CHECK(!verdict(ends, 2, &where), "the first bytes of read-only data and the last of data");
    Rec rel[1] = {{OFF(0x3104), 1, bytes, 0}}, before[1] = {{OFF(0x30f9), 7, bytes, 0}}, after[1] = {{OFF(0x3108), 4, bytes, 0}}, onto[1] = {{OFF(0x30fd), 4, bytes, 0}}, second[1] = {{OFF(0x4007), 1, bytes, 0}};
    CHECK(refused(rel, 1, "relocated", 0x3104) && refused(onto, 1, "relocated", 0x30fd) && refused(second, 1, "relocated", 0x4007), "a relocated pointer, touched anywhere");
    CHECK(!verdict(before, 1, &where) && !verdict(after, 1, &where), "the bytes before and behind it");
    Rec twice[2] = {{OFF(0x3200), 8, bytes, 0}, {OFF(0x3204), 2, bytes, 0}}, touching[2] = {{OFF(0x3200), 4, bytes, 0}, {OFF(0x3204), 4, bytes, 0}};
    CHECK(refused(twice, 2, "overlap", 0x3204), "two records over the same bytes");
    CHECK(!verdict(touching, 2, &where), "two records side by side");
    /* applying */
    static uint8_t file[256];
    static ModIpsRecord rec[8];
    unsigned n;
    size_t size = make(file, good, 4);
    CHECK(!mod_ips_parse(file, size, rec, 8, &n) && !mod_ips_check(&module, rec, n, &where), "the good file again");
    mod_ips_apply(&module, rec, n);
    uint32_t got; memcpy(&got, image + 0x100, 4);
    CHECK(got == mov && !memcmp(image + 0x3200, bytes, 8) && !memcmp(image + 0x2100, bytes, 3) && !image[0x2103], "the bytes are where the records put them");
    CHECK(image[0x3300] == 0xee && image[0x330f] == 0xee && !image[0x3310] && !image[0x32ff], "a run fills its bytes and no more");
    memcpy(&got, image + 0x104, 4); CHECK(got == NOP, "the next instruction is untouched");
    build_module();
}

int main(int argc, char **argv) {
    build_module();
    if (argc > 1 && !strcmp(argv[1], "load")) {
        mod_ips_load(&module);
        uint32_t word; memcpy(&word, image + 0x100, 4);
        printf("LOADED\t%08x\t%02x\t%02x\n", word, image[0x2100], image[0x3200]);
        return 0;
    }
    if (argc > 2 && !strcmp(argv[1], "exefs")) {
        char dir[4200];
        printf("EXEFS\t%s\n", mod_exefs_dir(argv[2], dir, sizeof dir) ? dir : "-");
        return 0;
    }
    parser(); names(); instructions(); sites(); records();
    printf("%u mod IPS cases, %u failed\n", cases, failed);
    return failed != 0;
}
