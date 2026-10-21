// Lighting module: GAME CLEAN-UP - removes the game's own fake effects that the engine replaces (see lighting_core.h).
// A draw filter (aurora::lighting::set_draw_filter) sees every GX draw before it is recorded; what it drops is gone.
//   * the blob shadow under the character (and objects): a mesh that follows the ground, alpha-blended (src alpha / inverse src
//     alpha), no depth write, one TEV stage, a 32x32 IA8 texture, many vertices - found with LIGHT_BLOBLOG=1 (logs the
//     candidates of a frame); LIGHT_KEEPBLOB=1 keeps it
//   Planned: the glow quads that fake light sources (they become real lights, see the plan).
#include "lighting_core.h"
#include "sys.h"
#include "settings_ui.h"
#include <cstdlib>

namespace lighting {
namespace {
int g_cand = 0;
uint64_t g_frames = 0;
void frame_start() { ++g_frames; g_cand = 0; }

bool draw_filter(const aurora::lighting::DrawInfo& d) {
  // the real-time shadows replace the game's circles unless the user keeps them (light_blob); no shadows = keep them
  if (settings::g.light_blob || !(settings::g.light_shadows || settings::g.light_replace)) return false;
  if (!d.perspective || d.offscreen || d.depthWrite) return false;
  static const bool glog = getenv("LIGHT_GLOWLOG") != nullptr;   // additive 3D draws (glows): where they are, to source lights
  if (glog && d.additive && d.perspective && !d.offscreen && g_frames % 240 == 0)
    logf("[glow] additive vtx %u tex %ux%u fmt %u world %d origin (%.0f,%.0f,%.0f) tev %u\n", d.vtxCount, d.texW, d.texH, d.texFmt, (int)d.world,
         d.origin[0], d.origin[1], d.origin[2], d.tevStages);
  static const bool log = getenv("LIGHT_BLOBLOG") != nullptr;
  static const bool keep = getenv("LIGHT_KEEPBLOB") != nullptr;
  const int i = g_cand++;
  if (log && g_frames % 240 == 0)
    logf("[blob] cand %d vtx %u idx %u tex %ux%u fmt %u blended %d blend %u/%u tev %u ztest %d\n", i, d.vtxCount, d.indexCount, d.texW,
         d.texH, d.texFmt, (int)d.blended, d.blendSrc, d.blendDst, d.tevStages, (int)d.depthTest);
  const bool ground_mesh = d.blended && d.depthTest && d.blendSrc == 4 && d.blendDst == 5 &&
                           d.texW == 32 && d.texH == 32 && d.vtxCount > 16;
  // The character also uses a paletted, two-stage ground mesh (seen in Iron Jungle).
  // Keep additive glows, ordinary transparent quads and the stage's other texture families.
  const bool blob = ground_mesh && ((d.texFmt == 5 && d.tevStages == 1) ||
                                    (d.texFmt == 8 && d.tevStages == 2));
  return blob && !keep;
}

Module g_module{"game clean-up", 0, "", "", "", frame_start, nullptr, nullptr, draw_filter};
}  // namespace
Module* game_module() { return &g_module; }
}  // namespace lighting
