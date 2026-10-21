// [ShadowRecomp lighting] Scene capture and re-drawing from other viewpoints (include/aurora/lighting.hpp).
//
// Every GX draw becomes a DrawData (command_processor.cpp push_gx_draw). While capture is on, the 3D draws that write
// depth into the EFB are also copied here; a light-view variant of their pipeline (ShaderConfig::lightPass, see
// shader.cpp / gx.cpp build_pipeline) draws the same vertices, bind groups and uniforms from a light's point of view.
#include "../gfx/pipeline_cache.hpp"
#include "../gfx/recording.hpp"
#include "../gfx/resource_cache.hpp"
#include "../gfx/resources.hpp"
#include "gx.hpp"
#include "../internal.hpp"
#include "../webgpu/gpu.hpp"
#include "pipeline.hpp"

#include <aurora/lighting.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace aurora::lighting {
struct CapturedFrame {
  struct Draw {
    gfx::PipelineRef pipeline;   // light-view variant
    gfx::BindGroupRef textures;  // 0 = none (opaque variant)
    gfx::Range vertRange, idxRange, uniformRange;
    gx::DrawImmediateData imm;
    uint32_t vtxCount, indexCount, instanceCount;
    uint8_t cam = 0;
  };
  std::vector<Draw> draws;
  // dominant distant light (view space)
  float lightDir[3]{}, lightColor[3]{};
  float lightScore = 0;
  // the distinct model-view matrices of the frame (3x4 row-major): one of them is the pure view matrix (world
  // geometry has an identity model matrix) - the engine picks it to anchor everything in world space
  float mv[32][12]{};
  uint32_t mvCount = 0;
  // used bytes of the shared GPU buffers (the cache copy only needs these)
  uint32_t hwVert = 0, hwIdx = 0, hwUni = 0;
  void note(const gfx::Range& v, const gfx::Range& i, const gfx::Range& u) {
    hwVert = std::max(hwVert, v.offset + v.size);
    hwIdx = std::max(hwIdx, i.offset + i.size);
    hwUni = std::max(hwUni, u.offset + u.size);
  }
};
} // namespace aurora::lighting

namespace aurora::gx::lighting_capture {
namespace {
using aurora::lighting::CapturedFrame;
bool g_on = false;
uint32_t g_camId = 0;
bool g_lit = false;   // scene draws get the lit variant (lightPass 3)
// render worker: group 3 of lit draws for the frame being encoded (set by the engine's custom draw)
wgpu::BindGroup g_sceneGroup;
uint32_t g_sceneOffset = 0;
CapturedFrame* g_frame = nullptr;
bool g_lastCaptured = false;   // the last push_gx_draw was captured (merges extend it)
constexpr size_t kMaxDraws = 16384;

gfx::RenderTargetLayout light_layout() {
  gfx::RenderTargetLayout l{};
  l.key = 0x5352'4C49'4748'5450ull;   // "SRLIGHTP": our own layout key
  l.colorAttachmentCount = 0;
  l.depthStencilFormat = wgpu::TextureFormat::Depth32Float;
  l.sampleCount = 1;
  return l;
}

// normal pipeline ref -> light-view variant ref (configs are immutable per ref)
std::unordered_map<gfx::PipelineRef, gfx::PipelineRef> g_variants;

gfx::PipelineRef variant_for(const PipelineConfig& config, gfx::PipelineRef normal) {
  if (auto it = g_variants.find(normal); it != g_variants.end()) return it->second;
  PipelineConfig v = config;
  v.shaderConfig.lightPass = config.shaderConfig.alphaCompare ? 2 : 1;
  v.shaderConfig.fogType = GX_FOG_NONE;
  v.shaderConfig.fogRangeEnabled = false;
  v.cullMode = GX_CULL_NONE;
  v.blendMode = GX_BM_NONE;
  v.blendFacSrc = GX_BL_ONE;
  v.blendFacDst = GX_BL_ZERO;
  v.blendOp = GX_LO_CLEAR;
  v.dstAlpha = UINT32_MAX;
  v.polygonOffsetBits = v.polygonOffsetScaleBits = v.polygonOffsetClampBits = 0;
  v.depthCompare = true;
  v.depthFunc = GX_LESS;
  v.depthUpdate = true;
  v.alphaUpdate = false;
  v.colorUpdate = false;
  const auto ref = gfx::find_transient_pipeline(v, light_layout());
  g_variants.emplace(normal, ref);
  return ref;
}

void note_lights(CapturedFrame& f) {
  for (int c = 0; c < 2; ++c) {
    if (!g_gxState.colorChannelConfig[c].lightingEnabled) continue;
    const auto& mask = g_gxState.colorChannelState[c].lightMask;
    for (int i = 0; i < GX::MaxLights; ++i) {
      if (!mask.test(i)) continue;
      const auto& l = g_gxState.lights[i];
      const float px = l.pos.x(), py = l.pos.y(), pz = l.pos.z();
      const float d = std::sqrt(px * px + py * py + pz * pz);
      if (d < 1.0e4f) continue;   // only distant (directional) lights
      const float r = l.color.x(), g = l.color.y(), b = l.color.z();
      const float score = 0.2126f * r + 0.7152f * g + 0.0722f * b;
      if (score <= f.lightScore) continue;
      f.lightScore = score;
      f.lightDir[0] = -px / d;   // pointing from the light into the scene
      f.lightDir[1] = -py / d;
      f.lightDir[2] = -pz / d;
      f.lightColor[0] = r;
      f.lightColor[1] = g;
      f.lightColor[2] = b;
    }
  }
}
} // namespace

void on_draw(const PipelineConfig& config, const DrawData& d) noexcept {
  g_lastCaptured = false;
  if (!g_on) return;
  if (config.shaderConfig.lineMode != 0 || !config.depthCompare || !config.depthUpdate) return;
  if (g_gxState.projType != GX_PERSPECTIVE || gfx::is_offscreen()) return;
  extern bool g_castSkinned;
  if (!g_castSkinned && config.shaderConfig.colorChannels[0].lightingEnabled &&
      config.shaderConfig.attrs[GX_VA_PNMTXIDX].attrType != GX_NONE) return;   // a character: no engine shadow from it
  if (!g_frame) g_frame = new CapturedFrame;
  if (g_frame->draws.size() >= kMaxDraws) return;
  note_lights(*g_frame);
  if (g_frame->mvCount < 32) {
    const auto& m = g_gxState.pnMtx[g_gxState.currentPnMtx].pos;
    float v[12];
    std::memcpy(v, &m, sizeof v);
    bool seen = false;
    for (uint32_t i = 0; i < g_frame->mvCount; ++i) seen = seen || std::memcmp(g_frame->mv[i], v, sizeof v) == 0;
    if (!seen) std::memcpy(g_frame->mv[g_frame->mvCount++], v, sizeof v);
  }
  const bool cutout = static_cast<bool>(config.shaderConfig.alphaCompare);
  g_frame->draws.push_back({
      .pipeline = variant_for(config, d.pipeline),
      .textures = cutout ? d.bindGroups.textureBindGroup : gfx::BindGroupRef{},
      .vertRange = d.vertRange,
      .idxRange = d.idxRange,
      .uniformRange = d.uniformRange,
      .imm = d.immediateData,
      .vtxCount = d.vtxCount,
      .indexCount = d.indexCount,
      .instanceCount = d.instanceCount,
      .cam = static_cast<uint8_t>(std::min<uint32_t>(g_camId, 7)),
  });
  g_frame->note(d.vertRange, d.idxRange, d.uniformRange);
  g_lastCaptured = true;
}

bool scene_lit() noexcept { return g_lit; }
float g_worldView[12]{};
bool g_worldViewOk = false, g_litObjects = false;
bool g_objectReceive = true;   // static objects (not the map) receive the sun shadow on their own look (set_object_receive)
bool object_receive() noexcept { return g_objectReceive && !g_litObjects; }
bool world_draw() noexcept {
  if (g_litObjects) return true;
  if (!g_worldViewOk || g_gxState.currentPnMtx >= g_gxState.pnMtx.size()) return false;
  float m[12];
  std::memcpy(m, &g_gxState.pnMtx[g_gxState.currentPnMtx].pos, sizeof m);
  for (int i = 0; i < 12; ++i) {
    const float tol = (i % 4 == 3) ? 0.05f + 1e-4f * std::fabs(g_worldView[i]) : 2e-3f;   // translation column: relative
    if (!(std::fabs(m[i] - g_worldView[i]) <= tol)) return false;
  }
  return true;
}
bool g_castSkinned = true;
// ---- shader modules (include/aurora/lighting.hpp ShaderModule): the lit shader is assembled from the registered snippets
struct Mod { std::string name, decls, pre, apply, post; uint32_t base = 0, slots = 0; };
std::mutex g_modMutex;
std::vector<Mod> g_mods;
bool g_modsFrozen = false;   // generated pipeline hashes assume a fixed module set
uint32_t g_nextSlot = 4;   // 0..3: the core's
std::string subst(std::string s, uint32_t base) {
  const std::string key = "{BASE}", val = std::to_string(base);
  for (size_t at = s.find(key); at != std::string::npos; at = s.find(key, at + val.size())) s.replace(at, key.size(), val);
  return s;
}
std::string join(std::string Mod::*field) {
  std::lock_guard lk(g_modMutex);
  g_modsFrozen = true;
  std::string out;
  for (const auto& m : g_mods) out += subst(m.*field, m.base) + "\n";
  return out;
}
aurora::lighting::DrawFilter g_filter = nullptr;
void* g_filterUser = nullptr;
bool filter_active() noexcept { return g_filter != nullptr; }
std::string module_decls() { return join(&Mod::decls); }
std::string module_pre() { return join(&Mod::pre); }
std::string module_apply() { return join(&Mod::apply); }
std::string module_post() { return join(&Mod::post); }
bool filter_draw(const aurora::lighting::DrawInfo& info) noexcept { return g_filter && g_filter(info, g_filterUser); }
std::mutex g_litMutex;
std::unordered_map<gfx::PipelineRef, gfx::PipelineRef> g_litMap;
void register_lit(gfx::PipelineRef lit, gfx::PipelineRef normal) noexcept {
  std::lock_guard lk(g_litMutex);
  g_litMap.emplace(lit, normal);
}
bool lit_fallback(gfx::PipelineRef lit, gfx::PipelineRef& normal) noexcept {
  std::lock_guard lk(g_litMutex);
  const auto it = g_litMap.find(lit);
  if (it == g_litMap.end()) return false;
  normal = it->second;
  return true;
}
const wgpu::BindGroup* scene_group(uint32_t& dynamicOffset) noexcept {
  if (!g_sceneGroup) return nullptr;
  dynamicOffset = g_sceneOffset;
  return &g_sceneGroup;
}

void on_merge(const DrawData& merged) noexcept {
  if (!g_lastCaptured || !g_frame || g_frame->draws.empty()) return;
  auto& last = g_frame->draws.back();
  last.vertRange = merged.vertRange;
  last.idxRange = merged.idxRange;
  last.vtxCount = merged.vtxCount;
  last.indexCount = merged.indexCount;
  g_frame->note(merged.vertRange, merged.idxRange, merged.uniformRange);
}
} // namespace aurora::gx::lighting_capture

namespace aurora::lighting {
using namespace aurora::gx::lighting_capture;

void set_capture(bool on) noexcept {
  aurora::gx::lighting_capture::g_on = on;
  if (!on) aurora::gx::lighting_capture::g_lastCaptured = false;
}
bool capture_on() noexcept { return aurora::gx::lighting_capture::g_on; }
uint32_t register_shader_module(const ShaderModule& module) noexcept {
  using namespace aurora::gx::lighting_capture;
  std::lock_guard lk(g_modMutex);
  if (g_modsFrozen) return UINT32_MAX;   // reject a shader/CPU slot mismatch after compilation has begun
  for (const auto& existing : g_mods)
    if (existing.name == (module.name ? module.name : "")) return UINT32_MAX;
  Mod m;
  m.name = module.name ? module.name : "";
  m.decls = module.decls ? module.decls : "";
  m.pre = module.pre ? module.pre : "";
  m.apply = module.apply ? module.apply : "";
  m.post = module.post ? module.post : "";
  m.slots = module.slots;
  m.base = g_nextSlot;
  g_nextSlot += module.slots;
  if (g_nextSlot > SceneSlotCount) { g_nextSlot -= module.slots; return UINT32_MAX; }
  g_mods.push_back(std::move(m));
  return g_mods.back().base;
}
void set_draw_filter(DrawFilter filter, void* user) noexcept {
  aurora::gx::lighting_capture::g_filterUser = user;
  aurora::gx::lighting_capture::g_filter = filter;
}
void set_capture_camera(uint32_t index) noexcept { aurora::gx::lighting_capture::g_camId = index; }
void set_scene_lit(bool on) noexcept { aurora::gx::lighting_capture::g_lit = on; }
void set_world_view(const float m[12], bool objects) noexcept {
  std::memcpy(aurora::gx::lighting_capture::g_worldView, m, sizeof aurora::gx::lighting_capture::g_worldView);
  aurora::gx::lighting_capture::g_worldViewOk = true;
  aurora::gx::lighting_capture::g_litObjects = objects;
}
void set_cast_skinned(bool on) noexcept { aurora::gx::lighting_capture::g_castSkinned = on; }
void set_object_receive(bool on) noexcept { aurora::gx::lighting_capture::g_objectReceive = on; }
wgpu::BindGroupLayout lit_scene_layout() noexcept { return gx::lit_scene_bind_group_layout(); }
void set_scene_group(const wgpu::BindGroup& group, uint32_t dynamicOffset) noexcept {
  aurora::gx::lighting_capture::g_sceneGroup = group;
  aurora::gx::lighting_capture::g_sceneOffset = dynamicOffset;
}

CapturedFrame* take_captured() noexcept {
  auto*& f = aurora::gx::lighting_capture::g_frame;
  CapturedFrame* out = f;
  f = nullptr;
  aurora::gx::lighting_capture::g_lastCaptured = false;
  return out;
}
uint32_t captured_count(const CapturedFrame* frame) noexcept {
  return frame ? static_cast<uint32_t>(frame->draws.size()) : 0;
}
void release(CapturedFrame* frame) noexcept { delete frame; }

bool dominant_light(const CapturedFrame* frame, float dirView[3], float color[3]) noexcept {
  if (!frame || frame->lightScore <= 0) return false;
  for (int i = 0; i < 3; ++i) {
    dirView[i] = frame->lightDir[i];
    color[i] = frame->lightColor[i];
  }
  return true;
}

uint32_t view_candidates(const CapturedFrame* frame, float out[32][12]) noexcept {
  if (!frame) return 0;
  std::memcpy(out, frame->mv, sizeof frame->mv);
  return frame->mvCount;
}

namespace {
// The shared GPU buffers are rewritten from offset 0 every frame; the shadow pass of frame N+1 draws frame N's geometry
// from this copy (same offsets, so a captured draw replays unchanged).
struct Cache {
  wgpu::Buffer vert, uni, idx, stor;
  wgpu::BindGroup staticGroup, uniformGroup;
  bool valid = false;
} g_cache;
} // namespace

void cache_store(const CapturedFrame* frame, const wgpu::CommandEncoder& cmd) noexcept {
  if (!frame) return;
  auto& res = gfx::detail::resources();
  const wgpu::Device& dev = webgpu::g_device;
  if (!g_cache.vert) {
    auto mk = [&](const char* label, uint64_t size, wgpu::BufferUsage usage) {
      const wgpu::BufferDescriptor bd{.label = label, .usage = usage | wgpu::BufferUsage::CopyDst, .size = size};
      return dev.CreateBuffer(&bd);
    };
    g_cache.vert = mk("Light cache vertex", gfx::VertexBufferSize, wgpu::BufferUsage::Storage);
    g_cache.stor = mk("Light cache storage", gfx::StorageBufferSize, wgpu::BufferUsage::Storage);
    g_cache.uni = mk("Light cache uniform", gfx::UniformBufferSize, wgpu::BufferUsage::Uniform);
    g_cache.idx = mk("Light cache index", gfx::IndexBufferSize, wgpu::BufferUsage::Index);
    const std::array se{wgpu::BindGroupEntry{.binding = 0, .buffer = g_cache.vert},
                        wgpu::BindGroupEntry{.binding = 1, .buffer = g_cache.stor},
                        wgpu::BindGroupEntry{.binding = 2, .buffer = res.shadowBuffer}};
    const wgpu::BindGroupDescriptor sd{.label = "Light cache static", .layout = res.staticBindGroupLayout,
                                       .entryCount = se.size(), .entries = se.data()};
    g_cache.staticGroup = dev.CreateBindGroup(&sd);
    const wgpu::BindGroupEntry ue{.binding = 0, .buffer = g_cache.uni, .size = gx::MaxUniformSize};
    const wgpu::BindGroupDescriptor ud{.label = "Light cache uniform", .layout = res.uniformBindGroupLayout,
                                       .entryCount = 1, .entries = &ue};
    g_cache.uniformGroup = dev.CreateBindGroup(&ud);
  }
  auto copy = [&](const wgpu::Buffer& src, const wgpu::Buffer& dst, uint32_t used, uint64_t cap) {
    const uint64_t n = std::min<uint64_t>(AURORA_ALIGN(used, 4), cap);
    if (n) cmd.CopyBufferToBuffer(src, 0, dst, 0, n);
  };
  copy(res.vertexBuffer, g_cache.vert, frame->hwVert, gfx::VertexBufferSize);
  copy(res.indexBuffer, g_cache.idx, frame->hwIdx, gfx::IndexBufferSize);
  copy(res.uniformBuffer, g_cache.uni, frame->hwUni, gfx::UniformBufferSize);
  copy(res.storageBuffer, g_cache.stor, gfx::StorageBufferSize, gfx::StorageBufferSize);   // array data: size unknown
  g_cache.valid = true;
}
bool cache_valid() noexcept { return g_cache.valid; }

wgpu::Buffer shadow_buffer() noexcept { return gfx::detail::resources().shadowBuffer; }

wgpu::BindGroupLayout light_pass_layout() noexcept { return gx::light_pass_bind_group_layout(); }
wgpu::TextureFormat light_depth_format() noexcept { return wgpu::TextureFormat::Depth32Float; }

uint32_t replay(const CapturedFrame* frame, const wgpu::RenderPassEncoder& pass, const wgpu::BindGroup& group3,
                const uint32_t* camOffsets, uint32_t camCount) noexcept {
  if (!frame) return 0;
  if (!g_cache.valid) return 0;
  pass.SetBindGroup(0, g_cache.staticGroup);
  uint32_t curCam = UINT32_MAX;
  WGPURenderPipeline current = nullptr;
  gfx::BindGroupRef currentTex = 1;   // never a real ref: forces the first bind
  uint32_t issued = 0;
  for (const auto& d : frame->draws) {
    gfx::CompiledPipeline pipeline;
    if (!gfx::get_pipeline(d.pipeline, pipeline) || !pipeline.main) continue;
    if (d.cam != curCam) {   // each camera has its own view -> light matrix
      curCam = d.cam;
      const uint32_t off = camOffsets[std::min<uint32_t>(d.cam, camCount - 1)];
      pass.SetBindGroup(3, group3, 1, &off);
    }
    if (d.textures != currentTex) {
      if (d.textures) {
        const auto bg = gfx::find_bind_group(d.textures);
        if (!bg) continue;
        pass.SetBindGroup(2, bg);
      } else {
        pass.SetBindGroup(2, gx::g_emptyTextureBindGroup);
      }
      currentTex = d.textures;
    }
    if (pipeline.main.Get() != current) {
      pass.SetPipeline(pipeline.main);
      current = pipeline.main.Get();
    }
    pass.SetImmediates(0, &d.imm, sizeof(d.imm));
    const uint32_t off = d.uniformRange.offset;
    pass.SetBindGroup(1, g_cache.uniformGroup, 1, &off);
    if (d.indexCount == 0) {
      pass.Draw(d.vtxCount, d.instanceCount);
    } else {
      pass.SetIndexBuffer(g_cache.idx, wgpu::IndexFormat::Uint16, d.idxRange.offset, d.idxRange.size);
      pass.DrawIndexed(d.indexCount, d.instanceCount);
    }
    ++issued;
  }
  return issued;
}
} // namespace aurora::lighting
