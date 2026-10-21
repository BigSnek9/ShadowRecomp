// Hemisphere sky light, occluded by the BAKED sky visibility / enclosure volume (lighting L2, lighting_occlusion.cpp):
// one trilinear lookup in each of two float volumes per pixel, plus the player as a sphere occluder (dynamic layer).
// No per-frame geometry replay (iteration3's five depth views are gone): off-screen and unloaded static geometry
// occludes, characters only through the sphere. No baked file for the stage = plain hemisphere light (no occlusion).
#include "lighting_core.h"
#include "lighting_environment.h"
#include "lighting_occlusion.h"
#include "settings_ui.h"
#include "stage_workshop.h"
#include "sys.h"
#include <algorithm>

namespace lighting {
namespace {
void camera(CameraCtx& ctx) {
  const int stage = lighting_stage();
  const bool wanted = settings::g.light_ao > 0;   // replace mode and the overlay (its AO share: core slot 1 z)
  static OccVolume vol;
  if (ctx.index == 0) vol = occlusion_update(ctx.cam, stage, wanted);   // the volume for this frame (all cameras)
  float* sl = ctx.slots;
  const V3 up = ctx.cam.world_to_view_dir({0, 1, 0});
  const auto& p = atmosphere();
  sl[0] = up.x; sl[1] = up.y; sl[2] = up.z; sl[3] = settings::g.light_sky / 100.0f * p.sky_strength;
  sl[4] = p.sky.x; sl[5] = p.sky.y; sl[6] = p.sky.z;
  sl[8] = p.ground.x; sl[9] = p.ground.y; sl[10] = p.ground.z;
  const bool on = wanted && vol.valid;
  const float ao_radius = std::clamp(settings::g.light_ao_radius, 25, 300) / 100.f;
  sl[12] = on ? std::clamp(settings::g.light_ao, 0, 100) / 100.f : 0.f;
  sl[13] = on ? 1.f : 0.f;
  sl[14] = vol.cell * 0.6f * ao_radius;
  sl[15] = settings::g.light_ao_mode == 1 ? 1.f : 0.f;   // #113: screen-space AO replaces the baked enclosure darkening   // lookup offset along the normal (out of the surface's own cell)
  // view -> world rows (p_world = M * (lview, 1)), from the camera of THIS draw
  const M4 m = ctx.cam.view_to_world();
  memcpy(sl + 16, m.m, 12 * sizeof(float));
  sl[28] = vol.origin.x; sl[29] = vol.origin.y; sl[30] = vol.origin.z; sl[31] = vol.cell;
  sl[32] = float(vol.dims[0]); sl[33] = float(vol.dims[1]); sl[34] = float(vol.dims[2]); sl[35] = float(vol.count);
  // dynamic layer: the player as a sphere (centre about chest height), radius scaled by the AO reach
  float pp[3];
  if (on && stage_workshop::player_position(pp)) { sl[36] = pp[0]; sl[37] = pp[1] + 6.f; sl[38] = pp[2]; sl[39] = 9.f * ao_radius; }
}

// Sun registers first and declares the shared static storage buffer `lshadow`; the volumes start at kSkyDepthOffset.
const char* kDecls = R"WGSL(
fn k_at(b: u32, dx: u32, dy: u32, x: u32, y: u32, z: u32) -> f32 { return lshadow[16777216u + b + x + dx * (y + dy * z)]; }
fn k_vol(b: u32, q: vec3f) -> f32 {
  let o = L({BASE}u + 7u);
  let dm = L({BASE}u + 8u);
  let g = (q - o.xyz) / o.w - vec3f(0.5);
  if (any(g < vec3f(0.0)) || any(g > dm.xyz - vec3f(1.0))) { return 1.0; }
  let i0 = vec3u(floor(g));
  let f = fract(g);
  let i1 = min(i0 + vec3u(1u), vec3u(dm.xyz) - vec3u(1u));
  let dx = u32(dm.x); let dy = u32(dm.y);
  let c00 = mix(k_at(b, dx, dy, i0.x, i0.y, i0.z), k_at(b, dx, dy, i1.x, i0.y, i0.z), f.x);
  let c10 = mix(k_at(b, dx, dy, i0.x, i1.y, i0.z), k_at(b, dx, dy, i1.x, i1.y, i0.z), f.x);
  let c01 = mix(k_at(b, dx, dy, i0.x, i0.y, i1.z), k_at(b, dx, dy, i1.x, i0.y, i1.z), f.x);
  let c11 = mix(k_at(b, dx, dy, i0.x, i1.y, i1.z), k_at(b, dx, dy, i1.x, i1.y, i1.z), f.x);
  return mix(mix(c00, c10, f.y), mix(c01, c11, f.y), f.z);
}
)WGSL";
const char* kPre = R"WGSL(
    let k_up = L({BASE}u);
    var k_skyvis = 1.0; var k_enclosure = 1.0;
    let k_par = L({BASE}u + 3u);
    if (k_par.y > 0.0) {
      let k_p4 = vec4f(in.lview, 1.0);
      let k_r0 = L({BASE}u + 4u); let k_r1 = L({BASE}u + 5u); let k_r2 = L({BASE}u + 6u);
      let k_pw = vec3f(dot(k_r0, k_p4), dot(k_r1, k_p4), dot(k_r2, k_p4));
      let k_nw = normalize(vec3f(dot(k_r0.xyz, l_n), dot(k_r1.xyz, l_n), dot(k_r2.xyz, l_n)));
      let k_q = k_pw + k_nw * k_par.z;
      let k_n = u32(L({BASE}u + 8u).w);
      k_skyvis = mix(1.0, k_vol(0u, k_q), k_par.x);
      k_enclosure = mix(1.0, k_vol(k_n, k_q), k_par.x * (1.0 - k_par.w));
      l_skyvis = k_skyvis; l_encl = k_enclosure;   // the overlay's AO and bounce tint (core)
      let k_sp = L({BASE}u + 9u);
      if (k_sp.w > 0.0) {
        let k_d = k_sp.xyz - k_pw;
        let k_l2 = max(dot(k_d, k_d), 1.0);
        let k_c = clamp(dot(k_nw, k_d * inverseSqrt(k_l2)), 0.0, 1.0) * k_sp.w * k_sp.w / k_l2;
        l_occ *= 1.0 - clamp(k_c, 0.0, 0.85) * k_par.x;
      }
    }
    let k_hemisphere = clamp(dot(l_n, k_up.xyz) * 0.5 + 0.5, 0.0, 1.0);
    l_ambient += mix(L({BASE}u + 2u).rgb * k_enclosure, L({BASE}u + 1u).rgb * k_skyvis, k_hemisphere) * k_up.w;)WGSL";
const char* kApply = R"WGSL(
    if (l_debug() == 7u) { l_has_override = true; l_override = vec3f(k_skyvis); }
    if (l_debug() == 8u) { l_has_override = true; l_override = vec3f(k_enclosure * l_occ); })WGSL";

Module g_module{"sky", 10, kDecls, kPre, kApply, nullptr, camera, nullptr, nullptr};
}  // namespace
Module* sky_module() { return &g_module; }
}  // namespace lighting
