#version 450
layout(location=0) in vec2 uv;
layout(location=0) out vec4 color;
// Propagated through final encoding: an unsafe depth tap selects the actual
// earlier encoded real image, not a reconstructed scene/UI approximation.
layout(location=1) out float authority_fallback;
layout(set=0,binding=0) uniform sampler2D first_scene;
layout(set=0,binding=1) uniform sampler2D first_depth;
layout(set=0,binding=3) uniform sampler2D second_scene;
layout(set=0,binding=4) uniform sampler2D second_depth;
// Earlier-frame UI replayed over black/white, sampled in canvas coordinates.
// These linear images encode the affine framebuffer blend without estimating alpha.
layout(set=0,binding=5) uniform sampler2D ui_black;
layout(set=0,binding=6) uniform sampler2D ui_white;
layout(set=0,binding=7,std430) readonly buffer FirstAuthority { uint first_authority[]; };
layout(set=0,binding=8,std430) readonly buffer SecondAuthority { uint second_authority[]; };
layout(push_constant) uniform Push {
    vec4 row[8]; // Row-major N clip -> midpoint clip, then N+1 -> midpoint.
} pc;
layout(constant_id=0) const bool scene_scale=false;
// TOTK_FG_STATIC_HOLD (default on). The two warps know only the camera. A surface
// that moves WITH the camera (Link and his horse at a gallop, the paraglider) is
// screen-static: both real frames show it at the same pixel, but each camera warp
// moves it by half the camera motion in opposite directions, and the blend draws
// two translucent copies on every generated frame (horse-riding "shake"). Hold a
// pixel only when the two real frames agree at the SAME screen position, at the
// same camera-relative depth, and either they are near-identical there or that
// agreement is clearly better than the two warped samples'. Moving scenery
// differs at a fixed pixel, so it keeps the camera result.
layout(constant_id=1) const bool static_hold=false;

// Matrix inversion/raster UV roundoff must not invent sub-texel contrast in an
// identity warp. EASU's direction estimate amplifies those tiny false gradients.
// Preserve exact texels at aligned centers; genuinely displaced samples retain
// the existing hardware bilinear filter. 1e-4 pixel is below sampler precision.
vec3 scene_sample(sampler2D image,vec2 p) {
    vec2 pixel=p*vec2(textureSize(image,0))-0.5,aligned=round(pixel);
    if (scene_scale && all(lessThanEqual(abs(pixel-aligned),vec2(1e-4))))
        return texelFetch(image,clamp(ivec2(aligned),ivec2(0),textureSize(image,0)-1),0).rgb;
    return texture(image,p).rgb;
}
bool finite_value(float v) { return !isnan(v) && !isinf(v); }
bool valid_depth(float d) { return finite_value(d) && d>=0.0 && d<1.0-1e-6; }
bool inside(sampler2D image,vec2 p) {
    vec2 edge=0.5/vec2(textureSize(image,0));
    return all(greaterThanEqual(p,edge)) && all(lessThanEqual(p,vec2(1.0)-edge));
}
bool authority_at(int which,vec2 p) {
    uint w=which==0 ? first_authority[0] : second_authority[0];
    if (w==0u) return true; // No unproven span: the camera root owns all depth.
    uint h=which==0 ? first_authority[1] : second_authority[1];
    if (any(isnan(p)) || any(isinf(p)) || any(lessThan(p,vec2(0.0))) ||
        any(greaterThan(p,vec2(1.0)))) return false;
    uvec2 texel=uvec2(clamp(floor(p*vec2(w,h)),vec2(0.0),vec2(w-1u,h-1u)));
    uint index=16u+w*h+texel.y*w+texel.x;
    return (which==0 ? first_authority[index] : second_authority[index])!=0u;
}
bool color_authority(sampler2D image,int which,vec2 p) {
    // Check all four color-filter taps, including zero-weight boundary taps.
    // Scene and native depth can have independent x/y extents.
    ivec2 extent=textureSize(image,0),base=ivec2(floor(p*vec2(extent)-0.5));
    for (int y=0;y<2;y++) for (int x=0;x<2;x++) {
        ivec2 tap=clamp(base+ivec2(x,y),ivec2(0),extent-1);
        if (!authority_at(which,(vec2(tap)+0.5)/vec2(extent))) return false;
    }
    return true;
}
bool project_source(sampler2D image,int which,int base,vec2 p,out vec2 projected,
                    out float depth,out float midpoint_depth,inout bool unsafe) {
    if (!inside(image,p)) return false;
    if (!authority_at(which,p)) { unsafe=true; return false; }
    depth=texture(image,p).r;
    if (!valid_depth(depth)) return false;
    // Undo the game's flipped viewport and Vulkan's NVN depth conversion.
    vec4 clip=vec4(2.0*p.x-1.0,1.0-2.0*p.y,2.0*depth-1.0,1.0);
    vec4 midpoint=vec4(dot(pc.row[base],clip),dot(pc.row[base+1],clip),
                       dot(pc.row[base+2],clip),dot(pc.row[base+3],clip));
    if (any(isnan(midpoint)) || any(isinf(midpoint)) || midpoint.w<=1e-5 || abs(midpoint.z)>midpoint.w) return false;
    projected=vec2(midpoint.x,-midpoint.y)/midpoint.w*0.5+0.5;
    midpoint_depth=(midpoint.z/midpoint.w+1.0)*0.5;
    return valid_depth(midpoint_depth) && all(lessThanEqual(abs(projected-p),vec2(0.04)));
}
bool continuous_depth(sampler2D image,int which,vec2 p,float center,inout bool unsafe) {
    vec2 texel=1.0/vec2(textureSize(image,0));
    float lo=center,hi=center;
    const vec2 offsets[4]=vec2[4](vec2(-1,0),vec2(1,0),vec2(0,-1),vec2(0,1));
    for (int i=0;i<4;i++) {
        vec2 q=p+offsets[i]*texel;
        if (!inside(image,q)) return false;
        if (!authority_at(which,q)) { unsafe=true; return false; }
        float d=texture(image,q).r;
        if (!valid_depth(d)) return false;
        lo=min(lo,d); hi=max(hi,d);
    }
    // Same conservative source-surface gate as extrapolation, without a near factor.
    return hi-lo<=0.02 && (1.0-lo)<=(1.0-hi)*1.08;
}
bool candidate(sampler2D scene,sampler2D depth_image,int which,int base,
               out vec3 rgb,out float midpoint_depth,out bool unsafe) {
    unsafe=false;
    if (!inside(depth_image,uv)) return false;
    vec2 source=uv,projected;
    float depth;
    // Independently invert both source mappings. Do not clamp a newly exposed
    // surface or a nonconvergent edge onto an unrelated source pixel.
    for (int i=0;i<3;i++) {
        if (!project_source(depth_image,which,base,source,projected,depth,midpoint_depth,unsafe)) return false;
        source+=uv-projected;
    }
    vec2 inverse_extent=1.0/vec2(textureSize(depth_image,0));
    if (!project_source(depth_image,which,base,source,projected,depth,midpoint_depth,unsafe) ||
        any(greaterThan(abs(projected-uv),inverse_extent*1.5)) ||
        !continuous_depth(depth_image,which,source,depth,unsafe)) return false;
    if (!color_authority(scene,which,source)) { unsafe=true; return false; }
    rgb=scene_sample(scene,source);
    return !any(isnan(rgb)) && !any(isinf(rgb));
}
// Relative colour difference: linear HDR values, scaled by the brighter one.
float color_error(vec3 a,vec3 b) {
    const vec3 luma=vec3(0.2126,0.7152,0.0722);
    return length(a-b)/(0.05+max(dot(a,luma),dot(b,luma)));
}
// Mean unwarped difference over the pixel and four neighbours two texels away,
// so a single coincidentally equal texel of moving texture cannot qualify.
bool static_pair(out vec3 held,out float error) {
    held=vec3(0.0); error=1e9;
    float a_depth=texture(first_depth,uv).r,b_depth=texture(second_depth,uv).r;
    if (!valid_depth(a_depth) || !valid_depth(b_depth)) return false;
    float a=1.0-a_depth,b=1.0-b_depth;
    if (max(a,b)>min(a,b)*1.04) return false; // same FG_INTERP_DEPTH_RATIO as the blend
    vec2 texel=2.0/vec2(textureSize(first_scene,0));
    const vec2 offsets[5]=vec2[5](vec2(0,0),vec2(-1,0),vec2(1,0),vec2(0,-1),vec2(0,1));
    float sum=0.0;
    for (int i=0;i<5;i++) {
        vec2 q=clamp(uv+offsets[i]*texel,vec2(0.0),vec2(1.0));
        vec3 x=scene_sample(first_scene,q),y=scene_sample(second_scene,q);
        if (any(isnan(x)) || any(isinf(x)) || any(isnan(y)) || any(isinf(y))) return false;
        if (i==0) held=(x+y)*0.5;
        sum+=color_error(x,y);
    }
    error=sum*0.2;
    return true;
}
void main() {
    vec3 first_rgb=vec3(0.0),second_rgb=vec3(0.0);
    float first_z=1.0,second_z=1.0;
    bool first_valid=false,second_valid=false,first_unsafe=false,second_unsafe=false;
    bool unsafe=!authority_at(0,uv) || !authority_at(1,uv);
    if (!unsafe) {
        first_valid=candidate(first_scene,first_depth,0,0,first_rgb,first_z,first_unsafe);
        second_valid=candidate(second_scene,second_depth,1,4,second_rgb,second_z,second_unsafe);
        unsafe=first_unsafe || second_unsafe;
    }
    // A hazardous tap in EITHER candidate forbids both warps. Never turn a
    // failed authority check into permission to use the other camera's warp.
    if (unsafe) { first_valid=false; second_valid=false; }
    authority_fallback=unsafe ? 1.0 : 0.0;
    vec3 scene=scene_sample(first_scene,uv); // Non-authority fallback semantics unchanged.
    if (first_valid && second_valid) {
        // Compare in the same midpoint camera, not at the original screen pixel.
        // Shared near cancels in near/(1-depth); keep FG_INTERP_DEPTH_RATIO in sync.
        float a=1.0-first_z,b=1.0-second_z;
        if (max(a,b)<=min(a,b)*1.04) scene=(first_rgb+second_rgb)*0.5;
        else scene=first_z<second_z ? first_rgb : second_rgb;
    } else if (first_valid) scene=first_rgb;
    else if (second_valid) scene=second_rgb; // One-sided disocclusion, no stale blend.
    vec3 held; float held_error;
    if (static_hold && !unsafe && (first_valid || second_valid) && static_pair(held,held_error) && held_error<=0.08) {
        // Near-identical real frames at this pixel: holding is the true midpoint for
        // anything screen-static and indistinguishable from the warp elsewhere; the
        // two warps can agree with EACH OTHER on wrong content (both land on the
        // same stripe of a displaced pattern). Otherwise hold only when both warps
        // are valid and disagree clearly more than the unwarped pair.
        float warped_error=first_valid && second_valid ? color_error(first_rgb,second_rgb) : 0.0;
        if (held_error<=0.03 || (first_valid && second_valid && warped_error>held_error*2.0+0.04)) scene=held;
    }
    vec3 black,white;
    if (scene_scale) {
        black=texelFetch(ui_black,ivec2(gl_FragCoord.xy),0).rgb;
        white=texelFetch(ui_white,ivec2(gl_FragCoord.xy),0).rgb;
    } else {
        black=texture(ui_black,uv).rgb; white=texture(ui_white,uv).rgb;
    }
    color=vec4(scene*(white-black)+black,1.0);
}
