// The lighting engine's CORE - see docs/lighting/LIGHTING-ENGINE.md (plan, design, status) and lighting_core.h (the module interface).
//
// Per frame N+1:
//   camera_begin (the exact camera matrix, in drawing order, FIFO marker 0x04): every distinct camera becomes an index; the
//     modules fill their slots of the per-camera scene uniform block, the block is pushed and a custom draw publishes it as
//     group 3 of the lit scene draws that follow.
//   before_hud (the scene is complete, the HUD starts): the modules get the boundary callback; this frame's captured draws
//     are copied to a GPU cache (the shared buffers are rewritten every frame) and become "the previous frame" for N+2.
#include "lighting.h"
#include "lighting_core.h"
#include "lighting_environment.h"
#include "sys.h"
#include "settings_ui.h"
#include "game.h"
#include <aurora/gfx.hpp>
#include <aurora/lighting.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace lighting {
Module* sun_module();    // lighting_sun.cpp
Module* sky_module();    // lighting_sky.cpp
Module* game_module();   // lighting_game.cpp
Module* glow_module();   // lighting_glow.cpp
Module* ao_module();     // lighting_ao.cpp

namespace {
using namespace aurora::gfx;
namespace al = aurora::lighting;

constexpr uint32_t kSceneBytes = al::SceneSlotCount * 16;
std::vector<Module*> g_modules;
bool g_registered = false;
Env g_env;
uint32_t g_cam = 0;   // the screen camera (RwCamera*)
// #116: in_stage() drops whenever the player's move step pauses (grind rails, poles, ledge hangs, some springs), which
// switched the engine (and with it the lit shading) off and on mid-stage. Start on in_stage(); then stay on until a
// menu, a cutscene or the end of the stage. LIGHT_GATE=1 restores the old per-step gate (A/B tests).
bool engine_on() {
  static bool on = false;
  static const bool old_gate = getenv("LIGHT_GATE") != nullptr;
  bool now;
  if (!settings::g.light) now = false;
  else if (game::in_stage()) now = true;
  else if (old_gate || game::menus_active() || game::cutscene_active() || (int32_t)ld32(0x8057D748) < 0) now = false;
  else now = on;
  if (now != on) logf("[light] engine %s (in_stage %d menus %d cutscene %d stage %d)\n", now ? "on" : "off",
                      game::in_stage(), game::menus_active(), game::cutscene_active(), (int)(int32_t)ld32(0x8057D748));
  return on = now;
}

// ---- per-frame camera bookkeeping ----
Cam g_frame_cams[kMaxCams];
int g_frame_ncam = 0;
Cam g_cached_cams[kMaxCams];   // the cameras the cached geometry of the previous frame was drawn through
int g_cached_ncam = 0;
Cam g_cam_now;
al::CapturedFrame* g_prev = nullptr;   // previous frame's captured draws (their buffers are in the GPU cache)
uint64_t g_frames = 0;

// ---- render worker ----
enum TaskOp : uint32_t { kOpCache = 0, kOpRelease = 1 };
struct TaskPayload { uint32_t op; al::CapturedFrame* frame; };

void core_task(const EncoderTaskContext&, const wgpu::CommandEncoder& cmd, const void* payload, size_t size, void*) {
  if (size != sizeof(TaskPayload)) return;
  TaskPayload p;
  memcpy(&p, payload, sizeof p);
  if (p.op == kOpCache) al::cache_store(p.frame, cmd);
}
// the captured frame goes away once the task that last needs it has been submitted
void core_task_done(const EncoderTaskCompletionContext&, const void* payload, size_t size, void*) {
  if (size != sizeof(TaskPayload)) return;
  TaskPayload p;
  memcpy(&p, payload, sizeof p);
  if (p.op == kOpRelease) al::release(p.frame);
}

struct GroupState { wgpu::BindGroup group; const void* buffer = nullptr; } g_group;
struct GroupPayload { uint32_t offset; };
// inside the EFB pass: publish (or withdraw) group 3 for the lit draws that follow
void group_draw(const DrawContext& ctx, const wgpu::RenderPassEncoder&, const void* payload, size_t size, void*) {
  if (size != sizeof(GroupPayload)) return;
  GroupPayload p;
  memcpy(&p, payload, sizeof p);
  if (p.offset == UINT32_MAX) { al::set_scene_group({}, 0); return; }
  if (g_group.buffer != ctx.uniformBuffer.Get() || !g_group.group) {
    const wgpu::BindGroupEntry e{.binding = 0, .buffer = ctx.uniformBuffer, .size = kSceneBytes};
    const wgpu::BindGroupDescriptor d{.label = "Lit scene group", .layout = al::lit_scene_layout(), .entryCount = 1, .entries = &e};
    g_group.group = ctx.device.CreateBindGroup(&d);
    g_group.buffer = ctx.uniformBuffer.Get();
  }
  al::set_scene_group(g_group.group, p.offset);
}

DrawTypeId g_groupType = InvalidDrawType;
EncoderTaskId g_taskType = InvalidEncoderTask;

bool init() {
  if (g_taskType != InvalidEncoderTask) return true;
  register_builtin_modules();
  g_groupType = register_draw_type({.label = "light scene group", .draw = group_draw});
  g_taskType = register_encoder_task_type({.label = "light core", .callback = core_task, .afterSubmit = core_task_done});
  return g_groupType != InvalidDrawType && g_taskType != InvalidEncoderTask;
}
void push_task(const TaskPayload& tp) { push_encoder_task(g_taskType, &tp, sizeof tp); }

// every module's draw filter, in order
bool draw_filter(const al::DrawInfo& d, void*) {
  for (Module* m : g_modules)
    if (m->draw_filter && m->draw_filter(d)) return true;
  return false;
}
}  // namespace

Env& env() { return g_env; }

void register_builtin_modules() {
  if (g_registered) return;
  // Aurora compiles cached lit pipelines during initialization, before any game frame/camera exists.
  // Register the complete, fixed shader set first; lazy registration in init() leaves those shaders unlit.
  g_registered = true;
  register_module(sun_module());
  register_module(sky_module());
  register_module(game_module());
  register_module(environment_module());
  register_module(glow_module());
  register_module(ao_module());
}

void register_module(Module* m) {
  // the WGSL goes to Aurora once, before the first lit pipeline is built; the callbacks are called by the core
  al::ShaderModule sm{m->name, m->wgsl_decls, m->wgsl_pre, m->wgsl_apply, m->slots, m->wgsl_post};
  m->base = al::register_shader_module(sm);
  if (m->base == UINT32_MAX) { logf("[light] module %s: registration rejected (late, duplicate or out of slots)\n", m->name); return; }
  g_modules.push_back(m);
}

bool wants_camera() { return engine_on(); }
void note_screen_camera(uint32_t cam) { g_cam = cam; }

// The screen camera begins its update: from here the frame's 3D scene is drawn through this camera.
void camera_begin(const float ltm[12]) {
  g_cam_now.set(ltm);
  {   // stage geometry is drawn with exactly this matrix (identity model): only it gets the lit shader
    const Cam& k = g_cam_now;
    const float wv[12] = {-k.r.x, -k.r.y, -k.r.z, dot(k.r, k.p), k.u.x, k.u.y, k.u.z, -dot(k.u, k.p),
                          -k.a.x, -k.a.y, -k.a.z, dot(k.a, k.p)};
    al::set_world_view(wv, settings::g.light_objects);
    al::set_object_receive(settings::g.light_object_shadows);   // #A3: objects keep their look but receive the sun shadow
  }
  if (!engine_on() || is_offscreen() || !init()) return;
  // the game calls BeginUpdate many times per frame (28 seen), mostly with the same camera: only a changed view matrix is
  // a new camera for the engine (the group published for the previous one stays valid)
  static float last[12];
  static uint64_t last_frame = ~0ull;
  if (g_frame_ncam > 0 && last_frame == g_frames && !memcmp(last, ltm, sizeof last)) return;
  memcpy(last, ltm, sizeof last);
  last_frame = g_frames;
  const int idx = std::min(g_frame_ncam++, kMaxCams - 1);
  g_frame_cams[idx] = g_cam_now;
  al::set_capture_camera((uint32_t)idx);

  if (idx == 0) update_environment(g_cam_now);

  std::array<float, al::SceneSlotCount * 4> scene{};
  static const char* dbgEnv = getenv("SHADOW_LIGHT_DEBUG");
  scene[0] = settings::g.light_replace ? 1.0f : 0.0f;
  static const int kView[5] = {0, 13, 11, 12, 10};   // light_view -> the shader's debug code (overlay views)
  const int view = kView[std::clamp(settings::g.light_view, 0, 4)];
  scene[1] = (float)(settings::g.light_debug ? settings::g.light_debug : view ? view : dbgEnv ? atoi(dbgEnv) : 0);
  const Look lk = look();
  scene[2] = lk.exposure;                   // core slot 0 z / w (overlay: the look)
  static const float unlit_env = getenv("LIGHT_UNLIT") ? (float)atof(getenv("LIGHT_UNLIT")) : -1.f;   // test: unlit distance
  scene[3] = unlit_env >= 0 ? unlit_env : atmosphere().unlit_distance;
  static const float baked_env = getenv("LIGHT_BAKED") ? (float)atof(getenv("LIGHT_BAKED")) : -1.f;   // test: replace mode's baked share
  // core slot 1 x: replace mode's share of the game's baked light - the stage profile's 'baked', at least the setting
  // (#110: with 0 night streets, interiors and baked-colour surfaces like Lethal Highway's goop went black)
  scene[4] = baked_env >= 0 ? baked_env : std::max(atmosphere().baked, std::clamp(settings::g.light_baked, 0, 100) / 100.f);
  scene[5] = lk.saturation;                 // core slot 1 y: overlay saturation
  scene[6] = lk.ao;                         // core slot 1 z: overlay AO share
  scene[7] = settings::g.light_world_only ? 1.0f : 0.0f;   // core slot 1 w: characters keep the game shading
  scene[9] = lk.gi.x; scene[10] = lk.gi.y; scene[11] = lk.gi.z;   // core slot 2 yzw: overlay GI bounce tint
  scene[8] = lk.fill;                       // core slot 2 x: overlay ambient fill (1 = as is, 0 = no darkening)
  for (Module* m : g_modules) {
    if (!m->camera) continue;
    CameraCtx ctx{g_cam_now, idx, g_cached_cams, g_cached_ncam, g_prev, scene.data() + m->base * 4, m->base};
    m->camera(ctx);
  }
  static const bool slog = getenv("LIGHT_LOG") != nullptr;
  if (slog && g_frames % 240 == 0) {
    for (Module* m : g_modules) logf("[light] module %s base %u slots %u first %g %g %g %g\n", m->name, m->base, m->slots, scene[m->base * 4], scene[m->base * 4 + 1], scene[m->base * 4 + 2], scene[m->base * 4 + 3]);
    logf("[light] core slot0 %g %g\n", scene[0], scene[1]);
  }
  const Range r = push_uniform(reinterpret_cast<const uint8_t*>(scene.data()), kSceneBytes);
  const GroupPayload gp{r.offset};
  push_custom_draw(g_groupType, &gp, sizeof gp);
}

// A new Aurora frame: the scene capture and the lit variant follow the engine's state.
void frame_start() {
  // anything captured after last frame's boundary refers to last frame's buffers: drop it
  al::release(al::take_captured());
  const bool on = engine_on();
  ++g_frames;
  g_frame_ncam = 0;
  al::set_capture_camera(0);
  al::set_draw_filter(on ? draw_filter : nullptr, nullptr);
  al::set_capture(on);
  al::set_scene_lit(on && (settings::g.light_shadows || settings::g.light_replace));
  al::set_cast_skinned(settings::g.light_char_cast);   // characters / enemies in the shadow maps
  if (!on) {
    if (g_prev && init()) { push_task({kOpRelease, g_prev}); g_prev = nullptr; }
    g_cam_now.ok = false;
    return;
  }
  if (!init()) return;
  for (Module* m : g_modules)
    if (m->frame_start) m->frame_start();
  const GroupPayload gp{UINT32_MAX};   // withdraw last frame's group 3: a frame without a screen camera must not reuse stale uniforms
  push_custom_draw(g_groupType, &gp, sizeof gp);
}

float g_proj[6];
bool g_proj_ok = false;
bool scene_projection(float p[6]) { if (g_proj_ok) memcpy(p, g_proj, sizeof g_proj); return g_proj_ok; }
void before_hud(uint64_t frame, const float* proj) {
  if (proj) { memcpy(g_proj, proj, sizeof g_proj); g_proj_ok = true; }
  al::CapturedFrame* cf = al::take_captured();
  al::set_capture(false);   // the HUD is not scene geometry
  if (!init()) { al::release(cf); return; }
  const FrameInfo fi{cf, g_frame_cams, std::min(g_frame_ncam, kMaxCams)};
  for (Module* m : g_modules)
    if (m->boundary) m->boundary(fi);
  static const bool log = getenv("LIGHT_LOG") != nullptr;
  static uint64_t last_log = 0;
  const uint64_t now = sys_time_ns();
  if (log && now - last_log > 2000000000ull) {
    last_log = now;
    logf("[light] frame %llu: %u draws captured, %d camera(s), cam %08X, %zu modules, proj %s\n", (unsigned long long)frame,
         al::captured_count(cf), g_frame_ncam, g_cam, g_modules.size(), proj ? "yes" : "no");
  }
  // the previous frame's geometry is finished with (the modules' tasks of this frame were pushed earlier, so this runs after)
  if (g_prev) { push_task({kOpRelease, g_prev}); g_prev = nullptr; }
  if (cf) {
    g_cached_ncam = std::max(1, std::min(g_frame_ncam, kMaxCams));
    for (int k = 0; k < g_cached_ncam; ++k) g_cached_cams[k] = g_frame_cams[k];
    push_task({kOpCache, cf});
    g_prev = cf;
  }
}
}  // namespace lighting
