// Lighting module: the SUN with real-time cascaded shadow maps (see lighting_core.h for the module interface).
//
//   * sun direction: the game's own dominant distant light (read from the lit draws of each frame, smoothed over TIME), lowered
//     to a maximum elevation (a sun straight overhead hides every shadow), or the user's own azimuth / elevation
//   * cascades: 2-4 world-anchored spheres around the first camera (texel-snapped: no shimmer), each a depth map of the PREVIOUS
//     frame's cached geometry (shadows lag one frame, the camera is exact) rendered through Aurora's light-view pipeline variants
//   * the cascade depth is copied into the engine's storage buffer (aurora::lighting::shadow_buffer, static bind group binding 2:
//     a texture / sampler in the lit draws' own bind group crashes the NVIDIA D3D12 driver) and read by the lit shader with a
//     rotated spiral PCF filter
//   * uniform slots (vec4): 0..15 the 4 cascade matrices (view space of THIS camera -> cascade clip, one row per slot), 16 sun
//     direction in view space (w = shadow maps valid), 17 shadow tint (w = cascade count), 18 cascade far distances, 19 cascade
//     texel sizes, 20 (-, 1/resolution, filter radius, taps), 21 sun colour (w = strength)
#include "lighting_core.h"
#include "lighting_environment.h"
#include "sys.h"
#include "settings_ui.h"
#include <aurora/gfx.hpp>
#include <aurora/lighting.hpp>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace lighting {
namespace {
using namespace aurora::gfx;
namespace al = aurora::lighting;

constexpr int kMaxCascades = 4;

// ------------------------------------------------------------------ game thread state
V3 g_sun_to{0, 1, 0};   // world, pointing TOWARDS the sun
bool g_sun_ok = false;
uint64_t g_sun_ns = 0;

M4 g_C[kMaxCascades];
float g_far[kMaxCascades], g_texel[kMaxCascades];
uint32_t g_nc = 0, g_res = 1024;

// The sun the engine uses this frame: the game's own direction (lowered so it casts visible shadows) or the user's.
V3 sun_final(const Cam& cam) {
  const auto& s = settings::g;
  const float kDeg = 3.14159265f / 180.0f;
  if (s.light_sun_custom) {
    const float az = std::clamp(s.light_sun_az, 0, 360) * kDeg, el = std::clamp(s.light_sun_elev, 2, 90) * kDeg;
    return {std::cos(el) * std::sin(az), std::sin(el), std::cos(el) * std::cos(az)};
  }
  V3 g = g_sun_to;
  const float maxel = std::clamp(s.light_sun_maxelev, 15, 90) * kDeg;
  if (std::asin(std::clamp(g.y, -1.0f, 1.0f)) > maxel) {
    V3 h{g.x, 0, g.z};
    // A vertical authored sun has no horizontal bearing. Use a fixed WORLD bearing, not camera.right:
    // otherwise turning the camera moves every shadow in the level.
    if (dot(h, h) < 0.02f) h = {0.7071068f, 0, -0.7071068f};
    h = norm(h);
    g = {h.x * std::cos(maxel), std::sin(maxel), h.z * std::cos(maxel)};
  }
  const auto& p = atmosphere();
  const float az = p.sun_azimuth * kDeg, el = p.sun_elevation * kDeg;
  const V3 authored{std::cos(el) * std::sin(az), std::sin(el), std::cos(el) * std::cos(az)};
  return norm(g + (authored - g) * p.sun_direction_override);
}

// ------------------------------------------------------------------ render worker
// (#105, read by widescreen.cpp's caster test on the game thread)
V3 g_shadow_dir{0, -1, 0};
float g_shadow_reach = 0;
struct Shadow {
  wgpu::Texture tex;   // the cascades, rendered as depth (never bound to a shader)
  wgpu::TextureView layer[kMaxCascades];
  uint32_t res = 0, nc = 0;
  wgpu::BindGroup lightPassGroup;
  const void* groupBuffer = nullptr;
} g_sh;

struct TaskPayload {
  uint32_t nc, res, ncam;
  uint32_t offs[kMaxCascades * kMaxCams];   // [cascade * kMaxCams + camera]: dynamic offsets of the view_to_clip matrices
  al::CapturedFrame* frame;
};

void ensure_shadow(const wgpu::Device& dev, uint32_t res) {
  if (g_sh.tex && g_sh.res == res) return;
  g_sh = {};
  const wgpu::TextureDescriptor td{
      .label = "Sun shadow cascades",
      .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc,
      .dimension = wgpu::TextureDimension::e2D,
      .size = {res, res, kMaxCascades},
      .format = wgpu::TextureFormat::Depth32Float,
  };
  g_sh.tex = dev.CreateTexture(&td);
  for (uint32_t i = 0; i < kMaxCascades; ++i) {
    const wgpu::TextureViewDescriptor lv{
        .format = wgpu::TextureFormat::Depth32Float, .dimension = wgpu::TextureViewDimension::e2D,
        .baseArrayLayer = i, .arrayLayerCount = 1, .aspect = wgpu::TextureAspect::DepthOnly};
    g_sh.layer[i] = g_sh.tex.CreateView(&lv);
  }
  g_sh.res = res;
}

uint32_t g_drawsLast = 0;

// between the passes of the frame encoder: render each cascade from the cached geometry, copy its depth to the buffer
void shadow_task(const EncoderTaskContext& ctx, const wgpu::CommandEncoder& cmd, const void* payload, size_t size, void*) {
  if (size != sizeof(TaskPayload)) return;
  TaskPayload p;
  memcpy(&p, payload, sizeof p);
  if (!p.nc || !p.frame) return;
  ensure_shadow(ctx.device, p.res);
  if (g_sh.groupBuffer != ctx.uniformBuffer.Get() || !g_sh.lightPassGroup) {
    const wgpu::BindGroupEntry le{.binding = 0, .buffer = ctx.uniformBuffer, .size = 64};
    const wgpu::BindGroupDescriptor ld{.label = "Light pass group", .layout = al::light_pass_layout(), .entryCount = 1, .entries = &le};
    g_sh.lightPassGroup = ctx.device.CreateBindGroup(&ld);
    g_sh.groupBuffer = ctx.uniformBuffer.Get();
  }
  const wgpu::Buffer buf = al::shadow_buffer();
  for (uint32_t c = 0; c < p.nc; ++c) {
    const wgpu::RenderPassDepthStencilAttachment da{
        .view = g_sh.layer[c], .depthLoadOp = wgpu::LoadOp::Clear, .depthStoreOp = wgpu::StoreOp::Store, .depthClearValue = 1.0f};
    const wgpu::RenderPassDescriptor rd{.label = "Sun shadow", .colorAttachmentCount = 0, .depthStencilAttachment = &da};
    const auto pass = cmd.BeginRenderPass(&rd);
    g_drawsLast = al::replay(p.frame, pass, g_sh.lightPassGroup, &p.offs[c * kMaxCams], std::max(p.ncam, 1u));
    pass.End();
    const wgpu::TexelCopyTextureInfo from{.texture = g_sh.tex, .mipLevel = 0, .origin = {0, 0, c}, .aspect = wgpu::TextureAspect::DepthOnly};
    const wgpu::TexelCopyBufferInfo to{.layout = {.offset = (uint64_t)c * p.res * p.res * 4, .bytesPerRow = p.res * 4, .rowsPerImage = p.res}, .buffer = buf};
    const wgpu::Extent3D ext{p.res, p.res, 1};
    cmd.CopyTextureToBuffer(&from, &to, &ext);
  }
}

EncoderTaskId g_taskType = InvalidEncoderTask;

// ------------------------------------------------------------------ game thread callbacks
void camera(CameraCtx& ctx) {
  const auto& s = settings::g;
  static int last_stage = -1;
  if (ctx.index == 0 && last_stage != environment_stage()) {
    last_stage = environment_stage();
    g_sun_ok = false; g_sun_ns = 0; g_sun_to = {0, 1, 0};
    env().sun_known = false; env().sun_colour = {1, 0.95f, 0.85f};
  }
  const V3 sun = sun_final(ctx.cam);
  if (ctx.index == 0) {   // the frame's first camera: the cascades, and their shadow pass from last frame's geometry
    if (g_taskType == InvalidEncoderTask) g_taskType = register_encoder_task_type({.label = "sun shadow", .callback = shadow_task});
    g_res = std::min(1024u << std::clamp(s.light_shadow_res, 0, 3), 2048u);   // per cascade; 2048 max: the cascades share one storage buffer
    static const int kNc[4] = {2, 3, 4, 4};
    g_nc = s.light_shadows && g_sun_ok ? kNc[std::clamp(s.light_quality, 0, 3)] : 0;
    if (!ctx.prev || ctx.ncached <= 0 || !al::cache_valid() || g_taskType == InvalidEncoderTask) g_nc = 0;
    static const float kFar[4] = {150.f, 500.f, 1600.f, 5000.f};
    const float distK = std::clamp(s.light_shadow_dist, 25, 300) / 100.0f;
    const V3 f = sun * -1.0f;   // from the sun into the scene
    g_shadow_dir = f;
    g_shadow_reach = g_nc ? std::min(kFar[std::min<uint32_t>(g_nc, 3) - 1] * distK, 4000.f) : 0.f;
    const V3 lr = norm(cross(std::fabs(f.y) > 0.95f ? V3{1, 0, 0} : V3{0, 1, 0}, f));
    const V3 lu = cross(f, lr);
    TaskPayload tp{g_nc, g_res, (uint32_t)ctx.ncached, {}, g_nc ? ctx.prev : nullptr};
    for (uint32_t c = 0; c < g_nc; ++c) {
      const float rad = kFar[c] * distK;
      const float texel = 2.0f * rad / (float)g_res;
      const float span = rad * 3.0f + 1500.0f;
      const V3 cp = ctx.cam.p;
      // world-anchored cascade: its centre moves in whole texels only (no shimmer while the camera moves)
      const float lx = std::floor(dot(lr, cp) / texel) * texel, ly = std::floor(dot(lu, cp) / texel) * texel;
      const float lz = dot(f, cp);
      g_C[c] = {{lr.x / rad, lr.y / rad, lr.z / rad, -lx / rad,
                 lu.x / rad, lu.y / rad, lu.z / rad, -ly / rad,
                 f.x / (2 * span), f.y / (2 * span), f.z / (2 * span), -(lz - span) / (2 * span),
                 0, 0, 0, 1}};
      g_far[c] = rad;
      g_texel[c] = texel;
      for (int k = 0; k < ctx.ncached && k < kMaxCams; ++k) {   // last frame's geometry, seen through the camera it was drawn with
        const M4 old = mul(g_C[c], ctx.cached[k].view_to_world());
        tp.offs[c * kMaxCams + k] = push_uniform(reinterpret_cast<const uint8_t*>(old.m), sizeof old.m).offset;
      }
    }
    if (g_nc) push_encoder_task(g_taskType, &tp, sizeof tp);
  }
  float* sl = ctx.slots;
  const M4 wNow = ctx.cam.view_to_world();
  for (uint32_t c = 0; c < g_nc; ++c) {
    const M4 now = mul(g_C[c], wNow);
    memcpy(sl + c * 16, now.m, sizeof now.m);   // 4 slots: one matrix row each
    sl[18 * 4 + c] = g_far[c];
    sl[19 * 4 + c] = g_texel[c];
  }
  for (uint32_t c = g_nc; c < kMaxCascades; ++c) { sl[18 * 4 + c] = 1.0e9f; sl[19 * 4 + c] = 1.0f; }
  const V3 sv = ctx.cam.world_to_view_dir(sun);
  sl[16 * 4 + 0] = sv.x; sl[16 * 4 + 1] = sv.y; sl[16 * 4 + 2] = sv.z; sl[16 * 4 + 3] = g_nc ? 1.0f : 0.0f;
  // what is left in shadow when the game's own lighting stays: the sky's light (cool, dim) - tuned by eye
  // (overlay: the look's shadow colour - profile shadow_strength / shadow_tint, the Look preset and sliders)
  const Look lk = look();
  sl[17 * 4 + 0] = lk.shadow.x; sl[17 * 4 + 1] = lk.shadow.y; sl[17 * 4 + 2] = lk.shadow.z; sl[17 * 4 + 3] = (float)g_nc;
  static const float kRadius[4] = {1.0f, 1.5f, 2.0f, 3.0f}, kTaps[4] = {4, 8, 12, 16};
  sl[20 * 4 + 1] = 1.0f / (float)g_res;
  sl[20 * 4 + 2] = kRadius[std::clamp(s.light_quality, 0, 3)];
  sl[20 * 4 + 3] = kTaps[std::clamp(s.light_quality, 0, 3)];
  // the sun keeps the game's own colour (the stage's mood), pulled halfway to white: its full tint (red in Westopolis) was
  // baked into the stage's vertex light too and would be applied twice
  const V3 c = env().sun_known ? V3{0.5f + 0.5f * env().sun_colour.x, 0.5f + 0.5f * env().sun_colour.y, 0.5f + 0.5f * env().sun_colour.z}
                               : V3{1.0f, 0.95f, 0.85f};
  const auto& p = atmosphere();
  sl[21 * 4 + 0] = c.x * p.sun_tint.x; sl[21 * 4 + 1] = c.y * p.sun_tint.y; sl[21 * 4 + 2] = c.z * p.sun_tint.z;
  sl[21 * 4 + 3] = s.light_sun / 100.0f * 0.8f * p.sun_strength * lk.sun;
}

// the game's own sun: its dominant distant light in the lit draws of this frame, to world space (smoothed over time)
void boundary(const FrameInfo& fi) {
  if (!fi.captured || fi.ncams <= 0) return;
  float d[3], c[3];
  if (!al::dominant_light(fi.captured, d, c)) return;
  const V3 to = norm(fi.cams[0].view_to_world_dir(V3{-d[0], -d[1], -d[2]}));   // dir is FROM the light: flip
  const uint64_t now = sys_time_ns();
  const float k = g_sun_ok ? 1.0f - std::exp(-(float)(now - g_sun_ns) * 1e-9f / 0.25f) : 1.0f;
  g_sun_to = g_sun_ok ? norm(g_sun_to + (to - g_sun_to) * k) : to;
  g_sun_ns = now;
  g_sun_ok = true;
  env().sun_colour = {c[0], c[1], c[2]};
  env().sun_known = true;
  // LIGHT_SUNLOG=1: the game's own sun every 2 s (towards the sun, world space) - checks light.bin's angle order
  static const bool slog = getenv("LIGHT_SUNLOG") != nullptr;
  static uint64_t last = 0;
  if (slog && now - last > 2000000000ull) {
    last = now;
    logf("[sun] game sun to (%.3f,%.3f,%.3f) azimuth %.1f elevation %.1f colour %.2f,%.2f,%.2f\n", g_sun_to.x, g_sun_to.y, g_sun_to.z,
         std::atan2(g_sun_to.x, g_sun_to.z) * 57.29578f, std::asin(std::clamp(g_sun_to.y, -1.f, 1.f)) * 57.29578f, c[0], c[1], c[2]);
  }
}

const char* kDecls = R"WGSL(
@group(0) @binding(2) var<storage, read> lshadow: array<f32>;   // cascade c, row y, column x: ((c * res) + y) * res + x
fn s_sun() -> vec4f { return L({BASE}u + 16u); }               // view space, towards the sun; w: shadow maps valid
fn s_nc() -> u32 { return u32(L({BASE}u + 17u).w); }
fn s_dim() -> u32 { return u32(round(1.0 / L({BASE}u + 20u).y)); }
fn s_clip(c: u32, p: vec3f) -> vec3f {
    let b = {BASE}u + c * 4u;
    let p4 = vec4f(p, 1.0);
    return vec3f(dot(L(b), p4), dot(L(b + 1u), p4), dot(L(b + 2u), p4));
}
// golden-angle spiral: well spread taps, no constant tables
fn s_disk(i: u32, n: u32) -> vec2f {
    let r = sqrt((f32(i) + 0.5) / f32(max(n, 1u)));
    let a = f32(i) * 2.39996323;
    return vec2f(cos(a), sin(a)) * r;
}
fn s_cascade(d: f32) -> u32 {
    let n = s_nc();
    var c = 0u;
    for (var i = 0u; i < 3u; i++) {
        if (i + 1u >= n || d < L({BASE}u + 18u)[i]) { break; }
        c = i + 1u;
    }
    return c;
}
// debug: what the shadow map holds at this receiver (x: stored depth, y: the receiver's own light depth, z: cascade)
fn s_probe(p: vec3f) -> vec3f {
    let c = s_cascade(-p.z);
    let q = s_clip(c, p);
    let uv = vec2f(q.x * 0.5 + 0.5, 0.5 - q.y * 0.5);
    let dimu = s_dim();
    let tc = clamp(vec2<i32>(floor(uv * f32(dimu))), vec2<i32>(0), vec2<i32>(i32(dimu) - 1));
    return vec3f(lshadow[(c * dimu + u32(tc.y)) * dimu + u32(tc.x)], q.z, f32(c));
}
// sun visibility 0..1 at view position p (geometric normal n): rotated spiral PCF over the cascade's depth buffer
fn s_sample(c: u32, p: vec3f, n: vec3f, frag: vec2f) -> f32 {
    let sun = s_sun().xyz;
    let ndl = clamp(dot(n, sun), 0.0, 1.0);
    let texel = L({BASE}u + 19u)[c];
    // receiver bias in world space: along the normal (more at grazing angles) and towards the sun
    let q = s_clip(c, p + n * (texel * (1.0 + 3.0 * (1.0 - ndl))) + sun * (texel * 2.0));
    let uv = vec2f(q.x * 0.5 + 0.5, 0.5 - q.y * 0.5);
    if (any(uv <= vec2f(0.0)) || any(uv >= vec2f(1.0)) || q.z >= 1.0 || q.z <= 0.0) { return 1.0; }
    let prm = L({BASE}u + 20u);
    let taps = u32(prm.w);
    let dimu = s_dim();
    let dimi = vec2<i32>(i32(dimu));
    let dimf = vec2f(dimi);
    let radius = prm.z * prm.y;
    let a = l_ign(frag) * 6.2831853;
    let rot = mat2x2f(cos(a), sin(a), -sin(a), cos(a));
    var s = 0.0;
    for (var i = 0u; i < taps; i++) {
        let tc = clamp(vec2<i32>(floor((uv + rot * s_disk(i, taps) * radius) * dimf)), vec2<i32>(0), dimi - vec2<i32>(1));
        s += select(0.0, 1.0, q.z - 0.0001 <= lshadow[(c * dimu + u32(tc.y)) * dimu + u32(tc.x)]);
    }
    return s / f32(max(taps, 1u));
}
fn s_shadow(p: vec3f, n: vec3f, frag: vec2f) -> f32 {
    let nc = s_nc();
    let d = -p.z;
    if (nc == 0u || d >= L({BASE}u + 18u)[nc - 1u]) { return 1.0; }
    let c = s_cascade(d);
    let visibility = s_sample(c, p, n, frag);
    let far = L({BASE}u + 18u)[c];
    let q = s_clip(c, p);
    // Blend BOTH view-distance boundaries and the light-space edges of each cascade.
    // Lateral receivers can leave the near map before they reach its view-depth threshold.
    let edge = max(abs(q.x), abs(q.y));
    let fade = max(smoothstep(far * 0.8, far, d), smoothstep(0.8, 1.0, edge));
    if (fade <= 0.0) { return visibility; }
    if (c + 1u < nc) { return mix(visibility, s_sample(c + 1u, p, n, frag), fade); }
    return mix(visibility, 1.0, fade);
}
)WGSL";

const char* kPre = R"WGSL(
    let s_ndl = dot(l_n, s_sun().xyz);
    var s_vis = 1.0;
    // GX-lit draws (characters ...: the game shades them; only the shadow is added): the terminator wraps 0.35 past the
    // facet normal and the receiver is pushed out twice as far, so low-poly facets do not band
    let s_from = select(0.0, -0.35, l_gxlit || l_objrecv);
    if (s_sun().w > 0.5 && s_ndl > s_from) { s_vis = s_shadow(in.lview + l_n * select(0.0, 4.0, l_gxlit || l_objrecv), l_n, in.pos.xy); }
    let s_facing = smoothstep(s_from, 0.3, s_ndl);
    if (l_debug() == 9u) { l_has_override = true; l_override = vec3f(s_vis, s_vis * select(1.0, 0.6, s_sun().w < 0.5), s_facing); }   // 9: sun visibility (r), maps missing = yellow-ish, facing (b)
    // overlay (vanilla lighting kept): surfaces turned away from the sun also take 60 % of the shadow colour (the dynamic
    // sun direction; in Glyphic Canyon most walls face away and the layer was invisible); characters / replace unchanged
    let s_dark = (1.0 - s_vis) * s_facing + select(0.0, (1.0 - s_facing) * 0.4, !l_gxlit && !l_objrecv && !l_replace());
    l_shade = l_shade * mix(vec3f(1.0), L({BASE}u + 17u).rgb, s_dark);
    l_sshade = l_sshade * mix(vec3f(1.0), L({BASE}u + 17u).rgb, s_dark);
    l_direct = l_direct + L({BASE}u + 21u).rgb * (max(s_ndl, 0.0) * s_vis * L({BASE}u + 21u).w);
    l_sunmid = l_sunmid + L({BASE}u + 21u).rgb * (0.5 * L({BASE}u + 21u).w);)WGSL";

const char* kApply = R"WGSL(
    let s_dbg = l_debug();
    if (s_dbg == 1u) {
        l_has_override = true;
        l_override = vec3f(select(0.3, s_vis, s_ndl > 0.0));
    } else if (s_dbg == 2u) {
        let s_cc = array<vec3f, 4>(vec3f(1.0, 0.35, 0.35), vec3f(0.35, 1.0, 0.35), vec3f(0.35, 0.5, 1.0), vec3f(1.0, 1.0, 0.3));
        let s_ci = s_cascade(-in.lview.z);
        let s_in = -in.lview.z < L({BASE}u + 18u)[max(s_nc(), 1u) - 1u];
        l_has_override = true;
        l_override = select(vec3f(0.5), s_cc[s_ci], s_in) * (0.4 + 0.6 * s_vis);
    } else if (s_dbg == 3u) {
        l_has_override = true;
        l_override = l_n * 0.5 + vec3f(0.5);
    } else if (s_dbg == 5u) {
        l_has_override = true;
        l_override = L({BASE}u + 21u).rgb;
    } else if (s_dbg == 6u) {
        l_has_override = true;
        l_override = L({BASE}u + 16u).xyz * 0.5 + vec3f(0.5);
    } else if (s_dbg == 4u) {
        let s_pr = s_probe(in.lview);
        l_has_override = true;
        l_override = vec3f(s_pr.x, s_pr.y, clamp(abs(s_pr.x - s_pr.y) * 40.0, 0.0, 1.0));
    })WGSL";

Module g_module{"sun", 22, kDecls, kPre, kApply, nullptr, camera, boundary, nullptr};
}  // namespace

Module* sun_module() { return &g_module; }
bool shadow_sweep(float dir[3], float* reach) {
  if (g_shadow_reach <= 0) return false;
  dir[0] = g_shadow_dir.x; dir[1] = g_shadow_dir.y; dir[2] = g_shadow_dir.z;
  *reach = g_shadow_reach;
  return true;
}
}  // namespace lighting
