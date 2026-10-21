#pragma once

#include "../gfx/pipeline_cache.hpp"
#include "gx.hpp"
#include <aurora/lighting.hpp>
#include <string>

namespace aurora::gx {
struct DrawData {
  gfx::PipelineRef pipeline;
  GXBindGroups bindGroups;
  gfx::Range vertRange;
  gfx::Range idxRange;
  gfx::Range uniformRange;
  DrawImmediateData immediateData;
  uint32_t vtxCount;
  uint32_t indexCount;
  uint32_t instanceCount;
  uint32_t dstAlpha;
};

constexpr uint32_t GXPipelineConfigVersion = 13;
struct PipelineConfig {
  uint32_t version = GXPipelineConfigVersion;
  uint32_t msaaSamples = 1; // deprecated
  ShaderConfig shaderConfig;
  GXCompare depthFunc;
  GXCullMode cullMode;
  GXBlendMode blendMode;
  GXBlendFactor blendFacSrc, blendFacDst;
  GXLogicOp blendOp;
  uint32_t dstAlpha;
  uint32_t polygonOffsetBits;
  uint32_t polygonOffsetScaleBits;
  uint32_t polygonOffsetClampBits;
  bool depthCompare, depthUpdate, alphaUpdate, colorUpdate;
};
static_assert(std::has_unique_object_representations_v<PipelineConfig>);

struct PipelineOptions {
  DstAlphaMode dstAlphaMode = DstAlphaMode::None;
  bool colorUpdate = false;
  bool alphaUpdate = false;
  bool depthUpdate = false;
};

gfx::CompiledPipeline create_pipeline(const PipelineConfig& config, const gfx::RenderTargetLayout& layout);
void render(const DrawData& data, const wgpu::RenderPassEncoder& pass);

void queue_surface(const u8* dlStart, uint32_t dlSize, bool bigEndian) noexcept;

namespace lighting_capture {   // [ShadowRecomp lighting] lib/gx/lighting.cpp
void on_draw(const PipelineConfig& config, const DrawData& draw) noexcept;   // after every pushed GX draw
void on_merge(const DrawData& merged) noexcept;                              // after a draw merged into the last
bool scene_lit() noexcept;
bool world_draw() noexcept;   // the current position matrix is the camera's view (stage geometry), see set_world_view
bool object_receive() noexcept;   // static objects get the shadow-only lit variant (set_object_receive)
bool filter_active() noexcept;
std::string module_decls();   // the lit shader's module-scope WGSL: the core's + every registered module's
std::string module_pre();
std::string module_apply();
std::string module_post();
bool filter_draw(const aurora::lighting::DrawInfo& info) noexcept;   // true = drop the draw   // game thread: scene draws get the lit variant (lightPass 3)
// render worker: group 3 for lit draws (null group = not published yet -> the draw uses its fallback pipeline)
const wgpu::BindGroup* scene_group(uint32_t& dynamicOffset) noexcept;
// a lit pipeline (lightPass 3) and the normal pipeline it replaces; DrawData has no room for a second ref
void register_lit(gfx::PipelineRef lit, gfx::PipelineRef normal) noexcept;
bool lit_fallback(gfx::PipelineRef lit, gfx::PipelineRef& normal) noexcept;   // false = not a lit pipeline
} // namespace lighting_capture
} // namespace aurora::gx
