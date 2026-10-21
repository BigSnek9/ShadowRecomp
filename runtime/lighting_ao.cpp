// Lighting module: SCREEN-SPACE AMBIENT OCCLUSION (#113, settings light_ao_mode 1 / light_ao_quality / light_ao /
// light_ao_radius). Ground-truth AO in the style of Intel's XeGTAO (MIT; algorithm only, written fresh here): horizon
// angles along a few screen-space slices around each pixel, the visible arc integrated against the pixel's normal
// (cosine weighted), a distance falloff so far geometry adds no halo, then a depth-aware blur.
//
// How it fits the engine (lighting is applied inline in every lit draw, there is no full-scene depth before shading):
//   1. game thread, first camera of frame N: the AO view = the CURRENT camera with the game's own projection (the
//      previous frame's GX perspective, lighting.cpp scene_projection) at a fixed resolution (quality preset);
//   2. render worker (encoder task, like the sun cascades): the previous frame's cached geometry is re-drawn depth-only
//      through that view (aurora::lighting::replay), a compute pass computes GTAO from the depth, a second one blurs it
//      (depth-aware 5x5) into the static storage buffer at kAoOffset: per pixel (visibility, view distance);
//   3. lit shader (this module's WGSL): each pixel projects its view position into the AO view, fetches the 4 nearest
//      texels whose stored distance matches its own (no halos at depth edges, no smearing from the one-frame-old
//      geometry): replace mode multiplies its ambient (l_occ), the overlay the game's colour (l_shade).
// Static geometry is exact (it is re-drawn with this frame's camera); moving objects lag one frame (their texels fail
// the depth test and get no AO instead of a wrong one). The sky module's baked enclosure darkening is switched off in
// this mode (its sky visibility stays: that is sky light, not AO). AO_LOG=1: one line every 2 s.
#include "lighting_core.h"
#include "settings_ui.h"
#include "sys.h"
#include <aurora/gfx.hpp>
#include <aurora/lighting.hpp>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

namespace lighting {
bool scene_projection(float p[6]);   // lighting.cpp: the previous frame's GX perspective (p0..p5)

namespace {
using namespace aurora::gfx;
namespace al = aurora::lighting;

constexpr float kNear = 4.0f, kFar = 30000.0f;
struct Preset { uint32_t w, h, slices, steps; };
constexpr Preset kPresets[3] = {{960, 540, 2, 6}, {1280, 720, 3, 8}, {1600, 900, 4, 10}};

// ------------------------------------------------------------------ game thread state
bool g_on = false;
float g_P[6] = {};
Cam g_cam0;
EncoderTaskId g_taskType = InvalidEncoderTask;

struct Params {          // the compute passes' uniform (std140-friendly: vec4s)
  float size[4];         // w, h, 1/w, 1/h
  float proj[4];         // p0, p1, p2, p3 (GX perspective: x_ndc = (p0 x + p1 z) / -z, y_ndc = (p2 y + p3 z) / -z)
  float zp[4];           // A, B (depth -> distance), radius (world units), falloff share
  float misc[4];         // slices, steps, final power, -
};
struct TaskPayload {
  uint32_t w, h, ncam;
  uint32_t offs[kMaxCams];
  al::CapturedFrame* frame;
  Params params;
};

// ------------------------------------------------------------------ render worker
struct Gpu {
  wgpu::Texture depth;
  wgpu::TextureView depthView;
  wgpu::Buffer raw, params;
  wgpu::ComputePipeline gtao, blur;
  wgpu::BindGroup gtaoGroup, blurGroup, lightPassGroup;
  const void* groupBuffer = nullptr;
  const void* outBuffer = nullptr;
  uint32_t w = 0, h = 0;
} g_gpu;

const char* kGtaoWgsl = R"WGSL(
struct Params { size: vec4f, proj: vec4f, zp: vec4f, misc: vec4f };
@group(0) @binding(0) var dtex: texture_depth_2d;
@group(0) @binding(1) var<uniform> P: Params;
@group(0) @binding(2) var<storage, read_write> raw: array<f32>;
const PI = 3.14159265;
fn dist_at(p: vec2i) -> f32 {
  let q = clamp(p, vec2i(0), vec2i(P.size.xy) - vec2i(1));
  let d = textureLoad(dtex, q, 0);
  if (d >= 1.0) { return 1e9; }
  return P.zp.y / (d + P.zp.x);
}
fn view_at(p: vec2f, dist: f32) -> vec3f {
  let ndc = vec2f((p.x + 0.5) * P.size.z * 2.0 - 1.0, 1.0 - (p.y + 0.5) * P.size.w * 2.0);
  return vec3f((ndc.x + P.proj.y) * dist / P.proj.x, (ndc.y + P.proj.w) * dist / P.proj.z, -dist);
}
fn fast_acos(x: f32) -> f32 {
  let a = abs(x);
  var r = -0.156583 * a + PI * 0.5;
  r = r * sqrt(1.0 - a);
  return select(PI - r, r, x >= 0.0);
}
@compute @workgroup_size(8, 8)
fn cs_main(@builtin(global_invocation_id) id: vec3u) {
  let w = u32(P.size.x); let h = u32(P.size.y);
  if (id.x >= w || id.y >= h) { return; }
  let px = vec2i(id.xy);
  let o = (id.y * w + id.x) * 2u;
  let d0 = dist_at(px);
  if (d0 > 1e8) { raw[o] = 1.0; raw[o + 1u] = 1e9; return; }
  let pos = view_at(vec2f(px), d0);
  // normal from the depth: the neighbour on each axis with the smaller step (no smearing over edges)
  let dl = dist_at(px - vec2i(1, 0)); let dr = dist_at(px + vec2i(1, 0));
  let du = dist_at(px - vec2i(0, 1)); let dd = dist_at(px + vec2i(0, 1));
  let pl = view_at(vec2f(px - vec2i(1, 0)), dl); let pr = view_at(vec2f(px + vec2i(1, 0)), dr);
  let pu = view_at(vec2f(px - vec2i(0, 1)), du); let pd = view_at(vec2f(px + vec2i(0, 1)), dd);
  let dx = select(pos - pl, pr - pos, abs(dr - d0) < abs(dl - d0));
  let dy = select(pos - pu, pd - pos, abs(dd - d0) < abs(du - d0));
  var n = normalize(cross(dx, dy));
  let v = normalize(-pos);
  if (dot(n, v) < 0.0) { n = -n; }
  let radius = P.zp.z;
  let falloffFrom = radius * (1.0 - P.zp.w);
  // the radius on screen (pixels), capped so a surface right at the camera does not sample half the screen
  let rpx = min(radius * P.proj.x * 0.5 * P.size.x / d0, 0.08 * P.size.x);
  if (rpx < 1.0) { raw[o] = 1.0; raw[o + 1u] = d0; return; }
  let slices = u32(P.misc.x); let steps = u32(P.misc.y);
  // interleaved gradient noise: a fixed per-pixel rotation / jitter (no temporal noise -> stable picture; the blur hides it)
  let ign = fract(52.9829189 * fract(dot(vec2f(px), vec2f(0.06711056, 0.00583715))));
  let ign2 = fract(ign * 1.61803398 + 0.5);
  var vis = 0.0;
  for (var s = 0u; s < slices; s++) {
    let phi = (f32(s) + ign) / f32(slices) * PI;
    let dir = vec3f(cos(phi), sin(phi), 0.0);          // view space (x right, y up)
    let omega = vec2f(dir.x, -dir.y);                   // screen space (y down)
    let ortho = dir - dot(dir, v) * v;
    let axis = normalize(cross(ortho, v));
    let pn = n - axis * dot(n, axis);
    let pnLen = length(pn);
    let cosN = clamp(dot(pn, v) / max(pnLen, 1e-5), 0.0, 1.0);
    let nAng = sign(dot(ortho, pn)) * fast_acos(cosN);
    let low0 = cos(nAng + PI * 0.5); let low1 = cos(nAng - PI * 0.5);
    var h0 = low0; var h1 = low1;
    for (var j = 0u; j < steps; j++) {
      var t = (f32(j) + ign2) / f32(steps);
      t = t * t;                                        // more samples near the pixel: thin contact shading
      let off = omega * max(t * rpx, 1.0 + f32(j));
      let sp0 = vec2f(px) + off; let sp1 = vec2f(px) - off;
      let s0 = view_at(sp0, dist_at(vec2i(round(sp0)))) - pos;
      let s1 = view_at(sp1, dist_at(vec2i(round(sp1)))) - pos;
      let l0 = length(s0); let l1 = length(s1);
      let w0 = clamp((radius - l0) / max(radius - falloffFrom, 1e-3), 0.0, 1.0);
      let w1 = clamp((radius - l1) / max(radius - falloffFrom, 1e-3), 0.0, 1.0);
      h0 = max(h0, mix(low0, dot(s0 / max(l0, 1e-4), v), w0));
      h1 = max(h1, mix(low1, dot(s1 / max(l1, 1e-4), v), w1));
    }
    var a0 = -fast_acos(h1);
    var a1 = fast_acos(h0);
    a0 = nAng + clamp(a0 - nAng, -PI * 0.5, PI * 0.5);
    a1 = nAng + clamp(a1 - nAng, -PI * 0.5, PI * 0.5);
    let i0 = (cosN + 2.0 * a0 * sin(nAng) - cos(2.0 * a0 - nAng)) * 0.25;
    let i1 = (cosN + 2.0 * a1 * sin(nAng) - cos(2.0 * a1 - nAng)) * 0.25;
    vis += pnLen * (i0 + i1);
  }
  vis = clamp(vis / f32(slices), 0.0, 1.0);
  raw[o] = pow(vis, P.misc.z);
  raw[o + 1u] = d0;
}
)WGSL";

const char* kBlurWgsl = R"WGSL(
struct Params { size: vec4f, proj: vec4f, zp: vec4f, misc: vec4f };
@group(0) @binding(0) var<storage, read> raw: array<f32>;
@group(0) @binding(1) var<uniform> P: Params;
@group(0) @binding(2) var<storage, read_write> outb: array<f32>;
@compute @workgroup_size(8, 8)
fn cs_main(@builtin(global_invocation_id) id: vec3u) {
  let w = u32(P.size.x); let h = u32(P.size.y);
  if (id.x >= w || id.y >= h) { return; }
  let o = (id.y * w + id.x) * 2u;
  let d0 = raw[o + 1u];
  var sum = 0.0; var wsum = 0.0;
  for (var y = -2; y <= 2; y++) {
    for (var x = -2; x <= 2; x++) {
      let q = clamp(vec2i(id.xy) + vec2i(x, y), vec2i(0), vec2i(i32(w) - 1, i32(h) - 1));
      let k = (u32(q.y) * w + u32(q.x)) * 2u;
      let dk = raw[k + 1u];
      let wd = exp(-abs(dk - d0) / max(0.02 * d0, 0.5));   // same surface only
      let ws = select(1.0, 0.5, abs(x) == 2 || abs(y) == 2);
      sum += raw[k] * wd * ws; wsum += wd * ws;
    }
  }
  outb[o] = sum / max(wsum, 1e-4);
  outb[o + 1u] = d0;
}
)WGSL";

wgpu::ComputePipeline make_pipeline(const wgpu::Device& dev, const char* code, const char* label) {
  const wgpu::ShaderSourceWGSL src{wgpu::ShaderSourceWGSL::Init{.code = code}};
  const wgpu::ShaderModuleDescriptor md{.nextInChain = &src, .label = label};
  const auto module = dev.CreateShaderModule(&md);
  const wgpu::ComputePipelineDescriptor pd{.label = label, .layout = nullptr,
                                           .compute = wgpu::ComputeState{.module = module, .entryPoint = "cs_main"}};
  return dev.CreateComputePipeline(&pd);
}

void ensure(const EncoderTaskContext& ctx, uint32_t w, uint32_t h) {
  const wgpu::Device& dev = ctx.device;
  if (!g_gpu.gtao) {
    g_gpu.gtao = make_pipeline(dev, kGtaoWgsl, "GTAO");
    g_gpu.blur = make_pipeline(dev, kBlurWgsl, "GTAO blur");
    const wgpu::BufferDescriptor pb{.label = "GTAO params", .usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst,
                                    .size = sizeof(Params)};
    g_gpu.params = dev.CreateBuffer(&pb);
  }
  if (g_gpu.groupBuffer != ctx.uniformBuffer.Get() || !g_gpu.lightPassGroup) {
    const wgpu::BindGroupEntry le{.binding = 0, .buffer = ctx.uniformBuffer, .size = 64};
    const wgpu::BindGroupDescriptor ld{.label = "GTAO light pass group", .layout = al::light_pass_layout(), .entryCount = 1, .entries = &le};
    g_gpu.lightPassGroup = dev.CreateBindGroup(&ld);
    g_gpu.groupBuffer = ctx.uniformBuffer.Get();
  }
  const wgpu::Buffer out = al::shadow_buffer();
  if (g_gpu.w == w && g_gpu.h == h && g_gpu.depth && g_gpu.outBuffer == out.Get()) return;
  g_gpu.w = w; g_gpu.h = h; g_gpu.outBuffer = out.Get();
  const wgpu::TextureDescriptor td{.label = "GTAO depth",
                                   .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding,
                                   .dimension = wgpu::TextureDimension::e2D, .size = {w, h, 1},
                                   .format = wgpu::TextureFormat::Depth32Float};
  g_gpu.depth = dev.CreateTexture(&td);
  g_gpu.depthView = g_gpu.depth.CreateView();
  const uint64_t bytes = uint64_t(w) * h * 2 * 4;
  const wgpu::BufferDescriptor rb{.label = "GTAO raw", .usage = wgpu::BufferUsage::Storage, .size = bytes};
  g_gpu.raw = dev.CreateBuffer(&rb);
  const wgpu::BindGroupEntry ge[3] = {{.binding = 0, .textureView = g_gpu.depthView},
                                      {.binding = 1, .buffer = g_gpu.params, .size = sizeof(Params)},
                                      {.binding = 2, .buffer = g_gpu.raw, .size = bytes}};
  const wgpu::BindGroupDescriptor gd{.label = "GTAO group", .layout = g_gpu.gtao.GetBindGroupLayout(0), .entryCount = 3, .entries = ge};
  g_gpu.gtaoGroup = dev.CreateBindGroup(&gd);
  const wgpu::BindGroupEntry be[3] = {{.binding = 0, .buffer = g_gpu.raw, .size = bytes},
                                      {.binding = 1, .buffer = g_gpu.params, .size = sizeof(Params)},
                                      {.binding = 2, .buffer = out, .offset = uint64_t(kAoOffset) * 4, .size = bytes}};
  const wgpu::BindGroupDescriptor bd{.label = "GTAO blur group", .layout = g_gpu.blur.GetBindGroupLayout(0), .entryCount = 3, .entries = be};
  g_gpu.blurGroup = dev.CreateBindGroup(&bd);
}

void ao_task(const EncoderTaskContext& ctx, const wgpu::CommandEncoder& cmd, const void* payload, size_t size, void*) {
  if (size != sizeof(TaskPayload)) return;
  TaskPayload p;
  memcpy(&p, payload, sizeof p);
  if (!p.frame || !p.w || !p.h) return;
  ensure(ctx, p.w, p.h);
  ctx.queue.WriteBuffer(g_gpu.params, 0, &p.params, sizeof p.params);
  {
    const wgpu::RenderPassDepthStencilAttachment da{.view = g_gpu.depthView, .depthLoadOp = wgpu::LoadOp::Clear,
                                                    .depthStoreOp = wgpu::StoreOp::Store, .depthClearValue = 1.0f};
    const wgpu::RenderPassDescriptor rd{.label = "GTAO depth", .colorAttachmentCount = 0, .depthStencilAttachment = &da};
    const auto pass = cmd.BeginRenderPass(&rd);
    al::replay(p.frame, pass, g_gpu.lightPassGroup, p.offs, std::max(p.ncam, 1u));
    pass.End();
  }
  const wgpu::ComputePassDescriptor cd{.label = "GTAO"};
  const auto pass = cmd.BeginComputePass(&cd);
  pass.SetPipeline(g_gpu.gtao);
  pass.SetBindGroup(0, g_gpu.gtaoGroup);
  pass.DispatchWorkgroups((p.w + 7) / 8, (p.h + 7) / 8);
  pass.SetPipeline(g_gpu.blur);
  pass.SetBindGroup(0, g_gpu.blurGroup);
  pass.DispatchWorkgroups((p.w + 7) / 8, (p.h + 7) / 8);
  pass.End();
}

// ------------------------------------------------------------------ game thread
M4 world_to_view(const Cam& k) {
  const V3 xr = k.r * -1.0f, yu = k.u, za = k.a * -1.0f;   // view axes in world: x = -right, y = up, z = -at
  return {{xr.x, xr.y, xr.z, -dot(xr, k.p), yu.x, yu.y, yu.z, -dot(yu, k.p), za.x, za.y, za.z, -dot(za, k.p), 0, 0, 0, 1}};
}

void camera(CameraCtx& ctx) {
  const auto& s = settings::g;
  float* sl = ctx.slots;
  const Preset& q = kPresets[std::clamp(s.light_ao_quality, 0, 2)];
  if (ctx.index == 0) {
    if (g_taskType == InvalidEncoderTask) g_taskType = register_encoder_task_type({.label = "gtao", .callback = ao_task});
    g_on = s.light_ao_mode == 1 && s.light_ao > 0 && ctx.prev && ctx.ncached > 0 && al::cache_valid() &&
           g_taskType != InvalidEncoderTask && scene_projection(g_P) && g_P[0] > 0 && g_P[2] > 0;
    g_cam0 = ctx.cam;
    if (g_on) {
      const float A = -kFar / (kFar - kNear), B = -kFar * kNear / (kFar - kNear);
      const M4 proj{{g_P[0], 0, g_P[1], 0, 0, g_P[2], g_P[3], 0, 0, 0, A, B, 0, 0, -1, 0}};
      const M4 w2v = world_to_view(ctx.cam);
      TaskPayload tp{q.w, q.h, (uint32_t)std::min(ctx.ncached, kMaxCams), {}, ctx.prev, {}};
      for (int k = 0; k < ctx.ncached && k < kMaxCams; ++k) {
        const M4 m = mul(proj, mul(w2v, ctx.cached[k].view_to_world()));
        tp.offs[k] = push_uniform(reinterpret_cast<const uint8_t*>(m.m), sizeof m.m).offset;
      }
      const float radius = 40.f * std::clamp(s.light_ao_radius, 25, 300) / 100.f;
      tp.params = Params{{float(q.w), float(q.h), 1.f / q.w, 1.f / q.h}, {g_P[0], g_P[1], g_P[2], g_P[3]},
                         {A, B, radius, 0.6f}, {float(q.slices), float(q.steps), 1.6f, 0}};   // depth -> distance: B / (d + A)
      push_encoder_task(g_taskType, &tp, sizeof tp);
    }
  }
  if (!g_on) return;   // slot 4.z = 0: the shader skips the lookup
  // this camera's view space -> the AO view (camera 0) for the lookup
  const M4 t = mul(world_to_view(g_cam0), ctx.cam.view_to_world());
  memcpy(sl, t.m, 12 * sizeof(float));
  sl[12] = g_P[0]; sl[13] = g_P[1]; sl[14] = g_P[2]; sl[15] = g_P[3];
  sl[16] = float(q.w); sl[17] = float(q.h); sl[18] = 1.f; sl[19] = std::clamp(s.light_ao, 0, 200) / 100.f;
  static const bool log = getenv("AO_LOG") != nullptr;
  static uint64_t last = 0;
  if (log && ctx.index == 0 && sys_time_ns() - last > 2000000000ull) {
    last = sys_time_ns();
    logf("[ao] gtao %ux%u, %u slices x %u steps, radius %.0f, strength %.2f, proj %.3f %.3f %.3f %.3f\n", q.w, q.h, q.slices, q.steps,
         40.f * std::clamp(s.light_ao_radius, 25, 300) / 100.f, sl[19], g_P[0], g_P[1], g_P[2], g_P[3]);
  }
}

const char* kDecls = R"WGSL(
fn g_ao_tap(x: i32, y: i32, w: i32, h: i32, dist: f32, wt: f32, acc: ptr<function, vec2f>) {
  if (x < 0 || y < 0 || x >= w || y >= h) { return; }
  let o = {AO_OFF}u + u32(y * w + x) * 2u;
  let ok = abs(lshadow[o + 1u] - dist) < max(0.03 * dist, 2.0);
  if (ok && wt > 0.0) { (*acc) += vec2f(lshadow[o] * wt, wt); }
}
// screen-space AO at view position p (this camera's view space): 1 = open, 0 = fully occluded
fn g_ao(p: vec3f) -> f32 {
  let s = L({BASE}u + 4u);
  if (s.z < 0.5) { return 1.0; }
  let p4 = vec4f(p, 1.0);
  let q = vec3f(dot(L({BASE}u), p4), dot(L({BASE}u + 1u), p4), dot(L({BASE}u + 2u), p4));
  let dist = -q.z;
  if (dist < 1.0) { return 1.0; }
  let pr = L({BASE}u + 3u);
  let ndc = vec2f((pr.x * q.x + pr.y * q.z) / dist, (pr.z * q.y + pr.w * q.z) / dist);
  let f = vec2f((ndc.x * 0.5 + 0.5) * s.x - 0.5, (0.5 - ndc.y * 0.5) * s.y - 0.5);
  let i = vec2i(floor(f));
  let t = f - floor(f);
  var acc = vec2f(0.0);
  let w = i32(s.x); let h = i32(s.y);
  g_ao_tap(i.x, i.y, w, h, dist, (1.0 - t.x) * (1.0 - t.y), &acc);
  g_ao_tap(i.x + 1, i.y, w, h, dist, t.x * (1.0 - t.y), &acc);
  g_ao_tap(i.x, i.y + 1, w, h, dist, (1.0 - t.x) * t.y, &acc);
  g_ao_tap(i.x + 1, i.y + 1, w, h, dist, t.x * t.y, &acc);
  if (acc.y < 1e-3) { return 1.0; }
  return pow(acc.x / acc.y, clamp(s.w, 0.0, 2.0));   // strength: 1 = as computed, lower = lighter, 2 = darker
}
)WGSL";
const char* kPre = R"WGSL(
    let g_aov = g_ao(in.lview);
    // replace mode: the ambient term; overlay: straight onto the game's colour at the slider's strength (not through the
    // overlay's AO share, which is tuned for the old blobby volume and would hide the contact shading)
    if (l_replace()) { l_occ = l_occ * g_aov; } else { l_shade = l_shade * g_aov; })WGSL";
const char* kApply = R"WGSL(
    if (l_debug() == 15u) { l_has_override = true; l_override = vec3f(g_aov); })WGSL";   // light_debug=15: the screen-space AO alone

Module g_module{"gtao", 5, nullptr, kPre, kApply, nullptr, camera, nullptr, nullptr};
std::string g_decls;
}  // namespace

Module* ao_module() {
  if (g_decls.empty()) {
    g_decls = kDecls;
    const std::string key = "{AO_OFF}", val = std::to_string(kAoOffset);
    for (size_t i; (i = g_decls.find(key)) != std::string::npos;) g_decls.replace(i, key.size(), val);
    g_module.wgsl_decls = g_decls.c_str();
  }
  return &g_module;
}
}  // namespace lighting
