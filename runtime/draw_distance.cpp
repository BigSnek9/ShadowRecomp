// Camera draw range: extend the actual RenderWare frustum and projection together.
// Object-specific LOD/streaming cutoffs are separate and are not bypassed here.
#include "draw_distance.h"
#include "sys.h"
#include "game.h"
#include "settings_ui.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <unordered_map>

void f_8048560C(Cpu&); // RwCameraSetFarClipPlane: camera +0x84, projection scale and dirty frustum.
namespace draw_distance {
namespace {
struct State { uint32_t frame; float original, output; bool extended = false; };
std::unordered_map<uint32_t, State> cameras;
}
void refresh_camera(uint32_t cam, Cpu& c) {
  static uint32_t last_stage = 0;
  const uint32_t stage = ld32(0x80584720);
  if (stage != last_stage) { cameras.clear(); last_stage = stage; }
  if (cam < 0x80000000u || cam > 0x817FFD00u || (ld32(cam) >> 24) != 4 || ld32(cam + 0x14) != 1) return;
  const float value = ldf32(cam + 0x84);
  if (!std::isfinite(value) || value < 10 || value > 100000) return;
  const uint32_t frame = ld32(cam + 4);
  auto [it, inserted] = cameras.try_emplace(cam, State{frame, value, value});
  auto& state = it->second;
  if (state.frame != frame) state = {frame, value, value};
  if (!state.extended || value != state.output) state.original = value;
  const float scale = game::in_stage() && !game::cutscene_active()
      ? std::clamp(settings::g.draw_distance, 100, 400) / 100.0f : 1.0f;
  const float wanted = std::min(state.original * scale, 60000.0f);
  state.output = wanted;
  state.extended = wanted != state.original;
  if (wanted == value) return;
  const Cpu saved = c;
  c.r[3] = cam; c.f[1] = wanted;
  f_8048560C(c); // use the real setter, not just a GPU projection patch
  c = saved;
  static const bool trace = getenv("DRAW_DIST_LOG") != nullptr;
  static int logs = 0;
  if (trace && logs++ < 80)
    logf("[draw-distance] stage %u camera %08X far %g -> %g (%g%%)\n", stage, cam, state.original, wanted, scale * 100);
}
void finish_camera(uint32_t cam, Cpu& c) {
  const auto it = cameras.find(cam);
  if (it == cameras.end() || !it->second.extended) return;
  auto& state = it->second;
  state.extended = false;
  // The render tree may derive another far distance from the temporary value while drawing.
  // Always restore the authored entry value at EndUpdate; otherwise that derived value compounds next frame.
  if (ld32(cam + 4) != state.frame || (ld32(cam) >> 24) != 4) return;
  const Cpu saved = c;
  c.r[3] = cam; c.f[1] = state.original;
  f_8048560C(c);
  c = saved;
}
}
