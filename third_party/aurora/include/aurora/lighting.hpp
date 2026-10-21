#pragma once
// [ShadowRecomp lighting] Scene capture + re-drawing from other viewpoints (shadow maps, voxelisation).
// See LIGHTING-ENGINE.md in the ShadowRecomp root.

#include <aurora/gfx.hpp>

#include <cstdint>

namespace aurora::lighting {

/// Game thread. While on, every 3D draw into the EFB that writes depth (opaque and cut-out geometry; perspective
/// projection, triangles, not in an offscreen pass) is also kept for re-drawing.
void set_capture(bool on) noexcept;
/// Game thread: the screen camera (0, 1, 2 ... within the frame) the following captured draws are drawn through. The game
/// draws the scene through more than one RenderWare camera; each one has its own view matrix.
void set_capture_camera(uint32_t index) noexcept;
bool capture_on() noexcept;

/// Game thread. While on, the 3D scene draws into the EFB that write depth use the engine-lit variant of their
/// pipeline (shader lightPass 3: group 3 = scene lighting uniform + shadow cascades). Until set_scene_group() has
/// published a group for the frame, or while the lit pipeline compiles, the normal pipeline draws them.
void set_scene_lit(bool on) noexcept;
/// [ShadowRecomp] false = skinned GX-lit draws (characters, enemies) are not captured: they cast no engine shadow
void set_cast_skinned(bool on) noexcept;
/// [ShadowRecomp] the current camera's world-to-view matrix (3x4 row-major, as GX loads it for stage geometry, whose
/// model matrix is identity). Only draws with exactly this position matrix get the lit variant (the map); objects,
/// characters and skinned models keep the game's look. objects = true lights everything again (the old behaviour).
void set_world_view(const float m[12], bool objects) noexcept;
/// [ShadowRecomp] true (default) = static objects (not skinned, not the map) get a lit variant that only multiplies their
/// own colour by the sun shadow (#A3: a pillar's sand mound under another shadow); false = objects stay fully vanilla
void set_object_receive(bool on) noexcept;
/// Layout of group 3 for lit draws: binding 0 uniform only (dynamic offset; WGSL LightScene in shader.cpp).
/// Shadow storage lives in static group 0 binding 2; do not add textures/samplers here (NVIDIA driver workaround).
wgpu::BindGroupLayout lit_scene_layout() noexcept;
/// 64 MiB storage buffer the lit shaders read the sun cascades from: cascade c, row y, column x at float index
/// ((c * res) + y) * res + x (res <= 2048, c < 4). Fill it with a depth -> buffer copy in an encoder task.
wgpu::Buffer shadow_buffer() noexcept;
/// Render worker (e.g. from a custom draw callback, in drawing order): group 3 + dynamic offset used by the lit draws
/// encoded after it. A null group = lit draws fall back to their normal pipeline.
void set_scene_group(const wgpu::BindGroup& group, uint32_t dynamicOffset) noexcept;

/// What a draw is, as far as a lighting module needs to know (see set_draw_filter).
struct DrawInfo {
  bool perspective;           // 3D (perspective projection), not a 2D / HUD draw
  bool offscreen;             // into a render-to-texture pass
  bool depthTest, depthWrite;
  bool blended;               // standard alpha blending
  bool additive;              // blend factors add light (src one / src alpha with dst one)
  uint32_t blendSrc, blendDst;   // GXBlendFactor
  uint32_t vtxCount, indexCount;
  uint32_t texW, texH, texFmt;   // first sampled texture (0 when none); texFmt = GXTexFmt
  uint32_t tevStages;
  float origin[3];             // [ShadowRecomp] the position matrix translation: the draw's model origin in view space
  bool world;                  // the position matrix is the camera's view (stage geometry)
};
/// Game thread, called for every GX draw just before it is recorded. Return true to DROP the draw (a module that removes
/// something the engine replaces: the game's blob shadows, fake glows ...). One filter; nullptr removes it.
using DrawFilter = bool (*)(const DrawInfo& info, void* user);
void set_draw_filter(DrawFilter filter, void* user) noexcept;

/// A lighting MODULE's contribution to the lit scene shader (shader variant lightPass 3). Modules are WGSL snippets that
/// share one uniform block (`L(i)`: vec4 slot i, slots 0..3 are the core's, a module owns the `slots` it asked for, starting
/// at {BASE}) and a few accumulators the core declares in fs_main:
///   var l_direct: vec3f   light that depends on the surface direction (sun, point lights)  - added by modules
///   var l_ambient: vec3f  light from everywhere (sky, bounce)                               - added by modules
///   var l_occ: f32        ambient occlusion factor                                          - multiplied by modules
///   var l_shade: vec3f    what the ORIGINAL lighting is multiplied by when the engine does not replace it (shadows)
///   l_n (geometric normal, view space), in.lview (view-space position), in.pos (fragment coordinates)
///   l_has_override / l_override: a debug view may replace the final colour
/// Final colour = replace ? albedo * (l_ambient * l_occ + l_direct) : original * l_shade.
/// `decls` = module-scope WGSL (functions, bindings), `pre` = statements at the top of fs_main (uniform control flow,
/// derivatives allowed), `apply` = statements after the TEV stages. "{BASE}" is replaced by the module's first slot.
/// Register before the first lit pipeline is built (the engine does it at start-up); the set cannot change afterwards.
struct ShaderModule {
  const char* name;
  const char* decls;
  const char* pre;
  const char* apply;
  uint32_t slots;
  const char* post = nullptr; // after the final lighting combination, before authored fog (e.g. atmosphere)
};
/// Returns the module's first uniform slot (a vec4 index into the block; 96 slots in all), or UINT32_MAX for
/// late/duplicate registration or exhausted slots. Register before Aurora initialization (including cache warmup).
uint32_t register_shader_module(const ShaderModule& module) noexcept;
constexpr uint32_t SceneSlotCount = 96;

/// One frame's captured draws (opaque).
struct CapturedFrame;

/// Game thread: takes the draws captured since the last call (nullptr when none). The caller owns it until
/// release() (normally from the encoder task / its afterSubmit, on the render worker).
CapturedFrame* take_captured() noexcept;
uint32_t captured_count(const CapturedFrame* frame) noexcept;
void release(CapturedFrame* frame) noexcept;

/// The game's dominant directional light seen in this frame's lit draws (view space, pointing FROM the light), its
/// colour (0..1) and how many lit draws used it. False when no lit draw used a distant light this frame.
bool dominant_light(const CapturedFrame* frame, float dirView[3], float color[3]) noexcept;

/// The first (up to 32) distinct model-view matrices of the frame's captured draws, 3x4 row-major. World geometry has
/// an identity model matrix, so the view matrix is among them.
uint32_t view_candidates(const CapturedFrame* frame, float out[32][12]) noexcept;

/// Group 3 of the light-view variants: one uniform buffer binding (mat4x4f view_to_clip, row-vector convention like
/// GX: clip = vec4(view_pos, 1) * M, i.e. upload the conventional column-vector matrix row-major), dynamic offset.
wgpu::BindGroupLayout light_pass_layout() noexcept;
/// Depth format of the light-view targets (standard Z: 0 near, cleared to 1, compare Less).
wgpu::TextureFormat light_depth_format() noexcept;

/// Render worker, inside an encoder task at the end of a frame's scene: copies the used part of the frame's GPU buffers
/// (vertices, indices, uniforms, array storage) into the cache that replay() draws from, so the NEXT frame can re-draw
/// this frame's geometry (the shared buffers are rewritten from offset 0 every frame).
void cache_store(const CapturedFrame* frame, const wgpu::CommandEncoder& cmd) noexcept;
bool cache_valid() noexcept;

/// Render worker, inside an encoder task: draws the cached geometry of `frame` (the one cache_store() copied) the captured geometry into `pass` (depth only target of
/// light_depth_format) with the light-view variants. Returns the number of draws issued (variants still compiling
/// are skipped).
/// camOffsets[k] = the dynamic offset (into group 3's uniform buffer) of the view_to_clip matrix of camera k.
uint32_t replay(const CapturedFrame* frame, const wgpu::RenderPassEncoder& pass, const wgpu::BindGroup& group3,
                const uint32_t* camOffsets, uint32_t camCount) noexcept;

} // namespace aurora::lighting
