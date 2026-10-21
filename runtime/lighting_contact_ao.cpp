// #148 CONTACT AO: ambient occlusion from this frame's own depth, for a prelit game (docs/lighting/AO-148.md).
//
// At the HUD marker (the 3D scene is complete) Aurora's resolve_pass snapshots the scene colour + depth, then:
//   1. encoder task, compute at half resolution: horizon search on the full-resolution depth along two directions per
//      pixel (a fixed 4x4 interleaved pattern = 32 directions per tile), contact reach (ao radius), an angle bias so
//      low-poly creases and gentle slopes collect nothing, a falloff so far geometry adds no halo;
//      then a depth-aware 4x4 blur (exactly one pattern tile: the pattern averages out, no noise, no history);
//   2. custom draw in the resumed scene pass: a full-screen triangle, depth-aware 2x2 upsample, and a darkening weight
//      from the pixel's own brightness (dark baked corners and bright / glowing pixels are left alone), multiplied
//      onto the scene (blend dst * src) before the HUD is drawn.
// Characters and moving objects are in the depth: no lag, no popping. Off by default (settings ao / ao_strength).
// AO_VIEW=1: the darkening alone (white = none), 2: the raw occlusion. AO_ALT: every other frame (tests). AO_RADIUS / AO_BIAS / AO_LOG.
#include "lighting_contact_ao.h"
#include "settings_ui.h"
#include "sys.h"
#include <aurora/gfx.hpp>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace contact_ao {
namespace {
using namespace aurora::gfx;

struct Params {      // std140-friendly vec4s, shared by the three passes
  float size[4];     // full W, H, half w, half h
  float proj[4];     // GX p0..p3: x_ndc = (p0 x + p1 z) / -z, y_ndc = (p2 y + p3 z) / -z
  float zv[4];       // p4, p5 (reversed-Z depth D: distance = p5 / (p4 - D)), radius (world units), angle bias
  float vp[4];       // the scene viewport on the target, 0..1: left, top, width, height
  float misc[4];     // strength (max darkening), fade start, fade end (distance), debug view
};
struct Payload { uint32_t slot; Params p; };

std::mutex g_mx;
struct Snap { wgpu::TextureView color, depth; uint32_t w = 0, h = 0; };
Snap g_ring[8];
uint32_t g_next = 0;
EncoderTaskId g_task = InvalidEncoderTask;
DrawTypeId g_draw = InvalidDrawType;
bool g_failed = false;

constexpr const char* kCommon = R"WGSL(
struct P { size: vec4f, proj: vec4f, zv: vec4f, vp: vec4f, misc: vec4f };
const PI = 3.14159265;
const LUMA = vec3f(0.2126, 0.7152, 0.0722);
fn dist_from(d: f32) -> f32 {
  if (d <= 0.0) { return 1e9; }                // reversed Z: the clear value = far
  let den = U.zv.x - d;
  if (den >= 0.0) { return 1e9; }
  return U.zv.y / den;
}
fn view_at(q: vec2f, dist: f32) -> vec3f {
  let uv = (q + 0.5) / U.size.xy;
  let ndc = vec2f((uv.x - U.vp.x) / U.vp.z * 2.0 - 1.0, 1.0 - (uv.y - U.vp.y) / U.vp.w * 2.0);
  return vec3f((ndc.x + U.proj.y) * dist / U.proj.x, (ndc.y + U.proj.w) * dist / U.proj.z, -dist);
}
)WGSL";

constexpr const char* kAo = R"WGSL(
@group(0) @binding(0) var dtex: texture_2d<f32>;
@group(0) @binding(1) var<uniform> U: P;
@group(0) @binding(2) var<storage, read_write> raw: array<vec2f>;
fn dist_at(q: vec2i) -> f32 {
  let c = clamp(q, vec2i(0), vec2i(U.size.xy) - vec2i(1));
  return dist_from(textureLoad(dtex, c, 0).r);
}
@compute @workgroup_size(8, 8)
fn cs_main(@builtin(global_invocation_id) id: vec3u) {
  let hw = u32(U.size.z); let hh = u32(U.size.w);
  if (id.x >= hw || id.y >= hh) { return; }
  let o = id.y * hw + id.x;
  let q = vec2i(id.xy) * 2;
  let d0 = dist_at(q);
  if (d0 > U.misc.z) { raw[o] = vec2f(1.0, d0); return; }
  let pos = view_at(vec2f(q), d0);
  // the normal from the depth: on each axis the neighbour with the smaller step (no smearing over edges)
  let dl = dist_at(q - vec2i(1, 0)); let dr = dist_at(q + vec2i(1, 0));
  let du = dist_at(q - vec2i(0, 1)); let dd = dist_at(q + vec2i(0, 1));
  let dx = select(pos - view_at(vec2f(q - vec2i(1, 0)), dl), view_at(vec2f(q + vec2i(1, 0)), dr) - pos, abs(dr - d0) < abs(dl - d0));
  let dy = select(pos - view_at(vec2f(q - vec2i(0, 1)), du), view_at(vec2f(q + vec2i(0, 1)), dd) - pos, abs(dd - d0) < abs(du - d0));
  var n = normalize(cross(dx, dy));
  if (dot(n, -pos) < 0.0) { n = -n; }
  let R = U.zv.z;
  let bias = U.zv.w;
  // the reach on screen in full-resolution pixels, capped (a surface at the camera must not sample half the screen)
  let rpx = min(U.proj.x * R / d0 * 0.5 * U.vp.z * U.size.x, 0.06 * U.size.x);
  if (rpx < 2.0) { raw[o] = vec2f(1.0, d0); return; }
  var bayer = array<f32, 16>(0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0, 3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0);
  let t = (id.x & 3u) + 4u * (id.y & 3u);
  let rot = (f32(t) + 0.5) / 16.0;
  let jit = (bayer[t] + 0.5) / 16.0;
  var occ = 0.0;
  for (var s = 0u; s < 2u; s++) {
    let phi = (f32(s) + rot) * 0.5 * PI;
    let dir = vec2f(cos(phi), sin(phi));
    for (var side = 0u; side < 2u; side++) {
      let sg = select(-1.0, 1.0, side == 0u);
      var best = 0.0;
      for (var j = 0u; j < 6u; j++) {
        let tt = (f32(j) + jit) / 6.0;
        let sp = round(vec2f(q) + dir * sg * max(tt * tt * rpx, 1.0 + f32(j)));
        let sd = dist_at(vec2i(sp));
        if (sd > 1e8) { continue; }
        let sv = view_at(sp, sd) - pos;
        let l = length(sv);
        if (l < 1e-3) { continue; }
        let fall = clamp(1.0 - l * l / (R * R), 0.0, 1.0);
        best = max(best, (dot(n, sv / l) - bias) * fall);
      }
      occ += best;
    }
  }
  occ = occ / (4.0 * (1.0 - bias));
  raw[o] = vec2f(clamp(1.0 - occ, 0.0, 1.0), d0);
}
)WGSL";

constexpr const char* kBlur = R"WGSL(
@group(0) @binding(0) var<storage, read> raw: array<vec2f>;
@group(0) @binding(1) var<uniform> U: P;
@group(0) @binding(2) var<storage, read_write> outb: array<vec2f>;
@compute @workgroup_size(8, 8)
fn cs_main(@builtin(global_invocation_id) id: vec3u) {
  let hw = i32(U.size.z); let hh = i32(U.size.w);
  if (i32(id.x) >= hw || i32(id.y) >= hh) { return; }
  let o = id.y * u32(hw) + id.x;
  let d0 = raw[o].y;
  if (d0 > U.misc.z) { outb[o] = vec2f(1.0, d0); return; }
  var sum = 0.0; var wsum = 0.0;
  for (var y = -1; y <= 2; y++) {
    for (var x = -1; x <= 2; x++) {
      let qx = clamp(i32(id.x) + x, 0, hw - 1); let qy = clamp(i32(id.y) + y, 0, hh - 1);
      let r = raw[u32(qy * hw + qx)];
      let w = select(0.0, 1.0, abs(r.y - d0) < 0.04 * d0 + 0.5);   // the same surface only
      sum += r.x * w; wsum += w;
    }
  }
  outb[o] = vec2f(sum / max(wsum, 1e-4), d0);
}
)WGSL";

constexpr const char* kComposite = R"WGSL(
@group(0) @binding(0) var ctex: texture_2d<f32>;
@group(0) @binding(1) var dtex: texture_2d<f32>;
@group(0) @binding(2) var<uniform> U: P;
@group(0) @binding(3) var<storage, read> aob: array<vec2f>;
@vertex
fn vs_main(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {
  let p = vec2f(f32((i << 1u) & 2u), f32(i & 2u));
  return vec4f(p * 2.0 - 1.0, 0.0, 1.0);
}
fn tap(b: vec2i, w: f32, d: f32, acc: ptr<function, vec2f>) {
  let hw = i32(U.size.z); let hh = i32(U.size.w);
  if (w <= 0.0 || b.x < 0 || b.y < 0 || b.x >= hw || b.y >= hh) { return; }
  let r = aob[u32(b.y * hw + b.x)];
  if (abs(r.y - d) < 0.05 * d + 1.0) { (*acc) += vec2f(r.x * w, w); }
}
@fragment
fn fs_main(@builtin(position) fp: vec4f) -> @location(0) vec4f {
  let q = vec2i(fp.xy);
  let qc = clamp(q, vec2i(0), vec2i(U.size.xy) - vec2i(1));
  let d = dist_from(textureLoad(dtex, qc, 0).r);
  if (d > U.misc.z) { return vec4f(1.0); }
  let h = vec2f(qc) * 0.5;
  let b = vec2i(floor(h));
  let f = h - floor(h);
  var acc = vec2f(0.0);
  tap(b, (1.0 - f.x) * (1.0 - f.y), d, &acc);
  tap(b + vec2i(1, 0), f.x * (1.0 - f.y), d, &acc);
  tap(b + vec2i(0, 1), (1.0 - f.x) * f.y, d, &acc);
  tap(b + vec2i(1, 1), f.x * f.y, d, &acc);
  let ao = select(1.0, acc.x / max(acc.y, 1e-4), acc.y > 1e-3);
  if (U.misc.w > 1.5) { return vec4f(vec3f(ao), 1.0); }   // AO_VIEW=2: the raw occlusion
  let fade = 1.0 - smoothstep(U.misc.y, U.misc.z, d);
  let L = dot(min(textureLoad(ctex, qc, 0).rgb, vec3f(1.0)), LUMA);
  // dark pixels (the bake's own corners, night) and bright / glowing ones get none: only the mid-lit surfaces
  let wl = smoothstep(0.03, 0.22, L) * (1.0 - smoothstep(0.65, 0.95, L));
  let k = clamp(U.misc.x * (1.0 - ao) * wl * fade, 0.0, 0.9);
  return vec4f(vec3f(1.0 - k), 1.0);
}
)WGSL";

// GPU_TIMESTAMPS=1 (tests): the compute passes' GPU time, averaged, logged every 2 s with AO_LOG ("[ao] gpu")
struct Timing {
  bool init = false, on = false;
  wgpu::QuerySet qs;
  wgpu::Buffer resolve, rb[3];
  std::atomic<int> state[3] = {0, 0, 0};   // 0 free, 1 copied (map after submit), 2 mapping, 3 mapped
  int pending = -1;
  double sum = 0; int n = 0; uint64_t last = 0;
} g_tm;
void timing_init(const wgpu::Device& dev) {
  g_tm.init = true;
  g_tm.on = dev.HasFeature(wgpu::FeatureName::TimestampQuery);
  if (!g_tm.on) return;
  const wgpu::QuerySetDescriptor qd{.label = "contact AO timestamps", .type = wgpu::QueryType::Timestamp, .count = 2};
  g_tm.qs = dev.CreateQuerySet(&qd);
  const wgpu::BufferDescriptor rd{.label = "contact AO ts resolve", .usage = wgpu::BufferUsage::QueryResolve | wgpu::BufferUsage::CopySrc, .size = 256};
  g_tm.resolve = dev.CreateBuffer(&rd);
  const wgpu::BufferDescriptor bd{.label = "contact AO ts readback", .usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst, .size = 16};
  for (auto& b : g_tm.rb) b = dev.CreateBuffer(&bd);
  logf("[ao] GPU timestamps on\n");
}
void timing_collect() {   // mapped readbacks -> the average
  for (int i = 0; i < 3; ++i) {
    if (g_tm.state[i].load() != 3) continue;
    uint64_t ts[2];
    memcpy(ts, g_tm.rb[i].GetConstMappedRange(0, 16), 16);
    g_tm.rb[i].Unmap();
    g_tm.state[i] = 0;
    if (ts[1] > ts[0]) { g_tm.sum += double(ts[1] - ts[0]) * 1e-6; ++g_tm.n; }
  }
  if (g_tm.n && sys_time_ns() - g_tm.last > 2000000000ull) {
    g_tm.last = sys_time_ns();
    logf("[ao] gpu compute %.3f ms (avg of %d frames)\n", g_tm.sum / g_tm.n, g_tm.n);
    g_tm.sum = 0; g_tm.n = 0;
  }
}
void after_submit(const EncoderTaskCompletionContext&, const void*, size_t, void*) {
  const int i = g_tm.pending;
  g_tm.pending = -1;
  if (i < 0) return;
  g_tm.state[i] = 2;
  g_tm.rb[i].MapAsync(wgpu::MapMode::Read, 0, 16, wgpu::CallbackMode::AllowSpontaneous,
                      [i](wgpu::MapAsyncStatus st, wgpu::StringView) { g_tm.state[i] = st == wgpu::MapAsyncStatus::Success ? 3 : 0; });
}

struct Gpu {
  wgpu::BindGroupLayout aoL, blurL, compL;
  wgpu::ComputePipeline ao, blur;
  wgpu::Buffer params, raw, out;
  uint32_t hw = 0, hh = 0;
  wgpu::BindGroup compGroup;   // this frame's (written by the task, used by the draw right after it)
  uint64_t pipeKey = ~0ull;
  wgpu::RenderPipeline comp;
} g_gpu;

wgpu::ShaderModule module(const wgpu::Device& dev, const char* body, const char* label) {
  const std::string code = std::string(kCommon) + body;   // WGSL module-scope declarations are order-independent
  const wgpu::ShaderSourceWGSL src{wgpu::ShaderSourceWGSL::Init{.code = code.c_str()}};
  const wgpu::ShaderModuleDescriptor md{.nextInChain = &src, .label = label};
  return dev.CreateShaderModule(&md);
}

wgpu::BindGroupLayoutEntry tex(uint32_t b, wgpu::ShaderStage st) {
  return wgpu::BindGroupLayoutEntry{.binding = b, .visibility = st,
                                    .texture = {.sampleType = wgpu::TextureSampleType::UnfilterableFloat,
                                                .viewDimension = wgpu::TextureViewDimension::e2D}};
}
wgpu::BindGroupLayoutEntry buf(uint32_t b, wgpu::ShaderStage st, wgpu::BufferBindingType t) {
  return wgpu::BindGroupLayoutEntry{.binding = b, .visibility = st, .buffer = {.type = t}};
}
wgpu::BindGroupLayout layout(const wgpu::Device& dev, const wgpu::BindGroupLayoutEntry* e, size_t n, const char* label) {
  const wgpu::BindGroupLayoutDescriptor d{.label = label, .entryCount = n, .entries = e};
  return dev.CreateBindGroupLayout(&d);
}
wgpu::ComputePipeline compute(const wgpu::Device& dev, const wgpu::BindGroupLayout& l, const char* body, const char* label) {
  const wgpu::PipelineLayoutDescriptor pl{.bindGroupLayoutCount = 1, .bindGroupLayouts = &l};
  const wgpu::ComputePipelineDescriptor pd{.label = label, .layout = dev.CreatePipelineLayout(&pl),
                                           .compute = wgpu::ComputeState{.module = module(dev, body, label), .entryPoint = "cs_main"}};
  return dev.CreateComputePipeline(&pd);
}

void ensure(const wgpu::Device& dev, uint32_t hw, uint32_t hh) {
  constexpr auto C = wgpu::ShaderStage::Compute;
  constexpr auto F = wgpu::ShaderStage::Fragment;
  if (!g_gpu.ao) {
    const wgpu::BindGroupLayoutEntry a[3] = {tex(0, C), buf(1, C, wgpu::BufferBindingType::Uniform), buf(2, C, wgpu::BufferBindingType::Storage)};
    g_gpu.aoL = layout(dev, a, 3, "contact AO");
    const wgpu::BindGroupLayoutEntry b[3] = {buf(0, C, wgpu::BufferBindingType::ReadOnlyStorage), buf(1, C, wgpu::BufferBindingType::Uniform),
                                             buf(2, C, wgpu::BufferBindingType::Storage)};
    g_gpu.blurL = layout(dev, b, 3, "contact AO blur");
    const wgpu::BindGroupLayoutEntry c[4] = {tex(0, F), tex(1, F), buf(2, F, wgpu::BufferBindingType::Uniform),
                                             buf(3, F, wgpu::BufferBindingType::ReadOnlyStorage)};
    g_gpu.compL = layout(dev, c, 4, "contact AO composite");
    g_gpu.ao = compute(dev, g_gpu.aoL, kAo, "contact AO");
    g_gpu.blur = compute(dev, g_gpu.blurL, kBlur, "contact AO blur");
    const wgpu::BufferDescriptor pb{.label = "contact AO params", .usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst,
                                    .size = sizeof(Params)};
    g_gpu.params = dev.CreateBuffer(&pb);
  }
  if (g_gpu.hw == hw && g_gpu.hh == hh && g_gpu.raw) return;
  g_gpu.hw = hw; g_gpu.hh = hh;
  const uint64_t bytes = uint64_t(hw) * hh * 8;
  const wgpu::BufferDescriptor rb{.label = "contact AO raw", .usage = wgpu::BufferUsage::Storage, .size = bytes};
  g_gpu.raw = dev.CreateBuffer(&rb);
  const wgpu::BufferDescriptor ob{.label = "contact AO", .usage = wgpu::BufferUsage::Storage, .size = bytes};
  g_gpu.out = dev.CreateBuffer(&ob);
}

void task(const EncoderTaskContext& ctx, const wgpu::CommandEncoder& cmd, const void* payload, size_t size, void*) {
  g_gpu.compGroup = nullptr;
  if (size != sizeof(Payload)) return;
  Payload p;
  memcpy(&p, payload, sizeof p);
  Snap s;
  {
    std::lock_guard lk(g_mx);
    s = g_ring[p.slot % 8];
  }
  if (!s.color || !s.depth || !s.w || !s.h) return;
  const uint32_t hw = (s.w + 1) / 2, hh = (s.h + 1) / 2;
  ensure(ctx.device, hw, hh);
  ctx.queue.WriteBuffer(g_gpu.params, 0, &p.p, sizeof p.p);
  const uint64_t bytes = uint64_t(hw) * hh * 8;
  const wgpu::BindGroupEntry a[3] = {{.binding = 0, .textureView = s.depth}, {.binding = 1, .buffer = g_gpu.params, .size = sizeof(Params)},
                                     {.binding = 2, .buffer = g_gpu.raw, .size = bytes}};
  const wgpu::BindGroupDescriptor ad{.label = "contact AO", .layout = g_gpu.aoL, .entryCount = 3, .entries = a};
  const auto aoGroup = ctx.device.CreateBindGroup(&ad);
  const wgpu::BindGroupEntry b[3] = {{.binding = 0, .buffer = g_gpu.raw, .size = bytes}, {.binding = 1, .buffer = g_gpu.params, .size = sizeof(Params)},
                                     {.binding = 2, .buffer = g_gpu.out, .size = bytes}};
  const wgpu::BindGroupDescriptor bd{.label = "contact AO blur", .layout = g_gpu.blurL, .entryCount = 3, .entries = b};
  const auto blurGroup = ctx.device.CreateBindGroup(&bd);
  const wgpu::BindGroupEntry c[4] = {{.binding = 0, .textureView = s.color}, {.binding = 1, .textureView = s.depth},
                                     {.binding = 2, .buffer = g_gpu.params, .size = sizeof(Params)}, {.binding = 3, .buffer = g_gpu.out, .size = bytes}};
  const wgpu::BindGroupDescriptor cd{.label = "contact AO composite", .layout = g_gpu.compL, .entryCount = 4, .entries = c};
  g_gpu.compGroup = ctx.device.CreateBindGroup(&cd);
  if (!g_tm.init) timing_init(ctx.device);
  if (g_tm.on) timing_collect();
  const wgpu::PassTimestampWrites tw{.querySet = g_tm.qs, .beginningOfPassWriteIndex = 0, .endOfPassWriteIndex = 1};
  const wgpu::ComputePassDescriptor pd{.label = "contact AO", .timestampWrites = g_tm.on ? &tw : nullptr};
  const auto pass = cmd.BeginComputePass(&pd);
  pass.SetPipeline(g_gpu.ao);
  pass.SetBindGroup(0, aoGroup);
  pass.DispatchWorkgroups((hw + 7) / 8, (hh + 7) / 8);
  pass.SetPipeline(g_gpu.blur);
  pass.SetBindGroup(0, blurGroup);
  pass.DispatchWorkgroups((hw + 7) / 8, (hh + 7) / 8);
  pass.End();
  if (g_tm.on && g_tm.pending < 0) {
    for (int i = 0; i < 3; ++i) {
      if (g_tm.state[i].load() != 0) continue;
      cmd.ResolveQuerySet(g_tm.qs, 0, 2, g_tm.resolve, 0);
      cmd.CopyBufferToBuffer(g_tm.resolve, 0, g_tm.rb[i], 0, 16);
      g_tm.state[i] = 1;
      g_tm.pending = i;
      break;
    }
  }
}

void draw(const DrawContext& ctx, const wgpu::RenderPassEncoder& pass, const void* payload, size_t size, void*) {
  if (!g_gpu.compGroup || size != sizeof(uint32_t)) return;
  uint32_t debug;
  memcpy(&debug, payload, sizeof debug);
  const auto& L = ctx.layout;
  const uint64_t key = L.key * 2 + debug;
  if (key != g_gpu.pipeKey || !g_gpu.comp) {
    g_gpu.pipeKey = key;
    const wgpu::BlendState mul{.color = {.operation = wgpu::BlendOperation::Add, .srcFactor = wgpu::BlendFactor::Zero, .dstFactor = wgpu::BlendFactor::Src},
                               .alpha = {.operation = wgpu::BlendOperation::Add, .srcFactor = wgpu::BlendFactor::Zero, .dstFactor = wgpu::BlendFactor::One}};
    wgpu::ColorTargetState targets[MaxColorAttachments];
    for (uint32_t i = 0; i < L.colorAttachmentCount && i < MaxColorAttachments; ++i) {
      targets[i].format = L.colorAttachments[i].format;
      targets[i].blend = i == 0 && !debug ? &mul : nullptr;
      targets[i].writeMask = i == 0 ? wgpu::ColorWriteMask::All : wgpu::ColorWriteMask::None;
    }
    const auto mod = module(ctx.device, kComposite, "contact AO composite");
    const wgpu::FragmentState fs{.module = mod, .entryPoint = "fs_main", .targetCount = L.colorAttachmentCount, .targets = targets};
    const wgpu::DepthStencilState ds{.format = L.depthStencilFormat, .depthWriteEnabled = wgpu::OptionalBool::False,
                                     .depthCompare = wgpu::CompareFunction::Always};
    const wgpu::PipelineLayoutDescriptor pl{.bindGroupLayoutCount = 1, .bindGroupLayouts = &g_gpu.compL};
    const wgpu::RenderPipelineDescriptor rd{
        .label = "contact AO composite",
        .layout = ctx.device.CreatePipelineLayout(&pl),
        .vertex = wgpu::VertexState{.module = mod, .entryPoint = "vs_main"},
        .primitive = wgpu::PrimitiveState{.topology = wgpu::PrimitiveTopology::TriangleList},
        .depthStencil = L.depthStencilFormat != wgpu::TextureFormat::Undefined ? &ds : nullptr,
        .multisample = wgpu::MultisampleState{.count = std::max(L.sampleCount, 1u), .mask = UINT32_MAX},
        .fragment = &fs,
    };
    g_gpu.comp = ctx.device.CreateRenderPipeline(&rd);
  }
  const uint32_t w = L.colorAttachments[0].width, h = L.colorAttachments[0].height;
  pass.SetPipeline(g_gpu.comp);
  pass.SetBindGroup(0, g_gpu.compGroup);
  if (w && h) {
    pass.SetViewport(0, 0, float(w), float(h), 0, 1);
    pass.SetScissorRect(0, 0, w, h);
  }
  pass.Draw(3);
  g_gpu.compGroup = nullptr;
}
}  // namespace

void run(const float* proj) {
  const auto& s = settings::g;
  if (!s.ao || !proj || g_failed) return;
  static const bool kAlt = getenv("AO_ALT") != nullptr;   // tests: AO on every other frame (FRAMES = exact before / after)
  static uint32_t alt = 0;
  if (kAlt && (alt++ & 1)) return;
  if (proj[0] <= 0 || proj[2] <= 0 || proj[8] <= 0 || proj[9] <= 0) return;
  if (g_task == InvalidEncoderTask) {
    g_task = register_encoder_task_type({.label = "contact AO", .callback = task, .afterSubmit = after_submit});
    g_draw = register_draw_type({.label = "contact AO composite", .draw = draw});
    if (g_task == InvalidEncoderTask || g_draw == InvalidDrawType) { g_failed = true; return; }
  }
  ResolvedTargets rt;
  if (!resolve_pass(ResolveDesc{.color = true, .depth = true}, rt)) return;
  if (!rt.color || !rt.depth || !rt.width || !rt.height) {
    static bool warned = false;
    if (!warned) { warned = true; logf("[ao] no depth snapshot on this device: contact AO off\n"); }
    return;
  }
  static const float kRadius = [] { const char* e = getenv("AO_RADIUS"); return e ? float(atof(e)) : 25.0f; }();
  static const float kBias = [] { const char* e = getenv("AO_BIAS"); return e ? float(atof(e)) : 0.3f; }();
  static const int kView = getenv("AO_VIEW") ? atoi(getenv("AO_VIEW")) : 0;
  Payload p{};
  {
    std::lock_guard lk(g_mx);
    p.slot = g_next++ % 8;
    g_ring[p.slot] = Snap{rt.color, rt.depth, rt.width, rt.height};
  }
  const float strength = 0.6f * std::clamp(s.ao_strength, 0, 200) / 100.f;
  p.p = Params{{float(rt.width), float(rt.height), float((rt.width + 1) / 2), float((rt.height + 1) / 2)},
               {proj[0], proj[1], proj[2], proj[3]},
               {proj[4], proj[5], kRadius, std::clamp(kBias, 0.f, 0.9f)},
               {proj[6], proj[7], proj[8], proj[9]},
               {strength, 1500.f, 2500.f, float(kView)}};
  push_encoder_task(g_task, &p, sizeof p);
  const uint32_t debug = kView ? 1u : 0u;
  push_custom_draw(g_draw, &debug, sizeof debug);
  static const bool log = getenv("AO_LOG") != nullptr;
  static uint64_t last = 0;
  if (log && sys_time_ns() - last > 2000000000ull) {
    last = sys_time_ns();
    logf("[ao] contact AO %ux%u, radius %.1f, bias %.2f, strength %.2f, proj %g %g %g %g %g %g, viewport %.3f %.3f %.3f %.3f\n", rt.width,
         rt.height, kRadius, kBias, strength, proj[0], proj[1], proj[2], proj[3], proj[4], proj[5], proj[6], proj[7], proj[8], proj[9]);
  }
}
}  // namespace contact_ao
