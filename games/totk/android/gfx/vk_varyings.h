/* Varyings a fragment shader reads that its vertex shader never writes.

   Shaders are translated one stage at a time (tools/totk_xlate), so a fragment shader can declare an input Location the
   vertex shader it is drawn with has no output for. Maxwell reads such a varying as (0, 0, 0, 1); Vulkan makes the pair
   an interface mismatch whose input values are undefined (Adreno hands over whatever the slot holds: garbage, often NaN).
   The air-flow ribbons (vs 41eaeb77 + fs a2ddcd69, upstream issue AurelioB/totk-android#4) read v6.w that way and
   multiply their colour by it: white boxes. The Mac port adds such varyings to the vertex function (nvn_mtl.m
   msl_varyings); this is the same for SPIR-V, after totk-pc-recomp's runtime/vk_pipeline.c spirv_add_outputs (MIT,
   Copyright (c) 2026 Valiantsin Dzerakh): each missing Location becomes a vertex Output of the fragment input's numeric
   type, declared in the entry point interface and stored (0, 0, 0, 1) at the start of main, the w as the bits of 1.0f
   whatever the type (attribute memory is untyped). Translator patch 0005 already does this for the unwritten components
   of outputs the vertex shader does write.

   Plain C over SPIR-V words, no Vulkan: android/gfx/test_vk_varyings.c. */
#ifndef TOTK_VK_VARYINGS_H
#define TOTK_VK_VARYINGS_H
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

enum { VK_VARY_FLOAT = 0, VK_VARY_SINT = 1, VK_VARY_UINT = 2 };
/* The user Locations a module declares for one storage class (1 Input, 3 Output), scalar or vector variables only
   (built-ins, blocks and arrays are skipped), with each one's numeric kind (VK_VARY_*). 0 = ok, -1 = malformed. */
static inline int vk_spirv_io(const uint32_t *w, size_t nw, uint32_t storage, uint32_t *mask, uint8_t kind[32]) {
    *mask = 0;
    if (nw < 5 || w[0] != 0x07230203 || !w[3] || w[3] > (1u << 22)) return -1;
    uint32_t bound = w[3];
    int32_t *loc = malloc(bound * sizeof *loc); uint8_t *builtin = calloc(bound, 1);
    uint32_t *ptype = calloc(bound, sizeof *ptype), *pclass = calloc(bound, sizeof *pclass);
    uint8_t *tkind = malloc(bound);   /* scalar / vector numeric kind of a type id; 255 = not a scalar or vector */
    if (!loc || !builtin || !ptype || !pclass || !tkind) { free(loc); free(builtin); free(ptype); free(pclass); free(tkind); return -1; }
    memset(tkind, 255, bound);
    for (uint32_t i = 0; i < bound; i++) loc[i] = -1;
    int rc = 0;
    for (size_t i = 5; i < nw; i += w[i] >> 16) {
        uint32_t n = w[i] >> 16, op = w[i] & 0xffff;
        if (!n || n > nw - i) { rc = -1; break; }
        if (op == 71 && n >= 4 && w[i + 1] < bound && w[i + 2] == 30) loc[w[i + 1]] = (int32_t)w[i + 3];   /* Location */
        if (op == 71 && n >= 3 && w[i + 1] < bound && w[i + 2] == 11) builtin[w[i + 1]] = 1;               /* BuiltIn */
        if (op == 22 && n == 3 && w[i + 1] < bound) tkind[w[i + 1]] = VK_VARY_FLOAT;
        if (op == 21 && n == 4 && w[i + 1] < bound) tkind[w[i + 1]] = w[i + 3] ? VK_VARY_SINT : VK_VARY_UINT;
        if (op == 23 && n == 4 && w[i + 1] < bound && w[i + 2] < bound) tkind[w[i + 1]] = tkind[w[i + 2]];
        if (op == 32 && n == 4 && w[i + 1] < bound) { pclass[w[i + 1]] = w[i + 2]; ptype[w[i + 1]] = w[i + 3]; }
        if (op == 59 && n >= 4 && w[i + 1] < bound && w[i + 2] < bound && w[i + 3] == storage) {
            uint32_t id = w[i + 2], t = ptype[w[i + 1]];
            if (builtin[id] || loc[id] < 0 || loc[id] >= 32 || !t || t >= bound || tkind[t] == 255) continue;
            *mask |= 1u << loc[id]; if (kind) kind[loc[id]] = tkind[t];
        }
    }
    free(loc); free(builtin); free(ptype); free(pclass); free(tkind);
    if (rc) *mask = 0;
    return rc;
}

/* The module with an Output for each Location of `missing` (kind[] its numeric type), stored (0, 0, 0, 1) first thing in
   the entry point; NULL when the module does not have the expected layout or memory runs out (the caller keeps the
   original). Existing scalar, vector and pointer types are reused (SPIR-V forbids duplicates of those). */
static inline uint32_t *vk_spirv_add_outputs(const uint32_t *w, size_t nw, uint32_t missing, const uint8_t kind[32], size_t *out_n) {
    if (!missing || nw < 5 || w[0] != 0x07230203 || !w[3] || w[3] > (1u << 22)) return NULL;
    size_t ep_at = 0, ann_end = 0, fn_at = 0, store_at = 0;
    uint32_t bound = w[3], entry = 0, scalar[3] = {0, 0, 0}, vec[3] = {0, 0, 0}, ptr[3] = {0, 0, 0};
    for (size_t i = 5; i < nw; i += w[i] >> 16) {
        uint32_t n = w[i] >> 16, op = w[i] & 0xffff;
        if (!n || n > nw - i) return NULL;
        if (op == 15) { if (ep_at) return NULL; ep_at = i; entry = w[i + 2]; }   /* one entry point */
        if (!ann_end && ((op >= 19 && op <= 39) || (op >= 41 && op <= 52) || op == 59 || op == 54)) ann_end = i;
        if (op == 22 && n == 3 && w[i + 2] == 32) scalar[VK_VARY_FLOAT] = w[i + 1];
        if (op == 21 && n == 4 && w[i + 2] == 32) scalar[w[i + 3] ? VK_VARY_SINT : VK_VARY_UINT] = w[i + 1];
        if (op == 54 && !fn_at) fn_at = i;
        if (op == 54 && w[i + 2] == entry && !store_at) {   /* the entry function: after its first label and OpVariables */
            size_t j = i;
            while (j < nw && (w[j] & 0xffff) != 248) { if (!(w[j] >> 16)) return NULL; j += w[j] >> 16; }
            if (j >= nw) return NULL;
            j += w[j] >> 16;
            while (j < nw && ((w[j] & 0xffff) == 59 || (w[j] & 0xffff) == 8 || (w[j] & 0xffff) == 317)) { if (!(w[j] >> 16)) return NULL; j += w[j] >> 16; }
            store_at = j;
        }
    }
    if (!ep_at || !ann_end || !fn_at || !store_at || !(ep_at < ann_end && ann_end <= fn_at && fn_at < store_at)) return NULL;
    for (size_t i = 5; i < nw; i += w[i] >> 16) {   /* vec4 and Output pointers of the scalars, when declared */
        uint32_t n = w[i] >> 16, op = w[i] & 0xffff;
        for (int k = 0; k < 3; k++) {
            if (op == 23 && n == 4 && scalar[k] && w[i + 2] == scalar[k] && w[i + 3] == 4) vec[k] = w[i + 1];
            if (op == 32 && n == 4 && vec[k] && w[i + 2] == 3 && w[i + 3] == vec[k]) ptr[k] = w[i + 1];
        }
    }
    int count = 0; uint8_t used[3] = {0, 0, 0};
    for (int l = 0; l < 32; l++) if (missing >> l & 1) { if (kind[l] > 2) return NULL; used[kind[l]] = 1; count++; }
    /* new words: types and constants (at most 3 + 4 + 4 + 4 + 7 per kind), variables 4 each, decorations 4 each,
       entry point interface 1 each, stores 3 each */
    size_t extra = 3 * (3 + 4 + 4 + 4 + 4 + 7) + (size_t)count * (4 + 4 + 1 + 3);
    uint32_t *g = malloc((3 * 26 + (size_t)count * 4) * sizeof *g), *o = malloc((nw + extra) * sizeof *o);
    if (!g || !o) { free(g); free(o); return NULL; }
    size_t ng = 0;
    uint32_t composite[3] = {0, 0, 0};
    for (int k = 0; k < 3; k++) {
        if (!used[k]) continue;
        if (!scalar[k]) { scalar[k] = bound++; if (k == VK_VARY_FLOAT) { g[ng++] = 3u << 16 | 22; g[ng++] = scalar[k]; g[ng++] = 32; }
                          else { g[ng++] = 4u << 16 | 21; g[ng++] = scalar[k]; g[ng++] = 32; g[ng++] = k == VK_VARY_SINT; } }
        if (!vec[k]) { vec[k] = bound++; g[ng++] = 4u << 16 | 23; g[ng++] = vec[k]; g[ng++] = scalar[k]; g[ng++] = 4; }
        if (!ptr[k]) { ptr[k] = bound++; g[ng++] = 4u << 16 | 32; g[ng++] = ptr[k]; g[ng++] = 3; g[ng++] = vec[k]; }
        uint32_t zero = bound++, one = bound++;
        g[ng++] = 4u << 16 | 43; g[ng++] = scalar[k]; g[ng++] = zero; g[ng++] = 0;
        g[ng++] = 4u << 16 | 43; g[ng++] = scalar[k]; g[ng++] = one; g[ng++] = 0x3f800000;   /* the bits of 1.0f */
        composite[k] = bound++;
        g[ng++] = 7u << 16 | 44; g[ng++] = vec[k]; g[ng++] = composite[k]; g[ng++] = zero; g[ng++] = zero; g[ng++] = zero; g[ng++] = one;
    }
    uint32_t var[32];
    for (int l = 0; l < 32; l++) if (missing >> l & 1) {
        var[l] = bound++;
        g[ng++] = 4u << 16 | 59; g[ng++] = ptr[kind[l]]; g[ng++] = var[l]; g[ng++] = 3;   /* OpVariable Output */
    }
    size_t k = 0, epn = w[ep_at] >> 16;
    memcpy(o, w, ep_at * sizeof *o); k = ep_at;
    o[k] = (uint32_t)(epn + count) << 16 | 15; memcpy(o + k + 1, w + ep_at + 1, (epn - 1) * sizeof *o); k += epn;
    for (int l = 0; l < 32; l++) if (missing >> l & 1) o[k++] = var[l];
    memcpy(o + k, w + ep_at + epn, (ann_end - ep_at - epn) * sizeof *o); k += ann_end - ep_at - epn;
    for (int l = 0; l < 32; l++) if (missing >> l & 1) { o[k++] = 4u << 16 | 71; o[k++] = var[l]; o[k++] = 30; o[k++] = (uint32_t)l; }
    memcpy(o + k, w + ann_end, (fn_at - ann_end) * sizeof *o); k += fn_at - ann_end;
    memcpy(o + k, g, ng * sizeof *o); k += ng;
    memcpy(o + k, w + fn_at, (store_at - fn_at) * sizeof *o); k += store_at - fn_at;
    for (int l = 0; l < 32; l++) if (missing >> l & 1) { o[k++] = 3u << 16 | 62; o[k++] = var[l]; o[k++] = composite[kind[l]]; }
    memcpy(o + k, w + store_at, (nw - store_at) * sizeof *o); k += nw - store_at;
    o[3] = bound;
    free(g);
    *out_n = k;
    return o;
}
#endif
