#pragma once
// The lighting engine's MODULE interface (see LIGHTING-ENGINE.md "Architecture").
//
// The engine is a small core plus feature modules. The core (lighting.cpp) owns what every feature needs: the exact camera of
// each frame, the capture + GPU cache of the previous frame's geometry, the per-camera scene uniform block, the lit-shader
// plumbing and the draw filter dispatch. A module is one struct: its WGSL snippets (assembled into the lit scene shader by
// Aurora, aurora/lighting.hpp ShaderModule) and a few game-thread callbacks that fill its uniform slots and queue its GPU
// work. Existing modules: lighting_sun.cpp (sun + cascaded shadows), lighting_sky.cpp (hemisphere sky light),
// lighting_game.cpp (removing the game's own fake effects). Planned: voxel AO / sky occlusion, GI, lights made from the
// game's bloom, parallax. Adding one = a new lighting_<name>.cpp that defines a Module and calls register_module() from
// lighting::register_builtin_modules() (lighting.cpp) - nothing else changes.
#include "sys.h"
#include <aurora/lighting.hpp>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace lighting {

// ---- small maths (row-major 4x4, column-vector convention: C * v) ----
struct M4 { float m[16]; };
inline M4 mul(const M4& a, const M4& b) {
  M4 r;
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) {
      float s = 0;
      for (int k = 0; k < 4; ++k) s += a.m[i * 4 + k] * b.m[k * 4 + j];
      r.m[i * 4 + j] = s;
    }
  return r;
}
struct V3 { float x, y, z; };
// #109: the stage being played. 0x8057D748 = the running stage number in EVERY mode (story, select, expert; -1 outside
// a stage, see pause_menu.cpp current_stage); 0x80584720 is only Select Mode's stage request (0 in Story Mode, which
// left story stages without their lighting profile / bake). Outside a stage: the request, as before.
inline int lighting_stage() {
  const int32_t s = (int32_t)ld32(0x8057D748);
  return s > 0 ? s : (int)ld32(0x80584720);
}
inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline V3 norm(V3 a) { float l = std::sqrt(dot(a, a)); return l > 1e-12f ? a * (1.0f / l) : V3{0, 1, 0}; }

// The camera of a frame: RwFrame LTM rows right, up, at, pos. GX view space (x = -right, y = up, z = -at; verified against
// world geometry): p_world = pos - x * right + y * up - z * at.
struct Cam {
  V3 r{1, 0, 0}, u{0, 1, 0}, a{0, 0, 1}, p{0, 0, 0};
  bool ok = false;
  void set(const float l[12]) {
    r = {l[0], l[1], l[2]}; u = {l[3], l[4], l[5]}; a = {l[6], l[7], l[8]}; p = {l[9], l[10], l[11]};
    ok = true;
  }
  M4 view_to_world() const {   // columns: -right, up, -at, pos
    return {{-r.x, u.x, -a.x, p.x, -r.y, u.y, -a.y, p.y, -r.z, u.z, -a.z, p.z, 0, 0, 0, 1}};
  }
  V3 world_to_view_dir(V3 d) const { return {-dot(r, d), dot(u, d), -dot(a, d)}; }
  V3 view_to_world_dir(V3 d) const { return r * -d.x + u * d.y + a * -d.z; }
};
constexpr int kMaxCams = 4;   // distinct view matrices the engine tracks per frame (the game calls BeginUpdate ~28 times)
// Static group 0 binding 2: sun occupies the first 64 MiB, independent of chosen cascade resolution.
constexpr uint32_t kSkyDepthOffset = 4u * 2048u * 2048u; // float elements, not bytes
// #113: the screen-space AO (lighting_ao.cpp) after the sky volumes (5 MiB): per pixel (visibility, distance), up to 16 MiB
constexpr uint32_t kAoOffset = kSkyDepthOffset + 5u * 512u * 512u;

// ---- what a module sees ----
struct CameraCtx {
  const Cam& cam;                       // the camera being set up (view space of this camera = the lit shader's view space)
  int index;                            // 0 = the frame's first camera: the one to anchor world-space structures to
  const Cam* cached;                    // cameras the PREVIOUS frame's geometry was drawn through (cache cam k = capture camera k)
  int ncached;
  aurora::lighting::CapturedFrame* prev;   // the previous frame's captured draws, copied to the GPU cache; nullptr = none.
                                           // Valid until the engine releases it at the end of this frame: GPU tasks pushed from
                                           // here may hold the pointer, nobody may free it.
  float* slots;                         // this module's uniform slots (4 floats each), zeroed before every camera
  uint32_t base;                        // the module's first slot (= {BASE} in its WGSL)
};
struct FrameInfo {                      // the 3D scene of this frame is complete (the HUD starts)
  aurora::lighting::CapturedFrame* captured;   // this frame's captured scene draws (may be null)
  const Cam* cams;
  int ncams;
};

struct Module {
  const char* name;
  uint32_t slots;                       // uniform slots (vec4) the module needs
  const char* wgsl_decls;               // see aurora/lighting.hpp ShaderModule
  const char* wgsl_pre;
  const char* wgsl_apply;
  // game thread; every callback is optional
  void (*frame_start)();                                  // a new frame begins
  void (*camera)(CameraCtx&);                             // once per distinct camera, first camera first
  void (*boundary)(const FrameInfo&);                     // the scene is complete
  bool (*draw_filter)(const aurora::lighting::DrawInfo&); // true = drop this game draw
  uint32_t base = 0;                                      // assigned at registration
  const char* wgsl_post = nullptr;                        // after final lighting, before authored fog
};
void register_module(Module* module);

// ---- state modules share ----
struct Env {
  V3 sun_colour{1.0f, 0.95f, 0.85f};    // the colour of the stage's own sun light (from the game), set by the sun module
  bool sun_known = false;
};
Env& env();

}  // namespace lighting
