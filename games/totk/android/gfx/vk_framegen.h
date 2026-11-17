/* Opt-in presentation-only camera extrapolation/interpolation. Recording APIs run under vk_rec_lock. */
#pragma once
#include "vk_int.h"
#include "vk_record.h"

/* Encoded RGB byte identity is shared by mutable UNORM/SRGB views. Sampling
   retained real pixels always uses this raw view, never an SRGB decode. */
static inline VkFormat vk_fg_raw_format(VkFormat format) {
    return format==VK_FORMAT_R8G8B8A8_SRGB ? VK_FORMAT_R8G8B8A8_UNORM :
        format==VK_FORMAT_B8G8R8A8_SRGB ? VK_FORMAT_B8G8R8A8_UNORM : format;
}

/* Cached configuration check only; does not initialize Vulkan resources. */
enum {
    VK_FG_ELIGIBLE, VK_FG_NO_BOUNDARY, VK_FG_CHAIN_BROKEN, VK_FG_DEPTH_LOST,
    VK_FG_TRACK_CAPACITY, VK_FG_STRUCTURE_CHANGED, VK_FG_SUSPENDED, VK_FG_BACKOFF, VK_FG_TRACK_RESULTS
};
void vk_fg_track_result(unsigned result);
int vk_fg_enabled(void);
/* Cached selected interpolation mode, retained through per-pair refusals. */
int vk_fg_interpolating(void);
int vk_fg_static_hold(void);  /* TOTK_FG_STATIC_HOLD: camera interpolation keeps screen-static pixels */
/* Explicit fixed-real30 mode; immutable selection also sizes presenter ownership. */
int vk_fg_locked(void);
int vk_fg_adaptive(void); /* exact opt-in; captured UI, target-time generation */
/* JOB_TRACE's existing cached atomic only: no configuration initialization,
 * policy probe, driver call or change to admission when used at boundaries. */
int vk_fg_job_trace_enabled(void);
unsigned vk_fg_lead_refreshes(void); /* locked-only1(default) or2; fixed for the run */
/* Hybrid capture gate only; static selection remains valid for queued output
 * completion, timestamp allocation and bounded presenter ownership in native60 mode. */
int vk_fg_capturing(void);
void vk_fg_camera(const float cam[84]);
/* Audited alternate VS camera layouts: 0 unknown, 1 UBO5 view/projection,
   2 same plus UBO6[16].w view-depth bias (must be zero for continuation). */
unsigned vk_fg_camera_layout(const char *key);
int vk_fg_raster_depth_shader(const char *key); /* audited raster-z / invalid-sentinel-only export */
int vk_fg_plane_shader(const char *key, unsigned stage); /* 0 proved VS, 1 FS, 2 exact UNCERTIFIED experimental VS nomination */
/* Outside rendering: reserve fence-safe private ordered depth-VALUE state for
   this exact subresource. No depth image is copied until span_begin. */
int vk_fg_authority_begin(VkCommandBuffer cb, vk_tex *depth, unsigned dl, unsigned da);
/* Bracket EVERY actual unproven depth-writing run, before its first draw and
   before any following proven writer or depth mutation. Rect is a nonempty
   clipped native scissor, never proof of coverage. Begin reserves automatically.
   Experimental=1 requires the caller's exact approved runtime classification
   and the explicit default-off experiment. Any failure revokes the whole root. */
int vk_fg_authority_span_begin(VkCommandBuffer cb, vk_tex *depth, unsigned level, unsigned layer,
    unsigned x, unsigned y, unsigned width, unsigned height, int experimental);
int vk_fg_authority_span_end(VkCommandBuffer cb, vk_tex *depth, unsigned level, unsigned layer);
int vk_fg_uncertified_plane(void); /* Explicit quality assumption, NOT a proof. */
/* TOTK_FG_ADAPTIVE_LEAN (default on, exact 0 opts out; adaptive QCOM slot
 * scheduler only, resolved once at scheduler startup; scene AA/scale,
 * WRITER_DIAG and FRAMEGEN_DUMP keep it inactive). Recording owner; latched once per real
 * frame. When on, the adaptive QCOM path never reads ordered depth authority or
 * retained scene/depth/real pixels, so unknown depth writers keep their CPU
 * authority classification but record no GPU span/copy/pass split, and the
 * source skips its depth, scene (outside the host TEST bridge) and real copies.
 * The AFME input copy and every descriptor-bound image remain. */
int vk_fg_adaptive_lean(void);
/* Unknown-writer span decision: 1 means lean skipped it (counted), 0 means the
 * caller must run the ordinary ordered span. */
int vk_fg_adaptive_lean_span(void);
enum {
    VK_FG_LEAN_SOURCES, VK_FG_LEAN_SCENE_COPIES, VK_FG_LEAN_DEPTH_COPIES,
    VK_FG_LEAN_REAL_COPIES, VK_FG_LEAN_UNKNOWN_DRAWS, VK_FG_LEAN_ROOTS,
    VK_FG_LEAN_DIRECT, VK_FG_LEAN_DIRECT_FALLBACK, VK_FG_LEAN_COUNTS
};
/* DIAG-run-only instrumentation: requires INTERP + DIAG=1 + WRITER_DIAG=1.
   No GPU work when off. Begin/end MUST use a fresh physical primary outside
   rendering. Known writers are bracketed to invalidate stale residual owners;
   no diagnostic result participates in ordinary authority/admission. */
enum { VK_FG_WRITER_PROVEN, VK_FG_WRITER_UNPROVEN, VK_FG_WRITER_UNCERTIFIED };
int vk_fg_writer_diag(void);
int vk_fg_writer_available(void); /* live source not failed; cached/no GPU work */
int vk_fg_writer_begin(VkCommandBuffer cb, vk_tex *depth, unsigned level, unsigned layer,
    const char *vs, const char *fs, const char *check, unsigned classification);
int vk_fg_writer_end(VkCommandBuffer cb, vk_tex *depth, unsigned level, unsigned layer, uint64_t draws);
void vk_fg_writer_poison(const char *reason); /* pending span only; no GPU command */
/* Rendering must have ended. Copies complete selected scene/depth subresources.
   Contract: full-image axis-aligned mapping, zero crop/offset; independent x/y
   scale is derived from their extents in normalized UV. Camera matrices/extents
   belong to native depth. Aspect equality is not required; no arbitrary cropped,
   rotated or projectively remapped composite is represented by this API. */
int vk_fg_capture(VkCommandBuffer cb, vk_tex *scene, unsigned sl, unsigned sa,
                   vk_tex *depth, unsigned dl, unsigned da, uint32_t w, uint32_t h);
/* Walking-owned provenance for the selected real snapshot, retained only for dumps. */
void vk_fg_capture_geometry(uint64_t real_frame,const float depth_viewport[4],const int depth_scissor[4]);
int vk_fg_dump_requested(void);
/* Successful real spatial seed, outside rendering before first UI. Retains the
   exact native pre-UI pixels in the current source ring; depth stays low-res.
   Only interpolation scene mode uses this source; missing preservation refuses. */
int vk_fg_capture_native_scene(VkCommandBuffer cb, const vk_post_output *native);
/* Actual encoded real terminal, outside rendering before replay/generation.
   Active native UI captures its valid presented sibling (same format, complete
   host extent, GENERAL); missing/replaced native provenance refuses the pair,
   never resolves/copies the stale guest backing. Otherwise copies the guest
   subresource. Zero means no exact fallback retained; masked pairs require one,
   while known unmasked pairs retain their existing format behavior. */
int vk_fg_capture_real(VkCommandBuffer cb, vk_tex *final, unsigned level, unsigned layer);
/* Warp the immutable scene into a same-format private canvas before first UI.
   Replay receives only the real frame's already resolved Vulkan bindings,
   never guest pointers at presentation time. */
int vk_fg_prepare_canvas(VkCommandBuffer cb, uint32_t width, uint32_t height);
VkImageView vk_fg_canvas_view(void);
int vk_fg_replay_depth(VkCommandBuffer cb, vk_tex *depth, unsigned level, unsigned layer);
/* final=1 is the terminal transfer. final=0 replays UI (black background in
   interpolation); final=2 is interpolation's white-background UI capture. */
int vk_fg_replay_begin(VkCommandBuffer cb, uint32_t width, uint32_t height, VkFormat format, int final);
void vk_fg_replay_end(VkCommandBuffer cb, int final);
/* Compose retained older UI over the current midpoint. Zero means pair warmup
   or refusal; the current real capture still retires normally. */
int vk_fg_interp_compose(VkCommandBuffer cb);
void vk_fg_replay_draw(VkCommandBuffer cb, const vk_record_draw *draw, const VkDescriptorSet sets[2]);
/* Freeze the already-resolved terminal transfer for target-time adaptive work.
 * Replayed only on the recording owner, before this real frame is sealed. */
void vk_fg_adaptive_terminal(const vk_record_draw *draw, const VkDescriptorSet sets[2],
    uint32_t width, uint32_t height, VkFormat format);
/* Scheduler-only immutable terminal payload. Originals remain the real draw's
 * descriptors. Capture exact already-frozen bytes at their stream position;
 * source GPU retirement + generation refs own the private copies and sets. */
extern _Atomic int vk_fg_terminal_owned_active;
enum { VK_FG_TERMINAL_BYTES=2u<<20, VK_FG_TERMINAL_DESCRIPTORS=256 };
int vk_fg_terminal_owned_begin(void);
int vk_fg_terminal_owned_binding(unsigned set,VkDescriptorSetLayout layout,
    const VkWriteDescriptorSet *write,int writable);
void vk_fg_terminal_owned_fail(void);
enum {
    VK_FG_TERMINAL_INPUT_DESCRIPTOR=1,
    VK_FG_TERMINAL_INPUT_VERTEX,
    VK_FG_TERMINAL_INPUT_INDEX
};
typedef struct {
    unsigned role,stage,set,binding,element,stream,descriptor_type;
    VkBuffer buffer;
    VkDeviceSize offset,bytes;
    unsigned freeze; /* 0 unavailable;1 confirmed arena;2 original-stream snapshot */
} vk_fg_terminal_input;
/* Original-stream freeze refusal affects only the optional terminal capture;
 * its detailed input identity is logged only under JOB_TRACE. */
void vk_fg_terminal_owned_input_refuse(const vk_fg_terminal_input *input,
    const char *detail);
/* Copy the fully replayed terminal image to an idle presenter-owned slot.
   Output is encoded RGBA8 in GENERAL, with its corresponding display view. */
int vk_fg_generate(VkCommandBuffer cb, unsigned output_slot, uint32_t fw, uint32_t fh,
                   int srgb, vk_post_output *out);
int vk_fg_generate_phase(VkCommandBuffer cb, unsigned output_slot, uint32_t fw, uint32_t fh,
    int srgb, float phase, vk_post_output *out);
/* Slot-scheduler lane, recording owner under vk_rec_lock throughout. Exactly
 * one template per real, with one replay lease. Stamp is copied, not retained.
 * Prepare opens private command storage and records generation/native UI only;
 * append ALL presentation commands using prepare_cb, then prepare_finish before
 * sealing the real. No GL dispatch/ticket or real-stream state is consumed.
 * The presenter may read immutable metadata, never call these recording APIs.
 * Dispatch requires actual real publication + chosen phase, NO open recorder
 * CB, and a future host_now_ns deadline. Ring pressure/missed deadline skips.
 * On success an empty normal continuation is open; seal queues both private
 * commands and continuation through the ordinary FIFO ticket allocator.
 * After successful dispatch seal is REQUIRED even when canceled: GL work
 * cannot be unsubmitted and its ordinary continuation must close.
 * Cancel before dispatch releases immediately. After dispatch/seal the graph
 * stays owned until its ticket retires and complete runs exactly once.
 * Driver bridge failure quarantines the whole bounded graph permanently. */
typedef struct vk_fg_generation vk_fg_generation;
typedef struct { uint64_t frame,epoch; } vk_fg_generation_stamp;
enum {
    VK_FG_GENERATION_EMPTY, VK_FG_GENERATION_PREPARING, VK_FG_GENERATION_PREPARED,
    VK_FG_GENERATION_RECORDING, VK_FG_GENERATION_SUBMITTED, VK_FG_GENERATION_QUARANTINED
};
vk_fg_generation *vk_fg_generation_prepare(unsigned slot,uint32_t w,uint32_t h,
    int srgb,const vk_fg_generation_stamp *stamp);
VkCommandBuffer vk_fg_generation_prepare_cb(vk_fg_generation *generation);
int vk_fg_generation_prepare_finish(vk_fg_generation *generation);
int vk_fg_generation_dispatch(vk_fg_generation *generation,float phase,
    uint64_t deadline_ns,vk_post_output *out);
uint64_t vk_fg_generation_seal(vk_fg_generation *generation);
void vk_fg_generation_cancel(vk_fg_generation *generation);
/* Recorder snapshot pressure may cancel only unsubmitted templates. A canceled
 * lease can still need the open REAL primary sealed before its storage retires. */
unsigned vk_fg_generation_cancel_prepared(void);
unsigned vk_fg_generation_state_mask(void);
/* Set only by the scheduler template lane; stays set through disable/queue
 * drain because a quarantined open lease can outlive every presenter entry. */
extern _Atomic int vk_fg_generation_snapshot_retained;
unsigned vk_fg_generation_cancel_reason(const vk_fg_generation *generation);
typedef struct {
    uint64_t observations,cancellations,appends,pending,quarantined;
} vk_fg_snapshot_pressure_counts;
void vk_fg_snapshot_pressure_trace(unsigned owner_states,unsigned generation_states,
    unsigned cancelled,int append);
vk_fg_snapshot_pressure_counts vk_fg_snapshot_pressure_get(void);
int vk_fg_generation_retire(vk_fg_generation *generation);
int vk_fg_generation_state(const vk_fg_generation *generation);
unsigned vk_fg_generation_slot(const vk_fg_generation *generation);
int vk_fg_generation_stamp_get(const vk_fg_generation *generation,vk_fg_generation_stamp *stamp);
int vk_fg_generation_output(const vk_fg_generation *generation,vk_post_output *out);
/* vk_draw implements scheduling only at normal recorder boundaries; never
 * called recursively by generation sealing or from the presenter. */
void vk_fg_slot_boundary(void);
/* Default0; recorder sets only for successfully prepared, opt-in templates and
 * clears after the last dispatch/cancellation. Check BEFORE calling boundary. */
extern _Atomic int vk_fg_slot_boundary_active;
VkImageView vk_fg_raw_view(unsigned output_slot);
/* DIAG-only boundary after optional synthetic post, before its AHB presentation
   draw or fallback blit/overlay/readback. gpu_end closes this output-private
   bracket; no source query is reset or reused while a presentation is pinned. */
void vk_fg_output_copy_begin(VkCommandBuffer cb, unsigned output_slot);
/* Required exactly once after successful generate, after every synthetic post,
   overlay and output-copy command, immediately before the generated submission. */
void vk_fg_gpu_end(VkCommandBuffer cb, unsigned output_slot);
/* Optional DIAG selected-frame raw chain capture. Call after gpu_end, before
   submission; explicitly flushes/waits once, so exclude that timing window. */
void vk_fg_diag_dump(unsigned output_slot);
/* Exactly once per real frame, after ALL its copy/generated submissions.
   Both sources of an interpolation pair remain pinned through completion. */
void vk_fg_frame_end(uint64_t fence);
/* Completion-owned measured ticket cost; invalid/missing ranges remain unknown.
   Synthetic-only ticket ranges are tagged separately to avoid double counting. */
void vk_fg_gpu_sample(uint64_t ticket, double ms, int valid);
void vk_fg_synthetic_range(uint64_t first, uint64_t last);
/* Immutable real-present identity and observed AHB present-fence feedback.
   Missing fences never become lateness; timestamps are not hardware scanout. */
uint64_t vk_fg_real_frame_id(void);
void vk_fg_real_feedback(uint64_t frame, uint64_t desired_ns,
    uint64_t display_ns, uint64_t refresh_ns);
/* After the generated output fence, including outputs dropped by the presenter.
   Zero rejects this output (budget, authority fraction or unavailable census). */
int vk_fg_complete(unsigned output_slot);
/* Invalidates camera history/current eligibility; recorded sources still retire normally. */
void vk_fg_skip(void);
/* Recorder-owned scene boundary: discard temporal history, not this frame's
   already recorded certificate. External surface reset remains invalidating. */
void vk_fg_history_cut(void);
/* Thread-safe resize/surface-loss request, consumed on the recording thread. */
void vk_fg_reset(void);
/* Thread-safe missed-deadline report: extrapolation suspends permanently;
   interpolation drops only the affected pair and retains real cadence. */
void vk_fg_late(void);
/* Exactly once per real present; reports each 300 real frames. */
void vk_fg_stats(void);

/* Extractor's independent real-frame boundary, implemented in vk_draw.c. */
/* Walking-thread-owned cache; ordinary draws/inline writes need no atomic/env lookup. */
extern int vk_fg_extract_active;
void vk_fg_extract_present(const tex_t *t);
/* Shared exact-mode lifecycle used by Android presentation and host fixtures. */
void vk_fg_present_begin(const tex_t *t);
void vk_fg_present_end(uint64_t covering_fence);
#ifdef TOTK_VK_TESTS
#ifndef TOTK_ANDROID_APP
/* Host recorder-hook fixture only. Substitutes presenter selection, not the
 * chunk boundary, generation dispatch, FIFO, GPU execution or retirement.
 * Recording owner registers/unregisters; an unset callback is the normal stub. */
void vk_fg_test_slot_boundary_callback(void (*callback)(void *),void *arg);
#endif
void vk_fg_test_present_begin(const tex_t *t);
/* Actual private-command/lease fixture. Substitutes only Android AFME pixels
 * with the retained current scene and GL handoff with a real Vulkan binary
 * semaphore; never available in production, not AFME correctness evidence. */
int vk_fg_test_generation_bridge(int enabled);
int vk_fg_test_generation_bridge_active(void);
void vk_fg_test_generation_facts(const vk_fg_generation *generation,uint64_t facts[6],float *phase);
/* Read-only exact production accounting rows. Ticket facts:
 * stored-ticket,measured,synthetic,last-sample. Frame facts:
 * frame,first,last,status,synthetic,last-frame; costs: real,incremental ms. */
int vk_fg_test_budget_ticket(uint64_t ticket,uint64_t facts[4],double *ms);
int vk_fg_test_budget_frame(uint64_t frame,uint64_t facts[6],double costs[2]);
/* Read-only fixture observation; caller holds recorder ownership and may
 * request any subset. Facts: source,previous,used,writes,copies,immutable. */
int vk_fg_test_terminal_facts(const vk_fg_generation *generation,uint64_t facts[6],
    VkDescriptorBufferInfo *buffer,VkDescriptorSet sets[2],vk_record_draw *draw);
int vk_fg_test_terminal_copy(const vk_fg_generation *generation,void *destination,
    size_t capacity);
int vk_fg_test_generation_quarantine(vk_fg_generation *generation,int enabled);
int vk_fg_test_terminal_owned(int enabled);
/* Runtime override of the once-resolved LEAN/DIRECT_OUT selectors (0/1), only
 * while no generation is live; clears the current frame latch. */
int vk_fg_test_adaptive_lean(int lean,int direct);
void vk_fg_test_lean_counts(uint64_t counts[VK_FG_LEAN_COUNTS]); /* lifetime */
/* Host bridge stand-in for an adaptive QCOM refusal/suspension: prepare and
 * dispatch see the engine as unable to generate (real-only fallback). */
void vk_fg_test_generation_refuse(int refused);
/* Current original-bind capture, including failed captures with no generation.
 * Facts: current-source,ready,failed,used,writes,copies. Refusal is DIAG-only. */
int vk_fg_test_terminal_current(uint64_t facts[6],vk_fg_terminal_input *refusal);
void vk_fg_test_present_end(uint64_t covering_fence);
void vk_fg_test_lifecycle(uint64_t facts[8]);
void vk_fg_test_source_lifecycle(uint64_t facts[3]);
uint64_t vk_fg_test_authority_trace_count(const char *reason);
void vk_fg_test_job_ownership(uint64_t counts[2]); /* source refs, busy outputs */
void vk_fg_test_present_cancel(void);
int vk_fg_test_canvas_programs(prog_t *ui,prog_t *terminal);
void vk_fg_test_pass_counts(uint64_t counts[4]); /* warp, batched UI, terminal, output */
/* Open actual single-color pass: the production frozen replay target selector,
   including host/native attachment extent. view is NULL (identity only). */
int vk_fg_test_replay_target(vk_post_output *out);
/* Exercise the production image publisher and boundary, with real Vulkan images. */
void vk_fg_test_alias_lineage(const tex_t *root, const tex_t *other, unsigned results[3]);
unsigned vk_fg_test_nominate_canvas(vk_tex *canvas); /* captured/invalid/open/inline/active bits0..4 */
/* Synthetic camera/root and sampled composition edges; actual clear commands
   between these hooks execute through the production draw walker. */
void vk_fg_test_scene_begin(const tex_t *scene, const tex_t *depth, const float camera[84]);
int vk_fg_test_capture_reads(const tex_t *canvas, const vk_treg *inputs, unsigned count);
unsigned vk_fg_test_depth_contexts(const tex_t *scene, const tex_t *depth, const float camera[84]);
int vk_fg_test_scene_continuations(const tex_t *scene, const tex_t *depth,
    const tex_t *canvas, const float camera[84], unsigned negative);
/* GPU unknown-export fixture uses production admission/ordered spans/publication. */
int vk_fg_test_authority_draw_begin(const tex_t *scene, const tex_t *depth,
    const float camera[84], int seed, const VkRect2D *scissor);
void vk_fg_test_authority_draw_end(void);
/* kind0/1 exact VS variants; kind2..8 unsupported runtime/shader negatives.
   Result bits0 ordinary baseline,1 isolated experimental span. */
unsigned vk_fg_test_uncertified_draw_begin(const tex_t *scene, const tex_t *depth,
    const float camera[84], int seed, unsigned kind, const VkRect2D *scissor);
void vk_fg_test_uncertified_draw_note(int close);
int vk_fg_test_authority_result(unsigned slot, uint32_t counts[4]);
/* Fenced per-pair old/current {width,height,raw,dilated,radius_x,radius_y}. */
int vk_fg_test_authority_pair(unsigned slot, uint32_t counts[12]);
int vk_fg_test_uncertified_result(unsigned slot, uint32_t counts[2]);
int vk_fg_test_uncertified_source(uint32_t *count);
/* DIAG required. Caller owns the generated completion fence, before complete().
   Returns source/output completed phase-pair counts and min/max milliseconds;
   zero means unavailable, incomplete or invalid (never a fabricated duration). */
int vk_fg_test_profile(unsigned slot, uint32_t counts[2], double limits[2]);
/* Fenced DIAG benchmark: replay envelope, midpoint compose, encoded output,
   retained-native copy (milliseconds). The output remains pinned until complete. */
int vk_fg_test_scene_cost(unsigned slot, double milliseconds[4]);
void vk_fg_test_uncertified_fail(int enabled); /* injects ordinary/experimental span-copy failure */
void vk_fg_test_authority_counts(uint64_t counts[5]); /* reservations, final copies, compares, skips, recorded */
void vk_fg_test_authority_span_counts(uint64_t counts[5]); /* before, after, completed, copied texels, failed roots */
void vk_fg_test_authority_fail(int enabled); /* injects reservation failure */
typedef struct {
    char vs[80], fs[80], check[80];
    unsigned classification;
    uint64_t gross, residual, draws, spans;
} vk_fg_writer_test_group;
/* Caller owns the covering real/output fence. Status words: groups, begins,
   closes, final, failed, overflow, residual unknown, residual unowned. */
int vk_fg_test_writer_result(unsigned slot, unsigned status[8], vk_fg_writer_test_group groups[64]);
void vk_fg_test_writer_fail(unsigned enabled, unsigned span_limit);
int vk_fg_test_writer_draw_begin(const tex_t *scene, const tex_t *depth,
    const float camera[84], int seed, unsigned identity, unsigned classification, const VkRect2D *scissor);
void vk_fg_test_writer_draw_note(int close);
#endif
void vk_fg_extract_end(void);
/* Walking-stream writes can invalidate a tracked image's backing storage. */
void vk_fg_extract_memory_write(uint64_t address, uint64_t size);
