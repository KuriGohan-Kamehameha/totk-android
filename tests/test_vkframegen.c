/* Actual Vulkan camera/depth shader acceptance, no game data or Android device.
   Link with the same backend objects/libs as test_vkpost, plus vk_framegen.c.
   Run modes in fresh processes include --off, --refused, --budget, --late,
   --interp, --interp-static, --interp-hold, --interp-hold-off, --interp-replay-buffers, --interp-depth-range, --interp-depth-range-nativeui, --interp-nativeui,
   --interp-authority, --interp-job-reuse, --interp-authority-ordering [normal|lag], --interp-dilation,
   --interp-authority-ordering-lag, --interp-headroom, --interp-present,
   --interp-uncertified, --interp-uncertified-off, --interp-uncertified-invalid,
   --interp-writer, --interp-writer-off, --interp-writer-invalid, --interp-writer-no-diag
   (or no argument).
   No argument keeps the existing TOTK_FRAMEGEN=1 extrapolation acceptance.
   The native UI and job-reuse modes require TOTK_GAME_V143=1, matching the
   standalone vknativeuitest build's production version gate.
   GPU software rasterizers need the test-only generous budget below; this is
   correctness evidence, never a mobile-device timing or visual-quality claim. */
#include "../games/totk/android/gfx/vk_int.h"
#include "../games/totk/android/gfx/vk_framegen.h"
#include "../games/totk/android/gfx/vk_framegen_qcom.h"
#include "../games/totk/android/gfx/vk_framegen_lsfg.h"
#include "../games/totk/android/gfx/framegen_dependencies.h"
#include "framegen_test_camera.h"
#include "../games/totk/android/gfx/vk_canvas.h"
#include "../games/totk/android/gfx/vk_present_slots.h"
#include "../games/totk/android/gfx/vk_present_policy.h"
#include "../games/totk/android/gfx/vk_present_job.h"
#include "../games/totk/android/gfx/vk_present_retire.h"
#include "../games/totk/android/gfx/framegen_slots.h"
#include "../games/totk/runtime/fps_hybrid_runtime.h"
#include "../games/totk/runtime/fps_adaptive_runtime.h"
#include "../games/totk/runtime/runtime.h"
#include "../games/totk/android/gfx/vk_framegen_spv.h"
#include "../games/totk/android/gfx/framegen_budget.h"
#include <unistd.h>

static unsigned checks, failures;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); } } while (0)
static unsigned W=FG_TEST_W,H=FG_TEST_H,DW=FG_TEST_W,DH=FG_TEST_H;
#define PIXELS ((size_t)W * H)
#define DEPTH_PIXELS ((size_t)DW * DH)

typedef struct { VkBuffer buffer; VkDeviceMemory memory; VkDeviceSize size; } host_buffer;
typedef struct {
    pool_t pool;
    tex_t scene_desc, depth_desc, final_desc, canvas_desc;
    vk_tex *scene, *depth, *final, *canvas;
    tex_t native_desc;
    vk_tex *native;
    float *native_pixels;
    VkImageView final_view;
    float *scene_pixels, *depth_pixels;
    float *depth_baseline; /* GPU-uploaded representation, including NaN canonicalization */
    uint8_t *final_pixels;
    VkDescriptorSetLayout ui_dsl, transfer_dsl;
    VkPipelineLayout ui_layout, transfer_layout;
    VkPipeline ui_pipeline, transfer_pipeline;
    VkSampler ui_sampler;
    host_buffer ui_uniform, ui_vertices;
    uint64_t ui_uniform_address;
    VkDescriptorBufferInfo ui_uniform_live;
    float guest_uniform[8], guest_vertices[6];
    unsigned real_passes, ui_repeats;
    int completed;
} fixture;
static struct {
    int enabled, negative_live;
} replay_uniform_mutation;
static struct {
    int enabled, negative, continuations;
    pool_t pool;
    tex_t depth_array, small;
    vk_treg sampled[3];
} partial_clear;
static struct {
    int enabled, kind;
    unsigned frame;
    VkPipeline pipeline;
    host_buffer uniform[2];
    uint32_t result[4], pair[12];
    int have_result, have_pair, defer_complete;
} authority;
static struct {
    int enabled, on, kind, refuse, have_result;
    unsigned frame;
    VkPipeline pipeline;
    host_buffer uniform[3];
    uint32_t admitted[2];
} experimental;

static VkFormat replay_probe_format=VK_FORMAT_R32G32B32A32_SFLOAT;
static int fixture_rgb1110, replay_no_ui;
static int (*writer_capture)(fixture *f, const float camera[84]);
static int writer_capture_refused;

static host_buffer host_create(VkDeviceSize size, VkBufferUsageFlags usage) {
    host_buffer b = {.size = size};
    VkBufferCreateInfo ci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VKCHECK(vkCreateBuffer(V.dev, &ci, NULL, &b.buffer));
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(V.dev, b.buffer, &req);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
        .memoryTypeIndex = (uint32_t)vk_pick_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VKCHECK(vkAllocateMemory(V.dev, &ai, NULL, &b.memory));
    VKCHECK(vkBindBufferMemory(V.dev, b.buffer, b.memory, 0));
    return b;
}
static void *host_map(host_buffer *b) {
    void *p = NULL; VKCHECK(vkMapMemory(V.dev, b->memory, 0, b->size, 0, &p)); return p;
}
static void host_destroy(host_buffer *b) {
    vkDestroyBuffer(V.dev, b->buffer, NULL); vkFreeMemory(V.dev, b->memory, NULL);
}
static void image_barrier(VkCommandBuffer cb, VkImage image, VkImageAspectFlags aspect,
                          VkImageLayout old_layout, VkImageLayout new_layout) {
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        .oldLayout = old_layout, .newLayout = new_layout, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image, .subresourceRange = {aspect,0,1,0,1}};
    VkDependencyInfo d = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &d);
}
static void upload(VkCommandBuffer cb, vk_tex *texture, const void *pixels, size_t size, host_buffer *staging) {
    *staging = host_create(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    memcpy(host_map(staging), pixels, size); vkUnmapMemory(V.dev, staging->memory);
    VkImageAspectFlags aspect = vk_tex_aspect(texture);
    image_barrier(cb, vk_tex_image(texture), aspect, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
    VkBufferImageCopy c = {.imageSubresource = {aspect,0,0,1}, .imageExtent = {vk_tex_width(texture,0),vk_tex_height(texture,0),1}};
    vkCmdCopyBufferToImage(cb, staging->buffer, vk_tex_image(texture), VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    image_barrier(cb, vk_tex_image(texture), aspect, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
    vk_tex_mark_gpu_written(texture);
}
static void read_image_extent(VkImage image, VkImageAspectFlags aspect, void *pixels, size_t bytes, unsigned w, unsigned h) {
    host_buffer output = host_create(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    mtl_sub_begin(); VkCommandBuffer cb = vk_cb();
    image_barrier(cb, image, aspect, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy c = {.imageSubresource = {aspect,0,0,1}, .imageExtent = {w,h,1}};
    vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, output.buffer, 1, &c);
    image_barrier(cb, image, aspect, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    uint64_t ticket = mtl_sub_commit();
    CHECK(mtl_wait(ticket, 10000000000ull), "readback fence completes");
    memcpy(pixels, host_map(&output), bytes); vkUnmapMemory(V.dev, output.memory); host_destroy(&output);
}
static void read_image(VkImage image, VkImageAspectFlags aspect, void *pixels, size_t bytes) {
    read_image_extent(image,aspect,pixels,bytes,W,H);
}
static void texture_desc(tex_t *t, pool_t *pool, size_t offset, int format, uint64_t serial, unsigned w, unsigned h) {
    memset(t, 0, sizeof *t); t->d.target = 1; t->d.format = format;
    t->d.width = w; t->d.height = h; t->d.depth = t->d.levels = 1;
    nvn_tex_layout(&t->d, &t->L); t->mem = (uint8_t *)pool->mem + offset; t->pool = pool; t->serial = serial;
    CHECK(offset + t->L.size <= pool->size, "synthetic texture storage fits fixture pool");
}
static void fixture_resample_depth(fixture *f) {
    if (DW==W && DH==H) return;
    /* Build the native-depth representation of the same normalized scene.
       The scratch baseline is overwritten by actual GPU readback after upload. */
    for (unsigned y=0;y<DH;y++) for (unsigned x=0;x<DW;x++) {
        unsigned cx=(unsigned)((x+0.5)*W/DW),cy=(unsigned)((y+0.5)*H/DH);
        f->depth_baseline[(size_t)y*DW+x]=f->depth_pixels[(size_t)cy*W+cx];
    }
    memcpy(f->depth_pixels,f->depth_baseline,DEPTH_PIXELS*sizeof(float));
}
static void sample_linear(const float *pixels,unsigned w,unsigned h,double x,double y,float out[4]) {
    int x0=(int)floor(x),y0=(int)floor(y); double fx=x-x0,fy=y-y0;
    for (unsigned c=0;c<4;c++) {
        double sum=0;
        for (int j=0;j<2;j++) for (int i=0;i<2;i++) {
            unsigned xx=(unsigned)fmax(0,fmin(w-1,x0+i)),yy=(unsigned)fmax(0,fmin(h-1,y0+j));
            sum+=pixels[((size_t)yy*w+xx)*4+c]*(i ? fx : 1-fx)*(j ? fy : 1-fy);
        }
        out[c]=(float)sum;
    }
}
static void fixture_pixels(fixture *f, int bad_depth) {
    for (unsigned y = 0; y < H; y++) for (unsigned x = 0; x < W; x++) {
        size_t i = (size_t)y * W + x;
        uint8_t encoded[4] = {(uint8_t)(x * 255u / (W-1)), (uint8_t)(y * 255u / (H-1)),64,173};
        for (int c = 0; c < 3; c++) f->scene_pixels[4*i+c] = fg_test_decode(encoded[c]/255.0f);
        f->scene_pixels[4*i+3] = 173.0f / 255.0f;
        memcpy(f->final_pixels + 4*i, encoded, 4);
        if (x >= 220 && x < 260 && y >= 30 && y < 60) {
            f->final_pixels[4*i] = f->final_pixels[4*i+1] = f->final_pixels[4*i+2] = 255; /* held HUD */
            f->final_pixels[4*i+3] = 255;
        }
        if (x >= 10 && x < 18 && y >= 10 && y < 18) {
            f->final_pixels[4*i]=f->final_pixels[4*i+2]=0;
            f->final_pixels[4*i+1]=f->final_pixels[4*i+3]=255;
        }
        f->depth_pixels[i] = fg_test_depth(bad_depth && x >= 160 ? 4 : 2);
        if (bad_depth && x >= 50 && x < 65 && y >= 40 && y < 55) f->depth_pixels[i] = NAN;
    }
    fixture_resample_depth(f);
}
static uint32_t pack_ufloat(float value, unsigned mantissa_bits) {
    if (!(value>0.0f)) return 0;
    if (!isfinite(value)) return 31u<<mantissa_bits;
    int exponent;
    float fraction=frexpf(value,&exponent)*2.0f-1.0f;
    int biased=exponent-1+15;
    if (biased<=0) {
        unsigned mantissa=(unsigned)lrintf(ldexpf(value,14+(int)mantissa_bits));
        return mantissa>(1u<<mantissa_bits) ? 1u<<mantissa_bits : mantissa;
    }
    if (biased>=31) return 31u<<mantissa_bits;
    unsigned mantissa=(unsigned)lrintf(fraction*(1u<<mantissa_bits));
    if (mantissa==(1u<<mantissa_bits)) {
        mantissa=0;
        if (++biased>=31) return 31u<<mantissa_bits;
    }
    return ((unsigned)biased<<mantissa_bits)|mantissa;
}
static uint32_t pack_r11g11b10(const float rgba[4]) {
    return pack_ufloat(rgba[0],6) |
        (pack_ufloat(rgba[1],6)<<11) |
        (pack_ufloat(rgba[2],5)<<22);
}
static void fixture_upload(fixture *f) {
    host_buffer staging[3];
    mtl_sub_begin(); VkCommandBuffer cb = vk_cb();
    if (f->native) {
        for (unsigned y=0;y<DH;y++) for (unsigned x=0;x<DW;x++)
            sample_linear(f->scene_pixels,W,H,(x+0.5)*W/DW-0.5,(y+0.5)*H/DH-0.5,
                f->native_pixels+((size_t)y*DW+x)*4);
        upload(cb,f->native,f->native_pixels,DEPTH_PIXELS*4*sizeof(float),&staging[0]);
        image_barrier(cb,vk_tex_image(f->scene),VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL);
        VkImageBlit scale={.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.srcOffsets={{0,0,0},{DW,DH,1}},
            .dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.dstOffsets={{0,0,0},{W,H,1}}};
        vkCmdBlitImage(cb,vk_tex_image(f->native),VK_IMAGE_LAYOUT_GENERAL,vk_tex_image(f->scene),
            VK_IMAGE_LAYOUT_GENERAL,1,&scale,VK_FILTER_LINEAR);
        image_barrier(cb,vk_tex_image(f->scene),VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL);
        vk_tex_mark_gpu_written(f->scene);
    } else if (fixture_rgb1110) {
        uint32_t *packed=malloc(PIXELS*sizeof *packed);
        CHECK(packed!=NULL,"allocate packed R11G11B10 fixture pixels");
        if (!packed) exit(1);
        for (size_t i=0;i<PIXELS;i++) packed[i]=pack_r11g11b10(f->scene_pixels+4*i);
        upload(cb,f->scene,packed,PIXELS*sizeof *packed,&staging[0]);
        free(packed);
    } else upload(cb, f->scene, f->scene_pixels, PIXELS * 4 * sizeof(float), &staging[0]);
    upload(cb, f->depth, f->depth_pixels, DEPTH_PIXELS * sizeof(float), &staging[1]);
    upload(cb, f->final, f->final_pixels, PIXELS * 4, &staging[2]);
    uint64_t ticket = mtl_sub_commit(); CHECK(mtl_wait(ticket, 10000000000ull), "fixture upload completes");
    for (int i = 0; i < 3; i++) host_destroy(&staging[i]);
    /* Depth transfers may canonicalize nonfinite payloads on a host driver.
     * Preservation is against the image actually supplied to generation, not
     * an assumed CPU NaN encoding before its first GPU upload. */
    read_image_extent(vk_tex_image(f->depth), VK_IMAGE_ASPECT_DEPTH_BIT, f->depth_baseline, DEPTH_PIXELS*sizeof(float),DW,DH);
    if (f->native) read_image(vk_tex_image(f->scene),VK_IMAGE_ASPECT_COLOR_BIT,f->scene_pixels,PIXELS*4*sizeof(float));
}
static void fixture_replay_init(fixture *f);
static void fixture_create(fixture *f) {
    memset(f, 0, sizeof *f);
    size_t stride=W==FG_TEST_W && H==FG_TEST_H ? 2u<<20 : 32u<<20;
    f->pool.size = stride*(DW==W && DH==H ? 4 : 5); f->pool.mem = aligned_alloc(4096, f->pool.size);
    CHECK(f->pool.mem != NULL, "allocate fixture guest storage");
    if (!f->pool.mem) exit(1);
    f->pool.flags = NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_GPU_CACHED;
    texture_desc(&f->scene_desc, &f->pool, 0, fixture_rgb1110 ? 0x3f : 0x2e, 80001,W,H); /* pre-UI canvas */
    texture_desc(&f->depth_desc, &f->pool, stride, 0x34, 80002,DW,DH); /* native D32F */
    texture_desc(&f->final_desc, &f->pool, stride*2, 0x25, 80003,W,H);
    texture_desc(&f->canvas_desc, &f->pool, stride*3, fixture_rgb1110 ? 0x3f : 0x2e, 80004,W,H);
    if (!fixture_rgb1110 && (DW!=W || DH!=H)) {
        texture_desc(&f->native_desc,&f->pool,stride*4,0x2e,80005,DW,DH);
        f->native_pixels=malloc(DEPTH_PIXELS*4*sizeof(float));
        CHECK(f->native_pixels!=NULL,"allocate actual 960x536 color source for nonuniform GPU upscale");
        if (!f->native_pixels) exit(1);
    }
    f->scene_pixels = malloc(PIXELS * 4 * sizeof(float)); f->depth_pixels = malloc(PIXELS * sizeof(float));
    f->final_pixels = malloc(PIXELS * 4);
    f->depth_baseline = malloc(PIXELS*sizeof(float));
    CHECK(f->scene_pixels && f->depth_pixels && f->final_pixels && f->depth_baseline, "allocate synthetic input pixels");
    if (!f->scene_pixels || !f->depth_pixels || !f->final_pixels || !f->depth_baseline) exit(1);
    mtl_sub_begin();
    f->scene = vk_tex_get(&f->scene_desc); f->depth = vk_tex_get(&f->depth_desc); f->final = vk_tex_get(&f->final_desc);
    f->canvas = vk_tex_get(&f->canvas_desc);
    if (f->native_pixels) f->native=vk_tex_get(&f->native_desc);
    CHECK(f->scene && f->depth && f->final, "create actual backend scene/depth/final textures");
    if (!f->scene || !f->depth || !f->final) exit(1);
    f->final_view = vk_tex_view(f->final, &f->final_desc, NULL, 0);
    uint64_t ticket = mtl_sub_commit(); CHECK(mtl_wait(ticket, 10000000000ull), "texture initialization completes");
    fixture_pixels(f, 0); fixture_upload(f);
    fixture_replay_init(f);
}

/* Real clear walker/GPU regression for the auxiliary subresources observed on
   Thor. Scene/camera and sampled-composition edges are synthetic test inputs;
   clear provenance, ancestry selection, source capture and synthesis are real. */
static void partial_clear_create(void) {
    partial_clear.enabled=1;
    partial_clear.pool=(pool_t){.size=4u<<20,.flags=NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS|NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    partial_clear.pool.mem=aligned_alloc(4096,partial_clear.pool.size);
    CHECK(partial_clear.pool.mem!=NULL,"allocate partial-clear auxiliary images");
    if (!partial_clear.pool.mem) exit(1);
    texture_desc(&partial_clear.depth_array,&partial_clear.pool,0,0x32,82001,512,512);
    tex_t *d=&partial_clear.depth_array;
    d->d.target=4; d->d.width=d->d.height=512; d->d.depth=d->d.levels=2;
    nvn_tex_layout(&d->d,&d->L);
    texture_desc(&partial_clear.small,&partial_clear.pool,2u<<20,0x25,82002,120,67);
    mtl_sub_begin();
    vk_tex *z=vk_tex_get(d),*rgba=vk_tex_get(&partial_clear.small);
    CHECK(vk_tex_width(z,1)==256 && vk_tex_layers(z)==2 && vk_tex_format(z)==VK_FORMAT_D16_UNORM,
          "fixture realizes observed 256x256 mip1 of two-layer D16 array");
    CHECK(vk_tex_width(rgba,0)==120 && vk_tex_height(rgba,0)==67 && vk_tex_format(rgba)==VK_FORMAT_R8G8B8A8_UNORM,
          "fixture realizes observed 120x67 RGBA8 auxiliary");
    /* Deterministic previous-frame contents, intentionally outside this frame's
       provenance epoch. The regional clear must preserve, not certify, them. */
    VkCommandBuffer cb=vk_cb();
    VkImageSubresourceRange zr={VK_IMAGE_ASPECT_DEPTH_BIT,0,2,0,2},cr={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    VkClearDepthStencilValue zv={0.25f,0}; VkClearColorValue cv={.float32={0.25f,0.25f,0.25f,1}};
    vk_barrier_all(cb);
    vkCmdClearDepthStencilImage(cb,vk_tex_image(z),VK_IMAGE_LAYOUT_GENERAL,&zv,1,&zr);
    vkCmdClearColorImage(cb,vk_tex_image(rgba),VK_IMAGE_LAYOUT_GENERAL,&cv,1,&cr);
    uint64_t ticket=mtl_sub_commit(); CHECK(mtl_wait(ticket,10000000000ull),"auxiliary history initialization completes");
    for (unsigned i=0;i<2;i++) {
        partial_clear.sampled[i]=(vk_treg){.t=*d,.has_v=1};
        partial_clear.sampled[i].v=(view_t){.set_levels=1,.base_level=1,.nlevels=1,.set_layers=1,.min_layer=i,.nlayers=1};
    }
    partial_clear.sampled[2]=(vk_treg){.t=partial_clear.small};
}
static int partial_clear_capture(fixture *f,const float camera[84]) {
    if (partial_clear.continuations)
        CHECK(vk_fg_test_scene_continuations(&f->native_desc,&f->depth_desc,&f->scene_desc,camera,0)==1,
            "depth-only, alternate-camera MRT and later standard MRT retain one proven root");
    else vk_fg_test_scene_begin(&f->scene_desc,&f->depth_desc,camera);
    p_scissor sc={3,5,40,30}; vk_draw_exec(OP_SCISSOR,&sc);
    p_clear_ds clear_depth={.depth_mask=1,.depth=1};
    for (unsigned layer=0;layer<2;layer++) {
        p_set_rt rt={.depth=&partial_clear.depth_array,.has_dview=1};
        rt.dview=partial_clear.sampled[layer].v;
        vk_draw_exec(OP_SET_RT,&rt);
        vk_draw_exec(OP_CLEAR_DS,&clear_depth);
        vk_end_rendering();
    }
    p_set_rt rt={.n=1,.color={&partial_clear.small}};
    vk_draw_exec(OP_SET_RT,&rt);
    p_clear_color clear_color={0,15,{1,0,0,1}};
    vk_draw_exec(OP_CLEAR_COLOR,&clear_color);
    vk_end_rendering();
    if (partial_clear.negative==1) {
        /* Actual guest write with no classified image writer; notification must
           invalidate the scene rather than being laundered by a later clear. */
        *(uint8_t *)f->scene_desc.mem=0;
        vk_fg_extract_memory_write((uint64_t)(uintptr_t)f->scene_desc.mem,f->scene_desc.L.size);
    } else if (partial_clear.negative==2) {
        p_copy_t2t crop={.src=&f->scene_desc,.dst=&f->canvas_desc,
            .sr={1,0,0,(int)W-1,(int)H,1},.dr={0,0,0,(int)W-1,(int)H,1}};
        vk_draw_exec(OP_COPY_T2T,&crop);
    }
    int captured=vk_fg_test_capture_reads(partial_clear.negative==2 ? &f->canvas_desc : &f->scene_desc,
        partial_clear.sampled,3);
    CHECK(captured==!partial_clear.negative,"partial clears select/capture the scene; unknown writes and unmodelled crops refuse");
    return captured;
}
static void partial_clear_pixels(void) {
    host_buffer host=host_create(2*256*256*2+512*512*2+120*67*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    mtl_sub_begin(); VkCommandBuffer cb=vk_cb(); vk_barrier_all(cb);
    vk_tex *z=vk_tex_get(&partial_clear.depth_array),*rgba=vk_tex_get(&partial_clear.small);
    VkBufferImageCopy depth[3]={
        {.imageSubresource={VK_IMAGE_ASPECT_DEPTH_BIT,1,0,1},.imageExtent={256,256,1}},
        {.bufferOffset=256*256*2,.imageSubresource={VK_IMAGE_ASPECT_DEPTH_BIT,1,1,1},.imageExtent={256,256,1}},
        {.bufferOffset=2*256*256*2,.imageSubresource={VK_IMAGE_ASPECT_DEPTH_BIT,0,0,1},.imageExtent={512,512,1}}};
    vkCmdCopyImageToBuffer(cb,vk_tex_image(z),VK_IMAGE_LAYOUT_GENERAL,host.buffer,3,depth);
    VkBufferImageCopy color={.bufferOffset=2*256*256*2+512*512*2,
        .imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={120,67,1}};
    vkCmdCopyImageToBuffer(cb,vk_tex_image(rgba),VK_IMAGE_LAYOUT_GENERAL,host.buffer,1,&color);
    uint64_t ticket=mtl_sub_commit(); CHECK(mtl_wait(ticket,10000000000ull),"partial-clear mip/layer readback completes");
    const uint16_t *p=host_map(&host);
    for (unsigned layer=0;layer<2;layer++) {
        CHECK(p[layer*256*256+6*256+4]==65535,"D16 mip1 layer%u rectangle is really cleared",layer);
        CHECK(abs((int)p[layer*256*256]-16384)<=1,"D16 layer%u remainder preserves previous-frame pixels",layer);
    }
    CHECK(abs((int)p[2*256*256+6*512+4]-16384)<=1,"partial mip1 clear does not change mip0");
    const uint8_t *c=(const uint8_t *)p+color.bufferOffset;
    CHECK(c[(6*120+4)*4]==255 && c[(6*120+4)*4+1]==0,"small RGBA8 rectangle is really cleared");
    CHECK(abs((int)c[0]-64)<=1 && abs((int)c[1]-64)<=1,"small RGBA8 remainder preserves previous-frame pixels");
    vkUnmapMemory(V.dev,host.memory); host_destroy(&host);
}
static void partial_clear_destroy(void) {
    mtl_tex_release(&partial_clear.depth_array); mtl_tex_release(&partial_clear.small);
    free(partial_clear.pool.mem);
}
static int native_ui_terminal;
static int scene_scale_fixture;
static unsigned native_terminal_w, native_terminal_h;
#include "framegen_test_replay.h"
static int authority_capture(fixture *f, const float camera[84]) {
    vk_fg_test_authority_fail(authority.kind==5);
    unsigned count=authority.kind==3 ? 2 : 1;
    VkRect2D rect=authority.kind==4 ? (VkRect2D){{0,0},{W*3/4,H}} :
        (VkRect2D){{90,65},{24,18}};
    for (unsigned draw=0;draw<count;draw++) {
        float uniform[8]={0,0,fg_test_depth(authority.kind==2 || draw ? 2 : 4),0,
            (draw ? 0.75f : 0.2f)+0.1f*authority.frame,0.3f,0.6f,1};
        memcpy(host_map(&authority.uniform[draw]),uniform,sizeof uniform);
        vkUnmapMemory(V.dev,authority.uniform[draw].memory);
        int accepted=vk_fg_test_authority_draw_begin(&f->scene_desc,&f->depth_desc,camera,!draw,&rect);
        CHECK(accepted==(authority.kind!=5),"unknown span obtains a pre-draw depth certificate or refuses on copy failure");
        VkDescriptorSet set=vk_alloc_set(f->ui_dsl);
        VkDescriptorBufferInfo buffer={authority.uniform[draw].buffer,0,sizeof uniform};
        VkWriteDescriptorSet write={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.dstBinding=0,
            .descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,.pBufferInfo=&buffer};
        vkUpdateDescriptorSets(V.dev,1,&write,0,NULL);
        vk_record_draw packet={.pipeline=authority.pipeline,.layout=f->ui_layout,.count=3,.instances=1};
        packet.dynamic.vp=(VkViewport){0,0,W,H,0,1}; packet.dynamic.sc=rect;
        packet.dynamic.topo=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        packet.dynamic.dt=packet.dynamic.dw=1; packet.dynamic.dc=VK_COMPARE_OP_ALWAYS;
        VkDescriptorSet sets[2]={set,0};
        vk_fg_replay_draw(vk_cb(),&packet,sets);
        vk_fg_test_authority_draw_end();
    }
    vk_fg_test_authority_fail(0);
    return vk_fg_test_capture_reads(&f->scene_desc,NULL,0);
}
static int experimental_capture(fixture *f, const float camera[84]) {
    float checked_camera[84]; memcpy(checked_camera,camera,sizeof checked_camera);
    checked_camera[65]=(1-checked_camera[38])*0.5f; camera=checked_camera;
    unsigned count=experimental.kind==4 ? 3 :
        experimental.kind>=2 && experimental.kind<=6 ? 2 : 1;
    experimental.refuse=(experimental.on && (experimental.kind==14 || experimental.kind==15)) ||
        experimental.kind==16;
    for (unsigned draw=0;draw<count;draw++) {
        int generic=(experimental.kind==2 || experimental.kind==5) ? draw==0 :
            (experimental.kind==3 || experimental.kind==4) ? draw>0 : 0;
        unsigned qualifier=experimental.kind==1 || (experimental.kind==6 && draw==1) ? 1 :
            experimental.kind>=7 && experimental.kind<=13 ? (unsigned)experimental.kind-5 : 0;
        float distance=generic ? (experimental.kind==5 || (experimental.kind==4 && draw==2) ? 4 : 8) : 4;
        float uniform[8]={0,0,fg_test_depth(distance),0,
            (generic ? 0.5f : 0.2f)+0.1f*(experimental.frame&1),0.3f,0.6f,1};
        memcpy(host_map(&experimental.uniform[draw]),uniform,sizeof uniform);
        vkUnmapMemory(V.dev,experimental.uniform[draw].memory);
        VkRect2D rect={{90,65},{24,18}};
        if ((experimental.kind==2 && generic) || (experimental.kind==6 && draw==1)) rect.offset.x=140;
        vk_fg_test_uncertified_fail(experimental.kind==14 && experimental.on);
        if (generic) {
            CHECK(vk_fg_test_authority_draw_begin(&f->scene_desc,&f->depth_desc,camera,!draw,&rect),
                "unrelated depth writer retains only ordinary masked authority");
        } else {
            unsigned state=vk_fg_test_uncertified_draw_begin(&f->scene_desc,&f->depth_desc,camera,!draw,qualifier,&rect);
            int allowed=experimental.on && qualifier<2 && experimental.kind!=14;
            CHECK(!!(state&2)==allowed,"only opt-in exact pair with matching runtime camera enters UNCERTIFIED span");
        }
        VkDescriptorSet set=vk_alloc_set(f->ui_dsl);
        VkDescriptorBufferInfo buffer={experimental.uniform[draw].buffer,0,sizeof uniform};
        VkWriteDescriptorSet write={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.dstBinding=0,
            .descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,.pBufferInfo=&buffer};
        vkUpdateDescriptorSets(V.dev,1,&write,0,NULL);
        vk_record_draw packet={.pipeline=experimental.pipeline,.layout=f->ui_layout,.count=3,.instances=1};
        packet.dynamic.vp=(VkViewport){0,0,W,H,0,1}; packet.dynamic.sc=rect;
        packet.dynamic.topo=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        packet.dynamic.dt=packet.dynamic.dw=1; packet.dynamic.dc=VK_COMPARE_OP_ALWAYS;
        VkDescriptorSet sets[2]={set,0}; vk_fg_replay_draw(vk_cb(),&packet,sets);
        if (experimental.kind==15 && experimental.on) vk_fg_test_uncertified_fail(1);
        vk_fg_test_uncertified_draw_note(draw+1==count && experimental.kind!=16);
        vk_fg_test_uncertified_fail(0);
    }
    if (experimental.kind==16) {
        p_clear_ds clear={.depth_mask=1,.depth=1};
        vk_draw_exec(OP_CLEAR_DS,&clear); /* must finish attribution BEFORE the clear */
    }
    return vk_fg_test_capture_reads(&f->scene_desc,NULL,0);
}
static int real_frame(fixture *f, const float camera[84], unsigned slot, int srgb, vk_post_output *out) {
    memset(out, 0xa5, sizeof *out); vk_post_output unchanged = *out;
    uint64_t before[4], after[4]; vk_fg_test_pass_counts(before);
    unsigned real_before=f->real_passes;
    int interp=vk_fg_interpolating();
    f->completed=0;
    float native_camera[84];
    if (DW!=W || DH!=H) {
        memcpy(native_camera,camera,sizeof native_camera);
        native_camera[72]=DW; native_camera[73]=DH; camera=native_camera;
    }
    mtl_sub_begin(); VkCommandBuffer cb = vk_cb();
    if (writer_capture) CHECK(writer_capture(f,camera)==!writer_capture_refused,
        "production draw fixture captures or refuses its exact source without stale authority");
    else if (experimental.enabled) {
        int captured=experimental_capture(f,camera);
        CHECK(captured==!experimental.refuse,"experimental failures/clears refuse rather than retain stale candidates");
    } else if (authority.enabled) {
        int captured=authority_capture(f,camera);
        CHECK(captured==(authority.kind!=5),"unknown depth writer preserves one masked root; unavailable snapshot refuses");
    } else if (partial_clear.enabled) (void)partial_clear_capture(f,camera);
    else { vk_fg_camera(camera); vk_fg_capture(cb, f->scene, 0, 0, f->depth, 0, 0, W, H); }
    /* Recorder-backed clears may advance the ordered primary section. Never
       append replay/generation to the pre-clear command buffer. */
    if (partial_clear.enabled || authority.enabled || experimental.enabled || writer_capture) cb=vk_cb();
    int spatial=native_ui_terminal && vk_scene_post_enabled();
    int prepared = vk_fg_prepare_canvas(cb,spatial?native_terminal_w:W,spatial?native_terminal_h:H);
    if (interp) CHECK(prepared==!((partial_clear.enabled && partial_clear.negative) ||
        (authority.enabled && authority.kind==5) || (experimental.enabled && experimental.refuse) ||
        (writer_capture && writer_capture_refused)),
        "interpolation prepares UI captures only after accepted source ancestry");
    int replayed=fixture_render_replay(f, cb, prepared);
    uint64_t real_ticket=0;
    if (vk_fg_locked() && scene_scale_fixture && replayed) {
        /* Match the production locked boundary: frozen UI/pair composition is
           real work, while output/encoding has a distinct synthetic ticket. */
        real_ticket=mtl_sub_commit();
        mtl_sub_begin(); cb=vk_cb();
    }
    int generated = vk_fg_generate(cb, slot, native_ui_terminal ? native_terminal_w : W,
        native_ui_terminal ? native_terminal_h : H, srgb, out);
    vk_fg_test_pass_counts(after);
    CHECK(f->real_passes-real_before==2,"real UI and terminal transfer remain exactly two real passes");
    if (interp) {
        CHECK(generated==replayed,"interpolation generates only after pair composition and terminal replay");
        CHECK(after[0]-before[0]==(unsigned)generated && after[1]-before[1]==(prepared ? 2u : 0u) &&
            after[2]-before[2]==(unsigned)generated && after[3]-before[3]==(unsigned)generated,
            "interpolation captures exactly two UI layers per real frame and one compose/transfer/output per valid pair");
    } else if (generated) CHECK(after[0]-before[0]==1 && after[1]-before[1]==1 &&
        after[2]-before[2]==1 && after[3]-before[3]==1,
        "generation adds exactly one warp, one batched UI, one transfer and one output pass");
    if (generated) {
        vk_fg_gpu_end(cb, slot);
        vk_fg_diag_dump(slot);
    }
    else {
        CHECK(!memcmp(out, &unchanged, sizeof *out), "skipped generation leaves output untouched");
        memset(out, 0, sizeof *out);   /* failed acceptance must not read sentinel image handles */
    }
    /* Production extractor modes report exactly once from extract_end. */
    if (!partial_clear.enabled && !authority.enabled && !experimental.enabled && !writer_capture) vk_fg_track_result(VK_FG_ELIGIBLE);
    uint64_t ticket = mtl_sub_commit();
    if (real_ticket && generated) vk_fg_synthetic_range(real_ticket+1,ticket);
    if (partial_clear.enabled || authority.enabled || experimental.enabled || writer_capture) vk_fg_extract_end();
    vk_fg_frame_end(ticket);
    CHECK(mtl_wait(ticket, 10000000000ull), "real/generated frame fence completes");
    authority.have_result=generated && vk_fg_test_authority_result(slot,authority.result);
    authority.have_pair=generated && vk_fg_test_authority_pair(slot,authority.pair);
    experimental.have_result=generated && vk_fg_test_uncertified_result(slot,experimental.admitted);
    if (generated && !authority.defer_complete) f->completed=vk_fg_complete(slot);
    vk_fg_stats();
    return generated;
}
static void check_pixel(const uint8_t *pixels, unsigned x, unsigned y, const uint8_t *expected, unsigned tolerance, const char *label) {
    const uint8_t *p = pixels + ((size_t)y * W + x) * 4;
    for (int c = 0; c < 4; c++) CHECK(abs((int)p[c] - expected[c]) <= (int)tolerance,
        "%s channel%d at %u,%u: %u expected %u", label, c, x, y, p[c], expected[c]);
}
static uint8_t encoded_channel(float linear) {
    float encoded = linear <= 0.0031308f ? 12.92f * linear : 1.055f * powf(linear, 1.0f/2.4f) - 0.055f;
    return (uint8_t)lrintf(fminf(1, fmaxf(0, encoded)) * 255);
}
static void expected_gradient(fixture *f, double source_u, double source_v, uint8_t out[4]) {
    double px = source_u * W - 0.5, py = source_v * H - 0.5;
    unsigned x = (unsigned)floor(px), y = (unsigned)floor(py);
    float fx = (float)(px-x), fy = (float)(py-y);
    for (int c = 0; c < 3; c++) {
        float a = f->scene_pixels[((size_t)y*W+x)*4+c] * (1-fx) + f->scene_pixels[((size_t)y*W+x+1)*4+c] * fx;
        float b = f->scene_pixels[((size_t)(y+1)*W+x)*4+c] * (1-fx) + f->scene_pixels[((size_t)(y+1)*W+x+1)*4+c] * fx;
        out[c] = encoded_channel(a*(1-fy)+b*fy);
    }
    out[3] = 173;
}
static void check_sources_unchanged(fixture *f) {
    float *scene = malloc(PIXELS*4*sizeof(float)), *depth = malloc(PIXELS*sizeof(float));
    CHECK(scene && depth, "allocate source readback buffers");
    if (!scene || !depth) exit(1);
    read_image(vk_tex_image(f->scene), VK_IMAGE_ASPECT_COLOR_BIT, scene,
        PIXELS*(fixture_rgb1110 ? sizeof(uint32_t) : 4*sizeof(float)));
    read_image_extent(vk_tex_image(f->depth), VK_IMAGE_ASPECT_DEPTH_BIT, depth, DEPTH_PIXELS*sizeof(float),DW,DH);
    if (fixture_rgb1110) {
        uint32_t *packed=(uint32_t *)scene;
        unsigned changed=0;
        for (size_t i=0;i<PIXELS;i++) changed+=packed[i]!=pack_r11g11b10(f->scene_pixels+4*i);
        CHECK(!changed,"capture/generation preserve all original packed R11G11B10 scene texels exactly");
    } else CHECK(!memcmp(scene, f->scene_pixels, PIXELS*4*sizeof(float)), "capture/generation preserve original float scene exactly");
    CHECK(!memcmp(depth, f->depth_baseline, DEPTH_PIXELS*sizeof(float)), "capture/generation preserve original GPU D32F exactly");
    free(scene); free(depth);
}
static void check_extractor_images_and_refusal(fixture *f) {
    tex_t independent, shared=f->scene_desc;
    texture_desc(&independent,&f->pool,0,0x25,80100,W,H);
    shared.serial=80101;
    mtl_sub_begin();
    vk_tex *other=vk_tex_get(&independent), *alias=vk_tex_get(&shared);
    CHECK(vk_tex_image(other)!=vk_tex_image(f->scene) && vk_tex_image(alias)==vk_tex_image(f->scene),
          "overlapping guest ranges realize independent-format images but compatible aliases share one image");
    unsigned result[3];
    vk_fg_test_alias_lineage(&f->scene_desc,&independent,result);
    CHECK(result[0]==FG_DEP_SELECTED && result[1]==FG_DEP_SELECTED,
          "production publisher preserves root when a DIFFERENT GPU image overlaps its guest range");
    CHECK(result[2]==FG_DEP_TAINTED,"real guest-range write still revokes independently stored scene proof");
    vk_fg_test_alias_lineage(&f->scene_desc,&shared,result);
    CHECK(result[0]==FG_DEP_SELECTED && result[1]==FG_DEP_NO_ORIGIN,
          "production publisher still observes destructive overwrite through a real shared-image alias");
    p_set_rt rt={.n=1,.color={&f->canvas_desc}};
    p_scissor sc={0,0,W-1,H-1};
    p_clear_color clear={0,15,{0,0,0,1}};
    vk_draw_exec(OP_SET_RT,&rt); vk_draw_exec(OP_SCISSOR,&sc); vk_draw_exec(OP_CLEAR_COLOR,&clear);
    vk_record_test_counts before,after; vk_record_test_counts_get(&before);
    unsigned state=vk_fg_test_nominate_canvas(f->canvas);
    CHECK(state==6,"refused boundary leaves real pass open, does not force inline UI, and stops remaining tracker work (state=%u)",state);
    vk_draw_exec(OP_CLEAR_COLOR,&clear); vk_record_test_counts_get(&after);
    CHECK(after.passes==before.passes,"refused probe does not inject an extra real render pass/store-reload");
    vk_end_rendering();
    uint64_t ticket=mtl_sub_commit(); vk_fg_extract_end(); vk_fg_frame_end(ticket); vk_fg_stats();
    CHECK(mtl_wait(ticket,10000000000ull),"refusal regression commands complete");
    vk_canvas_frame_end();
    /* Exercise the REAL extractor lifecycle, not just the pure countdown.
       Refusing loading frames must keep one pass and probe10/320 times. */
    unsigned probes=0, bad=0, waits=0;
    vk_record_test_counts_get(&before);
    for (unsigned frame=0;frame<320;frame++) {
        mtl_sub_begin();
        vk_draw_exec(OP_SET_RT,&rt); vk_draw_exec(OP_SCISSOR,&sc); vk_draw_exec(OP_CLEAR_COLOR,&clear);
        if (vk_fg_extract_active) { probes++; bad += vk_fg_test_nominate_canvas(f->canvas)!=6; }
        vk_end_rendering();
        ticket=mtl_sub_commit(); vk_fg_extract_end(); vk_fg_frame_end(ticket); vk_fg_stats(); vk_canvas_frame_end();
        waits += !mtl_wait(ticket,10000000000ull);
    }
    vk_record_test_counts_get(&after);
    CHECK(probes==10 && !bad,"actual failed-probe lifecycle resumes10/320 times without inline UI or closing the real pass");
    CHECK(after.passes-before.passes==320,"320 refused/backoff frames emit exactly320 real passes, no probe-only splits");
    CHECK(!waits,"all320 refusing-frame GPU submissions complete");
    mtl_tex_release(&independent); mtl_tex_release(&shared);
}

static int run_enabled(void) {
    setenv("TOTK_FRAMEGEN", "1", 1); setenv("TOTK_FRAMEGEN_BUDGET_MS", "10000", 1);
    mtl_init(); CHECK(vk_fg_enabled(), "explicit opt-in enables generation");
    fixture f; fixture_create(&f);
    check_extractor_images_and_refusal(&f);
    float camera[84], previous[84]; fg_test_camera(camera, 0, 0, 0, 0);
    vk_post_output out; uint8_t *pixels = malloc(PIXELS*4);
    CHECK(pixels != NULL, "allocate output pixels"); if (!pixels) exit(1);
    CHECK(!real_frame(&f, camera, 0, 0, &out), "first accepted real frame warms camera history only");
    CHECK(real_frame(&f, camera, 3, 1, &out), "identity second real frame produces output in fourth presenter slot");
    CHECK(out.w == W && out.h == H && out.format == VK_FORMAT_R8G8B8A8_SRGB && out.view,
        "encoded raw output exposes expected final extent and sRGB display view");
    if (out.image) {
        read_image(out.image, VK_IMAGE_ASPECT_COLOR_BIT, pixels, PIXELS*4);
        unsigned mismatch = 0;
        for (size_t i = 0; i < PIXELS*4; i++) if (abs((int)pixels[i]-f.final_pixels[i]) > 1) mismatch++;
        CHECK(!mismatch, "identity shader preserves full encoded scene and held HUD (%u mismatched channels)", mismatch);
    }
    vk_fg_reset(); fg_test_camera(previous, 0, -0.16, 0, 0);
    CHECK(!real_frame(&f, previous, 0, 0, &out), "reset discards previous camera history");
    CHECK(real_frame(&f, camera, 0, 0, &out), "translated camera generates gradient output");
    CHECK(out.format == VK_FORMAT_R8G8B8A8_UNORM, "UNORM output retains encoded RGB semantics");
    if (out.image) {
        read_image(out.image, VK_IMAGE_ASPECT_COLOR_BIT, pixels, PIXELS*4);
        uint8_t expected[4]; double u = (100.5/W), v = (100.5/H);
        expected_gradient(&f, u + 0.01125, v, expected);
        check_pixel(pixels, 100, 100, expected, 1, "translated analytic gradient golden");
        CHECK(pixels[((size_t)100*W+100)*4] > f.final_pixels[((size_t)100*W+100)*4], "translation changes scene rather than no-op/final copy");
        check_pixel(pixels, 230, 40, f.final_pixels+((size_t)40*W+230)*4, 0, "HUD remains at real-frame position");
        CHECK(f.guest_vertices[0]==1000 && f.guest_uniform[4]==0,
              "guest uniform/vertices changed after real frame, before deferred replay");
        unsigned ui_changed=0;
        for(unsigned y=30;y<60;y++) for(unsigned x=220;x<260;x++)
            ui_changed += memcmp(pixels+((size_t)y*W+x)*4,f.final_pixels+((size_t)y*W+x)*4,4)!=0;
        CHECK(!ui_changed,"every opaque UI pixel is unchanged using frozen real-frame bindings");
        check_pixel(pixels,12,12,f.final_pixels+((size_t)12*W+12)*4,0,"ordered partial UI clear is replayed unchanged in the same pass");
        expected_gradient(&f, 218.5/W + 0.01125, 40.5/H, expected);
        check_pixel(pixels, 218, 40, expected, 1, "scene beside HUD moves without a held-background halo");
        check_pixel(pixels, W-1, 100, f.final_pixels+((size_t)100*W+W-1)*4, 1, "disoccluded border holds real pixel");
    }
    vk_fg_reset(); fg_test_camera(previous, -0.1, 0, 0, 0);
    CHECK(!real_frame(&f, previous, 1, 0, &out), "rotation sequence warms history");
    CHECK(real_frame(&f, camera, 1, 0, &out), "rotation camera generates actual shader output");
    if (out.image) {
        read_image(out.image, VK_IMAGE_ASPECT_COLOR_BIT, pixels, PIXELS*4);
        unsigned x = W/2, y = H/2; double u = (x+0.5)/W, v = (y+0.5)/H;
        double xs = (double)H/W, ndcx = 2*u-1, ndcy = 1-2*v;
        double a = 0.05, denom = cos(a) + (ndcx/xs)*sin(a);
        double sx = ((ndcx/xs)*cos(a)-sin(a))/denom;
        uint8_t expected[4]; expected_gradient(&f, 0.5+0.5*xs*sx, 0.5-0.5*ndcy/denom, expected);
        check_pixel(pixels, x, y, expected, 1, "rigid rotation analytic gradient golden");
    }
    fixture_pixels(&f, 1); fixture_upload(&f); vk_fg_reset(); fg_test_camera(previous, 0, -0.16, 0, 0);
    CHECK(!real_frame(&f, previous, 2, 0, &out), "depth discontinuity sequence warms history");
    CHECK(real_frame(&f, camera, 2, 0, &out), "invalid depth frame generates conservative fallback output");
    if (out.image) {
        read_image(out.image, VK_IMAGE_ASPECT_COLOR_BIT, pixels, PIXELS*4);
        check_pixel(pixels, 55, 45, f.final_pixels+((size_t)45*W+55)*4, 1, "invalid depth holds real pixel");
        check_pixel(pixels, 159, 100, f.final_pixels+((size_t)100*W+159)*4, 1, "depth discontinuity/disocclusion holds real pixel");
    }
    fixture_pixels(&f,0); fixture_upload(&f); vk_fg_reset(); fg_test_camera(previous,0,-0.16,0,0);
    float translucent[8]={0,0,0,0,1,0,0,0.5f};
    memcpy(host_map(&f.ui_uniform),translucent,sizeof translucent); vkUnmapMemory(V.dev,f.ui_uniform.memory);
    f.ui_repeats=1;
    CHECK(!real_frame(&f,previous,0,0,&out),"translucent UI sequence warms camera history");
    CHECK(real_frame(&f,camera,0,0,&out),"translucent UI is redrawn over generated scene");
    if (out.image) {
        read_image(out.image,VK_IMAGE_ASPECT_COLOR_BIT,pixels,PIXELS*4);
        uint8_t expected[4]; expected_gradient(&f,230.5/W+0.01125,40.5/H,expected);
        for(unsigned c=0;c<3;c++) expected[c]=encoded_channel(0.5f*fg_test_decode(expected[c]/255.0f)+(c==0?0.5f:0));
        expected[3]=214;
        check_pixel(pixels,230,40,expected,2,"alpha UI blends against warped background, not a held real-frame patch");
    }
    translucent[5]=translucent[6]=translucent[7]=1;
    memcpy(host_map(&f.ui_uniform),translucent,sizeof translucent); vkUnmapMemory(V.dev,f.ui_uniform.memory);
    f.ui_repeats=128;
    check_sources_unchanged(&f);
    vk_fg_reset(); fg_test_camera(camera, 0, 100, 0, 0);
    CHECK(!real_frame(&f, camera, 0, 0, &out), "history reset suppresses generation");
    fg_test_camera(camera, 0, 120, 0, 0);
    CHECK(!real_frame(&f, camera, 0, 0, &out), "teleport camera cut suppresses generation");
    /* Exercise the bounded source/output rings and the 300-real-frame report,
     * not just one output's timestamp query. */
    fixture_pixels(&f,0); fixture_upload(&f); vk_fg_reset(); fg_test_camera(camera,0,0,0,0);
    CHECK(!real_frame(&f,camera,0,0,&out), "statistics sequence warms history");
    for (unsigned frame=0; frame<300; frame++)
        CHECK(real_frame(&f,camera,frame%4,frame&1,&out), "steady source/output reuse frame %u",frame);
    vk_gpu_idle(); mtl_tex_release(&f.scene_desc); mtl_tex_release(&f.depth_desc); mtl_tex_release(&f.final_desc);
    uint64_t passes[4]; vk_fg_test_pass_counts(passes);
    printf("framegen pass census: real=2/frame (off and on), 128 UI draws; private warp/UI/transfer/output=%llu/%llu/%llu/%llu (one each per generated frame)\n",
        (unsigned long long)passes[0],(unsigned long long)passes[1],(unsigned long long)passes[2],(unsigned long long)passes[3]);
    fixture_replay_destroy(&f);
    free(f.pool.mem); free(f.scene_pixels); free(f.depth_pixels); free(f.depth_baseline); free(f.final_pixels); free(pixels);
    return failures ? 1 : 0;
}
/* Distinct linear scenes make averaging two retained captures observably
   different from holding either real frame or extrapolating the newest one. */
static void interp_scene_base(unsigned frame, double x, double y, float rgb[3]) {
    unsigned phase=frame%4;
    double u=x/(W-1), v=y/(H-1);
    rgb[0]=(float)(0.05+0.1*phase+0.5*(phase==1 ? 1-u : u));
    rgb[1]=(float)(0.08+0.06*phase+0.2*v);
    rgb[2]=(float)(0.12+0.05*phase+0.15*u);
    if (DW!=W && y>=H-8)
        rgb[1]=(float)(0.05+0.03*phase+0.6*(y-(H-8))/7); /* bottom-row drift sentinel */
}
static void interp_native_pixel(unsigned frame,unsigned x,unsigned y,float rgb[3]) {
    double cx=(x+0.5)*W/DW-0.5,cy=(y+0.5)*H/DH-0.5;
    int x0=(int)floor(cx),y0=(int)floor(cy); double fx=cx-x0,fy=cy-y0;
    for (unsigned c=0;c<3;c++) rgb[c]=0;
    for (int j=0;j<2;j++) for (int i=0;i<2;i++) {
        float sample[3];
        interp_scene_base(frame,fmax(0,fmin(W-1,x0+i)),fmax(0,fmin(H-1,y0+j)),sample);
        for (unsigned c=0;c<3;c++) rgb[c]+=(float)(sample[c]*(i ? fx : 1-fx)*(j ? fy : 1-fy));
    }
}
static void interp_scene(unsigned frame,double x,double y,float rgb[3]) {
    if (DW==W && DH==H) { interp_scene_base(frame,x,y,rgb); return; }
    /* Independent CPU golden for native texel-center -> stretched canvas
       sampling, including clamped last source row. No isotropic shortcut. */
    double nx=(x+0.5)*DW/W-0.5,ny=(y+0.5)*DH/H-0.5;
    int x0=(int)floor(nx),y0=(int)floor(ny); double fx=nx-x0,fy=ny-y0;
    for (unsigned c=0;c<3;c++) rgb[c]=0;
    for (int j=0;j<2;j++) for (int i=0;i<2;i++) {
        float sample[3];
        interp_native_pixel(frame,(unsigned)fmax(0,fmin(DW-1,x0+i)),(unsigned)fmax(0,fmin(DH-1,y0+j)),sample);
        for (unsigned c=0;c<3;c++) rgb[c]+=(float)(sample[c]*(i ? fx : 1-fx)*(j ? fy : 1-fy));
    }
}
static void fixture_depth_distance(fixture *f, double near_plane, double far_plane, double distance) {
    double depth=far_plane/(far_plane-near_plane)-
        far_plane*near_plane/((far_plane-near_plane)*distance);
    for (size_t i=0;i<PIXELS;i++) f->depth_pixels[i]=(float)depth;
    fixture_resample_depth(f);
}
static void interp_camera_clip(float camera[84], double near_plane, double far_plane) {
    double projection[16]={0};
    projection[0]=(double)FG_TEST_H/FG_TEST_W; projection[5]=1;
    projection[10]=-(far_plane+near_plane)/(far_plane-near_plane);
    projection[11]=-2*far_plane*near_plane/(far_plane-near_plane);
    projection[14]=-1;
    double view[16]={0};
    for (unsigned i=0;i<12;i++) view[i]=camera[i];
    view[15]=1;
    double vp[16]; fg_math_mul(projection,view,vp);
    for (unsigned i=0;i<16;i++) { camera[28+i]=(float)projection[i]; camera[12+i]=(float)vp[i]; }
    camera[56]=(float)near_plane; camera[57]=(float)far_plane;
}
static void interp_camera_focal(float camera[84], double near_plane, double far_plane,
                                double focal) {
    double projection[16]={0},view[16],vp[16];
    projection[0]=(double)FG_TEST_H/W*focal; projection[5]=focal;
    projection[10]=-(far_plane+near_plane)/(far_plane-near_plane);
    projection[11]=-2*far_plane*near_plane/(far_plane-near_plane);
    projection[14]=-1;
    for (unsigned i=0;i<12;i++) view[i]=camera[i];
    view[12]=view[13]=view[14]=0; view[15]=1;
    fg_math_mul(projection,view,vp);
    for (unsigned i=0;i<16;i++) { camera[28+i]=(float)projection[i]; camera[12+i]=(float)vp[i]; }
    camera[56]=(float)near_plane; camera[57]=(float)far_plane;
    camera[70]=(float)(2*atan(1/focal));
}

static void fixture_interp_pixels(fixture *f, unsigned frame) {
    for (unsigned y=0;y<H;y++) for (unsigned x=0;x<W;x++) {
        size_t i=(size_t)y*W+x;
        interp_scene_base(frame,x,y,f->scene_pixels+4*i);
        f->scene_pixels[4*i+3]=173.0f/255.0f;
        for (unsigned c=0;c<3;c++) f->final_pixels[4*i+c]=encoded_channel(f->scene_pixels[4*i+c]);
        f->final_pixels[4*i+3]=173;
        float depth=fg_test_depth(2);
        if (y>=80 && y<140) {
            if (x>=100 && x<130 && (frame&1)) depth=fg_test_depth(4);
            if (x>=140 && x<170 && !(frame&1)) depth=fg_test_depth(4);
            if (x>=180 && x<200 && !(frame&1)) depth=NAN;
            if (x>=210 && x<230 && (frame&1)) depth=NAN;
            if (x>=240 && x<260) depth=NAN;
        }
        f->depth_pixels[i]=depth;
    }
    fixture_resample_depth(f);
}
static void fixture_interp_ui(fixture *f, const float rgba[4]) {
    float uniform[8]={0};
    memcpy(uniform+4,rgba,4*sizeof(float));
    memcpy(f->guest_uniform,uniform,sizeof uniform);
    if (f->ui_uniform_address) memcpy((void *)(uintptr_t)f->ui_uniform_address,uniform,sizeof uniform);
    else {
        memcpy(host_map(&f->ui_uniform),uniform,sizeof uniform);
        vkUnmapMemory(V.dev,f->ui_uniform.memory);
    }
}
static void interp_expected_scene(unsigned first, unsigned second, double x, double y,
                                  double first_dx, double second_dx, unsigned sources, float rgb[3]) {
    float a[3], b[3];
    interp_scene(first,x+first_dx,y,a); interp_scene(second,x+second_dx,y,b);
    for (unsigned c=0;c<3;c++) rgb[c]=sources==3 ? (a[c]+b[c])*0.5f : sources==1 ? a[c] : b[c];
}
static void check_interp_linear(const uint8_t *pixels, unsigned x, unsigned y, const float rgb[3], const char *label) {
    uint8_t expected[4]={encoded_channel(rgb[0]),encoded_channel(rgb[1]),encoded_channel(rgb[2]),255};
    check_pixel(pixels,x,y,expected,1,label);
}
static void check_interp_scene(const uint8_t *pixels, unsigned first, unsigned second,
                                unsigned x, unsigned y, unsigned sources, const char *label) {
    float rgb[3]; interp_expected_scene(first,second,x,y,0,0,sources,rgb);
    check_interp_linear(pixels,x,y,rgb,label);
}
static void check_interp_ui(const uint8_t *pixels, unsigned first, unsigned second,
                            const float rgba[4], int affine, const char *label) {
    float rgb[3]; interp_expected_scene(first,second,230,40,0,0,3,rgb);
    for (unsigned c=0;c<3;c++)
        rgb[c]=affine ? rgb[c]*(1-rgba[c])+rgba[c] : rgb[c]*(1-rgba[3])+rgba[c]*rgba[3];
    check_interp_linear(pixels,230,40,rgb,label);
}
/* Exercise production post startup and canvas ownership, unlike the shader
   goldens below, which intentionally inject capture/replay inputs directly.
   Model a low-res scene -> larger composition canvas -> padded guest window;
   these are synthetic sizes, not a claim that the device log identified all
   three extents. Native UI and host post are off just as on Thor. */
static void device_canvas_boundary(void) {
    CHECK(vk_canvas_on && !vk_native_ui_enabled(),"interp alone enables canvas discovery with native UI and host post off");
    pool_t pool={.size=32u<<20,.flags=NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS|NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    pool.mem=aligned_alloc(4096,pool.size);
    CHECK(pool.mem!=NULL,"allocate device-structure synthetic storage");
    if (!pool.mem) exit(1);
    tex_t desc[3]; vk_canvas_image image[3];
    const unsigned width[3]={960,1280,1920},height[3]={540,720,1080};
    mtl_sub_begin();
    for (unsigned i=0;i<3;i++) {
        texture_desc(&desc[i],&pool,i*8u*1024*1024,0x25,81001+i,width[i],height[i]);
        if (i==2) mtl_xv_window_tex(desc[i].serial);
        vk_tex *x=vk_tex_get(&desc[i]);
        CHECK(x!=NULL,"create device-structure image %u",i);
        CHECK(vk_tex_canvas_window(x)==(i==2),"only registered terminal image %u has window authority",i);
        image[i]=vk_tex_canvas_identity(x,0,0);
    }
    uint64_t ticket=mtl_sub_commit(); CHECK(mtl_wait(ticket,10000000000ull),"device-structure image initialization completes");
    CHECK(!vk_canvas_first_ui(&image[0],"7f2113e2"),"scene dimensions or UI shader alone cannot nominate an unknown canvas");
    CHECK(!vk_canvas_first_ui(&image[1],"7f2113e2"),"first frame fails closed until a terminal transfer establishes canvas identity");
    vk_canvas_transfer(&image[1],&image[2],1); /* first frame learns actual terminal source */
    CHECK(vk_canvas_present(&image[2]),"emitted terminal transfer reaches the exact registered window");
    vk_canvas_frame_end();
    CHECK(!vk_canvas_first_ui(&image[1],"unknown"),"unknown shader cannot nominate the learned canvas");
    CHECK(vk_canvas_first_ui(&image[1],"7f2113e2"),"next frame captures the learned canvas BEFORE its first UI draw");
    vk_canvas_ui_commit(&image[1]);
    CHECK(vk_canvas_current()->ui_committed,"emitted UI commits the pre-UI nomination");
    vk_canvas_transfer(&image[1],&image[2],0);
    CHECK(!vk_canvas_present(&image[2]),"prepared but unemitted transfer cannot authorize presentation");
    vk_canvas_transfer(&image[1],&image[2],1);
    CHECK(!vk_canvas_present(&image[0]),"different presented image rejects the chain");
    CHECK(vk_canvas_present(&image[2]),"scene/composition/UI/transfer/present chain survives native-UI-off startup");
    CHECK(vk_canvas_current()->canvas.width==1280 && vk_canvas_current()->window.width==1920,
          "composition and padded window retain distinct identities and sizes");
    vk_canvas_frame_end();
    vk_canvas_image recycled=image[1]; recycled.incarnation++;
    CHECK(!vk_canvas_first_ui(&recycled,"7f2113e2"),"recycled image handle cannot inherit an old canvas boundary");
    vk_gpu_idle();
    for (unsigned i=0;i<3;i++) mtl_tex_release(&desc[i]);
    free(pool.mem);
}

static int run_interp_depth_range(void) {
    setenv("TOTK_FRAMEGEN","interp",1);
    setenv("TOTK_FRAMEGEN_BUDGET_MS","10000",1);
    setenv("TOTK_POST_AA","off",1);
    mtl_init();
    CHECK(vk_fg_enabled() && vk_fg_interpolating(),"depth-range fixture selects production interpolation");
    fixture f; fixture_create(&f); f.ui_repeats=1;
    const float white[4]={1,1,1,1};
    float camera[84]; fg_test_camera(camera,0,0,0,0);
    vk_post_output out={0}; uint8_t *pixels=malloc(PIXELS*4);
    CHECK(pixels!=NULL,"allocate depth-range interpolation output"); if (!pixels) exit(1);
    fixture_interp_pixels(&f,0); fixture_depth_distance(&f,1.0,25000.0,2.0);
    fixture_upload(&f); fixture_interp_ui(&f,white); interp_camera_clip(camera,1.0,25000.0);
    CHECK(!real_frame(&f,camera,0,0,&out),"first far-range source warms production history");
    fixture_interp_pixels(&f,1); fixture_depth_distance(&f,0.1,5000.0,2.0);
    fixture_upload(&f); fixture_interp_ui(&f,white); interp_camera_clip(camera,0.1,5000.0);
    CHECK(real_frame(&f,camera,1,0,&out),"adjacent source with substantial near/far drift produces a real Vulkan midpoint");
    CHECK(f.completed && out.image,"changed depth-range pair completes and exposes generated pixels");
    if (out.image) {
        read_image(out.image,VK_IMAGE_ASPECT_COLOR_BIT,pixels,PIXELS*4);
        check_interp_scene(pixels,0,1,80,60,3,"depth-range midpoint preserves exact static-view spatial pixel golden");
        check_interp_scene(pixels,0,1,250,120,3,"depth-range midpoint remains spatially correct across the full view");
    }
    /* A two-sided zoom pair must sample each source at the world point
       projected by the coefficient-wise midpoint, not crossfade unwarped
       pixels. The CPU target below uses the known analytic focal ratios. */
    vk_fg_reset(); fg_test_camera(camera,0,0,0,0);
    fixture_interp_pixels(&f,0); fixture_depth_distance(&f,0.1,5000,2);
    fixture_upload(&f); fixture_interp_ui(&f,white);
    interp_camera_focal(camera,0.1,5000,1.02);
    CHECK(!real_frame(&f,camera,0,0,&out),"first FOV source warms production history");
    fixture_interp_pixels(&f,1); fixture_depth_distance(&f,0.1,5000,2);
    fixture_upload(&f); fixture_interp_ui(&f,white);
    interp_camera_focal(camera,0.1,5000,0.98);
    CHECK(real_frame(&f,camera,1,0,&out) && f.completed,
          "adjacent FOV-changing source pair generates a real Vulkan midpoint");
    if (out.image) {
        read_image(out.image,VK_IMAGE_ASPECT_COLOR_BIT,pixels,PIXELS*4);
        for (unsigned y=28;y<78;y+=20) for (unsigned x=40;x<280;x+=40) {
            float a[3],b[3],rgb[3];
            const double midx=(W-1)*0.5,midy=(H-1)*0.5;
            const double ax=midx+(x-midx)*1.02, ay=midy+(y-midy)*1.02;
            const double bx=midx+(x-midx)*0.98, by=midy+(y-midy)*0.98;
            interp_scene(0,ax,ay,a); interp_scene(1,bx,by,b);
            for(unsigned c=0;c<3;c++) rgb[c]=(a[c]+b[c])*0.5f;
            if (x>=220 && x<260 && y>=30 && y<60)
                for (unsigned c=0;c<3;c++) rgb[c]=rgb[c]*(1-white[3])+white[c]*white[3];
            check_interp_linear(pixels,x,y,rgb,"FOV midpoint matches independent focal-ratio pixel golden");
        }
        float edge[3];
        interp_scene(1,(W-1)*0.5+ (0-(W-1)*0.5)*0.98,
                     (H-1)*0.5+(90-(H-1)*0.5)*0.98,edge);
        check_interp_linear(pixels,0,90,edge,
            "one-sided left FOV edge uses the only in-bounds source");
        interp_scene(1,(W-1)*0.5+ (W-1-(W-1)*0.5)*0.98,
                     (H-1)*0.5+(90-(H-1)*0.5)*0.98,edge);
        check_interp_linear(pixels,W-1,90,edge,
            "one-sided right FOV edge uses the only in-bounds source");
        check_interp_ui(pixels,0,1,white,0,"FOV midpoint keeps the screen-space HUD unwarped");
    }
    /* Changed-depth-range + translated cameras must reproject, not merely
       crossfade the unwarped inputs. An interior grid catches holes/ghosts
       against the existing one-code-value pixel tolerance. */
    vk_fg_reset(); fixture_interp_pixels(&f,0); fixture_depth_distance(&f,1,25000,2);
    fixture_upload(&f); fixture_interp_ui(&f,white);
    fg_test_camera(camera,0,-0.16,0,0); interp_camera_clip(camera,1,25000);
    CHECK(!real_frame(&f,camera,0,0,&out),"translated changed-range pair warms its first source");
    fixture_interp_pixels(&f,1); fixture_depth_distance(&f,0.1,5000,2);
    fixture_upload(&f); fixture_interp_ui(&f,white);
    fg_test_camera(camera,0,0.16,0,0); interp_camera_clip(camera,0.1,5000);
    CHECK(real_frame(&f,camera,1,0,&out) && f.completed,
          "translated changed-range pair completes a real Vulkan midpoint");
    if (out.image) {
        read_image(out.image,VK_IMAGE_ASPECT_COLOR_BIT,pixels,PIXELS*4);
        for (unsigned y=64;y<160;y+=4) for (unsigned x=32;x<288;x+=4) {
            float rgb[3]; interp_expected_scene(0,1,x,y,0.0225*W,-0.0225*W,3,rgb);
            check_interp_linear(pixels,x,y,rgb,"changed-range translated grid matches analytic midpoint without holes");
        }
        check_interp_ui(pixels,0,1,white,0,"changed-range translated midpoint preserves unwarped HUD");
    }
    vk_fg_reset(); fg_test_camera(camera,0,0,0,0); interp_camera_clip(camera,1,25000);
    fixture_interp_pixels(&f,2); fixture_depth_distance(&f,1,25000,2); fixture_upload(&f);
    CHECK(!real_frame(&f,camera,2,0,&out),"lateral-projection negative warms its source");
    fg_test_camera(camera,0,0,0,0); interp_camera_clip(camera,0.1,5000);
    camera[36]=0.01f; camera[20]=0.01f;
    CHECK(fg_math_camera(camera,W,H)==FG_MATH_OK,"lateral negative is individually valid, not malformed input");
    fixture_interp_pixels(&f,3); fixture_depth_distance(&f,0.1,5000,2); fixture_upload(&f);
    CHECK(!real_frame(&f,camera,3,0,&out),"changed depth range does not permit a lateral projection shift");
    vk_fg_reset(); fg_test_camera(camera,0,0,0,0); interp_camera_clip(camera,1,25000);
    fixture_interp_pixels(&f,4); fixture_depth_distance(&f,1,25000,2); fixture_upload(&f);
    CHECK(!real_frame(&f,camera,0,0,&out),"FOV negative warms its source");
    fg_test_camera(camera,0,0,0,0); interp_camera_clip(camera,0.1,5000);
    camera[28]*=1.1f; camera[12]*=1.1f;
    camera[33]*=1.1f; camera[17]*=1.1f; camera[70]=(float)(2*atan(1/1.1));
    CHECK(fg_math_camera(camera,W,H)==FG_MATH_OK,"FOV negative is individually valid, not malformed input");
    fixture_interp_pixels(&f,5); fixture_depth_distance(&f,0.1,5000,2); fixture_upload(&f);
    CHECK(!real_frame(&f,camera,1,0,&out),"changed depth range does not permit a FOV jump beyond three degrees");
    vk_fg_reset(); fg_test_camera(camera,0,0,0,0); interp_camera_clip(camera,1,25000);
    fixture_interp_pixels(&f,6); fixture_depth_distance(&f,1,25000,2); fixture_upload(&f);
    CHECK(!real_frame(&f,camera,0,0,&out),"camera-cut negative warms its source");
    fg_test_camera(camera,0,20,0,0); interp_camera_clip(camera,0.1,5000);
    fixture_interp_pixels(&f,7); fixture_depth_distance(&f,0.1,5000,2); fixture_upload(&f);
    CHECK(!real_frame(&f,camera,1,0,&out),"depth-range drift does not permit a camera cut");
    vk_gpu_idle(); mtl_tex_release(&f.scene_desc); mtl_tex_release(&f.depth_desc); mtl_tex_release(&f.final_desc);
    fixture_replay_destroy(&f); free(f.pool.mem); free(f.scene_pixels); free(f.depth_pixels);
    free(f.depth_baseline); free(f.final_pixels); free(pixels);
    return failures ? 1 : 0;
}

static int run_interp(int device) {
    if (device>=2) { W=1280; H=720; DW=960; DH=536; }
    setenv("TOTK_FRAMEGEN",device<0?"hybrid":"interp",1); setenv("TOTK_FRAMEGEN_BUDGET_MS","10000",1);
    if (device<0) {
        setenv("TOTK_FPS60","1",1); setenv("TOTK_FPS_AUTO","0",1);
        setenv("TOTK_ACQUIRE_SLACK","0",1);
    }
    if (device) {
        setenv("TOTK_NATIVE_UI","0",1); setenv("TOTK_POST_AA","off",1); setenv("TOTK_POST_SCALE","off",1);
        setenv("TOTK_RENDER_RES","960x540",1); setenv("TOTK_OUTPUT_RES","0",1); setenv("TOTK_VK_PRESENT","ahb",1);
    }
    mtl_init(); CHECK(vk_fg_enabled() && vk_fg_interpolating(),"explicit interp opt-in selects delayed two-real-frame mode");
    uint64_t hybrid_now=1000000000ull;
    if (device<0) {
        CHECK(runtime_fps_hybrid_selected() && !vk_fg_capturing(),
            "hybrid starts native60 with extraction off, not per-frame interval2 interpolation");
        for (unsigned i=0;i<1000;i++) {
            hybrid_now+=25000000;
            runtime_fps_hybrid_acquire(hybrid_now,16666667,1,1,1.3,1.3);
            runtime_fps_hybrid_present(hybrid_now+1,4000,0);
        }
        CHECK(runtime_fps_hybrid_interpolating() && vk_fg_capturing(),
            "sustained real40 enters real30 interpolation after scene settle and two wall-time windows");
        vk_fg_reset();
    }
    if (device>=2) partial_clear_create();
    partial_clear.continuations=device==3;
    if (device) device_canvas_boundary();
    fixture f; fixture_create(&f); f.ui_repeats=1;
    VkPipeline standard_ui=f.ui_pipeline;
    VkPipeline affine_ui=replay_pipeline(f.ui_layout,VK_FORMAT_R32G32B32A32_SFLOAT,2);
    float camera[84]; fg_test_camera(camera,0,0,0,0);
    mtl_sub_begin();
    float context_camera[84]; memcpy(context_camera,camera,sizeof context_camera);
    context_camera[72]=DW; context_camera[73]=DH;
    unsigned context_states=vk_fg_test_depth_contexts(f.native ? &f.native_desc : &f.scene_desc,
        &f.depth_desc,context_camera);
    CHECK(context_states&1,"depth-off to depth-on context recognizes the live scene camera immediately");
    CHECK(context_states&2,"depth-on to depth-off context cannot inherit the previous camera admission");
    vk_fg_extract_end();
    uint64_t context_ticket=mtl_sub_commit();
    vk_fg_frame_end(context_ticket);
    CHECK(mtl_wait(context_ticket,10000000000ull),"depth-state context regression completes");
    vk_fg_reset();
    CHECK(!vk_fg_raster_depth_shader("ecbe4aa4417b3f806ef471833eac0165958c3416642d8a23987c1376dc2681ba-ef49c76e8bd9"),
        "world-plane export must never become an unconditional raster/sentinel whitelist");
    if (device==3) for (unsigned negative=1;negative<=20;negative++) {
        mtl_sub_begin();
        int selected=vk_fg_test_scene_continuations(&f.native_desc,&f.depth_desc,&f.scene_desc,context_camera,negative);
        CHECK(negative==6 || negative==7 || negative==11 || negative==18 ? selected==1 : !selected,
            "continuation case%u: runtime-proven plane spans select; unsupported mixed depth contexts remain fail-closed",negative);
        vk_fg_extract_end(); uint64_t ticket=mtl_sub_commit(); vk_fg_frame_end(ticket);
        CHECK(mtl_wait(ticket,10000000000ull),"negative continuation transaction retires");
    }
    const float white[4]={1,1,1,1}, translucent[4]={0.1f,0.6f,0.9f,0.5f};
    const float affine[4]={0.25f,0.5f,0.75f,0}, green[4]={0,1,0,1}, blue[4]={0,0,1,1};
    vk_post_output out; uint8_t *pixels=malloc(PIXELS*4);
    CHECK(pixels!=NULL,"allocate interpolation output pixels"); if (!pixels) exit(1);
    fixture_interp_pixels(&f,0); fixture_upload(&f); fixture_interp_ui(&f,white);
    CHECK(!real_frame(&f,camera,0,0,&out),"first interpolation real frame captures scene/depth and both UI layers without output");
    fixture_interp_pixels(&f,1); fixture_upload(&f); fixture_interp_ui(&f,translucent);
    CHECK(real_frame(&f,camera,3,1,&out),"adjacent real N+1 composes the first midpoint in fourth presenter slot");
    CHECK(f.completed,"generous-budget interpolation pair is accepted at GPU completion");
    CHECK(out.w==W && out.h==H && out.format==VK_FORMAT_R8G8B8A8_SRGB && out.view,
          "interpolation exposes encoded output with the requested sRGB display view");
    if (out.image) {
        read_image(out.image,VK_IMAGE_ASPECT_COLOR_BIT,pixels,PIXELS*4);
        if (device>=2) {
            for (unsigned y=H-8;y<H;y++)
                check_interp_scene(pixels,0,1,W-100,y,y<H-2 ? 3 : 1,
                    "nonuniform 960x536 -> 1280x720 mapping has no half-pixel/bottom-row drift");
            check_interp_scene(pixels,0,1,W-1,H-1,1,"nonuniform bottom-right invalid-depth border retains the exact older canvas pixel");
        }
        check_interp_scene(pixels,0,1,80,100,3,"static camera averages two distinct linear scenes before encoding");
        check_interp_scene(pixels,0,1,115,100,1,"two retained depth captures select the nearer earlier surface");
        check_interp_scene(pixels,0,1,155,100,2,"two retained depth captures select the nearer later surface");
        check_interp_scene(pixels,0,1,190,100,2,"invalid earlier depth uses the valid later scene");
        check_interp_scene(pixels,0,1,220,100,1,"invalid later depth uses the valid earlier scene");
        check_interp_scene(pixels,0,1,250,100,1,"two invalid depths conservatively hold the earlier scene");
        uint8_t opaque[4]={255,255,255,255};
        unsigned changed=0;
        for (unsigned y=30;y<60;y++) for (unsigned x=220;x<260;x++)
            changed+=memcmp(pixels+((size_t)y*W+x)*4,opaque,4)!=0;
        CHECK(!changed,"every earlier-N opaque HUD pixel stays crisp instead of using the changed N+1 UI");
        const uint8_t cleared[4]={0,255,0,255};
        check_pixel(pixels,12,12,cleared,0,"ordered partial UI clear survives black/white capture");
        check_interp_scene(pixels,0,1,219,40,3,"midpoint background immediately beside held HUD has no frozen-scene halo");
        CHECK(f.guest_vertices[0]==1000 && f.guest_uniform[4]==0,
              "both UI capture passes consume frozen draws, not mutated guest memory");
    }
    check_sources_unchanged(&f);
    /* The next pair must use N+1's just-captured translucent UI, not N's HUD
       and not the new draw's alpha-independent RGB blend. */
    fixture_interp_pixels(&f,2); fixture_upload(&f); fixture_interp_ui(&f,affine); f.ui_pipeline=affine_ui;
    CHECK(real_frame(&f,camera,0,0,&out),"third consecutive real frame advances interpolation history without another warmup");
    CHECK(f.completed && out.format==VK_FORMAT_R8G8B8A8_UNORM,"next pair completes with encoded UNORM semantics");
    if (out.image) {
        read_image(out.image,VK_IMAGE_ASPECT_COLOR_BIT,pixels,PIXELS*4);
        check_interp_scene(pixels,1,2,80,100,3,"consecutive pair uses only the newest adjacent scenes");
        check_interp_ui(pixels,1,2,translucent,0,"earlier translucent UI blends over the midpoint, not a held real-frame patch");
    }
    fixture_interp_pixels(&f,3); fixture_upload(&f); fixture_interp_ui(&f,green); f.ui_pipeline=standard_ui;
    CHECK(real_frame(&f,camera,1,0,&out),"fourth real frame composes retained RGB-affine UI");
    CHECK(f.completed,"RGB-affine pair is accepted at GPU completion");
    if (out.image) {
        read_image(out.image,VK_IMAGE_ASPECT_COLOR_BIT,pixels,PIXELS*4);
        check_interp_ui(pixels,2,3,affine,1,"black/white capture recovers per-channel RGB transmittance despite zero source alpha");
        check_interp_scene(pixels,2,3,219,40,3,"RGB-affine UI does not contaminate its neighboring background");
    }
    /* A cut rejects just this pair. The cut's current real frame and UI must
       become history so its immediately adjacent successor can recover. */
    fixture_interp_pixels(&f,4); fixture_upload(&f); fixture_interp_ui(&f,blue); fg_test_camera(camera,0,20,0,0);
    CHECK(!real_frame(&f,camera,2,0,&out),"camera-cut pair skips output while retaining current real scene/depth/UI");
    CHECK(vk_fg_enabled() && vk_fg_interpolating(),"invalid camera pair does not suspend interpolation mode");
    fixture_interp_pixels(&f,5); fixture_upload(&f); fixture_interp_ui(&f,green);
    CHECK(real_frame(&f,camera,2,0,&out),"the next adjacent valid pair recovers immediately after a camera cut");
    CHECK(f.completed,"recovered pair meets the normal completion contract");
    if (out.image) {
        read_image(out.image,VK_IMAGE_ASPECT_COLOR_BIT,pixels,PIXELS*4);
        check_interp_scene(pixels,4,5,80,100,3,"recovery uses the cut's real frame, never a stale pre-cut scene");
        check_interp_ui(pixels,4,5,blue,0,"recovery retains the rejected pair's current UI for its next adjacent pair");
    }
    check_sources_unchanged(&f);
    /* Analytic translated midpoint: with z=2 and camera x=-.16,+.16, source
       samples lie +/- .0225 normalized UV from the target midpoint. Opposite
       red gradients ensure this cannot pass by static scene crossfade. */
    vk_fg_reset(); fixture_interp_pixels(&f,0); fixture_upload(&f); fixture_interp_ui(&f,white);
    fg_test_camera(camera,0,-0.16,0,0);
    CHECK(!real_frame(&f,camera,0,0,&out),"explicit reset discards interpolation history but still captures warmup UI");
    fixture_interp_pixels(&f,1); fixture_upload(&f); fixture_interp_ui(&f,green); fg_test_camera(camera,0,0.16,0,0);
    CHECK(real_frame(&f,camera,3,0,&out),"translated adjacent cameras generate their midpoint rather than an extrapolated future view");
    CHECK(f.completed,"translated midpoint pair completes within generous budget");
    if (out.image) {
        read_image(out.image,VK_IMAGE_ASPECT_COLOR_BIT,pixels,PIXELS*4);
        float rgb[3]; interp_expected_scene(0,1,100,60,0.0225*W,-0.0225*W,3,rgb);
        check_interp_linear(pixels,100,60,rgb,"both depth-reprojected scenes match the analytic camera midpoint");
        check_interp_ui(pixels,0,1,white,0,"earlier screen-space HUD stays crisp during camera interpolation");
    }
    if (device>=2) partial_clear_pixels();
    uint64_t passes[4]; vk_fg_test_pass_counts(passes);
    CHECK(f.real_passes==16 && passes[0]==5 && passes[1]==16 && passes[2]==5 && passes[3]==5,
          "eight interpolation real frames emit sixteen UI captures and only five valid midpoint outputs");
    printf("framegen interp pass census: real=2/frame; UI captures=2/frame; compose/transfer/output=%llu/%llu/%llu\n",
        (unsigned long long)passes[0],(unsigned long long)passes[2],(unsigned long long)passes[3]);
    if (device>=2) {
        partial_clear.negative=1;
        CHECK(!real_frame(&f,camera,0,0,&out),"unknown writer after accepted partial-clear frames generates no output");
        CHECK(vk_fg_enabled(),"unknown source refusal does not disable interpolation");
        partial_clear.negative=2;
        CHECK(!real_frame(&f,camera,1,0,&out),"unmodelled cropped scene-to-canvas copy generates no midpoint");
        CHECK(vk_fg_enabled(),"unsupported mapping refuses only the affected interpolation pair");
        partial_clear_destroy();
    }
    if (device<0) {
        for (unsigned i=0;i<600;i++) {
            hybrid_now+=runtime_fps_hybrid_interpolating()?33333333:16666667;
            runtime_fps_hybrid_acquire(hybrid_now,16666667,1,1,.5,.5);
            runtime_fps_hybrid_present(hybrid_now+1,4000,0);
        }
        CHECK(!runtime_fps_hybrid_interpolating() && !vk_fg_capturing() &&
              vk_fg_enabled() && vk_fg_interpolating(),
            "native return disables new extraction but preserves static interpolation completion semantics");
        vk_fg_history_cut();
    }
    vk_gpu_idle(); mtl_tex_release(&f.scene_desc); mtl_tex_release(&f.depth_desc); mtl_tex_release(&f.final_desc);
    if (f.native) { mtl_tex_release(&f.native_desc); free(f.native_pixels); }
    fixture_replay_destroy(&f); vkDestroyPipeline(V.dev,affine_ui,NULL);
    free(f.pool.mem); free(f.scene_pixels); free(f.depth_pixels); free(f.depth_baseline); free(f.final_pixels); free(pixels);
    return failures ? 1 : 0;
}
/* Horse riding (2026-11-16, Thor recording): Link and the horse move WITH the
   camera, so both real frames show them at the same pixel while the camera
   translates. Camera-only warps move such a surface by half the camera motion in
   opposite directions and the blend drew two translucent copies on every
   generated frame. A near striped "rider" (z=1.5) is identical at the same screen
   position in both frames; the background keeps the per-frame phase colours of
   the translated-midpoint golden, so its expected camera blend is unchanged. */
#define HOLD_X0 20u
#define HOLD_X1 90u
#define HOLD_Y0 95u
#define HOLD_Y1 170u
static void interp_hold_rider(unsigned x, float rgb[3]) {
    int odd=(x/4)&1;
    rgb[0]=odd ? 0.80f : 0.10f; rgb[1]=odd ? 0.20f : 0.60f; rgb[2]=odd ? 0.10f : 0.80f;
}
static void fixture_interp_hold_pixels(fixture *f, unsigned frame) {
    fixture_interp_pixels(f,frame);
    for (unsigned y=HOLD_Y0;y<HOLD_Y1;y++) for (unsigned x=HOLD_X0;x<HOLD_X1;x++) {
        size_t i=(size_t)y*W+x;
        interp_hold_rider(x,f->scene_pixels+4*i);
        for (unsigned c=0;c<3;c++) f->final_pixels[4*i+c]=encoded_channel(f->scene_pixels[4*i+c]);
        f->depth_pixels[i]=fg_test_depth(1.5);
    }
    fixture_resample_depth(f);
}
static int run_interp_hold(int off) {
    setenv("TOTK_FRAMEGEN","interp",1); setenv("TOTK_FRAMEGEN_BUDGET_MS","10000",1);
    if (off) setenv("TOTK_FG_STATIC_HOLD","0",1); else unsetenv("TOTK_FG_STATIC_HOLD");
    mtl_init();
    CHECK(vk_fg_interpolating(),"screen-static hold uses production interpolation");
    CHECK(vk_fg_static_hold()==!off,"TOTK_FG_STATIC_HOLD %s selects hold %s",off ? "=0" : "unset",off ? "off" : "on");
    fixture f; fixture_create(&f); f.ui_repeats=1;
    const float white[4]={1,1,1,1};
    vk_post_output out; uint8_t *pixels=malloc(PIXELS*4);
    CHECK(pixels!=NULL,"allocate hold output pixels"); if (!pixels) exit(1);
    float camera[84];
    fixture_interp_hold_pixels(&f,0); fixture_upload(&f); fixture_interp_ui(&f,white);
    fg_test_camera(camera,0,-0.16,0,0);
    CHECK(!real_frame(&f,camera,0,0,&out),"first real frame of the translated pair captures history");
    fixture_interp_hold_pixels(&f,1); fixture_upload(&f); fixture_interp_ui(&f,white);
    fg_test_camera(camera,0,0.16,0,0);
    CHECK(real_frame(&f,camera,3,0,&out),"translated pair with a camera-locked rider generates a midpoint");
    CHECK(f.completed && out.image,"hold pair completes");
    if (out.image) {
        read_image(out.image,VK_IMAGE_ASPECT_COLOR_BIT,pixels,PIXELS*4);
        /* Background: the analytic camera midpoint, exactly as in run_interp. */
        float rgb[3]; interp_expected_scene(0,1,100,60,0.0225*W,-0.0225*W,3,rgb);
        check_interp_linear(pixels,100,60,rgb,"background keeps the depth-reprojected camera midpoint");
        unsigned held=0,total=0;
        /* z=1.5: each warp moves 0.03 UV (9.6 px), inside the shader 0.04 UV limit as on the
           Thor recording (~40 px of 1280); 18 px from each edge exceeds it. */
        for (unsigned y=HOLD_Y0+18;y<HOLD_Y1-18;y++) for (unsigned x=HOLD_X0+18;x<HOLD_X1-18;x++) {
            float r[3]; interp_hold_rider(x,r);
            const uint8_t *p=pixels+((size_t)y*W+x)*4;
            int ok=1;
            for (unsigned c=0;c<3;c++) if (abs((int)p[c]-(int)encoded_channel(r[c]))>1) ok=0;
            held+=ok; total++;
        }
        if (off) CHECK(held<total/2,"hold off: camera warps displace the rider (the shipped ghost; %u of %u interior pixels exact)",held,total);
        else CHECK(held==total,"hold on: every interior rider pixel is the real rider, no displaced copy (%u of %u exact)",held,total);
    }
    check_sources_unchanged(&f);
    vk_gpu_idle(); mtl_tex_release(&f.scene_desc); mtl_tex_release(&f.depth_desc); mtl_tex_release(&f.final_desc);
    fixture_replay_destroy(&f);
    free(f.pool.mem); free(f.scene_pixels); free(f.depth_pixels); free(f.depth_baseline); free(f.final_pixels); free(pixels);
    return failures ? 1 : 0;
}
/* Actual static-camera output oracle using the game's observed 1280x720
   B10G11R11 canvas and 960x536 depth extent. Compare the entire generated
   RGBA terminal to the earlier independently captured real terminal. */
static void fixture_interp_static_pixels(fixture *f) {
    fixture_pixels(f,0);
    for (unsigned y=0;y<H;y++) for (unsigned x=0;x<W;x++) {
        size_t i=(size_t)y*W+x;
        double nx=(x+0.5-W*0.5)/(W*0.5), ny=(y+0.5-H*0.5)/(H*0.5);
        float radial=(float)fmax(0.0,1.0-0.52*(nx*nx+ny*ny));
        float band=y < H/3 ? 0.18f : y < 2*H/3 ? 0.48f : 0.78f;
        f->scene_pixels[4*i]=radial*band;
        f->scene_pixels[4*i+1]=radial*(0.85f-band*0.45f);
        f->scene_pixels[4*i+2]=radial*(0.25f+band*0.5f);
        f->scene_pixels[4*i+3]=1.0f;
    }
    for (unsigned y=100;y<116;y++) for (unsigned x=120;x<136;x++)
        f->depth_pixels[(size_t)y*DW+x]=NAN;
}
static unsigned interp_static_channel_tolerance(float value, unsigned channel) {
    if (channel==3) return 1; /* one RGBA8 rounding code; alpha is not R11G11B10 */
    const int mantissa=channel==2 ? 5 : 6;
    const float min_normal=ldexpf(1.0f,-14);
    float ulp;
    if (!(value>=min_normal)) ulp=ldexpf(1.0f,-14-mantissa);
    else {
        int exponent=0;
        frexpf(value,&exponent);
        ulp=ldexpf(1.0f,exponent-1-mantissa);
    }
    /* One native R11/R11/B10 ULP propagated to the 8-bit terminal, plus
       independent real/generated RGBA8 rounding (at most one code total). */
    return 1u+(unsigned)ceilf(255.0f*ulp);
}
static unsigned interp_static_pixel_tolerance(const fixture *f, size_t pixel, unsigned channel) {
    return interp_static_channel_tolerance(f->scene_pixels[pixel*4+channel],channel);
}
static int run_interp_static(void) {
    W=1280; H=720; DW=960; DH=536;
    fixture_rgb1110=1; replay_no_ui=1;
    replay_probe_format=VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    setenv("TOTK_FRAMEGEN","interp",1);
    setenv("TOTK_FRAMEGEN_BUDGET_MS","10000",1);
    setenv("TOTK_POST_AA","off",1);
    mtl_init();
    CHECK(vk_fg_interpolating(),"static-scene oracle uses production interpolation");
    fixture f; fixture_create(&f);
    CHECK(vk_tex_format(f.scene)==VK_FORMAT_B10G11R11_UFLOAT_PACK32 &&
          vk_tex_format(f.canvas)==VK_FORMAT_B10G11R11_UFLOAT_PACK32,
          "static canvas and scene use Thor's B10G11R11_UFLOAT_PACK32 format (VkFormat 122)");
    f.ui_repeats=0;
    vk_post_output out={0};
    float camera[84]; fg_test_camera(camera,0,0,0,0);
    uint8_t *real=malloc(PIXELS*4),*generated=malloc(PIXELS*4);
    CHECK(real && generated,"allocate independent real and generated terminal readbacks");
    if (!real || !generated) exit(1);
    for (unsigned frame=0;frame<2;frame++) {
        fixture_interp_static_pixels(&f);
        fixture_upload(&f);
        CHECK(real_frame(&f,camera,frame,0,&out)==(frame!=0),
            "static identical real frame %u preserves normal warmup/output cadence",frame);
        if (frame==0)
            read_image(vk_tex_image(f.final),VK_IMAGE_ASPECT_COLOR_BIT,real,PIXELS*4);
    }
    CHECK(f.completed && out.image,"static identical pair produces a completed real Vulkan output");
    if (out.image) {
        read_image(out.image,VK_IMAGE_ASPECT_COLOR_BIT,generated,PIXELS*4);
        unsigned mismatch=0,first=0; int max_delta=0;
        for (size_t i=0;i<PIXELS;i++) for (unsigned c=0;c<4;c++) {
            int delta=abs((int)generated[4*i+c]-(int)real[4*i+c]);
            unsigned tolerance=interp_static_pixel_tolerance(&f,i,c);
            if (delta>(int)tolerance) { if (!mismatch) first=(unsigned)i; mismatch++; }
            if (delta>max_delta) max_delta=delta;
        }
        CHECK(!mismatch,
            "static generated RGBA stays within one native R11/R11/B10 ULP plus RGBA8 rounding at every pixel, including the invalid-depth fallback rectangle (%u channel mismatches; first pixel %u,%u; max delta %d)",
            mismatch,first%W,first/W,max_delta);
    }
    check_sources_unchanged(&f);
    vk_gpu_idle(); mtl_tex_release(&f.scene_desc); mtl_tex_release(&f.depth_desc);
    mtl_tex_release(&f.final_desc); mtl_tex_release(&f.canvas_desc);
    fixture_replay_destroy(&f); free(f.pool.mem); free(f.scene_pixels); free(f.depth_pixels);
    free(f.depth_baseline); free(f.final_pixels); free(real); free(generated);
    replay_probe_format=VK_FORMAT_R32G32B32A32_SFLOAT;
    return failures ? 1 : 0;
}
static int run_interp_replay_buffers(void) {
    W=1280; H=720; DW=960; DH=536;
    fixture_rgb1110=1; replay_no_ui=0;
    replay_probe_format=VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    replay_uniform_mutation.enabled=1;
    setenv("TOTK_FRAMEGEN","interp",1);
    setenv("TOTK_FRAMEGEN_BUDGET_MS","10000",1);
    setenv("TOTK_POST_AA","off",1);
    mtl_init();
    CHECK(vk_fg_interpolating(),"late-UBO replay regression uses production interpolation");
    fixture f; fixture_create(&f); f.ui_repeats=1;
    CHECK(f.ui_uniform_address && f.ui_uniform_live.buffer,
        "regression UI uniform is backed by a real resolved guest pool VkBuffer");
    CHECK(vk_tex_format(f.scene)==VK_FORMAT_B10G11R11_UFLOAT_PACK32 &&
          vk_tex_format(f.canvas)==VK_FORMAT_B10G11R11_UFLOAT_PACK32,
        "late-UBO regression retains the Thor B10G11R11 scene/canvas storage path");
    vk_post_output out={0};
    float camera[84]; fg_test_camera(camera,0,0,0,0);
    uint8_t *real=malloc(PIXELS*4),*generated=malloc(PIXELS*4),*real_baseline=malloc(PIXELS*4);
    CHECK(real && generated && real_baseline,"allocate independent real/generated terminal readbacks");
    if (!real || !generated || !real_baseline) exit(1);
    for (int negative=1;negative>=0;negative--) {
        replay_uniform_mutation.negative_live=negative;
        vk_fg_reset(); f.real_passes=0;
        memset(&out,0,sizeof out);
        for (unsigned frame=0;frame<2;frame++) {
            fixture_interp_static_pixels(&f);
            fixture_upload(&f);
            const float transparent[4]={0,0,0,0};
            fixture_interp_ui(&f,transparent);
            CHECK(real_frame(&f,camera,frame,0,&out)==(frame!=0),
                "late-UBO %s binding preserves static pair warmup/output cadence",
                negative ? "live" : "frozen");
            if (frame==1) {
                read_image(vk_tex_image(f.final),VK_IMAGE_ASPECT_COLOR_BIT,real,PIXELS*4);
                if (negative) memcpy(real_baseline,real,PIXELS*4);
            }
        }
        CHECK(f.completed && out.image,"late-UBO %s case completes a real Vulkan midpoint",
            negative ? "live" : "frozen");
        unsigned changed_inside=0,changed_outside=0; int max_delta=0;
        if (out.image) {
            read_image(out.image,VK_IMAGE_ASPECT_COLOR_BIT,generated,PIXELS*4);
            for (unsigned y=0;y<H;y++) for (unsigned x=0;x<W;x++) {
                size_t i=(size_t)y*W+x;
                int in_probe=x>=W/2-40 && x<W/2+40 && y>=H/2-30 && y<H/2+30;
                for (unsigned c=0;c<4;c++) {
                    int delta=abs((int)generated[4*i+c]-(int)real[4*i+c]);
                    unsigned tolerance=interp_static_pixel_tolerance(&f,i,c);
                    if (delta>(int)tolerance) {
                        if (in_probe) changed_inside++;
                        else changed_outside++;
                    }
                    if (delta>max_delta) max_delta=delta;
                }
            }
        }
        if (negative)
            CHECK(changed_inside>100 && !changed_outside,
                "unfrozen live descriptor reproduces large late-color attenuation only inside the UI probe (%u inside, %u outside, max delta %d)",
                changed_inside,changed_outside,max_delta);
        else
            CHECK(!changed_inside && !changed_outside,
                "production-owned UI bytes preserve the full-pixel real/generated oracle after the original pool UBO is GPU-mutated (%u inside, %u outside, max delta %d)",
                changed_inside,changed_outside,max_delta);
        if (!negative) {
            unsigned terminal_mismatch=0;
            for (size_t i=0;i<PIXELS;i++) for (unsigned c=0;c<4;c++) {
                int delta=abs((int)real[4*i+c]-(int)real_baseline[4*i+c]);
                terminal_mismatch+=delta>(int)interp_static_pixel_tolerance(&f,i,c);
            }
            CHECK(!terminal_mismatch,
                "GPU mutation after the real UI draw leaves the actual real terminal unchanged across live and frozen bindings (%u channel mismatches)",
                terminal_mismatch);
        }
        check_sources_unchanged(&f);
    }
    mtl_sub_begin();
    VkCommandBuffer cb=vk_cb();
    const float gpu_color[4]={0.035f,0.035f,0.035f,1.0f};
    vkCmdUpdateBuffer(cb,f.ui_uniform_live.buffer,f.ui_uniform_live.offset+4*sizeof(float),
        sizeof gpu_color,gpu_color);
    vk_gpu_write(f.ui_uniform_address,8*sizeof(float));
    VkDescriptorBufferInfo refused={0};
    VkDeviceSize align=V.props.limits.minUniformBufferOffsetAlignment;
    if (!align) align=16;
    CHECK(!vk_ub_replay_read(f.ui_uniform_address,8*sizeof(float),align,&refused),
        "production snapshot refuses a real pending GPU-only UBO producer instead of reading stale guest bytes");
    (void)mtl_sub_commit();
    vk_gpu_idle(); mtl_tex_release(&f.scene_desc); mtl_tex_release(&f.depth_desc);
    mtl_tex_release(&f.final_desc); mtl_tex_release(&f.canvas_desc);
    fixture_replay_destroy(&f); free(f.pool.mem); free(f.scene_pixels); free(f.depth_pixels);
    free(f.depth_baseline); free(f.final_pixels); free(real); free(generated); free(real_baseline);
    replay_uniform_mutation.enabled=0;
    replay_uniform_mutation.negative_live=0;
    replay_no_ui=1; replay_probe_format=VK_FORMAT_R32G32B32A32_SFLOAT;
    return failures ? 1 : 0;
}

/* Use a real promoted vk_tex and native sibling. The guest terminal is 320x180,
   its presented attachment is 480x270, and both synthetic captures pass
   through vk_fg_capture_real. A distinct midpoint and unsafe region prove that
   geometry and the fallback came from the image actually presented. */
static int run_interp_nativeui(int depth_range) {
    native_terminal_w=FG_TEST_W*3/2; native_terminal_h=FG_TEST_H*3/2;
    native_ui_terminal=1;
    setenv("TOTK_NATIVE_UI","1",1);
    setenv("TOTK_FRAMEGEN","interp",1);
    setenv("TOTK_FRAMEGEN_BUDGET_MS","10000",1);
    setenv("TOTK_POST_AA","off",1);
    mtl_init();
    CHECK(vk_native_ui_enabled() && vk_fg_interpolating(),
        "native UI interpolation fixture enables both production paths");
    vk_native_window_size(native_terminal_w,native_terminal_h);
    CHECK(vk_fg_raw_format(VK_FORMAT_R8G8B8A8_SRGB)==vk_fg_raw_format(VK_FORMAT_R8G8B8A8_UNORM) &&
        vk_fg_raw_format(VK_FORMAT_B8G8R8A8_SRGB)==vk_fg_raw_format(VK_FORMAT_B8G8R8A8_UNORM) &&
        vk_fg_raw_format(VK_FORMAT_R8G8B8A8_UNORM)!=vk_fg_raw_format(VK_FORMAT_B8G8R8A8_UNORM),
        "mutable SRGB/UNORM views preserve encoded byte identity without accepting changed channel order");
    vk_present_rect full=vk_present_native_crop((vk_present_rect){0,0,W,H},
        W,H,native_terminal_w,native_terminal_h);
    CHECK(full.x==0 && full.y==0 && full.w==native_terminal_w && full.h==native_terminal_h,
        "generated native output uses the full native crop, not the guest-sized upper-left two thirds");
    vk_present_rect partial=vk_present_native_crop((vk_present_rect){80,40,160,100},
        W,H,native_terminal_w,native_terminal_h);
    CHECK(partial.x==120 && partial.y==60 && partial.w==240 && partial.h==150,
        "real and generated presenter share endpoint-scaled nonzero native crop geometry");
    fixture f; fixture_create(&f); f.ui_repeats=1;
    authority.enabled=1; authority.kind=1; authority.frame=0;
    authority.pipeline=replay_pipeline(f.ui_layout,VK_FORMAT_R32G32B32A32_SFLOAT,3);
    for (unsigned i=0;i<2;i++) authority.uniform[i]=host_create(8*sizeof(float),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    if (vk_scene_post_enabled()) mtl_xv_window_tex(f.final_desc.serial);
    else vk_tex_native_promote(f.final);
    vk_tex_native_ui_begin(f.final);
    CHECK(vk_tex_native_view(f.final,NULL,1)!=VK_NULL_HANDLE,
        "promoted encoded terminal realizes its native attachment view");
    vk_post_output native={0};
    float camera[84]; fg_test_camera(camera,0,0,0,0);
    if (depth_range) interp_camera_clip(camera,1.0,25000.0);
    const float transparent[4]={0};
    size_t native_bytes=(size_t)native_terminal_w*native_terminal_h*4;
    uint8_t *real_pixels=malloc(native_bytes),*output_pixels=malloc(native_bytes);
    CHECK(real_pixels && output_pixels,"allocate native terminal pixel goldens");
    if (!real_pixels || !output_pixels) exit(1);
    vk_post_output out={0};
    fixture_interp_pixels(&f,0);
    if (depth_range) fixture_depth_distance(&f,1.0,25000.0,2.0);
    else for (size_t i=0;i<PIXELS;i++) f.depth_pixels[i]=fg_test_depth(2);
    fixture_upload(&f); fixture_interp_ui(&f,transparent);
    CHECK(!real_frame(&f,camera,0,0,&out),
        "first native real frame captures its actual presented terminal without a midpoint");
    CHECK(vk_tex_native_output(f.final,&native) && native.w==native_terminal_w &&
        native.h==native_terminal_h && native.format==VK_FORMAT_R8G8B8A8_UNORM,
        "read the completed first native real terminal at its actual extent");
    read_image_extent(native.image,VK_IMAGE_ASPECT_COLOR_BIT,real_pixels,native_bytes,native_terminal_w,native_terminal_h);

    fixture_interp_pixels(&f,1);
    authority.frame=1;
    if (depth_range) { fixture_depth_distance(&f,0.1,5000.0,2.0); interp_camera_clip(camera,0.1,5000.0); }
    else for (size_t i=0;i<PIXELS;i++) f.depth_pixels[i]=fg_test_depth(2);
    fixture_upload(&f); fixture_interp_ui(&f,transparent);
    CHECK(real_frame(&f,camera,1,0,&out),
        "second native real frame generates the midpoint at the presented native extent");
    CHECK(f.completed && out.w==native_terminal_w && out.h==native_terminal_h,
        "native midpoint completes at the exact 3:2 terminal extent");
    CHECK(authority.have_result && authority.result[2]==24*18 && authority.result[3]>24*18,
        "native pair exercises changed-depth authority and conservative fallback dilation");
    if (out.image) {
        read_image_extent(out.image,VK_IMAGE_ASPECT_COLOR_BIT,output_pixels,native_bytes,native_terminal_w,native_terminal_h);
        unsigned safe_x=120,safe_y=150;
        double scene_x=(safe_x+0.5)*W/native_terminal_w-0.5;
        double scene_y=(safe_y+0.5)*H/native_terminal_h-0.5;
        float rgb[3]; interp_expected_scene(0,1,scene_x,scene_y,0,0,3,rgb);
        uint8_t expected[4]={encoded_channel(rgb[0]),encoded_channel(rgb[1]),encoded_channel(rgb[2]),255};
        const uint8_t *actual=output_pixels+((size_t)safe_y*native_terminal_w+safe_x)*4;
        for (unsigned c=0;c<4;c++) CHECK(abs((int)actual[c]-expected[c])<=1,
            "native midpoint channel%u at %u,%u: %u expected %u",
            c,safe_x,safe_y,actual[c],expected[c]);
        unsigned fallback_bad=0;
        /* Generation preserves encoded RGB bits and intentionally emits opaque
           alpha, independently of the game's terminal alpha. */
        for (unsigned y=102;y<120;y++) for (unsigned x=138;x<168;x++) {
            size_t offset=((size_t)y*native_terminal_w+x)*4;
            fallback_bad+=memcmp(output_pixels+offset,real_pixels+offset,3)!=0 ||
                output_pixels[offset+3]!=255;
        }
        const uint8_t *got=output_pixels+((size_t)102*native_terminal_w+138)*4;
        const uint8_t *want=real_pixels+((size_t)102*native_terminal_w+138)*4;
        CHECK(!fallback_bad,
            "unsafe depth region preserves every native real RGB fallback pixel and opaque alpha without black holes or silhouettes (%u mismatches; first sample %u,%u,%u,%u expected %u,%u,%u,255)",
            fallback_bad,got[0],got[1],got[2],got[3],want[0],want[1],want[2]);
    }
    vk_native_frame_end(); vk_gpu_idle();
    authority.enabled=0;
    vkDestroyPipeline(V.dev,authority.pipeline,NULL);
    for (unsigned i=0;i<2;i++) host_destroy(&authority.uniform[i]);
    mtl_tex_release(&f.scene_desc); mtl_tex_release(&f.depth_desc);
    mtl_tex_release(&f.final_desc); mtl_tex_release(&f.canvas_desc);
    fixture_replay_destroy(&f); free(f.pool.mem); free(f.scene_pixels); free(f.depth_pixels);
    free(f.depth_baseline); free(f.final_pixels); free(real_pixels); free(output_pixels);
    native_ui_terminal=0;
    return failures ? 1 : 0;
}
/* Locked admission uses completed Vulkan queries with deterministic 0.1-ms
   intervals, as the queue-graph probe does. This tests adaptive ownership and
   pixels, not software-rasterizer headroom or device performance. */
static PFN_vkGetQueryPoolResults scene_query_results;
static VkResult VKAPI_CALL scene_locked_queries(VkDevice device,VkQueryPool pool,uint32_t first,uint32_t count,
        size_t size,void *data,VkDeviceSize stride,VkQueryResultFlags flags) {
    VkResult result=scene_query_results(device,pool,first,count,size,data,stride,flags);
    if (result==VK_SUCCESS && (flags&VK_QUERY_RESULT_64_BIT)) {
        uint64_t step=(uint64_t)(100000.0/V.props.limits.timestampPeriod);
        for (uint32_t i=0;i<count;i++) *(uint64_t *)((char *)data+i*stride)=(first+i+1)*step;
    }
    return result;
}
/* Static camera/scene makes the generated warp an exact identity. Real and
   generated pictures must then use the identical spatial filter BEFORE opaque
   native UI, including a per-native-pixel clipped clear. */
static int run_scene_scale(unsigned samples,int drs,int locked) {
    if (samples) { W=1280; H=720; DW=W; DH=H; setenv("TOTK_FRAMEGEN_DIAG","1",1); }
    if (drs) { W=640; H=360; DW=480; DH=268; }
    scene_scale_fixture=1; native_ui_terminal=1;
    native_terminal_w=W*3/2; native_terminal_h=H*3/2;
    setenv("TOTK_FRAMEGEN",locked?"locked":"interp",1);
    if (locked) {
        setenv("TOTK_FPS60","0",1); setenv("TOTK_FPS_AUTO","0",1);
        setenv("TOTK_UNCAPPED","0",1); setenv("TOTK_REALTIME","1",1);
        unsetenv("TOTK_FRAMEGEN_BUDGET_MS");
    } else setenv("TOTK_FRAMEGEN_BUDGET_MS","10000",1);
    setenv("TOTK_POST_AA","off",1);
    mtl_init();
    if (locked) {
        CHECK(vk_fg_locked()&&vk_fg_enabled(),"scene AA fixture uses actual locked adaptive mode");
        scene_query_results=vkGetQueryPoolResults; vkGetQueryPoolResults=scene_locked_queries;
    }
    CHECK(vk_scene_post_enabled()&&vk_native_ui_enabled(),"scene filter and effective native composition enabled");
    vk_native_window_size(native_terminal_w,native_terminal_h);
    fixture f; fixture_create(&f); f.ui_repeats=1;
    vk_tex_native_promote(f.canvas); mtl_xv_window_tex(f.final_desc.serial);
    float camera[84]; fg_test_camera(camera,0,0,0,0);
    camera[72]=DW; camera[73]=DH;
    if (samples) {
        authority.enabled=1; authority.kind=1; authority.defer_complete=1;
        authority.pipeline=replay_pipeline(f.ui_layout,VK_FORMAT_R32G32B32A32_SFLOAT,3);
        for (unsigned i=0;i<2;i++) authority.uniform[i]=host_create(8*sizeof(float),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    }
    const float opaque[4]={1,0,1,1};
    vk_post_output out={0};
    const unsigned warmup=16;
    double sum[4]={0},maximum[4]={0};
    /* Locked adaptive admission needs the first completed real GPU interval
       before its cold incremental probe, in addition to adjacent scene history. */
    for (unsigned frame=0;frame<(samples ? warmup+samples+1 : locked ? 3 : 2);frame++) {
        for (unsigned y=0;y<H;y++) for (unsigned x=0;x<W;x++) {
            size_t p=(size_t)y*W+x;
            float c=((x/5+y/7)&1)?0.8f:0.1f;
            f.scene_pixels[p*4]=c; f.scene_pixels[p*4+1]=c; f.scene_pixels[p*4+2]=c; f.scene_pixels[p*4+3]=1;
            f.depth_pixels[p]=fg_test_depth(2);
        }
        fixture_upload(&f); fixture_interp_ui(&f,opaque);
        uint64_t aa_before=vk_scene_test_aa_runs();
        unsigned slot=frame%VK_PRESENT_SLOT_CAPACITY;
        CHECK(real_frame(&f,camera,slot,0,&out)==(frame>=(locked?2u:1u)),
            "static scene frame%u generates after adjacent history and required real-cost warmup",frame);
        CHECK(vk_scene_test_aa_runs()-aa_before==(uint64_t)vk_scene_aa_enabled(),
            "SMAA executes exactly once per real seed; generated output never re-filters");
        if (samples && frame) {
            double cost[4];
            CHECK(vk_fg_test_scene_cost(slot,cost),"fenced scene-stage GPU timestamps exist");
            if (frame>warmup) for (unsigned c=0;c<4;c++) {
                sum[c]+=cost[c]; if (cost[c]>maximum[c]) maximum[c]=cost[c];
            }
            f.completed=vk_fg_complete(slot);
            CHECK(f.completed,"measured generated frame retires normally");
        }
    }
    if (samples) printf("SCENE_BENCH filter=%s source=%ux%u native=%ux%u depth=%ux%u samples=%u warmup=%u replay_ms=%.6f compose_ms=%.6f output_ms=%.6f native_copy_ms=%.6f generated_ms=%.6f max_compose_ms=%.6f\n",
        getenv("TOTK_SCENE_SCALE"),W,H,native_terminal_w,native_terminal_h,DW,DH,samples,warmup,
        sum[0]/samples,sum[1]/samples,sum[2]/samples,sum[3]/samples,(sum[0]+sum[2])/samples,maximum[1]);
    vk_post_output real={0};
    CHECK(f.completed && out.image && vk_tex_native_output(f.final,&real),"native real and generated scene-scale images exist");
    size_t bytes=(size_t)native_terminal_w*native_terminal_h*4;
    uint8_t *a=malloc(bytes),*b=malloc(bytes);
    if (out.image && real.image) {
        read_image_extent(real.image,VK_IMAGE_ASPECT_COLOR_BIT,a,bytes,native_terminal_w,native_terminal_h);
        read_image_extent(out.image,VK_IMAGE_ASPECT_COLOR_BIT,b,bytes,native_terminal_w,native_terminal_h);
        unsigned bad=0,ui_bad=0,max_error=0,first_x=0,first_y=0;
        for (unsigned y=0;y<native_terminal_h;y++) for (unsigned x=0;x<native_terminal_w;x++) {
            size_t p=((size_t)y*native_terminal_w+x)*4;
            for (unsigned c=0;c<4;c++) {
                unsigned error=abs((int)a[p+c]-b[p+c]);
                if (error>1) { if (!bad) { first_x=x;first_y=y; } bad++; }
                if (error>max_error) max_error=error;
            }
            if (x>=330 && x<390 && y>=45 && y<90)
                ui_bad+=a[p]!=255 || a[p+1]!=0 || a[p+2]!=255 || memcmp(a+p,b+p,4)!=0;
            if (x>=15 && x<27 && y>=15 && y<27)
                ui_bad+=a[p]!=0 || a[p+1]!=255 || a[p+2]!=0 || memcmp(a+p,b+p,4)!=0;
        }
        CHECK(!bad,"generated identity scene uses same spatial upscale as real native seed (%u channels; max=%u first=%u,%u real=%u generated=%u)",
            bad,max_error,first_x,first_y,a[((size_t)first_y*native_terminal_w+first_x)*4],b[((size_t)first_y*native_terminal_w+first_x)*4]);
        CHECK(!ui_bad,"opaque native UI/clear pixels remain exact after scene upscale (%u pixels)",ui_bad);
    }
    if (!samples) {
        /* Missing pre-UI retention must fail closed, never bind an absent
           low-res color image or regenerate with an unrelated scaler. */
        mtl_sub_begin(); VkCommandBuffer cb=vk_cb(); vk_fg_camera(camera);
        CHECK(vk_fg_capture(cb,f.scene,0,0,f.depth,0,0,W,H),"missing-retention negative case captures valid low-res mapping");
        CHECK(vk_fg_prepare_canvas(cb,native_terminal_w,native_terminal_h),"missing-retention case prepares native UI extent");
        CHECK(!vk_fg_interp_compose(cb),"missing native scene retention refuses interpolation");
        uint64_t ticket=mtl_sub_commit(); vk_fg_frame_end(ticket);
        CHECK(mtl_wait(ticket,10000000000ull),"refused source retires without retaining stale native pixels");
    } else {
        authority.enabled=authority.defer_complete=0;
        vkDestroyPipeline(V.dev,authority.pipeline,NULL);
        for (unsigned i=0;i<2;i++) host_destroy(&authority.uniform[i]);
    }
    free(a);free(b); vk_gpu_idle();
    mtl_tex_release(&f.scene_desc);mtl_tex_release(&f.depth_desc);mtl_tex_release(&f.final_desc);
    if (f.native_pixels) { mtl_tex_release(&f.native_desc); free(f.native_pixels); }
    fixture_replay_destroy(&f);free(f.pool.mem);free(f.scene_pixels);free(f.depth_pixels);free(f.depth_baseline);free(f.final_pixels);
    if (locked) vkGetQueryPoolResults=scene_query_results;
    return failures!=0;
}
/* Actual GPU output allocation/retirement in every presenter entry, coupled to
 * deterministic scheduling inputs. These timestamps model Odin, not scanout
 * measurements from software Vulkan or an Android compositor. */
static int run_interp_present(void) {
    setenv("TOTK_FRAMEGEN","interp",1); setenv("TOTK_FRAMEGEN_BUDGET_MS","10000",1);
    mtl_init(); CHECK(vk_fg_interpolating(),"present fixture uses delayed interpolation");
    fixture f; fixture_create(&f); f.ui_repeats=1;
    const float white[4]={1,1,1,1};
    float camera[84]; fg_test_camera(camera,0,0,0,0);
    vk_present_slots slots={0}; vk_present_interp interp={.retained=-1};
    vk_post_output out;
    unsigned generated=0,handoffs=0,expired=0,used=0;
    const uint64_t refresh=16666667,base=1000000000;
    for (unsigned frame=0;frame<=2*VK_PRESENT_SLOT_CAPACITY;frame++) {
        fixture_interp_pixels(&f,frame); fixture_upload(&f); fixture_interp_ui(&f,white);
        unsigned index=frame%VK_PRESENT_SLOT_CAPACITY;
        int recorded=real_frame(&f,camera,index,frame&1,&out);
        uint64_t callback=base+frame*2*refresh;
        vk_present_slot slot=vk_present_slots_reserve(&slots,1,callback,refresh,2,callback+1000000);
        uint64_t source_ready=slot.desired_ns+3000000;
        vk_present_interp_pair pair=vk_present_interp_push(&interp,(int)index,frame+1,1,slot,source_ready);
        CHECK(recorded==(frame!=0),"present frame %u produces exactly one output after warmup",frame);
        if (!recorded) continue;
        generated++; used|=1u<<index;
        CHECK(f.completed && out.image && out.view,"present entry %u completes real Vulkan output allocation/retirement",index);
        CHECK(pair.reason==VK_INTERP_READY && pair.midpoint_ns==slot.desired_ns+refresh,
            "reserved-late parent %u still schedules its own half-tick child",frame);
        framegen_child_reason reason=framegen_interp_child_status(source_ready,pair.midpoint_ns,source_ready+2100000);
        handoffs+=reason==FRAMEGEN_CHILD_READY; expired+=reason!=FRAMEGEN_CHILD_READY;
        CHECK(reason==FRAMEGEN_CHILD_READY,"2.1ms Odin child %u meets its own vsync despite parent's reserved-late time",frame);
        CHECK(framegen_interp_child_status(source_ready,pair.midpoint_ns,pair.midpoint_ns)==FRAMEGEN_CHILD_DEADLINE,
            "genuinely expired child %u is never handed off at/after its vsync",frame);
    }
    CHECK(used==(1u<<VK_PRESENT_SLOT_CAPACITY)-1,"all bounded output entries, including the buffered extra slot, are reusable");
    CHECK(generated==2*VK_PRESENT_SLOT_CAPACITY && handoffs==generated && !expired,"present model generated/handed-off=%u/%u expired=%u",generated,handoffs,expired);
    check_sources_unchanged(&f);
    vk_gpu_idle(); mtl_tex_release(&f.scene_desc); mtl_tex_release(&f.depth_desc); mtl_tex_release(&f.final_desc);
    fixture_replay_destroy(&f);
    free(f.pool.mem); free(f.scene_pixels); free(f.depth_pixels); free(f.depth_baseline); free(f.final_pixels);
    return failures ? 1 : 0;
}

static int run_interp_budget(void) {
    setenv("TOTK_FRAMEGEN","interp",1); setenv("TOTK_FRAMEGEN_BUDGET_MS","0.000000001",1);
    mtl_init(); CHECK(vk_fg_enabled() && vk_fg_interpolating(),"tiny-budget fixture starts in interpolation mode");
    fixture f; fixture_create(&f); f.ui_repeats=1;
    float camera[84]; fg_test_camera(camera,0,0,0,0);
    vk_post_output out; unsigned rejected=0;
    for (unsigned frame=0;frame<8;frame++) {
        int generated=real_frame(&f,camera,frame%4,0,&out);
        CHECK(generated==(frame!=0),"tiny-budget frame %u still records each valid adjacent pair after warmup",frame);
        if (generated) {
            CHECK(!f.completed,"tiny budget rejects pair %u with vk_fg_complete=0",frame);
            rejected+=!f.completed;
        }
        CHECK(vk_fg_enabled() && vk_fg_interpolating(),"budget rejection skips pair %u without process-wide interpolation suspension",frame);
    }
    CHECK(rejected==7,"all seven measured over-budget pairs are rejected while source/output rings remain reusable");
    vk_gpu_idle(); mtl_tex_release(&f.scene_desc); mtl_tex_release(&f.depth_desc); mtl_tex_release(&f.final_desc);
    fixture_replay_destroy(&f);
    free(f.pool.mem); free(f.scene_pixels); free(f.depth_pixels); free(f.depth_baseline); free(f.final_pixels);
    return failures ? 1 : 0;
}
static int run_off(int requested) {
    if (requested) setenv("TOTK_FRAMEGEN", "1", 1); else unsetenv("TOTK_FRAMEGEN");
    CHECK(!vk_fg_enabled(), "default-off or incompatible startup policy records no generation");
    float camera[84]; fg_test_camera(camera, 0, 0, 0, 0);
    vk_post_output out; memset(&out, 0xa5, sizeof out); vk_post_output unchanged = out;
    vk_fg_camera(camera); vk_fg_capture(VK_NULL_HANDLE, NULL, 0, 0, NULL, 0, 0, W, H);
    CHECK(!vk_fg_generate(VK_NULL_HANDLE, 0, W, H, 0, &out), "disabled generator records no output");
    CHECK(!memcmp(&out, &unchanged, sizeof out), "disabled generator leaves output unchanged");
    vk_fg_frame_end(0); vk_fg_complete(0); vk_fg_skip(); vk_fg_stats();
    if (!requested) {
        mtl_init(); fixture f; fixture_create(&f);
        CHECK(!real_frame(&f,camera,0,0,&out),"disabled path renders real scene/UI without generation");
        uint64_t passes[4]; vk_fg_test_pass_counts(passes);
        CHECK(f.real_passes==2 && !(passes[0]|passes[1]|passes[2]|passes[3]),
              "off baseline has two real passes and no private generation/UI pass");
        printf("framegen off pass census: real=2/frame, private=0, UI draws=128\n");
        vk_gpu_idle(); mtl_tex_release(&f.scene_desc); mtl_tex_release(&f.depth_desc); mtl_tex_release(&f.final_desc);
        fixture_replay_destroy(&f);
        free(f.pool.mem); free(f.scene_pixels); free(f.depth_pixels); free(f.depth_baseline); free(f.final_pixels);
    }
    return failures ? 1 : 0;
}
static int run_budget(void) {
    setenv("TOTK_FRAMEGEN","1",1); setenv("TOTK_FRAMEGEN_BUDGET_MS","0.000000001",1);
    mtl_init(); fixture f; fixture_create(&f);
    float camera[84]; fg_test_camera(camera,0,0,0,0);
    vk_post_output out;
    for (unsigned frame=0; frame<8 && vk_fg_enabled(); frame++)
        (void)real_frame(&f,camera,frame%4,0,&out);
    CHECK(!vk_fg_enabled(),"observed GPU budget overrun suspends generation");
    CHECK(!real_frame(&f,camera,0,0,&out),"suspended generator leaves real-only operation");
    vk_gpu_idle(); mtl_tex_release(&f.scene_desc); mtl_tex_release(&f.depth_desc); mtl_tex_release(&f.final_desc);
    fixture_replay_destroy(&f);
    free(f.pool.mem); free(f.scene_pixels); free(f.depth_pixels); free(f.depth_baseline); free(f.final_pixels);
    return failures ? 1 : 0;
}
static int run_late(void) {
    setenv("TOTK_FRAMEGEN","1",1);
    CHECK(vk_fg_enabled(),"late-source fixture starts enabled without GPU work");
    vk_fg_late();
    CHECK(!vk_fg_enabled(),"late source notification suspends subsequent generation");
    return failures ? 1 : 0;
}
typedef struct {
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkPipeline pipeline;
    host_buffer retained;
    uint32_t *retained_map, *retained_copy;
} dilation_fixture;
typedef struct { uint32_t raw, exact, tiled, rx, ry; } dilation_census;
static uint32_t dilation_random(uint32_t *state) {
    *state=*state*1664525u+1013904223u;
    return *state;
}
static void dilation_barrier(VkCommandBuffer cb, VkPipelineStageFlags2 src, VkAccessFlags2 read,
                             VkPipelineStageFlags2 dst, VkAccessFlags2 write) {
    VkMemoryBarrier2 barrier={VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,.srcStageMask=src,.srcAccessMask=read,
        .dstStageMask=dst,.dstAccessMask=write};
    VkDependencyInfo dependency={VK_STRUCTURE_TYPE_DEPENDENCY_INFO,.memoryBarrierCount=1,
        .pMemoryBarriers=&barrier};
    vkCmdPipelineBarrier2(cb,&dependency);
}
static void dilation_release_retained(dilation_fixture *f) {
    if (!f->retained.buffer) return;
    CHECK(!memcmp(f->retained_map,f->retained_copy,f->retained.size),
        "another pair dispatch never mutates a still-retained pair mask/census");
    vkUnmapMemory(V.dev,f->retained.memory);
    host_destroy(&f->retained);
    free(f->retained_copy);
    f->retained=(host_buffer){0};
}
/* Exercise the production shader itself. The independent CPU reference uses a
   summed-area table for the OLD exact native-pixel rectangular dilation; it
   neither repeats the coarse algorithm nor relies on matching only a census. */
static dilation_census dilation_gpu_case(dilation_fixture *f, unsigned w, unsigned h,
                                         const uint8_t *raw, unsigned rx, unsigned ry,
                                         const float *motion, const char *label) {
    size_t pixels=(size_t)w*h,words=16+2*pixels,stride=(size_t)w+1;
    uint32_t *integral=calloc(stride*(h+1),sizeof *integral);
    CHECK(integral!=NULL,"allocate independent exact dilation golden");
    if (!integral) exit(1);
    for (unsigned y=0;y<h;y++)
        for (unsigned x=0;x<w;x++) {
            size_t i=(size_t)(y+1)*stride+x+1;
            integral[i]=(raw[(size_t)y*w+x]!=0)+integral[i-1]+integral[i-stride]-integral[i-stride-1];
        }
    dilation_census result={.raw=integral[(size_t)h*stride+w]};
    host_buffer source=host_create(words*4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    host_buffer pair=host_create((words+16)*4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    uint32_t *source_map=host_map(&source),*pair_map=host_map(&pair);
    memset(source_map,0,source.size); memset(pair_map,0xa5,pair.size);
    float depth=fg_test_depth(2); uint32_t depth_word;
    memcpy(&depth_word,&depth,sizeof depth_word);
    uint32_t header[16]={w,h,result.raw,0,depth_word,depth_word,(uint32_t)pixels};
    memcpy(source_map,header,sizeof header); memcpy(pair_map,header,sizeof header);
    pair_map[10]=rx; pair_map[11]=ry; pair_map[12]=4;
    for (unsigned y=0;y<h;y++)
        for (unsigned x=0;x<w;x++) {
            size_t p=(size_t)y*w+x;
            source_map[16+p]=integral[(size_t)(y+1)*stride+x+1]-integral[(size_t)y*stride+x+1];
            source_map[16+pixels+p]=depth_word;
        }
    mtl_sub_begin(); VkCommandBuffer cb=vk_cb();
    VkDescriptorSet set=vk_alloc_set(f->dsl);
    VkDescriptorBufferInfo buffers[2]={{source.buffer,0,source.size},{pair.buffer,0,words*4}};
    VkWriteDescriptorSet writes[2];
    for (unsigned i=0;i<2;i++) writes[i]=(VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet=set,.dstBinding=i,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo=&buffers[i]};
    vkUpdateDescriptorSets(V.dev,2,writes,0,NULL);
    dilation_barrier(cb,VK_PIPELINE_STAGE_2_HOST_BIT,VK_ACCESS_2_HOST_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_STORAGE_READ_BIT|VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,f->pipeline);
    vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,f->layout,0,1,&set,0,NULL);
    struct { uint32_t phase,w,h,reserved; float transform[16]; uint32_t rect[4]; } push={.w=w,.h=h};
    if (motion) {
        memcpy(push.transform,motion,sizeof push.transform); push.phase=3;
        vkCmdPushConstants(cb,f->layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof push,&push);
        vkCmdDispatch(cb,1,1,1);
        dilation_barrier(cb,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_STORAGE_READ_BIT|VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    }
    uint64_t coarse_w=((uint64_t)w+3)/4,coarse_h=((uint64_t)h+3)/4;
    uint32_t groups=(uint32_t)((coarse_w*coarse_h+255)/256);
    for (unsigned phase=1;phase<=2;phase++) {
        push.phase=phase;
        vkCmdPushConstants(cb,f->layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof push,&push);
        vkCmdDispatch(cb,groups,1,1);
        dilation_barrier(cb,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            phase==1 ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_2_HOST_BIT,
            phase==1 ? VK_ACCESS_2_SHADER_STORAGE_READ_BIT|VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT : VK_ACCESS_2_HOST_READ_BIT);
    }
    uint64_t ticket=mtl_sub_commit();
    CHECK(mtl_wait(ticket,10000000000ull),"dilation GPU golden %s %ux%u completes",label,w,h);
    result.rx=pair_map[10]; result.ry=pair_map[11]; result.tiled=pair_map[3];
    CHECK(result.rx<=w && result.ry<=h && pair_map[12]==4,
        "dilation %s retains native radii and reports separate 4x4 granularity",label);
    unsigned false_known=0,invalid_mask=0,changed_source=memcmp(source_map,header,sizeof header)!=0;
    uint32_t counted=0;
    for (unsigned y=0;y<h;y++)
        for (unsigned x=0;x<w;x++) {
            unsigned lo_x=x>result.rx ? x-result.rx : 0,lo_y=y>result.ry ? y-result.ry : 0;
            unsigned hi_x=x+result.rx<w ? x+result.rx+1 : w;
            unsigned hi_y=y+result.ry<h ? y+result.ry+1 : h;
            uint32_t sum=integral[(size_t)hi_y*stride+hi_x]-integral[(size_t)lo_y*stride+hi_x]-
                integral[(size_t)hi_y*stride+lo_x]+integral[(size_t)lo_y*stride+lo_x];
            size_t p=(size_t)y*w+x;
            uint32_t known=pair_map[16+pixels+p];
            result.exact+=sum!=0; counted+=known==0;
            false_known+=sum!=0 && known!=0; invalid_mask+=known>1;
            changed_source+=source_map[16+p]!=integral[(size_t)(y+1)*stride+x+1]-
                integral[(size_t)y*stride+x+1] || source_map[16+pixels+p]!=depth_word;
        }
    CHECK(!false_known,"dilation %s %ux%u radius%u/%u: EVERY old exact unknown remains GPU-unknown (%u false known)",
        label,w,h,result.rx,result.ry,false_known);
    CHECK(!invalid_mask && result.tiled==counted && result.tiled>=result.exact && result.tiled<=pixels,
        "dilation %s counts actual full-native pixels, including every partial edge/bottom tile",label);
    CHECK(pair_map[2]==result.raw && !changed_source,
        "dilation %s never changes raw VALUE census, source prefix, depth range or final depth words",label);
    CHECK((uint64_t)result.exact*100<=pixels*35 || (uint64_t)result.tiled*100>pixels*35,
        "dilation %s NEVER accepts at35%% when old exact mask refuses",label);
    unsigned damaged=0;
    for (size_t i=(size_t)coarse_w*coarse_h;i<pixels;i++) damaged+=pair_map[16+i]!=0xa5a5a5a5u;
    for (size_t i=words;i<words+16;i++) damaged+=pair_map[i]!=0xa5a5a5a5u;
    CHECK(!damaged,"dilation %s touches only coarse scratch and in-bounds fullres mask",label);
    dilation_release_retained(f);
    f->retained=pair; f->retained_map=pair_map; f->retained_copy=malloc(pair.size);
    CHECK(f->retained_copy!=NULL,"retain GPU pair immutability golden");
    if (!f->retained_copy) exit(1);
    memcpy(f->retained_copy,pair_map,pair.size);
    vkUnmapMemory(V.dev,source.memory); host_destroy(&source); free(integral);
    return result;
}
static int run_interp_dilation(void) {
    setenv("TOTK_FRAMEGEN","interp",1); mtl_init();
    dilation_fixture f={0};
    VkDescriptorSetLayoutBinding bindings[2];
    for (unsigned i=0;i<2;i++) bindings[i]=(VkDescriptorSetLayoutBinding){.binding=i,
        .descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo ds={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount=2,.pBindings=bindings};
    VKCHECK(vkCreateDescriptorSetLayout(V.dev,&ds,NULL,&f.dsl));
    VkPushConstantRange range={VK_SHADER_STAGE_COMPUTE_BIT,0,96};
    VkPipelineLayoutCreateInfo li={VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,
        .pSetLayouts=&f.dsl,.pushConstantRangeCount=1,.pPushConstantRanges=&range};
    VKCHECK(vkCreatePipelineLayout(V.dev,&li,NULL,&f.layout));
    VkShaderModuleCreateInfo mi={VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize=sizeof framegen_authority_comp_spv,.pCode=framegen_authority_comp_spv};
    VkShaderModule module; VKCHECK(vkCreateShaderModule(V.dev,&mi,NULL,&module));
    VkComputePipelineCreateInfo ci={VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,
            .module=module,.pName="main"},.layout=f.layout};
    VKCHECK(vkCreateComputePipelines(V.dev,V.pcache,1,&ci,NULL,&f.pipeline));
    vkDestroyShaderModule(V.dev,module,NULL);
    uint32_t random=0x71c4d923u;
    for (unsigned h=1;h<=9;h++) for (unsigned w=1;w<=9;w++) {
        uint8_t raw[81];
        for (unsigned p=0;p<w*h;p++) raw[p]=(dilation_random(&random)>>24)<48;
        raw[w*h-1]=1;
        unsigned rx=dilation_random(&random)%(w+1),ry=dilation_random(&random)%(h+1);
        dilation_gpu_case(&f,w,h,raw,rx,ry,NULL,"tiny randomized/bottom-edge");
    }
    for (unsigned corner=0;corner<16;corner++) {
        uint8_t raw[19*13]={0};
        raw[(4+corner/4)*19+4+corner%4]=1;
        dilation_gpu_case(&f,19,13,raw,corner%8,(corner*3)%8,NULL,"single unknown tile phase/corner");
    }
    static const unsigned extent[][2]={{320,180},{960,536},{961,537},{1,537},{961,1},{7,259},{259,7},{257,257}};
    for (unsigned e=0;e<sizeof extent/sizeof *extent;e++) {
        unsigned w=extent[e][0],h=extent[e][1]; size_t pixels=(size_t)w*h;
        uint8_t *raw=calloc(pixels,1);
        CHECK(raw!=NULL,"allocate native/odd extent masks"); if (!raw) exit(1);
        unsigned cap_x=(unsigned)ceil(0.04*w)+1,cap_y=(unsigned)ceil(0.04*h)+1;
        if (cap_x>w) cap_x=w;
        if (cap_y>h) cap_y=h;
        dilation_gpu_case(&f,w,h,raw,0,0,NULL,"empty");
        memset(raw,1,pixels);
        dilation_gpu_case(&f,w,h,raw,cap_x,cap_y,NULL,"all unknown maximum radius");
        for (size_t p=0;p<pixels;p++) raw[p]=(dilation_random(&random)>>22)==0;
        unsigned rx=dilation_random(&random)%(cap_x+1),ry=dilation_random(&random)%(cap_y+1);
        dilation_gpu_case(&f,w,h,raw,rx,ry,NULL,"sparse random native radius");
        memset(raw,0,pixels); raw[pixels-1]=1;
        dilation_gpu_case(&f,w,h,raw,3>w ? w : 3,5>h ? h : 5,NULL,"single bottom-right partial tile");
        memset(raw+(size_t)(h-1)*w,1,w);
        dilation_gpu_case(&f,w,h,raw,cap_x,cap_y,NULL,"entire final native row");
        float motion[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
        dilation_census minimum=dilation_gpu_case(&f,w,h,raw,0,0,motion,"current motion minimum certificate");
        CHECK(minimum.rx==1 && minimum.ry==1,"identity motion keeps exact native radius1");
        memset(motion,0,sizeof motion);
        dilation_census maximum=dilation_gpu_case(&f,w,h,raw,0,0,motion,"current motion singular/hard cap");
        CHECK(maximum.rx==cap_x && maximum.ry==cap_y,"uncertified motion keeps unchanged hard-cap native radii");
        for (unsigned n=0;n<4;n++) {
            memset(motion,0,sizeof motion); motion[0]=motion[5]=motion[10]=motion[15]=1;
            motion[3]=((int)(dilation_random(&random)%201)-100)*0.0005f;
            motion[7]=((int)(dilation_random(&random)%201)-100)*0.0005f;
            motion[12]=((int)(dilation_random(&random)%21)-10)*0.0001f;
            for (size_t p=0;p<pixels;p++) raw[p]=(dilation_random(&random)>>24)<4;
            dilation_gpu_case(&f,w,h,raw,0,0,motion,"randomized current motion and mask");
        }
        free(raw);
    }
    /* Native area=3200, so exactly1120 unknown pixels is exactly35%, not a
       rounded percentage. Equality accepts; one more reference pixel refuses.
       Conservative-only threshold crossing is allowed and explicitly tested. */
    uint8_t threshold[80*40]={0};
    for (unsigned y=0;y<40;y++) memset(threshold+y*80,1,28);
    dilation_census equality=dilation_gpu_case(&f,80,40,threshold,0,0,NULL,"exactly35-percent equality");
    CHECK(equality.exact==1120 && equality.tiled==1120 && equality.tiled*100==80*40*35,
        "35-percent equality remains accepted, not changed to strict-less-than");
    threshold[39*80+27]=0;
    dilation_census below=dilation_gpu_case(&f,80,40,threshold,0,0,NULL,"one native pixel below35-percent");
    CHECK(below.exact==1119 && below.tiled==1120,"below-threshold partial occupancy may conservatively reach equality");
    threshold[39*80+28]=1;
    dilation_census extra=dilation_gpu_case(&f,80,40,threshold,0,0,NULL,"35-percent reference/tile-only refusal");
    CHECK(extra.exact==1120 && extra.tiled>1120,"tile-only growth can refuse exact35-percent coverage without relaxing authority");
    threshold[39*80+27]=1;
    dilation_census above=dilation_gpu_case(&f,80,40,threshold,0,0,NULL,"one native pixel above35-percent");
    CHECK(above.exact==1121 && above.tiled>1120,"one pixel over35-percent refuses in both methods");
    memset(threshold,0,sizeof threshold);
    for (unsigned y=0;y<40;y++) memset(threshold+y*80,1,26);
    dilation_census radius_equality=dilation_gpu_case(&f,80,40,threshold,2,1,NULL,"nonzero native radius equality");
    CHECK(radius_equality.exact==1120 && radius_equality.tiled==1120,
        "nonzero native-radius dilation also preserves exact35-percent equality");
    dilation_release_retained(&f); vk_gpu_idle();
    vkDestroyPipeline(V.dev,f.pipeline,NULL); vkDestroyPipelineLayout(V.dev,f.layout,NULL);
    vkDestroyDescriptorSetLayout(V.dev,f.dsl,NULL);
    return failures ? 1 : 0;
}
static uint32_t dilation_rectangle_count(unsigned w, unsigned h, unsigned x, unsigned y,
                                         unsigned rw, unsigned rh, unsigned rx, unsigned ry) {
    unsigned lo_x=(x>rx ? x-rx : 0)/4*4,hi_x=((x+rw-1+rx)/4+1)*4;
    unsigned coarse_radius=(ry+3)/4,lo_y=y/4>coarse_radius ? (y/4-coarse_radius)*4 : 0;
    unsigned hi_y=((y+rh-1)/4+coarse_radius+1)*4;
    if (hi_x>w) hi_x=w;
    if (hi_y>h) hi_y=h;
    return (hi_x-lo_x)*(hi_y-lo_y);
}
static uint64_t authority_trace_count(const char *reason) {
    return vk_fg_test_authority_trace_count(reason);
}
static void check_authority_trace_delta(const char *reason, uint64_t before,
        uint64_t expected_on, int trace_on, const char *message) {
    uint64_t after=authority_trace_count(reason);
    CHECK(after==before+(trace_on ? expected_on : 0),"%s (reason=%s before=%llu after=%llu trace=%d)",
        message,reason,(unsigned long long)before,(unsigned long long)after,trace_on);
}
static int authority_trace_enabled(void) {
    const char *value=getenv("TOTK_FRAMEGEN_JOB_TRACE");
    return value && !strcmp(value,"1");
}
static int run_interp_authority(void) {
    int trace_on=authority_trace_enabled();
    setenv("TOTK_FRAMEGEN","interp",1);
    setenv("TOTK_FRAMEGEN_BUDGET_MS","1000",1);
    setenv("TOTK_FRAMEGEN_UNKNOWN_PERCENT","35",1);
    mtl_init(); CHECK(vk_fg_enabled() && vk_fg_interpolating(),"general depth-value authority fixture starts");
    fixture f; fixture_create(&f); f.ui_repeats=1;
    authority.enabled=1;
    authority.pipeline=replay_pipeline(f.ui_layout,VK_FORMAT_R32G32B32A32_SFLOAT,3);
    for (unsigned i=0;i<2;i++) authority.uniform[i]=host_create(8*sizeof(float),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    uint8_t *pixels=malloc(PIXELS*4),*earlier=malloc(PIXELS*4);
    CHECK(pixels && earlier,"allocate exact encoded authority goldens");
    if (!pixels || !earlier) exit(1);
    const float transparent[4]={0}; float camera[84]; vk_post_output out;
    for (unsigned kind=1;kind<=4;kind++) {
        vk_fg_reset(); authority.kind=(int)kind;
        for (unsigned frame=0;frame<2;frame++) {
            authority.frame=frame; fixture_interp_pixels(&f,frame);
            /* Every baseline sample is ordinary proven-camera z=2. */
            for (size_t i=0;i<DEPTH_PIXELS;i++) f.depth_pixels[i]=fg_test_depth(2);
            fixture_upload(&f); fixture_interp_ui(&f,transparent);
            fg_test_camera(camera,0,frame ? 0.16 : -0.16,0,0);
            int generated=real_frame(&f,camera,(kind+frame)%4,0,&out);
            CHECK(generated==(frame!=0),"authority case%u frame%u retains adjacent pair cadence",kind,frame);
            if (!frame) {
                read_image(vk_tex_image(f.final),VK_IMAGE_ASPECT_COLOR_BIT,earlier,PIXELS*4);
                /* The real RGBX presenter forces opaque alpha, unlike this
                   guest-terminal stand-in. Compare against that real contract. */
                for (size_t i=0;i<PIXELS;i++) earlier[4*i+3]=255;
            }
            else {
                CHECK(authority.have_result,"authority case%u exposes fenced comparison counters",kind);
                if (authority.have_result) {
                    CHECK(authority.result[0]==DW && authority.result[1]==DH,"authority census uses native depth extent");
                    CHECK(authority.result[2]==(kind==1 ? 24*18 : kind==4 ? W*3/4*H : 0),
                        "bitwise certificate counts changed values only (same-value and restored writes remain proven)");
                    CHECK(kind==1 ? authority.result[3]>authority.result[2] :
                        kind==4 ? authority.result[3]>DW*DH*35/100 : authority.result[3]==0,
                        "dilated fallback census is conservative and retains zero for equal values");
                }
                CHECK(f.completed==(kind!=4),"unknown fraction over35 percent rejects before handoff, not low-coverage pairs");
                if (kind!=4 && out.image) {
                    read_image(out.image,VK_IMAGE_ASPECT_COLOR_BIT,pixels,PIXELS*4);
                    if (kind==1) {
                        check_pixel(pixels,102,74,earlier+((size_t)74*W+102)*4,0,"unknown depth outputs earlier encoded real pixel bitwise");
                        check_pixel(pixels,89,74,earlier+((size_t)74*W+89)*4,0,"dilated footprint also outputs earlier real bitwise");
                    } else {
                        float rgb[3]={kind==3 ? 0.8f : 0.25f,0.3f,0.6f};
                        check_interp_linear(pixels,102,74,rgb,"same-value/restored depth remains proven despite color changes");
                    }
                    float rgb[3]; interp_expected_scene(0,1,50,40,0.0225*W,-0.0225*W,3,rgb);
                    check_interp_linear(pixels,50,40,rgb,"known region outside conservative mask still reprojects/interpolates");
                }
            }
        }
    }
    CHECK(authority_trace_count("not-a-reason")==0,
        "unknown authority trace reason has a zero counter after trace initialization");
    /* The same retained source participates first in a stationary pair, then
       a moving pair. Its new radius must use THIS pair's delta, while the
       previous output's pair masks/census stay immutable until release. */
    vk_fg_reset(); authority.kind=1;
    uint32_t stationary_pair[12]={0};
    for (unsigned frame=0;frame<3;frame++) {
        authority.frame=frame; authority.defer_complete=frame==1;
        fixture_interp_pixels(&f,frame);
        for (size_t i=0;i<DEPTH_PIXELS;i++) f.depth_pixels[i]=fg_test_depth(2);
        fixture_upload(&f); fixture_interp_ui(&f,transparent);
        fg_test_camera(camera,0,frame==2 ? 0.16 : 0,0,0);
        CHECK(real_frame(&f,camera,frame,0,&out)==(frame!=0),
            "stationary-to-moving pair frame%u keeps adjacent history",frame);
        if (!frame) continue;
        CHECK(authority.have_pair,"per-pair immutable motion census exists after its output fence");
        for (unsigned source=0;source<2;source++) {
            const uint32_t *c=authority.pair+6*source;
            CHECK(c[0]==DW && c[1]==DH && c[2]==24*18,"pair census retains each source's exact depth-value comparison");
            CHECK(c[3]==dilation_rectangle_count(DW,DH,90,65,24,18,c[4],c[5]),
                "pair census matches conservative4x4 rectangle expansion for its unchanged native radius");
        }
        if (frame==1) {
            CHECK(authority.pair[4]<=2 && authority.pair[5]<=2 &&
                authority.pair[10]<=2 && authority.pair[11]<=2,
                "stationary pair uses only numerical/texel guard, not fixed4-percent dilation");
            memcpy(stationary_pair,authority.pair,sizeof stationary_pair);
        } else {
            CHECK(authority.pair[4]>stationary_pair[10],
                "retained older source is redilated using the new moving pair, not its previous stationary radius");
            CHECK(authority.pair[4]<(unsigned)ceil(0.04*DW)+1 &&
                authority.pair[10]<(unsigned)ceil(0.04*DW)+1,
                "complete z=2 depth range tightens translated radius below the global near-plane cap");
            uint32_t retained[12];
            CHECK(vk_fg_test_authority_pair(1,retained) && !memcmp(retained,stationary_pair,sizeof retained),
                "later pair cannot overwrite an earlier still-referenced output's mask census");
            CHECK(f.completed && vk_fg_complete(1),"moving and held stationary outputs retire their own pair resources");
        }
    }
    authority.defer_complete=0;
    /* -0 is shader-valid near depth. A uint-sign shortcut must not drop it
       from the complete range and silently tighten the moving-pair bound. */
    vk_fg_reset();
    for (unsigned frame=0;frame<2;frame++) {
        authority.frame=frame; fixture_interp_pixels(&f,frame);
        for (size_t i=0;i<DEPTH_PIXELS;i++) f.depth_pixels[i]=fg_test_depth(2);
        f.depth_pixels[5*DW+5]=-0.0f; f.depth_pixels[5*DW+6]=NAN;
        f.depth_pixels[5*DW+7]=1; f.depth_pixels[5*DW+8]=INFINITY;
        f.depth_pixels[5*DW+9]=-1;
        fixture_upload(&f); fixture_interp_ui(&f,transparent);
        fg_test_camera(camera,0,frame ? 0.16 : 0,0,0);
        CHECK(real_frame(&f,camera,frame,0,&out)==(frame!=0),"signed-zero range pair frame%u remains valid",frame);
        if (frame) CHECK(f.completed && authority.have_pair &&
            authority.pair[4]==(unsigned)ceil(0.04*DW)+1 &&
            authority.pair[10]==(unsigned)ceil(0.04*DW)+1,
            "GPU range includes signed zero and falls back to conservative near-depth displacement cap");
    }
    /* An unavailable snapshot cannot silently borrow a prior case's mask. */
    authority.kind=5; authority.frame=0; fixture_pixels(&f,0); fixture_upload(&f);
    fg_test_camera(camera,0,0,0,0);
    uint64_t injected_before=authority_trace_count("injected");
    CHECK(!real_frame(&f,camera,0,0,&out),"failed snapshot refuses frame rather than using stale authority");
    check_authority_trace_delta("injected",injected_before,1,trace_on,
        "existing injected snapshot failure records exactly its reason");
    authority.kind=2;
    CHECK(!real_frame(&f,camera,1,0,&out),"first valid frame after snapshot failure warms fresh history");
    CHECK(real_frame(&f,camera,2,0,&out) && f.completed,"second valid frame recovers without disabling interpolation");
    /* Keep three completed-but-unreleased generated outputs alive. Their four
       source references must prevent a fifth snapshot from overwriting any
       certificate still consumed by an in-flight presentation. */
    vk_fg_reset(); authority.defer_complete=1;
    for (unsigned frame=0;frame<4;frame++) {
        authority.frame=frame&1; fixture_pixels(&f,0); fixture_upload(&f);
        CHECK(real_frame(&f,camera,frame ? frame-1 : 3,0,&out)==(frame!=0),
            "pinned source lifetime frame%u records without reusing referenced history",frame);
    }
    mtl_sub_begin();
    uint64_t unavailable_before=authority_trace_count("source-unavailable");
    CHECK(!vk_fg_authority_begin(vk_cb(),f.depth,0,0),
        "all four pinned sources refuse a new snapshot rather than overwrite an old certificate");
    check_authority_trace_delta("source-unavailable",unavailable_before,1,trace_on,
        "four pinned sources report source-unavailable exactly once");
    uint64_t invalid_before=authority_trace_count("invalid");
    vk_fg_skip();
    CHECK(!vk_fg_authority_begin(vk_cb(),f.depth,0,0),
        "explicitly invalidated pinned frame still refuses authority before resource work");
    check_authority_trace_delta("invalid",invalid_before,1,trace_on,
        "explicit skip of refused authority reports invalid exactly once");
    uint64_t pinned_ticket=mtl_sub_commit(); vk_fg_frame_end(pinned_ticket);
    CHECK(mtl_wait(pinned_ticket,10000000000ull),"refused capacity probe retires without a stale source");
    for (unsigned slot=0;slot<3;slot++)
        CHECK(vk_fg_complete(slot),"pinned output%u releases its exact source pair only on completion",slot);
    authority.defer_complete=0;
    authority.enabled=0;
    mtl_sub_begin();
    uint64_t subresource_before=authority_trace_count("subresource");
    CHECK(!vk_fg_authority_begin(vk_cb(),f.scene,0,0),"color-only source is an unsupported depth certificate");
    check_authority_trace_delta("subresource",subresource_before,1,trace_on,
        "unsupported color source reports subresource exactly once");
    invalid_before=authority_trace_count("invalid");
    vk_fg_skip();
    CHECK(!vk_fg_authority_begin(vk_cb(),f.depth,0,0),
        "explicitly invalidated unsupported frame still refuses authority before resource work");
    check_authority_trace_delta("invalid",invalid_before,1,trace_on,
        "explicit skip of unsupported authority reports invalid exactly once");
    uint64_t ticket=mtl_sub_commit(); vk_fg_frame_end(ticket);
    CHECK(mtl_wait(ticket,10000000000ull),"unsupported-copy refusal retires any private reservation");
    mtl_sub_begin();
    CHECK(vk_fg_authority_begin(vk_cb(),f.depth,0,0),"reserve a fresh certificate before external reset");
    vk_fg_reset(); vk_fg_camera(camera);
    CHECK(!vk_fg_capture(vk_cb(),f.scene,0,0,f.depth,0,0,W,H),
        "actual surface reset invalidates pending certificate despite history-only cut support");
    ticket=mtl_sub_commit(); vk_fg_frame_end(ticket);
    CHECK(mtl_wait(ticket,10000000000ull),"external-reset snapshot retires under its own fence");
    mtl_sub_begin();
    CHECK(vk_fg_authority_begin(vk_cb(),f.depth,0,0),"reserve certificate for exact subresource mismatch negative");
    uint64_t mismatch_before=authority_trace_count("attempted-mismatch");
    CHECK(!vk_fg_authority_begin(vk_cb(),f.scene,0,0),"a repeated begin on a different subresource invalidates, never reuses");
    check_authority_trace_delta("attempted-mismatch",mismatch_before,1,trace_on,
        "repeated begin with wrong subresource reports attempted-mismatch exactly once");
    vk_fg_camera(camera);
    CHECK(!vk_fg_capture(vk_cb(),f.scene,0,0,f.depth,0,0,W,H),"mismatch cannot recover stale authority within the frame");
    ticket=mtl_sub_commit(); vk_fg_frame_end(ticket);
    CHECK(mtl_wait(ticket,10000000000ull),"mismatched certificate retires under its own fence");
    uint64_t counts[5]; vk_fg_test_authority_counts(counts);
    CHECK(counts[0]>=10 && counts[1]>=10 && counts[2]>=10 && counts[3]>=1,
        "authority diagnostics count root reservations/final comparisons and coverage skips");
    printf("authority census: reservations=%llu final-copies=%llu compares=%llu threshold-skips=%llu recorded=%llu\n",
        (unsigned long long)counts[0],(unsigned long long)counts[1],(unsigned long long)counts[2],
        (unsigned long long)counts[3],(unsigned long long)counts[4]);
    uint64_t trace_lifetime[5]={
        authority_trace_count("injected"),authority_trace_count("source-unavailable"),
        authority_trace_count("subresource"),authority_trace_count("attempted-mismatch"),
        authority_trace_count("invalid")
    };
    if (!trace_on)
        CHECK(trace_lifetime[0]==0 && trace_lifetime[1]==0 && trace_lifetime[2]==0 &&
            trace_lifetime[3]==0 && trace_lifetime[4]==0,
            "trace-off authority reason counters remain zero after negative fixtures");
    authority.enabled=1; authority.kind=2;
    for (unsigned frame=0;frame<300;frame++) {
        authority.frame=frame&1; fixture_pixels(&f,0); fixture_upload(&f);
        CHECK(real_frame(&f,camera,frame%4,0,&out)==(frame!=0),
            "authority diagnostics/source reuse frame%u preserves cadence",frame);
        if (frame) CHECK(f.completed,"steady certified frame%u completes below generous host budget",frame);
    }
    CHECK(authority_trace_count("injected")==trace_lifetime[0] &&
        authority_trace_count("source-unavailable")==trace_lifetime[1] &&
        authority_trace_count("subresource")==trace_lifetime[2] &&
        authority_trace_count("attempted-mismatch")==trace_lifetime[3] &&
        authority_trace_count("invalid")==trace_lifetime[4],
        "diagnostic lifetime reason counters survive 300-frame history resets");
    authority.enabled=0;
    vk_gpu_idle(); vkDestroyPipeline(V.dev,authority.pipeline,NULL);
    for (unsigned i=0;i<2;i++) host_destroy(&authority.uniform[i]);
    mtl_tex_release(&f.scene_desc); mtl_tex_release(&f.depth_desc); mtl_tex_release(&f.final_desc); mtl_tex_release(&f.canvas_desc);
    fixture_replay_destroy(&f); free(f.pool.mem); free(f.scene_pixels); free(f.depth_pixels);
    free(f.depth_baseline); free(f.final_pixels); free(pixels); free(earlier);
    return failures ? 1 : 0;
}

static _Atomic unsigned interp_job_complete_calls;
static int interp_job_complete(unsigned slot) {
    atomic_fetch_add_explicit(&interp_job_complete_calls,1,memory_order_relaxed);
    return vk_fg_complete(slot);
}
static void interp_job_ownership(unsigned worker, unsigned frame, uint64_t refs,
        uint64_t busy, const char *phase) {
    uint64_t counts[2]; vk_fg_test_job_ownership(counts);
    CHECK(counts[0]==refs && counts[1]==busy,
        "job reuse worker%u frame%u %s: source refs=%llu/%llu busy outputs=%llu/%llu",
        worker,frame,phase,(unsigned long long)counts[0],(unsigned long long)refs,
        (unsigned long long)counts[1],(unsigned long long)busy);
}
typedef struct {
    vk_present_job *job;
    unsigned slot, calls, index;
    pthread_t presenter;
    int on_worker;
    vk_present_job_completion completion;
} interp_job_retirement;
static int interp_job_retire_metadata(void *context, unsigned index, uint64_t deadline) {
    (void)deadline;
    interp_job_retirement *r=context;
    r->on_worker=!pthread_equal(pthread_self(),r->presenter);
    r->index=index;
    r->completion=vk_present_job_complete(r->job,r->slot,interp_job_complete);
    r->calls++;
    return 1;
}
/* Actual fenced GPU ownership, using the native terminal's distinct real-frame
   pixels. OFF retires metadata synchronously; ON uses the production portable
   queue with a callback stand-in. Neither path models Android compositor use. */
static int run_interp_job_reuse(void) {
    native_terminal_w=FG_TEST_W*3/2; native_terminal_h=FG_TEST_H*3/2;
    native_ui_terminal=1;
    setenv("TOTK_NATIVE_UI","1",1);
    setenv("TOTK_FRAMEGEN","interp",1);
    setenv("TOTK_FRAMEGEN_BUDGET_MS","10000",1);
    setenv("TOTK_FRAMEGEN_UNKNOWN_PERCENT","35",1);
    setenv("TOTK_POST_AA","off",1);
    mtl_init();
    CHECK(vk_native_ui_enabled() && vk_fg_enabled() && vk_fg_interpolating(),
        "job reuse enables actual interpolation and native terminal capture");
    vk_native_window_size(native_terminal_w,native_terminal_h);
    fixture f; fixture_create(&f); f.ui_repeats=1;
    authority.enabled=authority.defer_complete=1;
    authority.pipeline=replay_pipeline(f.ui_layout,VK_FORMAT_R32G32B32A32_SFLOAT,3);
    for (unsigned i=0;i<2;i++) authority.uniform[i]=host_create(8*sizeof(float),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    if (vk_scene_post_enabled()) mtl_xv_window_tex(f.final_desc.serial);
    else vk_tex_native_promote(f.final);
    vk_tex_native_ui_begin(f.final);
    CHECK(vk_tex_native_view(f.final,NULL,1)!=VK_NULL_HANDLE,
        "job reuse realizes the same promoted native terminal as the native UI golden");
    size_t bytes=(size_t)native_terminal_w*native_terminal_h*4;
    uint8_t *earlier=malloc(bytes),*pixels=malloc(bytes);
    CHECK(earlier && pixels,"allocate native job-reuse output goldens");
    if (!earlier || !pixels) exit(1);
    const float transparent[4]={0};
    float camera[84]; fg_test_camera(camera,0,0,0,0);
    const unsigned slot=3,frames=32; /* Eight four-source-ring wraps per mode. */
    atomic_store_explicit(&interp_job_complete_calls,0,memory_order_relaxed);
    VkImage physical_output=VK_NULL_HANDLE;
    for (unsigned worker=0;worker<2;worker++) {
        vk_fg_reset(); authority.kind=1; authority.frame=0;
        uint64_t passes_before[4],passes_after[4]; vk_fg_test_pass_counts(passes_before);
        unsigned real_before=f.real_passes;
        unsigned calls_before=atomic_load_explicit(&interp_job_complete_calls,memory_order_relaxed);
        fixture_interp_pixels(&f,0);
        for (size_t i=0;i<DEPTH_PIXELS;i++) f.depth_pixels[i]=fg_test_depth(2);
        fixture_upload(&f); fixture_interp_ui(&f,transparent);
        vk_post_output out={0},native={0};
        CHECK(!real_frame(&f,camera,slot,0,&out),
            "job reuse worker%u warms adjacent history exactly once",worker);
        interp_job_ownership(worker,0,0,0,"warmup has no generated ownership");
        int have_native=vk_tex_native_output(f.final,&native);
        CHECK(have_native && native.w==native_terminal_w && native.h==native_terminal_h,
            "job reuse reads the actual fenced native real terminal");
        if (!have_native) exit(1);
        read_image_extent(native.image,VK_IMAGE_ASPECT_COLOR_BIT,earlier,bytes,native_terminal_w,native_terminal_h);
        vk_present_job job={.completion_result=2};
        interp_job_retirement retirement={.job=&job,.slot=slot,.presenter=pthread_self()};
        vk_present_retire queue;
        if (worker) {
            int error=vk_present_retire_init(&queue,interp_job_retire_metadata,NULL,NULL,&retirement);
            CHECK(!error,"job reuse initializes the actual retirement queue");
            if (error) exit(1);
            error=vk_present_retire_start(&queue);
            CHECK(!error,"job reuse starts the actual retirement worker");
            if (error) exit(1);
        }
        unsigned accepted=0,refused=0,reset_from[3]={0},previous_kind=1;
        for (unsigned frame=1;frame<=frames;frame++) {
            /* The previous job is finished, but its physical slot retains all
               these poisoned observations until the NEW producer begins. */
            interp_job_ownership(worker,frame,0,0,"before reusing physical output");
            job.capacity_wait_start_ns=UINT64_C(5000000000)+frame;
            job.capacity_wait_end_ns=job.capacity_wait_start_ns+100;
            job.capacity_wait_count=frame+1; job.retire_async=1;
            unsigned old_cache=job.completion_result;
            int omit_begin=frame==10 || frame==26;
            CHECK(old_cache==1 || old_cache==2,"previous real completion leaves a known cache");
            if (!omit_begin) {
                if (old_cache<=2) reset_from[old_cache]++;
                vk_present_job_begin(&job);
                CHECK(!job.capacity_wait_start_ns && !job.capacity_wait_end_ns &&
                    !job.capacity_wait_count && !job.retire_async && !job.completion_result,
                    "NEW generated job worker%u frame%u clears capacity, retirement and cache%u",
                    worker,frame,old_cache);
            } else CHECK(old_cache==(frame==10 ? 2u : 1u),
                "owned omitted-reset negative starts with an actual accepted/refused prior cache");
            unsigned kind=frame==25 ? 4 : 1;
            int expected_accepted=kind!=4 && previous_kind!=4;
            authority.kind=(int)kind; authority.frame=frame&1;
            fixture_interp_pixels(&f,frame&1);
            for (size_t i=0;i<DEPTH_PIXELS;i++) f.depth_pixels[i]=fg_test_depth(2);
            fixture_upload(&f); fixture_interp_ui(&f,transparent);
            int generated=real_frame(&f,camera,slot,0,&out);
            CHECK(generated,
                "worker%u generated-to-generated physical-slot reuse frame%u survives source-ring wraps",
                worker,frame);
            if (!generated) exit(1);
            CHECK(!f.completed,"real_frame waits the covering GPU fence without automatic completion");
            interp_job_ownership(worker,frame,2,1,"GPU done still holds both sources and output");
            CHECK(out.image && out.view && out.w==native_terminal_w && out.h==native_terminal_h &&
                out.format==VK_FORMAT_R8G8B8A8_UNORM,"reused output retains exact native encoded extent");
            if (!physical_output) physical_output=out.image;
            CHECK(out.image==physical_output,"every generated job reuses the SAME physical output image");
            CHECK(authority.have_result && authority.have_pair,"fenced reused output has actual GPU authority census");
            if (authority.have_pair) for (unsigned side=0;side<2;side++) {
                unsigned source_kind=side ? kind : previous_kind;
                const uint32_t *c=authority.pair+side*6;
                CHECK(c[0]==DW && c[1]==DH && c[2]==(source_kind==4 ? W*3/4*H : 24*18),
                    "reused pair side%u counts the actual changed native-depth pixels",side);
                CHECK(c[3]==dilation_rectangle_count(DW,DH,source_kind==4 ? 0 : 90,
                    source_kind==4 ? 0 : 65,source_kind==4 ? W*3/4 : 24,
                    source_kind==4 ? H : 18,c[4],c[5]),
                    "reused pair side%u retains its conservative GPU dilation census",side);
                CHECK((c[3]*100>DW*DH*35)==(source_kind==4),
                    "kind4 is an actual threshold refusal, not a mocked completion result");
            }
            read_image_extent(out.image,VK_IMAGE_ASPECT_COLOR_BIT,pixels,bytes,native_terminal_w,native_terminal_h);
            unsigned fallback_bad=0;
            for (unsigned y=102;y<120;y++) for (unsigned x=138;x<168;x++) {
                size_t offset=((size_t)y*native_terminal_w+x)*4;
                fallback_bad+=memcmp(pixels+offset,earlier+offset,3)!=0 || pixels[offset+3]!=255;
            }
            CHECK(!fallback_bad,
                "reused output preserves every earlier native-only fallback RGB pixel and opaque alpha");
            if (expected_accepted) {
                unsigned x=120,y=150;
                double sx=(x+0.5)*W/native_terminal_w-0.5,sy=(y+0.5)*H/native_terminal_h-0.5;
                float rgb[3]; interp_expected_scene((frame-1)&1,frame&1,sx,sy,0,0,3,rgb);
                uint8_t expected[4]={encoded_channel(rgb[0]),encoded_channel(rgb[1]),encoded_channel(rgb[2]),255};
                const uint8_t *actual=pixels+((size_t)y*native_terminal_w+x)*4;
                for (unsigned c=0;c<4;c++) CHECK(abs((int)actual[c]-expected[c])<=1,
                    "worker%u frame%u actual native midpoint channel%u: %u expected %u",
                    worker,frame,c,actual[c],expected[c]);
            }
            unsigned before=atomic_load_explicit(&interp_job_complete_calls,memory_order_relaxed);
            if (!omit_begin) {
                job.capacity_wait_start_ns=frame;
                job.capacity_wait_end_ns=frame+100;
                job.capacity_wait_count=1; job.retire_async=worker;
            }
            uint64_t wait_start=job.capacity_wait_start_ns,wait_end=job.capacity_wait_end_ns;
            unsigned wait_count=job.capacity_wait_count,retire_async=job.retire_async;
            vk_present_job_completion completed=vk_present_job_complete(&job,slot,interp_job_complete);
            if (omit_begin) {
                CHECK(!completed.first && completed.accepted==(old_cache==2) &&
                    atomic_load_explicit(&interp_job_complete_calls,memory_order_relaxed)==before,
                    "omitting NEW reset reproduces cache%u suppressing actual completion",old_cache);
                interp_job_ownership(worker,frame,2,1,"owned negative reproduces the real source/output leak");
                /* real_frame already waited this output's covering fence.
                   Complete exactly once before any output/source reuse. */
                CHECK(interp_job_complete(slot)==expected_accepted,
                    "owned omitted-reset negative cleans up with actual fenced completion");
            } else CHECK(completed.first && completed.accepted==expected_accepted,
                "NEW job invokes actual completion once and caches GPU acceptance/refusal");
            CHECK(atomic_load_explicit(&interp_job_complete_calls,memory_order_relaxed)==before+1 &&
                job.completion_result==(expected_accepted ? 2u : 1u),
                "every generated job has exactly one actual completion and the correct cache");
            interp_job_ownership(worker,frame,0,0,"actual completion releases both sources and output");
            for (unsigned requeue=0;requeue<3;requeue++) {
                vk_present_job_completion cached=vk_present_job_complete(&job,slot,interp_job_complete);
                CHECK(!cached.first && cached.accepted==expected_accepted &&
                    atomic_load_explicit(&interp_job_complete_calls,memory_order_relaxed)==before+1,
                    "SAME-job YIELD-style lookup%u preserves cached completion without another callback",requeue);
            }
            if (expected_accepted) {
                accepted++;
                unsigned index=0;
                if (worker) {
                    vk_present_retire_token token={0};
                    int reserved=vk_present_retire_reserve(&queue,frame,&token);
                    CHECK(reserved,"accepted completed job reserves real worker metadata");
                    if (!reserved) exit(1);
                    index=token.index;
                    int published=vk_present_retire_publish(&queue,token);
                    CHECK(published,"accepted completed job publishes real worker metadata");
                    if (!published) exit(1);
                    int drained=vk_present_retire_drain(&queue,vk_present_retire_deadline());
                    CHECK(drained,"metadata callback stand-in drains before physical slot reuse");
                    if (!drained) exit(1);
                } else (void)interp_job_retire_metadata(&retirement,index,vk_present_retire_deadline());
                CHECK(retirement.calls==accepted && retirement.index==index &&
                    retirement.on_worker==(int)worker && !retirement.completion.first &&
                    retirement.completion.accepted,
                    "OFF/ON metadata retires on the expected thread using only the completed-job cache");
            } else refused++; /* Refused GPU output never has a compositor handoff. */
            CHECK(job.capacity_wait_start_ns==wait_start && job.capacity_wait_end_ns==wait_end &&
                job.capacity_wait_count==wait_count && job.retire_async==retire_async &&
                job.completion_result==(expected_accepted ? 2u : 1u) &&
                atomic_load_explicit(&interp_job_complete_calls,memory_order_relaxed)==before+1,
                "same-job requeues and metadata retirement never reset observations or repeat completion");
            interp_job_ownership(worker,frame,0,0,"retirement has no generator ownership to release again");
            have_native=vk_tex_native_output(f.final,&native);
            CHECK(have_native,"retain actual current native real pixels for the next pair");
            if (!have_native) exit(1);
            read_image_extent(native.image,VK_IMAGE_ASPECT_COLOR_BIT,earlier,bytes,native_terminal_w,native_terminal_h);
            previous_kind=kind;
        }
        CHECK(accepted==frames-2 && refused==2 && reset_from[1] && reset_from[2] &&
            atomic_load_explicit(&interp_job_complete_calls,memory_order_relaxed)==calls_before+frames,
            "worker%u covers eight ring wraps, both NEW cache resets, and two owned leak controls",worker);
        vk_fg_test_pass_counts(passes_after);
        CHECK(f.real_passes-real_before==2*(frames+1) &&
            passes_after[0]-passes_before[0]==frames && passes_after[1]-passes_before[1]==2*(frames+1) &&
            passes_after[2]-passes_before[2]==frames && passes_after[3]-passes_before[3]==frames,
            "job reuse preserves exact real/UI/compose/transfer/output GPU pass census");
        if (worker) {
            int joined=vk_present_retire_drain_join(&queue,vk_present_retire_deadline());
            CHECK(joined,"job-reuse retirement worker drains and joins cleanly");
            if (!joined) exit(1);
            CHECK(queue.retires==accepted && !vk_present_retire_count(&queue),
                "actual worker retires every accepted metadata job exactly once");
            CHECK(vk_present_retire_destroy(&queue),"joined job-reuse queue releases its host synchronization");
        }
    }
    printf("interp job reuse: same physical slot3,32 generated jobs/worker OFF+ON,actual GPU ownership,cache1/2 resets,fenced omitted-reset controls,native pixels; no Android compositor claim\n");
    vk_native_frame_end(); vk_gpu_idle();
    authority.enabled=authority.defer_complete=0;
    vkDestroyPipeline(V.dev,authority.pipeline,NULL);
    for (unsigned i=0;i<2;i++) host_destroy(&authority.uniform[i]);
    mtl_tex_release(&f.scene_desc); mtl_tex_release(&f.depth_desc); mtl_tex_release(&f.final_desc);
    fixture_replay_destroy(&f); free(f.pool.mem); free(f.scene_pixels); free(f.depth_pixels);
    free(f.depth_baseline); free(f.final_pixels); free(earlier); free(pixels);
    native_ui_terminal=0;
    return failures ? 1 : 0;
}
static int run_interp_headroom(int qcom) {
    setenv("TOTK_FRAMEGEN",qcom ? "locked" : "interp",1);
    if (qcom) {
        setenv("TOTK_FRAMEGEN_ENGINE",qcom==2 ? "lsfg" : "qcom",1);
        setenv("TOTK_FPS60","0",1); setenv("TOTK_UNCAPPED","0",1); setenv("TOTK_REALTIME","1",1);
    }
    unsetenv("TOTK_FRAMEGEN_BUDGET_MS"); /* exercise adaptive default, not test override */
    setenv("TOTK_FRAMEGEN_DIAG","1",1); /* validate real timestamp phase boundaries */
    mtl_init(); CHECK(vk_fg_enabled() && vk_fg_interpolating(),"adaptive interpolation fixture starts without fixed ceiling");
    if (qcom==1) CHECK(vk_fg_qcom_requested() && !vk_fg_qcom_active() && vk_fg_locked(),
        "host refuses EGL/AHB and retains the actual locked interpolation generator");
    if (qcom==2) CHECK(vk_fg_lsfg_requested() && !vk_fg_lsfg_active() && vk_fg_locked() && !vk_fg_qcom_requested(),
        "host refuses unpackaged user-DLL backend and retains the unchanged locked generator");
    fixture f; fixture_create(&f); f.ui_repeats=1;
    host_buffer presentation=host_create(PIXELS*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    const float transparent[4]={0}; float camera[84];
    fg_test_camera(camera,0,0,0,0); fixture_interp_ui(&f,transparent);
    unsigned recorded=0,accepted=0,warmup=0; vk_post_output output;
    /* Mirror production: source/replay/compose in the real submission, then
       generation in a new, explicitly tagged synthetic-only ticket range. */
    for (unsigned frame=0;frame<300;frame++) {
        mtl_sub_begin(); VkCommandBuffer cb=vk_cb();
        vk_fg_camera(camera);
        CHECK(vk_fg_capture(cb,f.scene,0,0,f.depth,0,0,W,H),"adaptive real capture remains available during budget warmup");
        int prepared=vk_fg_prepare_canvas(cb,W,H);
        int replayed=fixture_render_replay(&f,cb,prepared);
        uint64_t real_ticket=mtl_sub_commit(),last_ticket=real_ticket;
        int generated=0;
        if (replayed) {
            mtl_sub_begin(); cb=vk_cb();
            generated=vk_fg_generate(cb,frame%4,W,H,0,&output);
            CHECK(generated,"accepted composition records an idle synthetic output");
            if (generated) {
                vk_fg_output_copy_begin(cb,frame%4);
                image_barrier(cb,output.image,VK_IMAGE_ASPECT_COLOR_BIT,
                    VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL);
                VkBufferImageCopy copy={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
                    .imageExtent={W,H,1}};
                vkCmdCopyImageToBuffer(cb,output.image,VK_IMAGE_LAYOUT_GENERAL,presentation.buffer,1,&copy);
                vk_barrier_all(cb);
                vk_fg_gpu_end(cb,frame%4);
            }
            last_ticket=mtl_sub_commit();
            if (generated) vk_fg_synthetic_range(real_ticket+1,last_ticket);
        }
        vk_fg_track_result(VK_FG_ELIGIBLE);
        vk_fg_frame_end(last_ticket);
        CHECK(mtl_wait(last_ticket,10000000000ull),"adaptive real/synthetic covering fence completes");
        if (generated) {
            uint32_t phase_counts[2]; double phase_limits[2];
            CHECK(vk_fg_test_profile(frame%4,phase_counts,phase_limits),
                "fenced phase timestamps retain exact source/output ownership");
            CHECK(phase_counts[0]>0 && phase_counts[1]==3 &&
                isfinite(phase_limits[0]) && isfinite(phase_limits[1]) &&
                phase_limits[0]>=0 && phase_limits[1]>=phase_limits[0],
                "source stages plus output/post/presentation phases have measured nonnegative timestamps");
        }
        f.completed=generated && vk_fg_complete(frame%4);
        vk_fg_stats();
        recorded+=generated; accepted+=generated && f.completed; warmup+=!generated;
    }
    CHECK(warmup>0,"adaptive default waits for completed exact real-ticket provenance");
    CHECK(recorded>0 && accepted>0,"actual completed GPU samples admit safe adaptive generation after warmup");
    CHECK(vk_fg_enabled(),"adaptive missing/overload skips never permanently suspend interpolation");
    printf("adaptive GPU fixture: recorded=%u accepted=%u warmup-or-skipped=%u (actual timestamps, no fixed override)\n",
        recorded,accepted,warmup);
    vk_gpu_idle(); mtl_tex_release(&f.scene_desc); mtl_tex_release(&f.depth_desc);
    host_destroy(&presentation);
    mtl_tex_release(&f.final_desc); mtl_tex_release(&f.canvas_desc);
    fixture_replay_destroy(&f); free(f.pool.mem); free(f.scene_pixels); free(f.depth_pixels);
    free(f.depth_baseline); free(f.final_pixels);
    return failures ? 1 : 0;
}
static int run_interp_uncertified(unsigned flag) {
    setenv("TOTK_FRAMEGEN","interp",1); setenv("TOTK_FRAMEGEN_BUDGET_MS","1000",1);
    if (flag) setenv("TOTK_FRAMEGEN_UNCERTIFIED_PLANE",flag==1 ? "1" : "2",1);
    else unsetenv("TOTK_FRAMEGEN_UNCERTIFIED_PLANE");
    mtl_init(); experimental.on=flag==1; experimental.enabled=1;
    CHECK(vk_fg_uncertified_plane()==experimental.on,"only exact opt-in1 enables UNCERTIFIED plane assumptions");
    fixture f; fixture_create(&f); f.ui_repeats=1;
    experimental.pipeline=replay_pipeline(f.ui_layout,VK_FORMAT_R32G32B32A32_SFLOAT,3);
    for (unsigned i=0;i<3;i++) experimental.uniform[i]=host_create(8*sizeof(float),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    uint8_t *pixels=malloc(PIXELS*4),*earlier=malloc(PIXELS*4);
    CHECK(pixels && earlier,"allocate experimental exact-real/pixel goldens");
    if (!pixels || !earlier) exit(1);
    const float transparent[4]={0}; float camera[84]; vk_post_output output;
    for (unsigned kind=0;kind<=16;kind++) {
        experimental.kind=(int)kind; vk_fg_reset();
        for (unsigned frame=0;frame<2;frame++) {
            experimental.frame=frame; fixture_interp_pixels(&f,frame);
            for (size_t i=0;i<DEPTH_PIXELS;i++) f.depth_pixels[i]=fg_test_depth(2);
            fixture_upload(&f); fixture_interp_ui(&f,transparent);
            fg_test_camera(camera,0,frame ? 0.16 : -0.16,0,0);
            int generated=real_frame(&f,camera,(kind+frame)%4,0,&output);
            CHECK(generated==((frame!=0) && !experimental.refuse),
                "experimental case%u frame%u preserves pair cadence or fails closed",kind,frame);
            if (!frame) {
                read_image(vk_tex_image(f.final),VK_IMAGE_ASPECT_COLOR_BIT,earlier,PIXELS*4);
                for (size_t i=0;i<PIXELS;i++) earlier[4*i+3]=255; /* real RGBX contract */
            } else if (generated) {
                CHECK(f.completed && experimental.have_result && authority.have_result,
                    "experimental output and its exact fenced value census complete");
                unsigned admitted=experimental.on && (kind<=2 || kind==4 || kind==6) ?
                    (kind==6 ? 2 : 1)*24*18 : 0;
                unsigned unknown=(kind==2 || kind==6 ? 2 : 1)*24*18-admitted;
                CHECK(experimental.admitted[0]==admitted && experimental.admitted[1]==admitted,
                    "only changed isolated plane values matching FINAL depth count as UNCERTIFIED pixels");
                CHECK(authority.result[2]==unknown,
                    "other unknown writers, unchanged plane spans, invalid cameras and unlisted shaders stay masked");
                if (output.image) {
                    read_image(output.image,VK_IMAGE_ASPECT_COLOR_BIT,pixels,PIXELS*4);
                    if (admitted) {
                        float rgb[3]={kind==4 ? 0.55f : 0.25f,0.3f,0.6f};
                        check_interp_linear(pixels,102,74,rgb,"opt-in plane region interpolates only under explicit assumed authority");
                    } else check_pixel(pixels,102,74,earlier+((size_t)74*W+102)*4,0,
                        "default-off or unrelated unknown final value keeps bitwise older-real fallback");
                    if (kind==2)
                        check_pixel(pixels,152,74,earlier+((size_t)74*W+152)*4,0,
                            "an earlier unrelated unknown region is not laundered by a later plane span");
                    float rgb[3]; interp_expected_scene(0,1,50,40,0.0225*W,-0.0225*W,3,rgb);
                    check_interp_linear(pixels,50,40,rgb,"ordinary proven region is unchanged by experimental mode");
                }
            }
        }
    }
    /* Exercise once-per-source pixel counters and zero-extra-work default for
       an entire reporting window, not only a single special-case pair. */
    experimental.kind=0; vk_fg_reset();
    fg_test_camera(camera,0,0,0,0);
    for (unsigned frame=0;frame<300;frame++) {
        experimental.frame=frame&1; fixture_pixels(&f,0); fixture_upload(&f);
        fixture_interp_ui(&f,transparent);
        CHECK(real_frame(&f,camera,frame%4,0,&output)==(frame!=0),
            "experimental reporting frame%u keeps real/interpolated cadence",frame);
        if (frame) CHECK(f.completed && experimental.admitted[1]==(experimental.on ? 24*18 : 0),
            "experimental per-source pixel census is exact while default remains zero");
    }
    experimental.enabled=0; vk_gpu_idle();
    printf("UNCERTIFIED fixture flag=%u exact-pair attribution/default-off/other-writer guards passed\n",flag);
    vkDestroyPipeline(V.dev,experimental.pipeline,NULL);
    for (unsigned i=0;i<3;i++) host_destroy(&experimental.uniform[i]);
    mtl_tex_release(&f.scene_desc); mtl_tex_release(&f.depth_desc); mtl_tex_release(&f.final_desc); mtl_tex_release(&f.canvas_desc);
    free(f.depth_baseline); free(f.final_pixels); free(pixels); free(earlier);
    return failures ? 1 : 0;
}
static int run_locked_config(int refusal) {
    setenv("TOTK_FRAMEGEN","locked",1);
    setenv("TOTK_FPS60",refusal == 1 ? "1" : "0",1);
    setenv("TOTK_UNCAPPED","0",1); setenv("TOTK_REALTIME","1",1);
    unsetenv("TOTK_FRAMEGEN_BUDGET_MS");
    if (refusal == 4) setenv("TOTK_FRAMEGEN_LEAD_REFRESHES","3",1);
    if (refusal == 2) setenv("TOTK_FRAMEGEN_BUDGET_MS","10000",1);
    if (refusal == 3) unsetenv("TOTK_REALTIME");
    mtl_init();
    CHECK(!!vk_fg_enabled() == !refusal,"locked mode requires real30, realtime, total GPU budget and bounded lead");
    if (!refusal) {
        CHECK(vk_fg_interpolating() && vk_fg_capturing(),
              "locked mode always captures interpolation sources without hybrid switching");
        const char *lead=getenv("TOTK_FRAMEGEN_LEAD_REFRESHES");
        CHECK(vk_fg_lead_refreshes() == (lead && !strcmp(lead,"2") ? 2u : 1u),
              "locked lead is one refresh by default or exactly two when selected");
    }
    return failures ? 1 : 0;
}
static int run_locked_lead_two(void) {
    setenv("TOTK_FRAMEGEN_LEAD_REFRESHES","2",1);
    return run_locked_config(0);
}
static int run_qcom_config(unsigned kind) {
    const char *engine=kind==0 ? "qcom" : kind==1 ? "camera" : kind==2 ? "QCOM" : "";
    setenv("TOTK_FRAMEGEN_ENGINE",engine,1);
    int result=run_locked_config(0);
    CHECK(vk_fg_qcom_requested()==(kind==0),"QCOM engine selection is exact and opt-in");
    CHECK(!vk_fg_qcom_active(),"unsupported host interop falls back without enabling a second engine");
    CHECK(vk_fg_enabled() && vk_fg_locked() && vk_fg_interpolating(),
        "QCOM refusal leaves the existing locked generator available");
    return result || failures ? 1 : 0;
}

static int run_qcom_wrong_mode(void) {
    setenv("TOTK_FRAMEGEN_ENGINE","qcom",1);
    int result=run_enabled();
    CHECK(!vk_fg_qcom_requested() && !vk_fg_qcom_active() && !vk_fg_locked(),
        "QCOM selection never changes legacy extrapolation mode");
    return result || failures ? 1 : 0;
}

static int run_lsfg_config(unsigned kind) {
    setenv("TOTK_FRAMEGEN_ENGINE",kind==1 ? "LSFG" : "lsfg",1);
    int result=kind==2 ? run_enabled() : run_locked_config(0);
    CHECK(vk_fg_lsfg_requested()==(kind==0),"LSFG selection is exact and locked-only");
    CHECK(!vk_fg_lsfg_active() && !vk_fg_qcom_requested(),"host capability refusal does not select either external engine");
    CHECK(vk_fg_enabled(),"LSFG refusal retains the ordinary generator");
    return result || failures ? 1 : 0;
}

/* No Vulkan initialization or private DLL: opt-in parsing and non-Android ABI
   refusal must remain testable in the normal renderer fixture. Fresh process
   per value, as with the other cached startup controls. */
static int run_lsfg_probe(const char *value) {
    if (!strcmp(value,"unset")) unsetenv("TOTK_LSFG_PROBE");
    else setenv("TOTK_LSFG_PROBE",value,1);
    int enabled=!strcmp(value,"1");
    CHECK(vk_fg_lsfg_probe_requested()==enabled,"only literal1 opts into LSFG diagnostics (%s)",value);
    VkCommandBuffer recorded=(VkCommandBuffer)(uintptr_t)0x1111;
    VkCommandBuffer primary[]={(VkCommandBuffer)(uintptr_t)0x2222,recorded};
    uint64_t actual=0;
    CHECK(!vk_fg_lsfg_probe_assign(&actual,recorded,9,primary,1,9,77) && !actual,
        "unrelated sealed CB cannot receive the recorded generation's ticket");
    CHECK(!vk_fg_lsfg_probe_assign(&actual,recorded,9,primary,2,10,77) && !actual,
        "reused CB handle in a later recording incarnation remains unknown");
    CHECK(!vk_fg_lsfg_probe_assign(&actual,recorded,9,primary,2,9,0) && !actual,
        "missing actual ticket is not accepted as a successful assignment");
    CHECK(vk_fg_lsfg_probe_assign(&actual,recorded,9,primary,2,9,77) && actual==77,
        "actual multi-primary membership and incarnation assign the exact sealed ticket");
    CHECK(!vk_fg_lsfg_probe_assign(&actual,recorded,9,primary,2,9,88) && actual==77,
        "duplicate/later notifications never replace an already assigned ticket");
    VkImage image=(VkImage)(uintptr_t)0x1234;
    VkImageView view=(VkImageView)(uintptr_t)0x5678;
    CHECK(!vk_fg_lsfg_capture(VK_NULL_HANDLE,0,VK_NULL_HANDLE,VK_NULL_HANDLE,VK_FORMAT_R8_UNORM,64,64),
        "host stub cannot capture or allocate LSFG resources");
    CHECK(!vk_fg_lsfg_interpolate(0,1,77,.5f,NULL,&image,&view) &&
        image==(VkImage)(uintptr_t)0x1234 && view==(VkImageView)(uintptr_t)0x5678,
        "host refusal leaves borrowed output handles untouched");
    const float phases[]={0.0f,nextafterf(0.0f,1.0f),.125f,.3f,.5f,.75f,nextafterf(1.0f,0.0f),1.0f,
        NAN,INFINITY,-INFINITY,-.001f,1.001f};
    for (unsigned i=0;i<sizeof phases/sizeof phases[0];i++) {
        CHECK(!vk_fg_lsfg_interpolate(0,1,77,phases[i],NULL,&image,&view) &&
            !vk_fg_qcom_extrapolate(0,1,0,77,phases[i],&image,&view) &&
            image==(VkImage)(uintptr_t)0x1234 && view==(VkImageView)(uintptr_t)0x5678,
            "backend host stubs reject fractional generation without touching output handles");
    }
    vk_fg_qcom_complete(0);
    CHECK(!vk_fg_qcom_pending(),"host QCOM completion does not create output ownership");
    vk_fg_lsfg_submitting((VkCommandBuffer)(uintptr_t)0x9abc);
    vk_fg_lsfg_submitted(77);
    vk_fg_lsfg_sealed(primary,2,77);
    CHECK(!vk_fg_lsfg_pending(),"diagnostic seal notification cannot create pending work on host");
    const char *tmp=getenv("TMPDIR");
    char directory[1024];
    int length=snprintf(directory,sizeof directory,"%s/totk-lsfg-probe-XXXXXX",
        tmp && *tmp ? tmp : ".");
    CHECK(length>0 && (size_t)length<sizeof directory,"diagnostic scratch path fits");
    if (length<=0 || (size_t)length>=sizeof directory) return 1;
    char *created=mkdtemp(directory);
    CHECK(created!=NULL,"create private diagnostic receipt fixture directory");
    if (!created) return 1;
    CHECK(!vk_fg_lsfg_probe_dump(0,77,directory),"unsupported host snapshots never claim success");
    char path[1200]; snprintf(path,sizeof path,"%s/lsfg-probe.txt",directory);
    FILE *file=fopen(path,"r");
    CHECK(!!file==enabled,"strict-off writes no receipt; opted-in unsupported capture writes an error receipt");
    if (file) {
        char receipt[2048];
        size_t count=fread(receipt,1,sizeof receipt-1,file); receipt[count]=0;
        CHECK(!ferror(file) && count<sizeof receipt-1,"read diagnostic receipt without truncation/error");
        CHECK(strstr(receipt,"dumped-frame=77") && strstr(receipt,"status=error") &&
            strstr(receipt,"unsupported outside Android app"),
            "unsupported receipt preserves selected frame and explicit failure, never fake pixels");
        fclose(file);
        CHECK(!unlink(path),"remove diagnostic fixture receipt");
    }
    CHECK(!rmdir(directory),"strict-off or unsupported diagnostic leaves no other files");
    return failures ? 1 : 0;
}

#include "framegen_test_writer.h"
/* Startup-only JSON probe for the frozen legacy gate matrix. Deliberately do
 * not initialize Vulkan or mutate any environment before reading production
 * policy; every matrix row runs in its own process to exercise cached gates. */
static int run_fg_gate(void) {
    int enabled=!!vk_fg_enabled();
    printf("FG_GATE {\"enabled\":%d,\"interpolating\":%d,\"locked\":%d,"
        "\"capturing\":%d,\"lead_refreshes\":%u,\"hybrid_selected\":%d,\"adaptive\":%d}\n",
        enabled,!!vk_fg_interpolating(),!!vk_fg_locked(),!!vk_fg_capturing(),
        vk_fg_lead_refreshes(),!!runtime_fps_hybrid_selected(),!!vk_fg_adaptive());
    return 0;
}

/* Owned shaders use the same salt40/41 seed convention as test_vknativeui.
 * Only their TEST-only classification metadata is nominated; all attachments,
 * descriptors, draw emission, packet freezing and GPU work remain production. */
static void adaptive_shader_binary(int stage,int salt,uint8_t control[0x880],uint8_t code[0x230]) {
    memset(control,0,0x880); memset(code,salt,0x30);
    for (unsigned i=0;i<0x200;i++) code[0x30+i]=(uint8_t)(i*7+salt);
    uint32_t length=0x230,control_size=0x880;
    memcpy(control+0x20,&control_size,4); memcpy(control+0x700,&length,4);
    memcpy(control+0x704,&length,4); memcpy(control+0x714,&stage,4);
}
static void adaptive_walker_draw(prog_t *program,const tex_t *target,unsigned texture,
        int x,int y,unsigned width,unsigned height) {
    p_set_rt rt={.n=1,.color={target}}; vk_draw_exec(OP_SET_RT,&rt);
    p_viewports vp={.n=1,.v={{0,0,640,360,0,1}}}; vk_draw_exec(OP_VIEWPORTS,&vp);
    p_scissor sc={x,y,width,height}; vk_draw_exec(OP_SCISSOR,&sc);
    p_bind_handles handles={.stage=1,.n=1,.h={texture|(1u<<20)}};
    vk_draw_exec(OP_BIND_TEX,&handles);
    p_bind_program bind={program,3}; vk_draw_exec(OP_BIND_PROGRAM,&bind);
    p_draw draw={.prim=4,.count=3,.instances=1}; vk_draw_exec(OP_DRAW,&draw);
}
static PFN_vkCmdBeginRendering adaptive_post_begin;
static VkCommandBuffer adaptive_post_expected;
static unsigned adaptive_post_calls,adaptive_post_wrong;
static void VKAPI_CALL adaptive_post_trace(VkCommandBuffer cb,const VkRenderingInfo *info) {
    adaptive_post_calls++; adaptive_post_wrong+=cb!=adaptive_post_expected;
    adaptive_post_begin(cb,info);
}
static void adaptive_smaa_split(vk_tex *window) {
    vk_post_output native={0},post={0};
    CHECK(vk_post_enabled() && vk_tex_native_output(window,&native),
        "external split regression uses an actual native real terminal and enabled final SMAA");
    if (!native.image) return;
    VkSemaphore semaphore;
    VkSemaphoreCreateInfo info={VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VKCHECK(vkCreateSemaphore(V.dev,&info,NULL,&semaphore));
    host_buffer readback=host_create((size_t)native.w*native.h*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    mtl_sub_begin();
    VkCommandBuffer prefix=vk_cb(); vk_barrier_all(prefix);
    vk_submit_external_enable();
    uint64_t prefix_ticket=vk_submit_signal(semaphore);
    CHECK(prefix_ticket && !vk_submit_external_disable(),
        "real external prefix seals while its arena/semaphore lease remains owned");
    vk_submit_wait(semaphore);
    VkCommandBuffer suffix=vk_cb();
    CHECK(suffix && suffix!=prefix,
        "external producer split requires a fresh primary before recording synthetic/post commands");
    adaptive_post_expected=suffix; adaptive_post_calls=adaptive_post_wrong=0;
    adaptive_post_begin=vkCmdBeginRendering; vkCmdBeginRendering=adaptive_post_trace;
    int filtered=vk_post_run(suffix,0,vk_tex_native_view(window,NULL,0),
        native.w,native.h,0,0,native.w,native.h,native.w,native.h,0,&post);
    vkCmdBeginRendering=adaptive_post_begin;
    CHECK(filtered && post.image && adaptive_post_calls==3 && !adaptive_post_wrong,
        "all three actual SMAA render passes record in the post-split primary, never the sealed producer");
    if (filtered && post.image) {
        image_barrier(suffix,post.image,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy copy={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
            .imageExtent={post.w,post.h,1}};
        vkCmdCopyImageToBuffer(suffix,post.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback.buffer,1,&copy);
        image_barrier(suffix,post.image,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL);
    }
    uint64_t covering=mtl_sub_commit();
    CHECK(covering>prefix_ticket && mtl_wait(covering,UINT64_C(10000000000)),
        "SMAA suffix and its covering external lease actually retire on the GPU");
    vk_post_complete(0);
    if (filtered && post.image) {
        const uint8_t *pixels=host_map(&readback);
        CHECK(abs((int)pixels[0]-32)<=1 && abs((int)pixels[1]-64)<=1 &&
            abs((int)pixels[2]-128)<=1 && pixels[3]==255,
            "actual post-split real-terminal SMAA readback retains the flat scene corner");
        vkUnmapMemory(V.dev,readback.memory);
    }
    CHECK(vk_submit_external_disable(),"external mode drains only after its covering post-SMAA submission");
    host_destroy(&readback); vkDestroySemaphore(V.dev,semaphore,NULL);
}
static int run_adaptive_capture(void) {
    const char *zero[]={"TOTK_UNCAPPED","TOTK_FPS_AUTO","TOTK_ASYNC_REPLAY","TOTK_ACQUIRE_SLACK",
        "TOTK_ACQUIRE_CLOCK","TOTK_PACE","TOTK_MFX","DM_DETCLOCK","TOTK_VK_COMPILE_THREADS"};
    for (unsigned i=0;i<sizeof zero/sizeof zero[0];i++) setenv(zero[i],"0",1);
    setenv("TOTK_FRAMEGEN","adaptive",1); setenv("TOTK_FRAMEGEN_ENGINE","qcom",1);
    setenv("TOTK_FPS60","1",1); setenv("TOTK_REALTIME","1",1);
    setenv("TOTK_NATIVE_UI","1",1); setenv("TOTK_PRESENT_SLOTS","1",1);
    setenv("DM_PRESENT_INTERVAL","game",1); setenv("DM_PRESENT_HZ","60",1);
    setenv("TOTK_POST_AA","smaa",1); setenv("TOTK_POST_SCALE","off",1);
    setenv("TOTK_SCENE_AA","off",1); setenv("TOTK_SCENE_SCALE","off",1);
    setenv("TOTK_VK_CHUNK_DRAWS","0",1); unsetenv("TOTK_FRAMEGEN_BUDGET_MS");
    W=DW=640; H=DH=360;
    mtl_init();
    CHECK(vk_fg_adaptive() && vk_fg_enabled() && vk_fg_capturing() && vk_native_ui_enabled() && vk_canvas_on,
        "exact adaptive selects real extraction/native UI/canvas startup through production configuration");
    CHECK(vk_fg_qcom_requested() && !vk_fg_qcom_active(),
        "host EGL/AHB refusal stays genuine: this fixture never claims AFME-generated pixels");
    vk_native_window_size(960,540);
    pool_t pool={.size=32u<<20,.flags=NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS|NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    pool.mem=aligned_alloc(4096,pool.size);
    CHECK(pool.mem!=NULL,"allocate owned640-wide walker fixture"); if (!pool.mem) exit(1);
    mtl_pool_add(pool.mem,pool.size);
    tex_t canvas,depth,window,hud;
    /* Production capture accepts floating pre-UI scene formats, not an
       already display-encoded RGBA8 terminal. Keep the real window RGBA8. */
    texture_desc(&canvas,&pool,0,0x2e,82501,640,360);
    texture_desc(&depth,&pool,4u<<20,0x34,82502,640,360);
    texture_desc(&window,&pool,8u<<20,0x25,82503,640,360);
    texture_desc(&hud,&pool,12u<<20,0x25,82504,24,24);
    uint8_t control[2][0x880]; uint8_t *code=(uint8_t *)pool.mem+(16u<<20);
    adaptive_shader_binary(0,40,control[0],code);
    adaptive_shader_binary(1,41,control[1],code+0x1000);
    prog_t program={.serial=82510,.nstage=2};
    program.sd[0].data=(uintptr_t)code; program.sd[0].control=control[0];
    program.sd[1].data=(uintptr_t)(code+0x1000); program.sd[1].control=control[1];
    float vertices[]={-1,-1,3,-1,-1,3};
    memcpy((uint8_t *)pool.mem+(17u<<20),vertices,sizeof vertices);
    mtl_reg_tex(1,&hud,NULL); mtl_reg_tex(2,&canvas,NULL);
    samp_par sampler={.lod_max=1000}; mtl_reg_samp(1,&sampler);
    mtl_xv_window_tex(window.serial);
    mtl_sub_begin();
    uint8_t pixels[24*24*4];
    for (unsigned i=0;i<24*24;i++) {
        pixels[4*i]=(i&1)?255:0; pixels[4*i+1]=(i&1)?0:255; pixels[4*i+2]=0; pixels[4*i+3]=255;
    }
    NVNcopyRegion region={0,0,0,24,24,1};
    mtl_write_texels(&hud,0,0,&region,pixels,24*4,sizeof pixels);
    p_bind_vattr attr={.n=1}; va_st value={0x16,0,0}; memcpy(&attr.st[0],&value,4);
    vk_draw_exec(OP_BIND_VATTR,&attr);
    p_bind_vstream stream={.n=1}; stream.st[0].stride=8; vk_draw_exec(OP_BIND_VSTREAM,&stream);
    p_bind_vbuf buffer={.n=1,.r={{(uintptr_t)((uint8_t *)pool.mem+(17u<<20)),sizeof vertices}}};
    vk_draw_exec(OP_BIND_VBUF,&buffer);
    CHECK(vk_fg_test_canvas_programs(&program,&program),"explicit owned shader identities are nominated only for this TEST process");
    /* Bootstrap known canvas identity from an actual emitted transfer. This
       setup submission is not a guest present/frame-end or a failed probe. */
    adaptive_walker_draw(&program,&canvas,1,0,0,640,360);
    adaptive_walker_draw(&program,&window,2,0,0,640,360);
    vk_end_rendering();
    uint64_t ticket=mtl_sub_commit();
    CHECK(mtl_wait(ticket,UINT64_C(10000000000)),"owned terminal-learning draws retire");
    vk_native_frame_end(); vk_canvas_frame_end();
    uint64_t previous[8]; vk_fg_test_lifecycle(previous);
    float camera[84]; fg_test_camera(camera,0,0,0,0);
    /* The shared camera generator deliberately describes its320x180 golden;
       this actual walker fixture has a640x360 depth attachment. Projection
       aspect is unchanged, but camera extent authority must match that image. */
    camera[72]=(float)DW; camera[73]=(float)DH;
    CHECK(fg_math_camera(camera,DW,DH)==FG_MATH_OK &&
        vk_tex_format(vk_tex_get(&canvas))==VK_FORMAT_R32G32B32A32_SFLOAT,
        "walker capture inputs use the real depth extent and supported floating pre-UI scene format");
    for (unsigned frame=0;frame<4;frame++) {
        mtl_sub_begin();
        p_set_rt rt={.n=1,.color={&canvas},.depth=&depth}; vk_draw_exec(OP_SET_RT,&rt);
        p_viewports vp={.n=1,.v={{0,0,640,360,0,1}}}; vk_draw_exec(OP_VIEWPORTS,&vp);
        p_scissor sc={0,0,640,360}; vk_draw_exec(OP_SCISSOR,&sc);
        p_clear_color clear={0,15,{.125f,.25f,.5f,1}}; vk_draw_exec(OP_CLEAR_COLOR,&clear);
        p_clear_ds z={.depth_mask=1,.depth=fg_test_depth(2)}; vk_draw_exec(OP_CLEAR_DS,&z);
        uint64_t before[8]; vk_fg_test_lifecycle(before);
        CHECK(before[0] && before[1],
            "actual walker pass entry activates exact adaptive extraction before any synthetic root hook");
        vk_fg_test_scene_begin(&canvas,&depth,camera); /* Owned camera/root input, not a copied gate. */
        uint64_t passes_before[4],passes_after[4]; vk_fg_test_pass_counts(passes_before);
        adaptive_walker_draw(&program,&canvas,1,12,12,32,32);
        adaptive_walker_draw(&program,&window,2,0,0,640,360);
        vk_fg_test_present_begin(&window); /* Shared production present selector/extraction/replay. */
        uint64_t captured[8]; vk_fg_test_lifecycle(captured);
        vk_fg_test_pass_counts(passes_after);
        CHECK(captured[0] && captured[3] && captured[4] && captured[7]==3,
            "actual adaptive walker captures source ancestry and both native UI layers before present end");
        CHECK(passes_after[1]-passes_before[1]==2 && passes_after[0]==passes_before[0] &&
            passes_after[3]==passes_before[3],
            "host-refused AFME still records two real UI captures, no fake warp/generated output");
        vk_post_output untouched,output; memset(&output,0xa5,sizeof output); untouched=output;
        CHECK(!vk_fg_generate_phase(vk_cb(),0,960,540,0,.3f,&output) && !memcmp(&output,&untouched,sizeof output),
            "unavailable host AFME refuses fractional generation without manufacturing a result");
        if (frame==2) {
            uint64_t now=host_now_ns();
            runtime_fps_adaptive_frame stamp={0};
            for (unsigned i=0;i<450;i++) {
                now+=25000000;
                runtime_fps_adaptive_acquire(now,16666667,1);
                runtime_fps_adaptive_step(now,.025f);
                stamp=runtime_fps_adaptive_present(now+1,4000,0);
            }
            CHECK(stamp.eligible && stamp.sim_ns,"dropped-real regression begins with genuine mature runtime publication");
            vk_fg_test_present_cancel(); /* Same production dropped-real cancellation path. */
            runtime_fps_adaptive_frame reset=runtime_fps_adaptive_snapshot();
            CHECK(reset.epoch!=stamp.epoch && !reset.eligible && !reset.sim_ns,
                "a dropped real invalidates runtime epoch/phase, not merely its presentation bit");
        }
        ticket=mtl_sub_commit(); vk_fg_test_present_end(ticket);
        CHECK(mtl_wait(ticket,UINT64_C(10000000000)),"actual source/UI/terminal capture covering fence retires");
        uint64_t after[8]; vk_fg_test_lifecycle(after);
        CHECK(after[2]==previous[2]+1 && after[6]==previous[6]+1,
            "shared production frame-end selector advances extractor and budget once per real, even a dropped one");
        CHECK(frame==2 ? !after[5] : after[5] && after[7]==3,
            "dropped real clears temporal source history while accepted real retains captured UI history");
        memcpy(previous,after,sizeof previous);
        if (frame==3) adaptive_smaa_split(vk_tex_get(&window));
        vk_native_frame_end(); vk_canvas_frame_end();
    }
    CHECK(vk_fg_test_canvas_programs(NULL,NULL),"owned synthetic shader nominations restore original classification");
    vk_gpu_idle();
    mtl_tex_release(&canvas); mtl_tex_release(&depth); mtl_tex_release(&window); mtl_tex_release(&hud);
    mtl_pool_del(pool.mem); free(pool.mem);
    printf("adaptive capture: actual640-wide walker/nativeUI/history/frame-end, host AFME refusal, SMAA external suffix and dropped-real reset; no AFME output claim\n");
    return failures ? 1 : 0;
}

/* r5 production-walker corpus. Unlike test_vkframegen_prepared's deliberately
 * hand-authored packets, every terminal descriptor/vertex/index here is obtained
 * by OP_DRAW_IDX -> bind_stage -> draw_buffers -> production extraction. Only
 * shader classification/camera nomination and the external AFME bridge are TEST
 * inputs. Adaptive admission, accounting and source/recorder leases are real. */
typedef struct {
    pool_t pool;
    tex_t canvas,depth,window,hud;
    prog_t program;
    uint8_t control[2][0x880];
    uint64_t vertex,index,uniform;
    int readonly;
    unsigned geometry;
    int index_overlay;
    float camera[84];
} terminal_walker;
static const float terminal_vertices[6]={-1,-1,3,-1,-1,3};
static const uint16_t terminal_indices[3]={0,1,2};
static const float terminal_tint[4]={.75f,.5f,1,1};
enum { TERMINAL_U16,TERMINAL_U32,TERMINAL_U8,TERMINAL_U8_WIDENED,TERMINAL_QUAD };
static const float terminal_quad_vertices[8]={-1,-1,1,-1,1,1,-1,1};
static uint64_t terminal_index_address(const terminal_walker *f) {
    return f->geometry?f->index+256:f->index;
}
static unsigned terminal_index_bytes(const terminal_walker *f) {
    return f->geometry==TERMINAL_U32?12:
        f->geometry==TERMINAL_U8 || f->geometry==TERMINAL_U8_WIDENED?4:6;
}

static void terminal_inline_range(uint64_t address,const void *value,unsigned bytes) {
    struct {
        op_hdr update;
        uint64_t dst,n;
        uint8_t value[16];
        op_hdr end;
    } commands={.update={OP_UPDATE_UB,40},.dst=address,.n=bytes,.end={OP_END,8}};
    CHECK(bytes && bytes<=sizeof commands.value,"bounded inline fixture payload fits its real OP_UPDATE_UB packet");
    if (!bytes || bytes>sizeof commands.value) exit(1);
    memcpy(commands.value,value,bytes);
    mtl_exec((const uint8_t *)&commands);
}
static void terminal_inline(uint64_t address,const float value[4]) {
    terminal_inline_range(address,value,16);
}
static void terminal_guest_fill(terminal_walker *f,int changed) {
    void *vertices=(void *)(uintptr_t)f->vertex,*indices=(void *)(uintptr_t)f->index;
    void *uniform=(void *)(uintptr_t)f->uniform;
    dm_guest_write_prepare(vertices,sizeof terminal_vertices);
    dm_guest_write_prepare(indices,sizeof terminal_indices);
    dm_guest_write_prepare(uniform,16);
    memcpy(vertices,terminal_vertices,sizeof terminal_vertices);
    memcpy(indices,terminal_indices,sizeof terminal_indices);
    const float cpu_tint[4]={.125f,.125f,.125f,1};
    memcpy(uniform,cpu_tint,sizeof cpu_tint);
    if (changed) {
        /* Genuinely different later CPU geometry, not merely a dirty marker. */
        const float triangle[6]={-.75f,-.75f,.75f,-.75f,-.75f,.75f};
        const uint16_t reverse[3]={2,1,0};
        memcpy(vertices,triangle,sizeof triangle); memcpy(indices,reverse,sizeof reverse);
    }
}
static void terminal_walker_draw(terminal_walker *f,const tex_t *target,
        unsigned texture,int x,int y,unsigned width,unsigned height,const float tint[4]) {
    p_set_rt rt={.n=1,.color={target}}; vk_draw_exec(OP_SET_RT,&rt);
    p_viewports vp={.n=1,.v={{0,0,640,360,0,1}}}; vk_draw_exec(OP_VIEWPORTS,&vp);
    p_scissor sc={x,y,width,height}; vk_draw_exec(OP_SCISSOR,&sc);
    p_bind_handles handles={.stage=1,.n=1,.h={texture|(1u<<20)}};
    vk_draw_exec(OP_BIND_TEX,&handles);
    p_bind_program bind={&f->program,3}; vk_draw_exec(OP_BIND_PROGRAM,&bind);
    if (f->readonly) {
        p_bind_buf uniform={1,0,f->uniform,16}; /* fp_c3 maps to guest cbuf0 */
        vk_draw_exec(OP_BIND_UBO,&uniform);
        /* CPU contains .125; this ordered inline value must be frozen instead. */
        terminal_inline(f->uniform,tint);
    }
    int final=target==&f->window;
    uint64_t index=final?terminal_index_address(f):f->index;
    unsigned kind=final?f->geometry:TERMINAL_U16;
    if (final && f->index_overlay && kind!=TERMINAL_QUAD) {
        uint32_t indices32[3]={0,2,1};
        uint16_t indices16[4]={0,2,1,0};
        uint8_t indices8[4]={0,2,1,0xff};
        const void *values=kind==TERMINAL_U32?(const void *)indices32:
            kind==TERMINAL_U16?(const void *)indices16:(const void *)indices8;
        unsigned bytes=(terminal_index_bytes(f)+3)&~3u;
        terminal_inline_range(index,values,bytes);
    }
    if (kind==TERMINAL_QUAD) {
        p_bind_vbuf buffer={.n=1,.r={{f->vertex+256,sizeof terminal_quad_vertices}}};
        vk_draw_exec(OP_BIND_VBUF,&buffer);
        p_draw draw={.prim=7,.count=4,.instances=1};
        vk_draw_exec(OP_DRAW,&draw);
    } else {
        int u8=kind==TERMINAL_U8 || kind==TERMINAL_U8_WIDENED;
        p_draw_idx draw={.prim=u8?5:4,.itype=kind==TERMINAL_U32?2:u8?0:1,
            .count=u8?4:3,.instances=1,.ibuf=index};
        vk_draw_exec(OP_DRAW_IDX,&draw);
    }
}
static void terminal_walker_create(terminal_walker *f,int readonly) {
    memset(f,0,sizeof *f); f->readonly=readonly;
    f->pool=(pool_t){.size=32u<<20,
        .flags=NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS|NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    f->pool.mem=aligned_alloc(4096,f->pool.size);
    CHECK(f->pool.mem!=NULL,"allocate owned terminal walker pool");
    if (!f->pool.mem) exit(1);
    mtl_pool_add(f->pool.mem,f->pool.size);
    texture_desc(&f->canvas,&f->pool,0,0x2e,82601,640,360);
    texture_desc(&f->depth,&f->pool,4u<<20,0x34,82602,640,360);
    texture_desc(&f->window,&f->pool,8u<<20,0x25,82603,640,360);
    texture_desc(&f->hud,&f->pool,12u<<20,0x25,82604,24,24);
    uint8_t *code=(uint8_t *)f->pool.mem+(16u<<20);
    adaptive_shader_binary(0,readonly?44:40,f->control[0],code);
    adaptive_shader_binary(1,readonly?45:41,f->control[1],code+0x1000);
    f->program=(prog_t){.serial=82610,.nstage=2};
    f->program.sd[0].data=(uintptr_t)code; f->program.sd[0].control=f->control[0];
    f->program.sd[1].data=(uintptr_t)(code+0x1000); f->program.sd[1].control=f->control[1];
    f->vertex=(uintptr_t)((uint8_t *)f->pool.mem+(17u<<20));
    f->index=f->vertex+4096; f->uniform=f->vertex+8192;
    terminal_guest_fill(f,0);
    mtl_reg_tex(1,&f->hud,NULL); mtl_reg_tex(2,&f->canvas,NULL);
    samp_par sampler={.lod_max=1000}; mtl_reg_samp(1,&sampler);
    mtl_xv_window_tex(f->window.serial);
    vk_native_window_size(960,540);
    mtl_sub_begin();
    uint8_t pixels[24*24*4];
    for (unsigned i=0;i<24*24;i++) {
        pixels[4*i]=(i&1)?255:0; pixels[4*i+1]=(i&1)?0:255;
        pixels[4*i+2]=0; pixels[4*i+3]=255;
    }
    NVNcopyRegion region={0,0,0,24,24,1};
    mtl_write_texels(&f->hud,0,0,&region,pixels,24*4,sizeof pixels);
    p_bind_vattr attr={.n=1}; va_st value={0x16,0,0}; memcpy(&attr.st[0],&value,4);
    vk_draw_exec(OP_BIND_VATTR,&attr);
    p_bind_vstream stream={.n=1}; stream.st[0].stride=8; vk_draw_exec(OP_BIND_VSTREAM,&stream);
    p_bind_vbuf buffer={.n=1,.r={{f->vertex,sizeof terminal_vertices}}};
    vk_draw_exec(OP_BIND_VBUF,&buffer);
    CHECK(vk_fg_test_canvas_programs(&f->program,&f->program),
        "nominate only owned shader classification, never terminal payloads");
    const float white[4]={1,1,1,1};
    terminal_walker_draw(f,&f->canvas,1,0,0,640,360,white);
    terminal_walker_draw(f,&f->window,2,0,0,640,360,terminal_tint);
    vk_end_rendering();
    CHECK(mtl_wait(mtl_sub_commit(),UINT64_C(10000000000)),"actual indexed terminal-learning draws retire");
    vk_native_frame_end(); vk_canvas_frame_end();
    fg_test_camera(f->camera,0,0,0,0);
    f->camera[72]=(float)DW; f->camera[73]=(float)DH;
    CHECK(fg_math_camera(f->camera,DW,DH)==FG_MATH_OK,"owned walker camera matches actual depth extent");
}
static int terminal_unknown_writer; /* --adaptive-lean only */
static void terminal_walker_open(terminal_walker *f,unsigned gpu_writer,int changed) {
    terminal_guest_fill(f,changed);
    mtl_sub_begin();
    p_bind_vbuf buffer={.n=1,.r={{f->vertex,sizeof terminal_vertices}}};
    vk_draw_exec(OP_BIND_VBUF,&buffer);
    p_set_rt rt={.n=1,.color={&f->canvas},.depth=&f->depth}; vk_draw_exec(OP_SET_RT,&rt);
    p_viewports vp={.n=1,.v={{0,0,640,360,0,1}}}; vk_draw_exec(OP_VIEWPORTS,&vp);
    p_scissor sc={0,0,640,360}; vk_draw_exec(OP_SCISSOR,&sc);
    p_clear_color clear={0,15,{.125f,.25f,.5f,1}}; vk_draw_exec(OP_CLEAR_COLOR,&clear);
    p_clear_ds z={.depth_mask=1,.depth=fg_test_depth(2)}; vk_draw_exec(OP_CLEAR_DS,&z);
    vk_fg_test_scene_begin(&f->canvas,&f->depth,f->camera);
    if (terminal_unknown_writer) {
        /* Production extractor admission of an unproven scene depth writer:
         * ordered GPU spans with lean off, CPU classification only with lean on. */
        const VkRect2D rect={{90,65},{24,18}};
        CHECK(vk_fg_test_authority_draw_begin(&f->canvas,&f->depth,f->camera,0,&rect),
            "unknown scene depth writer keeps its CPU authority classification");
        vk_fg_test_authority_draw_end();
    }
    const float white[4]={1,1,1,1};
    terminal_walker_draw(f,&f->canvas,1,12,12,32,32,white);
    if (gpu_writer) {
        vk_end_rendering(); vk_barrier_all(vk_cb());
        uint64_t address=gpu_writer==VK_FG_TERMINAL_INPUT_VERTEX?f->vertex:terminal_index_address(f);
        VkDeviceSize bytes=gpu_writer==VK_FG_TERMINAL_INPUT_VERTEX?
            sizeof terminal_vertices:terminal_index_bytes(f);
        VkBuffer buffer; uint64_t offset,available;
        int resolved=vk_resolve_span(address,&buffer,&offset,&available);
        CHECK(resolved && available>=bytes,"pending producer resolves the actually consumed pool range");
        if (resolved && available>=bytes) {
            /* vkCmdUpdateBuffer requires a four-byte size. Padding does not
             * change the original raw index range the optional snapshot owns. */
            uint32_t words[6]={0};
            memcpy(words,(const void *)(uintptr_t)address,(size_t)bytes);
            VkDeviceSize written=(bytes+3)&~(VkDeviceSize)3;
            vkCmdUpdateBuffer(vk_cb(),buffer,offset,written,words);
            vk_gpu_write(address,written);
            vk_barrier_all(vk_cb());
        }
    }
    terminal_walker_draw(f,&f->window,2,0,0,640,360,terminal_tint);
    vk_fg_test_present_begin(&f->window);
    uint64_t life[8]; vk_fg_test_lifecycle(life);
    CHECK(life[3] && life[4] && life[7]==3,
        "actual bound stream extracts a source, pre-UI scene and both native-UI layers");
}
static uint64_t terminal_walker_close(void) {
    uint64_t ticket=mtl_sub_commit(); vk_fg_test_present_end(ticket);
    CHECK(ticket && mtl_wait(ticket,UINT64_C(10000000000)),
        "mandatory real present completes even when optional terminal capture refuses");
    vk_native_frame_end(); vk_canvas_frame_end();
    return ticket;
}
static void terminal_walker_destroy(terminal_walker *f) {
    CHECK(vk_fg_test_canvas_programs(NULL,NULL),"restore original shader classification");
    vk_gpu_idle();
    mtl_tex_release(&f->canvas); mtl_tex_release(&f->depth);
    mtl_tex_release(&f->window); mtl_tex_release(&f->hud);
    mtl_pool_del(f->pool.mem); free(f->pool.mem);
}
typedef struct {
    uint64_t facts[6];
    VkDescriptorBufferInfo buffer;
    VkDescriptorSet sets[2];
    vk_record_draw draw;
    uint8_t *bytes,*scratch;
} terminal_snapshot;
static void terminal_snapshot_read(vk_fg_generation *g,terminal_snapshot *s) {
    memset(s,0,sizeof *s);
    int observed=vk_fg_test_terminal_facts(g,s->facts,&s->buffer,s->sets,&s->draw);
    CHECK(observed && s->facts[5] && s->facts[2] && s->facts[3] && s->facts[4]>=3,
        "production terminal owns written descriptors plus consumed vertex and index copies");
    if (!observed) exit(1);
    s->bytes=malloc((size_t)s->buffer.range); s->scratch=malloc((size_t)s->buffer.range);
    CHECK(s->bytes && s->scratch,"allocate independent frozen-terminal observations");
    if (!s->bytes || !s->scratch) exit(1);
    CHECK(vk_fg_test_terminal_copy(g,s->bytes,(size_t)s->buffer.range),
        "read all actual source-private terminal bytes");
    unsigned inline_found=0;
    for (size_t i=0;i+sizeof terminal_tint<=s->buffer.range;i+=4)
        inline_found+=!memcmp(s->bytes+i,terminal_tint,sizeof terminal_tint);
    CHECK(inline_found,"readonly descriptor froze OP_UPDATE_UB tint, not stale CPU guest bytes");
}
static void terminal_snapshot_save(vk_fg_generation *g,terminal_snapshot *s) {
    terminal_snapshot_read(g,s);
    CHECK((s->draw.vertex_mask&1) && s->draw.vertex[0].buffer==s->buffer.buffer &&
        s->draw.vertex[0].offset<=s->buffer.range &&
        sizeof terminal_vertices<=s->buffer.range-s->draw.vertex[0].offset &&
        !memcmp(s->bytes+s->draw.vertex[0].offset,terminal_vertices,sizeof terminal_vertices),
        "actually consumed pool vertex bytes reached the private terminal binding");
    CHECK(s->draw.index==s->buffer.buffer && s->draw.index_type==VK_INDEX_TYPE_UINT16 &&
        s->draw.index_offset<=s->buffer.range &&
        sizeof terminal_indices<=s->buffer.range-s->draw.index_offset &&
        !memcmp(s->bytes+s->draw.index_offset,terminal_indices,sizeof terminal_indices),
        "actually consumed pool uint16 indices reached the private terminal binding");
}
static void terminal_snapshot_unchanged(vk_fg_generation *g,terminal_snapshot *s) {
    uint64_t facts[6]; VkDescriptorBufferInfo buffer; VkDescriptorSet sets[2]; vk_record_draw draw;
    int observed=vk_fg_test_terminal_facts(g,facts,&buffer,sets,&draw);
    CHECK(observed && !memcmp(facts,s->facts,sizeof facts) &&
        buffer.buffer==s->buffer.buffer && buffer.offset==s->buffer.offset && buffer.range==s->buffer.range &&
        !memcmp(sets,s->sets,sizeof sets) && !memcmp(&draw,&s->draw,sizeof draw),
        "source identity, private descriptors and emitted terminal geometry never migrate");
    CHECK(vk_fg_test_terminal_copy(g,s->scratch,(size_t)s->buffer.range) &&
        !memcmp(s->scratch,s->bytes,(size_t)s->buffer.range),
        "later CPU/inline writes and source/recorder reuse cannot change frozen terminal bytes");
}
static void terminal_ownership(uint64_t refs,uint64_t outputs,const char *label) {
    uint64_t counts[2]; vk_fg_test_job_ownership(counts);
    CHECK(counts[0]==refs && counts[1]==outputs,"%s: actual refs=%llu outputs=%llu",label,
        (unsigned long long)counts[0],(unsigned long long)counts[1]);
}
static void terminal_refusal_report(const char *label,const uint64_t facts[6],
        const vk_fg_terminal_input *input) {
    printf("terminal refusal: case=%s source=%llu role=%u stage=%u set=%u binding=%u element=%u stream=%u buffer=%#llx offset=%llu bytes=%llu freeze=%u ready=%llu failed=%llu\n",
        label,(unsigned long long)facts[0],input->role,input->stage,input->set,input->binding,
        input->element,input->stream,(unsigned long long)(uintptr_t)input->buffer,
        (unsigned long long)input->offset,(unsigned long long)input->bytes,input->freeze,
        (unsigned long long)facts[1],(unsigned long long)facts[2]);
}
static void terminal_real_corner(terminal_walker *f) {
    vk_post_output native={0}; uint8_t pixel[4]={0};
    int observed=vk_tex_native_output(vk_tex_get(&f->window),&native);
    CHECK(observed && native.image && native.w==960 && native.h==540,
        "mandatory original indexed terminal really produced a native output");
    if (observed && native.image)
        read_image_extent(native.image,VK_IMAGE_ASPECT_COLOR_BIT,pixel,sizeof pixel,1,1);
    CHECK(abs((int)pixel[0]-24)<=1 && abs((int)pixel[1]-32)<=1 &&
        abs((int)pixel[2]-128)<=1 && pixel[3]==255,
        "real GPU output consumes both the pool indexed triangle and ordered readonly UBO tint");
}
static vk_fg_generation *terminal_prepare(unsigned slot,unsigned *prepared) {
    vk_fg_generation_stamp stamp={vk_fg_real_frame_id(),1};
    uint64_t ticket=mtl_last_fence(),serial=vk_submit_serial();
    vk_fg_generation *g=vk_fg_generation_prepare(slot,960,540,0,&stamp);
    CHECK(mtl_last_fence()==ticket && vk_submit_serial()==serial,
        "real production-hook preparation allocates no ticket or hidden real-recorder split");
    if (g) {
        (*prepared)++;
        CHECK(vk_fg_generation_state(g)==VK_FG_GENERATION_PREPARING &&
            vk_fg_generation_prepare_cb(g) && vk_fg_generation_prepare_cb(g)!=vk_cb(),
            "engaged terminal records a genuine independent private command stream");
        CHECK(vk_fg_generation_prepare_finish(g),"private terminal stream closes before real seal");
    }
    return g;
}

/* Capture original consumed pool geometry/draw calls, not compiler bytes.
 * ZERO_BINDING16 is unconsumed by this shader and gets a fresh transient offset
 * each recording; compare its verified zero contents, size and stride instead.
 * With no buffer descriptor, any snapshot arena here is an optional side effect. */
static struct {
    PFN_vkCmdBindVertexBuffers2 vertex;
    PFN_vkCmdBindIndexBuffer index;
    PFN_vkCmdDrawIndexed draw;
    uint64_t words[128];
    unsigned n;
} terminal_commands;
static void terminal_command_word(uint64_t word) {
    CHECK(terminal_commands.n<128,"bounded real-command observation fits");
    if (terminal_commands.n<128) terminal_commands.words[terminal_commands.n++]=word;
}
static void VKAPI_CALL terminal_vertex_trace(VkCommandBuffer cb,uint32_t first,uint32_t count,
        const VkBuffer *buffers,const VkDeviceSize *offsets,const VkDeviceSize *sizes,const VkDeviceSize *strides) {
    terminal_command_word(1); terminal_command_word(first); terminal_command_word(count);
    for (unsigned i=0;i<count;i++) {
        if (first+i==16) {
            uint8_t zero[64],expected[64]={0};
            CHECK(sizes && sizes[i]==sizeof zero && strides && !strides[i] &&
                vk_replay_frozen_buffer_copy(buffers[i],offsets[i],sizeof zero,zero) &&
                !memcmp(zero,expected,sizeof zero),"unconsumed transient zero binding stays exactly zero");
            terminal_command_word(0); terminal_command_word(0);
        } else {
            terminal_command_word((uint64_t)(uintptr_t)buffers[i]); terminal_command_word(offsets[i]);
        }
        terminal_command_word(sizes?sizes[i]:UINT64_MAX); terminal_command_word(strides?strides[i]:UINT64_MAX);
    }
    terminal_commands.vertex(cb,first,count,buffers,offsets,sizes,strides);
}
static void VKAPI_CALL terminal_index_trace(VkCommandBuffer cb,VkBuffer buffer,VkDeviceSize offset,VkIndexType type) {
    terminal_command_word(2); terminal_command_word((uint64_t)(uintptr_t)buffer);
    terminal_command_word(offset); terminal_command_word(type);
    terminal_commands.index(cb,buffer,offset,type);
}
static void VKAPI_CALL terminal_draw_trace(VkCommandBuffer cb,uint32_t count,uint32_t instances,
        uint32_t first,int32_t offset,uint32_t base) {
    terminal_command_word(3); terminal_command_word(count); terminal_command_word(instances);
    terminal_command_word(first); terminal_command_word((uint32_t)offset); terminal_command_word(base);
    terminal_commands.draw(cb,count,instances,first,offset,base);
}
static void terminal_flag_off(terminal_walker *f) {
    uint64_t original[128]={0}; unsigned original_n=0;
    size_t bytes=(size_t)960*540*4;
    uint8_t *golden=malloc(bytes),*pixels=malloc(bytes);
    CHECK(golden && pixels,"allocate original-path native terminal goldens");
    if (!golden || !pixels) exit(1);
    for (int freeze=0;freeze<2;freeze++) {
        vk_fg_test_terminal_geometry_freeze(freeze);
        CHECK(!atomic_load_explicit(&vk_fg_terminal_owned_active,memory_order_relaxed) &&
            !vk_record_test_snapshot_mask(),"slots0 begins with no optional capture or snapshot arenas");
        terminal_commands.vertex=vkCmdBindVertexBuffers2;
        terminal_commands.index=vkCmdBindIndexBuffer; terminal_commands.draw=vkCmdDrawIndexed;
        terminal_commands.n=0;
        vkCmdBindVertexBuffers2=terminal_vertex_trace;
        vkCmdBindIndexBuffer=terminal_index_trace; vkCmdDrawIndexed=terminal_draw_trace;
        terminal_walker_open(f,0,0);
        vkCmdBindVertexBuffers2=terminal_commands.vertex;
        vkCmdBindIndexBuffer=terminal_commands.index; vkCmdDrawIndexed=terminal_commands.draw;
        uint64_t current[6];
        CHECK(!vk_fg_test_terminal_current(current,NULL),"slots0 never begins scheduler-owned terminal capture");
        (void)terminal_walker_close();
        CHECK(terminal_commands.n && !vk_record_test_snapshot_mask(),
            "slots0 emits real consumed pool geometry without any optional snapshot allocation");
        vk_post_output native={0};
        CHECK(vk_tex_native_output(vk_tex_get(&f->window),&native) && native.w==960 && native.h==540,
            "slots0 has the real native terminal output");
        if (native.image) read_image_extent(native.image,VK_IMAGE_ASPECT_COLOR_BIT,pixels,bytes,960,540);
        else memset(pixels,0,bytes);
        if (!freeze) {
            original_n=terminal_commands.n; memcpy(original,terminal_commands.words,sizeof original);
            memcpy(golden,pixels,bytes);
        } else {
            CHECK(original_n==terminal_commands.n &&
                !memcmp(original,terminal_commands.words,original_n*sizeof *original),
                "slots0 new-freeze disabled/enabled emits identical original real command arguments and pool bindings");
            CHECK(!memcmp(golden,pixels,bytes),
                "slots0 new-freeze disabled/enabled preserves every GPU-readback terminal byte");
        }
        CHECK(abs((int)pixels[0]-32)<=1 && abs((int)pixels[1]-64)<=1 &&
            abs((int)pixels[2]-128)<=1 && pixels[3]==255,
            "flag-off comparison is non-vacuous: indexed pool triangle actually samples the real scene");
    }
    free(golden); free(pixels);
}

/* Focused actual-bound geometry branches. Each accepted graph is prepared by
 * the same walker/present flow, then the original pool bytes are destroyed
 * before optional dispatch. In interp mode dispatch/readback is mandatory;
 * adaptive retains its genuine predictive admission and may refuse dispatch. */
static void terminal_index_matrix(terminal_walker *f,unsigned slot,int ownership_only,int trace,
        unsigned *prepared,unsigned *refused,unsigned *presents,unsigned *dispatched) {
    const char *names[]={"u16","u32","native-u8","widened-u8","quad-prefix"};
    int native_u8=V.has_index_u8;
    unsigned exercised=0;
    for (unsigned kind=TERMINAL_U16;kind<=TERMINAL_QUAD;kind++) {
        if (kind==TERMINAL_U8 && !native_u8) {
            printf("terminal index: case=native-u8 unavailable=host-capability; no extension fabricated\n");
            continue;
        }
        f->geometry=kind;
        unsigned repeats=kind==TERMINAL_QUAD?1:2;
        for (unsigned overlay=0;overlay<repeats;overlay++) {
            V.has_index_u8=kind==TERMINAL_U8_WIDENED?0:native_u8;
            f->index_overlay=(int)overlay;
            uint64_t address=terminal_index_address(f);
            uint32_t raw32[3]={0,1,2};
            uint16_t raw16[4]={0,1,2,0};
            uint8_t raw8[4]={0,1,2,0xff};
            const void *raw=kind==TERMINAL_U32?(const void *)raw32:
                kind==TERMINAL_U16?(const void *)raw16:(const void *)raw8;
            unsigned raw_bytes=kind==TERMINAL_QUAD?0:terminal_index_bytes(f);
            dm_guest_write_prepare((void *)(uintptr_t)address,16);
            memset((void *)(uintptr_t)address,0,16);
            memcpy((void *)(uintptr_t)address,raw,raw_bytes);
            dm_guest_write_prepare((void *)(uintptr_t)(f->vertex+256),sizeof terminal_quad_vertices);
            memcpy((void *)(uintptr_t)(f->vertex+256),terminal_quad_vertices,sizeof terminal_quad_vertices);
            vk_record_test_counts before,after; vk_record_test_counts_get(&before);
            terminal_walker_open(f,0,0);
            vk_record_test_counts_get(&after);
            if (kind==TERMINAL_U8_WIDENED)
                CHECK(!V.has_index_u8 && after.widened>before.widened,
                    "forced no-index-u8 capability actually emits the production CPU-widening branch");
            vk_fg_generation *g=terminal_prepare(slot,prepared);
            CHECK(g,"actual %s terminal with inline-index=%u prepares a private graph",names[kind],overlay);
            if (!g) { (void)terminal_walker_close(); (*presents)++; continue; }
            terminal_snapshot saved; terminal_snapshot_read(g,&saved);
            uint32_t expected32[6]={0,overlay?2:1,overlay?1:2,0,2,3};
            uint16_t expected16[4]={0,overlay?2:1,overlay?1:2,0xffff};
            uint8_t expected8[4]={0,overlay?2:1,overlay?1:2,0xff};
            VkIndexType expected_type=kind==TERMINAL_U32 || kind==TERMINAL_QUAD?VK_INDEX_TYPE_UINT32:
                kind==TERMINAL_U8?VK_INDEX_TYPE_UINT8_EXT:VK_INDEX_TYPE_UINT16;
            const void *expected=expected_type==VK_INDEX_TYPE_UINT32?(const void *)expected32:
                expected_type==VK_INDEX_TYPE_UINT8_EXT?(const void *)expected8:(const void *)expected16;
            unsigned index_bytes=kind==TERMINAL_QUAD?24:
                expected_type==VK_INDEX_TYPE_UINT32?12:expected_type==VK_INDEX_TYPE_UINT8_EXT?4:
                kind==TERMINAL_U8_WIDENED?8:6;
            CHECK(saved.draw.index==saved.buffer.buffer && saved.draw.index_type==expected_type &&
                saved.draw.count==(kind==TERMINAL_QUAD?6u:
                    kind==TERMINAL_U8 || kind==TERMINAL_U8_WIDENED?4u:3u) &&
                (!(kind==TERMINAL_U8 || kind==TERMINAL_U8_WIDENED) || saved.draw.dynamic.restart) &&
                saved.draw.index_offset<=saved.buffer.range &&
                index_bytes<=saved.buffer.range-saved.draw.index_offset &&
                !memcmp(saved.bytes+saved.draw.index_offset,expected,index_bytes),
                "actual %s frozen terminal stores the precise %s index prefix in its emitted private binding",
                names[kind],overlay?"OP_UPDATE_UB-overlaid":"original");
            const void *vertices=kind==TERMINAL_QUAD?(const void *)terminal_quad_vertices:(const void *)terminal_vertices;
            size_t vertex_bytes=kind==TERMINAL_QUAD?sizeof terminal_quad_vertices:sizeof terminal_vertices;
            CHECK(saved.draw.vertex[0].buffer==saved.buffer.buffer &&
                saved.draw.vertex[0].offset<=saved.buffer.range &&
                vertex_bytes<=saved.buffer.range-saved.draw.vertex[0].offset &&
                !memcmp(saved.bytes+saved.draw.vertex[0].offset,vertices,vertex_bytes),
                "actual %s terminal owns every consumed pool vertex",names[kind]);
            (void)terminal_walker_close(); (*presents)++;
            terminal_real_corner(f);
            dm_guest_write_prepare((void *)(uintptr_t)address,16);
            memset((void *)(uintptr_t)address,0,16);
            if (kind==TERMINAL_QUAD) {
                dm_guest_write_prepare((void *)(uintptr_t)(f->vertex+256),vertex_bytes);
                memset((void *)(uintptr_t)(f->vertex+256),0,vertex_bytes);
            }
            terminal_snapshot_unchanged(g,&saved);
            vk_post_output output={0};
            uint64_t ticket_before=mtl_last_fence();
            vk_rec_lock();
            int result=vk_fg_generation_dispatch(g,.375f,host_now_ns()+UINT64_C(10000000000),&output);
            uint64_t covering=result==1?vk_fg_generation_seal(g):0;
            vk_rec_unlock();
            if (ownership_only) CHECK(result==1,"ownership-only %s graph actually dispatches on the GPU",names[kind]);
            if (result==1) {
                (*dispatched)++;
                CHECK(covering==ticket_before+2 && mtl_wait(covering,UINT64_C(10000000000)),
                    "%s private terminal retires its actual two-ticket signal/consumer range",names[kind]);
                uint8_t pixel[4]={0};
                read_image_extent(output.image,VK_IMAGE_ASPECT_COLOR_BIT,pixel,sizeof pixel,1,1);
                CHECK(abs((int)pixel[0]-24)<=2 && abs((int)pixel[1]-32)<=2 &&
                    abs((int)pixel[2]-128)<=2 && pixel[3]==255,
                    "%s private GPU output survives original pool index/quad-vertex destruction",names[kind]);
                (void)vk_fg_complete(slot);
                CHECK(vk_fg_generation_state(g)==VK_FG_GENERATION_EMPTY && !vk_fg_complete(slot),
                    "%s private output retires exactly once",names[kind]);
            } else {
                CHECK(!ownership_only && !result && mtl_last_fence()==ticket_before,
                    "adaptive %s budget refusal manufactures no GPU work or output",names[kind]);
                vk_rec_lock(); vk_fg_generation_cancel(g); vk_rec_unlock();
            }
            terminal_ownership(0,0,"geometry-matrix output/cancellation releases all source references");
            free(saved.bytes); free(saved.scratch); exercised|=1u<<kind;
            printf("terminal index: case=%s inline-index=%u prepared=1 dispatched=%d raw-bytes=%u frozen-bytes=%u\n",
                names[kind],overlay,result==1,raw_bytes,index_bytes);
        }
        if (kind!=TERMINAL_QUAD) {
            f->index_overlay=0;
            V.has_index_u8=kind==TERMINAL_U8_WIDENED?0:native_u8;
            uint64_t address=terminal_index_address(f);
            uint32_t raw32[3]={0,1,2}; uint16_t raw16[3]={0,1,2}; uint8_t raw8[4]={0,1,2,0xff};
            const void *raw=kind==TERMINAL_U32?(const void *)raw32:
                kind==TERMINAL_U16?(const void *)raw16:(const void *)raw8;
            dm_guest_write_prepare((void *)(uintptr_t)address,terminal_index_bytes(f));
            memcpy((void *)(uintptr_t)address,raw,terminal_index_bytes(f));
            terminal_walker_open(f,VK_FG_TERMINAL_INPUT_INDEX,0);
            uint64_t current[6]={0}; vk_fg_terminal_input refusal={0};
            VkBuffer buffer; uint64_t offset,available;
            int resolved=vk_resolve_span(address,&buffer,&offset,&available);
            CHECK(vk_fg_test_terminal_current(current,&refusal) && current[2] && !current[1] &&
                (!trace || (refusal.role==VK_FG_TERMINAL_INPUT_INDEX && !refusal.freeze && resolved &&
                refusal.buffer==buffer && refusal.offset==offset && refusal.bytes==terminal_index_bytes(f))),
                "%s pending GPU writer refuses the ORIGINAL raw pool index range, not an already-widened temporary",names[kind]);
            if (trace) terminal_refusal_report(names[kind],current,&refusal);
            CHECK(!terminal_prepare(slot,prepared),"%s pending writer prepares no optional graph",names[kind]);
            (void)terminal_walker_close(); (*presents)++; (*refused)++;
            terminal_real_corner(f);
            terminal_ownership(0,0,"geometry-matrix pending writer retains no optional references");
        }
    }
    V.has_index_u8=native_u8; f->geometry=TERMINAL_U16; f->index_overlay=0;
    unsigned expected=(1u<<TERMINAL_U16)|(1u<<TERMINAL_U32)|
        (1u<<TERMINAL_U8_WIDENED)|(1u<<TERMINAL_QUAD)|(native_u8?1u<<TERMINAL_U8:0);
    CHECK(exercised==expected,"every host-supported actual terminal index branch prepared frozen bytes");
}

static int run_terminal_walker(unsigned mode) {
    int negative=mode==1,off=mode==2,ownership_only=mode==3;
    const char *zero[]={"TOTK_UNCAPPED","TOTK_FPS_AUTO","TOTK_ASYNC_REPLAY","TOTK_ACQUIRE_SLACK",
        "TOTK_ACQUIRE_CLOCK","TOTK_PACE","TOTK_MFX","DM_DETCLOCK","TOTK_VK_COMPILE_THREADS",
        "TOTK_VK_CHUNK_DRAWS","TOTK_VK_LAG_CAPTURE"};
    for (unsigned i=0;i<sizeof zero/sizeof zero[0];i++) setenv(zero[i],"0",1);
    setenv("TOTK_FRAMEGEN",ownership_only?"interp":"adaptive",1);
    const char *trace_value=getenv("TOTK_TEST_TERMINAL_TRACE");
    int trace=!(trace_value && !strcmp(trace_value,"0"));
    setenv("TOTK_FRAMEGEN_ENGINE","qcom",1); setenv("TOTK_FRAMEGEN_JOB_TRACE",trace?"1":"0",1);
    setenv("TOTK_FPS60",ownership_only?"0":"1",1); setenv("TOTK_REALTIME",ownership_only?"0":"1",1);
    setenv("TOTK_NATIVE_UI","1",1); setenv("TOTK_PRESENT_SLOTS","1",1);
    setenv("TOTK_FG_ADAPTIVE_SLOTS",off?"0":"1",1);
    setenv("DM_PRESENT_INTERVAL","game",1); setenv("DM_PRESENT_HZ","60",1);
    setenv("TOTK_POST_AA","off",1); setenv("TOTK_POST_SCALE","off",1);
    setenv("TOTK_SCENE_AA","off",1); setenv("TOTK_SCENE_SCALE","off",1);
    if (ownership_only) setenv("TOTK_FRAMEGEN_BUDGET_MS","10000",1);
    else unsetenv("TOTK_FRAMEGEN_BUDGET_MS");
    W=DW=640; H=DH=360;
    mtl_init();
    CHECK(vk_fg_enabled() && vk_fg_capturing() && vk_native_ui_enabled() && vk_canvas_on &&
        (ownership_only?vk_fg_interpolating():vk_fg_adaptive()),
        "production startup admits the actual nativeUI terminal walker configuration");
    CHECK(vk_fg_test_generation_bridge(1),"substitute only unavailable host AFME pixels/external bridge");
    if (!off) CHECK(vk_fg_test_terminal_owned(1),"scheduler owns terminal capture for this TEST stream");
    vk_fg_test_terminal_geometry_freeze(!negative);
    terminal_walker f; terminal_walker_create(&f,!off);
    unsigned prepared=0,refused=0,dispatched=0,presents=0;
    if (off) { terminal_flag_off(&f); presents=2; }
    else {
        const unsigned slot=2;
        /* Warmup is real extraction/history maturation, never synthetic
         * eligibility, fabricated source census or a budget override in adaptive. */
        for (unsigned frame=0;frame<6;frame++) {
            terminal_walker_open(&f,0,0);
            uint64_t current[6]; vk_fg_terminal_input refusal={0};
            int observed=vk_fg_test_terminal_current(current,&refusal);
            CHECK(observed,"production terminal freeze point exposes actual current-source capture facts");
            if (negative && observed) {
                VkBuffer pool_buffer; uint64_t offset,available;
                int resolved=vk_resolve_span(f.vertex,&pool_buffer,&offset,&available);
                CHECK(current[2] && !current[1],"old pool path fails only optional terminal capture");
                if (trace) {
                    CHECK(refusal.role==VK_FG_TERMINAL_INPUT_VERTEX &&
                        refusal.stage==0 && refusal.stream==0 && !refusal.freeze && resolved &&
                        refusal.buffer==pool_buffer && refusal.offset==offset &&
                        refusal.bytes==sizeof terminal_vertices,
                        "old pool path refuses the exact production vertex role/stream/buffer/offset/byte range");
                    terminal_refusal_report("old-pool",current,&refusal);
                }
                refused++;
            }
            vk_fg_generation *g=terminal_prepare(slot,&prepared);
            if (negative) CHECK(!g,"old pool-backed terminal storage prepares zero optional templates");
            if (g) {
                terminal_snapshot saved; terminal_snapshot_save(g,&saved);
                terminal_snapshot_unchanged(g,&saved);
                vk_rec_lock(); vk_fg_generation_cancel(g); vk_rec_unlock();
                CHECK(vk_fg_generation_state(g)==VK_FG_GENERATION_EMPTY,
                    "warmup preparation cancels without a synthetic submission");
                free(saved.bytes); free(saved.scratch);
            }
            (void)terminal_walker_close(); presents++;
            if (frame==5) terminal_real_corner(&f);
            terminal_ownership(0,0,"real warmup leaves no optional references");
        }
        CHECK(negative?!prepared:prepared>0,
            "R4-like actual production-hook stream templates_prepared=%u (expected %s)",
            prepared,negative?"zero":">0");
        if (!negative) {
            size_t bytes=(size_t)960*540*4;
            uint8_t *golden=malloc(bytes),*pixels=malloc(bytes);
            CHECK(golden && pixels,"allocate independent actual private-output readbacks");
            if (!golden || !pixels) exit(1);
            unsigned baseline=0;
            for (unsigned disposition=0;disposition<3;disposition++) {
                terminal_walker_open(&f,0,0);
                vk_fg_generation *g=terminal_prepare(slot,&prepared);
                CHECK(g,"mature real bound stream prepares an immutable terminal graph");
                if (!g) { (void)terminal_walker_close(); presents++; continue; }
                terminal_snapshot saved; terminal_snapshot_save(g,&saved);
                uint64_t real=terminal_walker_close(); presents++;
                uint64_t pin=0,skips=0; unsigned snapshots=~0u;
                CHECK(vk_record_test_replay_pin(real,&pin,&skips) && pin==UINT64_MAX &&
                    vk_record_test_replay_snapshots(real,&snapshots) && !snapshots,
                    "owned production graph pins the real recorder but no snapshot arena");
                unsigned source_slots=0,arena_slots=0,reused=0;
                for (unsigned newer=0;newer<12;newer++) {
                    arena_slots|=1u<<vk_record_test_snapshot_next_index();
                    terminal_walker_open(&f,0,disposition!=0);
                    /* Changed later readonly descriptor bytes are also real
                     * OP_UPDATE_UB, after the already emitted terminal draw. */
                    const float changed[4]={.25f,.75f,.125f,1};
                    terminal_inline(f.uniform,changed);
                    uint64_t current[6];
                    if (vk_fg_test_terminal_current(current,NULL)) {
                        source_slots|=1u<<(unsigned)current[0];
                        reused+=current[0]==saved.facts[0] || current[0]==saved.facts[1];
                    }
                    (void)terminal_walker_close(); presents++;
                    terminal_snapshot_unchanged(g,&saved);
                    terminal_ownership(2,0,"newer real preserves exactly the retained immutable source pair");
                }
                CHECK(source_slots && (source_slots&(source_slots-1)) && !reused && arena_slots==15,
                    "twelve actual reals wrap source and all snapshot/recorder intervals while excluding retained sources");
                if (disposition==2) {
                    CHECK(!vk_fg_test_terminal_owned(0),"disable refuses to detach a retained private graph");
                    vk_fg_reset();
                    vk_rec_lock(); vk_fg_history_cut(); vk_fg_generation_cancel(g); vk_rec_unlock();
                    CHECK(vk_fg_generation_state(g)==VK_FG_GENERATION_EMPTY,
                        "reset/history-cut cancels unsubmitted production template without a covering-ticket fiction");
                } else {
                    uint64_t before=mtl_last_fence(),expected=before+2;
                    vk_record_test_hold_completion(expected);
                    vk_post_output output={0};
                    vk_rec_lock();
                    int result=vk_fg_generation_dispatch(g,.375f,
                        host_now_ns()+UINT64_C(10000000000),&output);
                    uint64_t covering=result==1?vk_fg_generation_seal(g):0;
                    vk_rec_unlock();
                    CHECK(result>=0,"host TEST bridge never claims quarantined external GL work");
                    if (ownership_only) CHECK(result==1,
                        "fixed-interp ownership-only corpus dispatches actual consumed terminal commands");
                    if (result==1) {
                        dispatched++;
                        CHECK(covering==expected && output.image,
                            "actual generation signal and private terminal consume exactly two FIFO tickets");
                        uint64_t deadline=host_now_ns()+UINT64_C(10000000000);
                        while (vk_record_test_completion_held()!=covering && host_now_ns()<deadline) usleep(1000);
                        CHECK(vk_record_test_completion_held()==covering && !mtl_wait(covering,0) &&
                            !vk_fg_generation_retire(g) && !vk_fg_complete(slot),
                            "real delayed covering watermark prevents early output/source retirement");
                        terminal_ownership(2,1,"submitted production terminal retains output and both source references");
                        CHECK(!vk_fg_test_generation_bridge(0) && !vk_fg_test_terminal_owned(0),
                            "disable cannot detach submitted production private storage");
                        terminal_snapshot_unchanged(g,&saved);
                        if (disposition==1) {
                            vk_fg_generation_cancel(g); vk_fg_reset();
                            vk_rec_lock(); vk_fg_history_cut(); vk_rec_unlock();
                            CHECK(vk_fg_generation_state(g)==VK_FG_GENERATION_SUBMITTED,
                                "disable/reset/history loss cannot retire submitted terminal before its actual covering ticket");
                            terminal_ownership(2,1,"reset keeps uncompleted production output/source ownership");
                        }
                        vk_record_test_hold_completion(0);
                        CHECK(mtl_wait(covering,UINT64_C(10000000000)),
                            "actual generated terminal covering GPU ticket retires");
                        read_image_extent(output.image,VK_IMAGE_ASPECT_COLOR_BIT,pixels,bytes,960,540);
                        CHECK(abs((int)pixels[0]-24)<=2 && abs((int)pixels[1]-32)<=2 &&
                            abs((int)pixels[2]-128)<=2 && pixels[3]==255,
                            "private output readback non-vacuously consumes the captured indexed scene and readonly tint");
                        if (!baseline) { memcpy(golden,pixels,bytes); baseline=1; }
                        else CHECK(!memcmp(golden,pixels,bytes),
                            "later CPU vertex/index corruption, inline descriptor updates and ring reuse cannot change actual GPU output");
                        (void)vk_fg_complete(slot); /* Adaptive actual audit may reject over-budget output. */
                        CHECK(vk_fg_generation_state(g)==VK_FG_GENERATION_EMPTY && !vk_fg_complete(slot),
                            "actual covering completion releases production output exactly once");
                    } else {
                        vk_record_test_hold_completion(0);
                        CHECK(!ownership_only && mtl_last_fence()==before &&
                            vk_fg_generation_state(g)==VK_FG_GENERATION_PREPARED,
                            "genuine adaptive budget refusal creates no synthetic ticket or output claim");
                        vk_rec_lock(); vk_fg_generation_cancel(g); vk_rec_unlock();
                    }
                }
                terminal_ownership(0,0,"cancellation or actual covered retirement returns all private ownership");
                free(saved.bytes); free(saved.scratch);
                unsigned source_reused=0;
                for (unsigned newer=0;newer<8;newer++) {
                    terminal_walker_open(&f,0,0);
                    vk_fg_generation *fresh=terminal_prepare(slot,&prepared);
                    uint64_t facts[6]; VkDescriptorBufferInfo buffer;
                    if (fresh && vk_fg_test_terminal_facts(fresh,facts,&buffer,NULL,NULL)) {
                        if (facts[0]==saved.facts[0]) {
                            source_reused++;
                            CHECK(buffer.buffer==saved.buffer.buffer,
                                "released source reuses its same bounded private terminal allocation");
                        }
                    }
                    if (fresh) { vk_rec_lock(); vk_fg_generation_cancel(fresh); vk_rec_unlock(); }
                    (void)terminal_walker_close(); presents++;
                    terminal_ownership(0,0,"post-retirement source/recorder reuse leaves no phantom references");
                }
                CHECK(source_reused,"formerly retained production source is actually reused after covered retirement/cancellation");
            }
            free(golden); free(pixels);
            for (unsigned role=VK_FG_TERMINAL_INPUT_VERTEX;role<=VK_FG_TERMINAL_INPUT_INDEX;role++) {
                terminal_walker_open(&f,role,0);
                uint64_t current[6]={0}; vk_fg_terminal_input refusal={0};
                VkBuffer pool_buffer; uint64_t offset,available;
                uint64_t address=role==VK_FG_TERMINAL_INPUT_VERTEX?f.vertex:f.index;
                int resolved=vk_resolve_span(address,&pool_buffer,&offset,&available);
                CHECK(vk_fg_test_terminal_current(current,&refusal) && current[2] && !current[1] &&
                    (!trace || (refusal.role==role && refusal.stage==0 && !refusal.stream && !refusal.freeze &&
                    resolved && refusal.buffer==pool_buffer && refusal.offset==offset &&
                    refusal.bytes==(role==VK_FG_TERMINAL_INPUT_VERTEX?
                        sizeof terminal_vertices:sizeof terminal_indices))),
                    "actual pending GPU-only %s producer refuses optional capture with exact role/range when tracing",
                    role==VK_FG_TERMINAL_INPUT_VERTEX?"vertex":"index");
                if (trace) terminal_refusal_report("pending-gpu-producer",current,&refusal);
                CHECK(!terminal_prepare(slot,&prepared),"pending GPU-only input never prepares an optional terminal");
                (void)terminal_walker_close(); presents++; refused++;
                terminal_ownership(0,0,"pending-producer refusal cannot retain optional ownership");
                terminal_walker_open(&f,0,0);
                vk_fg_generation *recovery=terminal_prepare(slot,&prepared);
                CHECK(recovery,"a pending-writer refusal poisons only its optional interval, not the next real capture");
                if (recovery) { vk_rec_lock(); vk_fg_generation_cancel(recovery); vk_rec_unlock(); }
                (void)terminal_walker_close(); presents++;
                terminal_real_corner(&f);
                terminal_ownership(0,0,"pending-producer recovery releases every optional source reference");
            }
            if (ownership_only) CHECK(dispatched==2,
                "both baseline and later-mutated production terminal outputs were read back on the GPU");
            terminal_index_matrix(&f,slot,ownership_only,trace,&prepared,&refused,&presents,&dispatched);
        }
    }
    CHECK(vk_fg_test_terminal_owned(0),"drained walker disables optional owned capture");
    CHECK(vk_fg_test_generation_bridge(0),"drained walker releases only its TEST external bridge");
    terminal_walker_destroy(&f);
    printf("terminal walker: mode=%s templates_prepared=%u refusals=%u real_presents=%u dispatched=%u checks=%u failures=%u; actual OP indexed/%s/nativeUI, %s; no AFME/device quality claim\n",
        off?"slots-off":negative?"old-pool":ownership_only?"interp-ownership":"adaptive-owned",
        prepared,refused,presents,dispatched,checks,failures,
        off?"pool-only":"readonly-inline",
        ownership_only?"fixed-interp ownership-only GPU output lifetime":
        off?"original consumed-pool command and GPU output equality":
        "private preparation with unmodified adaptive budget; dispatch may legitimately refuse");
    return failures?1:0;
}

/* TOTK_FG_ADAPTIVE_LEAN / TOTK_FG_ADAPTIVE_DIRECT_OUT on the actual adaptive
 * owned walker with one production-admitted unknown scene depth writer per
 * real. Arms: off (golden), lean, lean+direct, lean with an engine refusal
 * (real-only fallback), lean recovery, off again (copies/spans resume). Only
 * AFME pixels are substituted: the TEST bridge samples the retained scene. */
typedef struct { uint64_t span[5],authority[5],lean[VK_FG_LEAN_COUNTS]; } lean_counters;
static void lean_counters_get(lean_counters *c) {
    vk_fg_test_authority_span_counts(c->span); vk_fg_test_authority_counts(c->authority);
    vk_fg_test_lean_counts(c->lean);
}
static int lean_ticket_row(uint64_t ticket,uint64_t row[4],double *ms) {
    uint64_t deadline=host_now_ns()+UINT64_C(2000000000);
    int exact;
    while (!((exact=vk_fg_test_budget_ticket(ticket,row,ms)) && row[1]) && host_now_ns()<deadline) usleep(1000);
    return exact && row[1];
}
/* lean: 0 off, 1 lean with a generating TEST bridge (it samples the scene copy
 * as AFME output), 2 lean with a refused engine (scene copy skipped too). */
static void lean_frame_counts(const lean_counters *a,const lean_counters *b,int lean,const char *arm) {
    if (lean) {
        CHECK(b->span[0]==a->span[0] && b->span[1]==a->span[1] && b->span[3]==a->span[3] &&
            b->authority[0]==a->authority[0] && b->authority[1]==a->authority[1] &&
            b->authority[2]==a->authority[2],
            "%s real records no authority reservation, span copy or final census",arm);
        CHECK(b->lean[VK_FG_LEAN_SOURCES]==a->lean[VK_FG_LEAN_SOURCES]+1 &&
            b->lean[VK_FG_LEAN_DEPTH_COPIES]==a->lean[VK_FG_LEAN_DEPTH_COPIES]+1 &&
            b->lean[VK_FG_LEAN_REAL_COPIES]==a->lean[VK_FG_LEAN_REAL_COPIES]+1 &&
            b->lean[VK_FG_LEAN_UNKNOWN_DRAWS]>a->lean[VK_FG_LEAN_UNKNOWN_DRAWS] &&
            b->lean[VK_FG_LEAN_ROOTS]==a->lean[VK_FG_LEAN_ROOTS]+1 &&
            b->lean[VK_FG_LEAN_SCENE_COPIES]==a->lean[VK_FG_LEAN_SCENE_COPIES]+(lean==2),
            "%s real skips depth/real copies and its unknown-writer root; scene copy %s",arm,
            lean==2?"skipped (refused bridge reads none)":"kept for the generating TEST bridge");
    } else {
        CHECK(b->span[0]>a->span[0] && b->span[1]>a->span[1] && b->span[3]>a->span[3] &&
            b->authority[0]==a->authority[0]+1 && b->authority[1]==a->authority[1]+1 &&
            b->authority[2]==a->authority[2]+1,
            "%s real records ordered spans, one reservation and one final census (non-vacuous writer)",arm);
        for (unsigned i=VK_FG_LEAN_SOURCES;i<=VK_FG_LEAN_ROOTS;i++)
            CHECK(b->lean[i]==a->lean[i],"%s real counts no lean skip",arm);
    }
}
static unsigned lean_arm(terminal_walker *f,unsigned slot,unsigned arm,uint8_t *golden,int *have_golden,
        uint8_t *pixels,size_t bytes,unsigned *prepared,unsigned *presents) {
    static const char *const names[]={"off","lean","lean-direct","lean-refused","lean-recovered","off-again"};
    const char *name=names[arm];
    int lean=arm>=1 && arm<=4, direct=arm==2;
    CHECK(vk_fg_test_adaptive_lean(lean,direct),"%s selector override applies between generations",name);
    if (arm==3) {
        /* A template prepared before the refusal must not dispatch after it. */
        terminal_walker_open(f,0,0);
        vk_fg_generation *g=terminal_prepare(slot,prepared);
        (void)terminal_walker_close(); (*presents)++;
        vk_fg_test_generation_refuse(1);
        if (g) {
            uint64_t before=mtl_last_fence(); vk_post_output output={0};
            vk_rec_lock();
            int result=vk_fg_generation_dispatch(g,.375f,host_now_ns()+UINT64_C(10000000000),&output);
            vk_rec_unlock();
            CHECK(!result && mtl_last_fence()==before && vk_fg_generation_state(g)==VK_FG_GENERATION_PREPARED,
                "engine refusal refuses dispatch explicitly with no ticket or output");
            vk_rec_lock(); vk_fg_generation_cancel(g); vk_rec_unlock();
        }
        for (unsigned frame=0;frame<4;frame++) {
            lean_counters a,b; lean_counters_get(&a);
            terminal_walker_open(f,0,0);
            CHECK(!terminal_prepare(slot,prepared),"refused engine prepares no child: real-only, no camera fallback reads lean sources");
            (void)terminal_walker_close(); (*presents)++;
            lean_counters_get(&b); lean_frame_counts(&a,&b,2,name);
            terminal_real_corner(f);
            terminal_ownership(0,0,"refused engine retains no optional references");
        }
        vk_fg_test_generation_refuse(0);
        return 0;
    }
    /* Genuine adaptive admission (no override) may refuse between its once-per-
     * 30-real probes on a software GPU; 80 reals bound two probe windows. */
    unsigned dispatched=0;
    for (unsigned frame=0;frame<80 && dispatched<2;frame++) {
        lean_counters a,b,c; lean_counters_get(&a);
        terminal_walker_open(f,0,0);
        vk_fg_generation *g=terminal_prepare(slot,prepared);
        uint64_t real=terminal_walker_close(); (*presents)++;
        lean_counters_get(&b);
        lean_frame_counts(&a,&b,lean,name);
        if (frame) CHECK(g,"%s mature real stream still prepares an immutable child",name);
        if (!g) continue;
        uint64_t before=mtl_last_fence(),charge=vk_fg_real_frame_id(); vk_post_output output={0};
        vk_rec_lock();
        int result=vk_fg_generation_dispatch(g,.375f,host_now_ns()+UINT64_C(10000000000),&output);
        uint64_t covering=result==1?vk_fg_generation_seal(g):0;
        vk_rec_unlock();
        lean_counters_get(&c);
        CHECK(c.lean[VK_FG_LEAN_DIRECT]==b.lean[VK_FG_LEAN_DIRECT] &&
            c.lean[VK_FG_LEAN_DIRECT_FALLBACK]==b.lean[VK_FG_LEAN_DIRECT_FALLBACK],
            "%s dispatch only submits the recorded child",name);
        CHECK(b.lean[VK_FG_LEAN_DIRECT]==a.lean[VK_FG_LEAN_DIRECT]+(unsigned)direct &&
            b.lean[VK_FG_LEAN_DIRECT_FALLBACK]==a.lean[VK_FG_LEAN_DIRECT_FALLBACK],
            "%s prepare %s the terminal replay into the matching RGBA8 output",name,direct?"redirects":"never redirects");
        CHECK(result>=0,"%s host TEST bridge never quarantines",name);
        if (result!=1) {
            CHECK(mtl_last_fence()==before && vk_fg_generation_state(g)==VK_FG_GENERATION_PREPARED,
                "%s adaptive budget refusal creates no ticket",name);
            vk_rec_lock(); vk_fg_generation_cancel(g); vk_rec_unlock();
            terminal_ownership(0,0,"refused lean child releases every reference");
            continue;
        }
        uint64_t facts[6]; vk_fg_test_generation_facts(g,facts,NULL);
        CHECK(covering==before+2 && facts[4]==before+1 && facts[1]==covering && output.image,
            "%s child owns exactly its two-ticket signal/consumer range",name);
        CHECK(mtl_wait(covering,UINT64_C(10000000000)),"%s child covering ticket retires",name);
        read_image_extent(output.image,VK_IMAGE_ASPECT_COLOR_BIT,pixels,bytes,960,540);
        CHECK(abs((int)pixels[0]-24)<=2 && abs((int)pixels[1]-32)<=2 &&
            abs((int)pixels[2]-128)<=2 && pixels[3]==255,
            "%s child output non-vacuously consumes the captured scene and terminal",name);
        if (!*have_golden) { memcpy(golden,pixels,bytes); *have_golden=1; }
        else {
            size_t differ=0;
            for (size_t i=0;i<bytes;i++) differ+=golden[i]!=pixels[i];
            CHECK(!differ,"%s child output is byte-identical to the flag-off golden (%zu differing bytes)",name,differ);
        }
        uint64_t row[4]; double ms;
        CHECK(lean_ticket_row(real,row,&ms) && !row[2],"%s source real ticket stays a REAL charge row",name);
        for (uint64_t ticket=before+1;ticket<=covering;ticket++)
            CHECK(lean_ticket_row(ticket,row,&ms) && row[2]==1 && isfinite(ms) && ms>=0,
                "%s child ticket %llu is a measured synthetic charge row",name,(unsigned long long)ticket);
        (void)vk_fg_complete(slot); /* adaptive actual audit may still reject */
        CHECK(vk_fg_generation_state(g)==VK_FG_GENERATION_EMPTY && !vk_fg_complete(slot),
            "%s child retires exactly once",name);
        terminal_ownership(0,0,"retired lean child returns every source/output reference");
        /* The next real seals the charged interval containing the child. */
        terminal_walker_open(f,0,0); (void)terminal_walker_close(); (*presents)++;
        uint64_t frame_row[6]={0}; double costs[2]; uint64_t deadline=host_now_ns()+UINT64_C(2000000000);
        int exact;
        while (!((exact=vk_fg_test_budget_frame(charge,frame_row,costs)) && frame_row[3]==FG_BUDGET_VALID) &&
            host_now_ns()<deadline) usleep(1000);
        CHECK(exact && frame_row[3]==FG_BUDGET_VALID && frame_row[1]<=before+1 && frame_row[2]>=covering &&
            frame_row[4]==1 && isfinite(costs[1]) && costs[1]>=0,
            "%s charge interval %llu seals valid with the child as its synthetic range",name,(unsigned long long)charge);
        dispatched++;
    }
    CHECK(dispatched>0,"%s dispatched and read back at least one actual child",name);
    return dispatched;
}
static int run_terminal_lean(void) {
    const char *zero[]={"TOTK_UNCAPPED","TOTK_FPS_AUTO","TOTK_ASYNC_REPLAY","TOTK_ACQUIRE_SLACK",
        "TOTK_ACQUIRE_CLOCK","TOTK_PACE","TOTK_MFX","DM_DETCLOCK","TOTK_VK_COMPILE_THREADS",
        "TOTK_VK_CHUNK_DRAWS","TOTK_VK_LAG_CAPTURE"};
    for (unsigned i=0;i<sizeof zero/sizeof zero[0];i++) setenv(zero[i],"0",1);
    setenv("TOTK_FRAMEGEN","adaptive",1); setenv("TOTK_FRAMEGEN_ENGINE","qcom",1);
    setenv("TOTK_FRAMEGEN_JOB_TRACE","1",1);
    setenv("TOTK_FPS60","1",1); setenv("TOTK_REALTIME","1",1);
    setenv("TOTK_NATIVE_UI","1",1); setenv("TOTK_PRESENT_SLOTS","1",1);
    setenv("TOTK_FG_ADAPTIVE_SLOTS","1",1);
    /* LEAN is a scheduler default: leave the caller's TOTK_FG_ADAPTIVE_LEAN
     * (absent = production default on; exact 0 = opt-out) to the startup path. */
    const char *lean_env=getenv("TOTK_FG_ADAPTIVE_LEAN");
    const int startup_lean=!lean_env || !strcmp(lean_env,"1");
    setenv("TOTK_FG_ADAPTIVE_DIRECT_OUT","0",1);
    setenv("DM_PRESENT_INTERVAL","game",1); setenv("DM_PRESENT_HZ","60",1);
    setenv("TOTK_POST_AA","off",1); setenv("TOTK_POST_SCALE","off",1);
    setenv("TOTK_SCENE_AA","off",1); setenv("TOTK_SCENE_SCALE","off",1);
    unsetenv("TOTK_FRAMEGEN_BUDGET_MS"); unsetenv("TOTK_FRAMEGEN_DIAG");
    W=DW=640; H=DH=360;
    mtl_init();
    CHECK(vk_fg_enabled() && vk_fg_adaptive() && vk_native_ui_enabled() && vk_canvas_on &&
        atomic_load_explicit(&vk_fg_terminal_owned_active,memory_order_relaxed),
        "production startup admits the adaptive scheduler walker");
    CHECK(vk_fg_test_generation_bridge(1),"substitute only unavailable host AFME pixels/external bridge");
    vk_fg_test_terminal_geometry_freeze(1);
    terminal_walker f; terminal_walker_create(&f,1);
    terminal_unknown_writer=1;
    unsigned prepared=0,presents=0,dispatched=0;
    /* Environment path: the startup selector (default on unless exact 0)
     * decides the first reals. Starting refused makes fresh source images take
     * the lean placeholder (color and depth clears) before any copy, as on
     * device; with LEAN=0 the same reals keep every ordered span and copy. */
    vk_fg_test_generation_refuse(1);
    for (unsigned frame=0;frame<4;frame++) {
        lean_counters a,b; lean_counters_get(&a);
        terminal_walker_open(&f,0,0);
        CHECK(!terminal_prepare(2,&prepared),"refused startup engine prepares no child");
        (void)terminal_walker_close(); presents++;
        lean_counters_get(&b);
        lean_frame_counts(&a,&b,startup_lean ? 2 : 0,startup_lean ? "startup-env-refused" : "startup-env-off-refused");
        terminal_real_corner(&f);
    }
    vk_fg_test_generation_refuse(0);
    size_t bytes=(size_t)960*540*4;
    uint8_t *golden=malloc(bytes),*pixels=malloc(bytes);
    CHECK(golden && pixels,"allocate lean output goldens");
    if (!golden || !pixels) return 1;
    int have_golden=0;
    for (unsigned arm=0;arm<6;arm++) {
        dispatched+=lean_arm(&f,2,arm,golden,&have_golden,pixels,bytes,&prepared,&presents);
        terminal_real_corner(&f);
    }
    /* Reach the next 300-real JOB_TRACE window so the lean summary line is emitted. */
    CHECK(vk_fg_test_adaptive_lean(1,1),"final lean window selector override");
    unsigned window=(presents+299)/300*300+1;
    while (presents<window) {
        terminal_walker_open(&f,0,0);
        vk_fg_generation *g=terminal_prepare(2,&prepared);
        if (g) { vk_rec_lock(); vk_fg_generation_cancel(g); vk_rec_unlock(); }
        (void)terminal_walker_close(); presents++;
    }
    terminal_ownership(0,0,"lean walker drains every optional reference");
    uint64_t counts[VK_FG_LEAN_COUNTS]; vk_fg_test_lean_counts(counts);
    terminal_unknown_writer=0;
    free(golden); free(pixels);
    CHECK(vk_fg_test_terminal_owned(0),"drained walker disables optional owned capture");
    CHECK(vk_fg_test_generation_bridge(0),"drained walker releases only its TEST external bridge");
    terminal_walker_destroy(&f);
    printf("adaptive lean: templates_prepared=%u real_presents=%u dispatched=%u lean_sources=%llu skipped_scene=%llu skipped_depth=%llu skipped_real=%llu unknown_draws=%llu roots=%llu direct=%llu direct_fallback=%llu startup_env_lean=%d checks=%u failures=%u; off/lean/lean-direct/recovered/off-again golden-identical, refused engine real-only, no AFME/device claim\n",
        prepared,presents,dispatched,(unsigned long long)counts[VK_FG_LEAN_SOURCES],
        (unsigned long long)counts[VK_FG_LEAN_SCENE_COPIES],
        (unsigned long long)counts[VK_FG_LEAN_DEPTH_COPIES],(unsigned long long)counts[VK_FG_LEAN_REAL_COPIES],
        (unsigned long long)counts[VK_FG_LEAN_UNKNOWN_DRAWS],(unsigned long long)counts[VK_FG_LEAN_ROOTS],
        (unsigned long long)counts[VK_FG_LEAN_DIRECT],(unsigned long long)counts[VK_FG_LEAN_DIRECT_FALLBACK],
        startup_lean,
        checks,failures);
    return failures?1:0;
}

int main(int argc, char **argv) {
    if (argc==2 && !strcmp(argv[1],"--fg-gate")) return run_fg_gate();
    if (argc==2 && !strcmp(argv[1],"--adaptive-lean")) return run_terminal_lean();
    if (argc==2 && !strcmp(argv[1],"--adaptive-capture")) return run_adaptive_capture();
    if (argc==2 && !strcmp(argv[1],"--adaptive-terminal-owned")) return run_terminal_walker(0);
    if (argc==2 && !strcmp(argv[1],"--adaptive-terminal-pool")) return run_terminal_walker(1);
    if (argc==2 && !strcmp(argv[1],"--adaptive-terminal-off")) return run_terminal_walker(2);
    if (argc==2 && !strcmp(argv[1],"--interp-terminal-owned")) return run_terminal_walker(3);
    int result = argc==3 && !strcmp(argv[1],"--lsfg-probe") ? run_lsfg_probe(argv[2]) :
                 argc==2 && !strcmp(argv[1],"--off") ? run_off(0) :
                 argc==2 && !strcmp(argv[1],"--refused") ? run_off(1) :
                 argc==2 && !strcmp(argv[1],"--budget") ? run_budget() :
                 argc==2 && !strcmp(argv[1],"--late") ? run_late() :
                 argc==2 && !strcmp(argv[1],"--qcom-config") ? run_qcom_config(0) :
                 argc==2 && !strcmp(argv[1],"--qcom-camera") ? run_qcom_config(1) :
                 argc==2 && !strcmp(argv[1],"--qcom-invalid") ? run_qcom_config(2) :
                 argc==2 && !strcmp(argv[1],"--qcom-empty") ? run_qcom_config(3) :
                 argc==2 && !strcmp(argv[1],"--qcom-wrong-mode") ? run_qcom_wrong_mode() :
                 argc==2 && !strcmp(argv[1],"--qcom-fallback") ? run_interp_headroom(1) :
                 argc==2 && !strcmp(argv[1],"--lsfg-config") ? run_lsfg_config(0) :
                 argc==2 && !strcmp(argv[1],"--lsfg-invalid") ? run_lsfg_config(1) :
                 argc==2 && !strcmp(argv[1],"--lsfg-wrong-mode") ? run_lsfg_config(2) :
                 argc==2 && !strcmp(argv[1],"--lsfg-fallback") ? run_interp_headroom(2) :
                 argc==2 && !strcmp(argv[1],"--locked-config") ? run_locked_config(0) :
                 argc==2 && !strcmp(argv[1],"--locked-lead-two") ? run_locked_lead_two() :
                 argc==2 && !strcmp(argv[1],"--locked-refuse-lead") ? run_locked_config(4) :
                 argc==2 && !strcmp(argv[1],"--locked-refuse-fps60") ? run_locked_config(1) :
                 argc==2 && !strcmp(argv[1],"--locked-refuse-budget") ? run_locked_config(2) :
                 argc==2 && !strcmp(argv[1],"--locked-refuse-realtime") ? run_locked_config(3) :
                 argc==2 && !strcmp(argv[1],"--interp-static") ? run_interp_static() :
                 argc==2 && !strcmp(argv[1],"--interp-hold") ? run_interp_hold(0) :
                 argc==2 && !strcmp(argv[1],"--interp-hold-off") ? run_interp_hold(1) :
                 argc==2 && !strcmp(argv[1],"--interp-replay-buffers") ? run_interp_replay_buffers() :
                 argc==2 && !strcmp(argv[1],"--interp-depth-range") ? run_interp_depth_range() :
                 argc==2 && !strcmp(argv[1],"--interp-depth-range-nativeui") ? run_interp_nativeui(1) :
                 argc==2 && !strcmp(argv[1],"--interp-nativeui") ? run_interp_nativeui(0) :
                 argc==2 && !strcmp(argv[1],"--scene-scale") ? run_scene_scale(0,0,0) :
                 argc==2 && !strcmp(argv[1],"--scene-scale-drs") ? run_scene_scale(0,1,0) :
                 argc==2 && !strcmp(argv[1],"--scene-locked") ? run_scene_scale(0,0,1) :
                 argc==2 && !strcmp(argv[1],"--scene-locked-drs") ? run_scene_scale(0,1,1) :
                 argc==2 && !strcmp(argv[1],"--scene-benchmark") ? run_scene_scale(60,0,0) :
                 argc==2 && !strcmp(argv[1],"--hybrid") ? run_interp(-1) :
                 argc==2 && !strcmp(argv[1],"--interp-present") ? run_interp_present() :
                 argc==2 && !strcmp(argv[1],"--interp-device") ? run_interp(1) :
                 argc==2 && !strcmp(argv[1],"--interp-partial-clear") ? run_interp(2) :
                 argc==2 && !strcmp(argv[1],"--interp-gameplay") ? run_interp(3) :
                 argc==2 && !strcmp(argv[1],"--interp-authority") ? run_interp_authority() :
                 argc==2 && !strcmp(argv[1],"--interp-job-reuse") ? run_interp_job_reuse() :
                 argc==2 && !strcmp(argv[1],"--interp-authority-ordering") ? run_interp_authority_ordering(0) :
                 argc==2 && !strcmp(argv[1],"--interp-authority-ordering-lag") ? run_interp_authority_ordering(1) :
                 argc==3 && !strcmp(argv[1],"--interp-authority-ordering") && !strcmp(argv[2],"normal") ?
                     run_interp_authority_ordering(0) :
                 argc==3 && !strcmp(argv[1],"--interp-authority-ordering") && !strcmp(argv[2],"lag") ?
                     run_interp_authority_ordering(1) :
                 argc==2 && !strcmp(argv[1],"--interp-dilation") ? run_interp_dilation() :
                 argc==2 && !strcmp(argv[1],"--interp-headroom") ? run_interp_headroom(0) :
                 argc==2 && !strcmp(argv[1],"--interp-uncertified") ? run_interp_uncertified(1) :
                 argc==2 && !strcmp(argv[1],"--interp-uncertified-off") ? run_interp_uncertified(0) :
                 argc==2 && !strcmp(argv[1],"--interp-uncertified-invalid") ? run_interp_uncertified(2) :
                 argc==2 && !strcmp(argv[1],"--interp-writer") ? run_interp_writer(1) :
                 argc==2 && !strcmp(argv[1],"--interp-writer-off") ? run_interp_writer(0) :
                 argc==2 && !strcmp(argv[1],"--interp-writer-invalid") ? run_interp_writer(2) :
                 argc==2 && !strcmp(argv[1],"--interp-writer-no-diag") ? run_interp_writer(3) :
                 argc==2 && !strcmp(argv[1],"--interp-budget") ? run_interp_budget() : run_enabled();
    printf("framegen Vulkan synthetic goldens: %u checks, %u failed\n", checks, failures);
    return result;
}
