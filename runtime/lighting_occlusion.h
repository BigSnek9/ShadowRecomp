#pragma once
// Lighting L2: baked occupancy (docs/lighting/OCCUPANCY-FORMAT.md) -> sky visibility / enclosure volume around the camera.
#include "lighting_core.h"

namespace lighting {
// What the sky module's shader samples: two float volumes in the static storage buffer at kSkyDepthOffset
// (sky visibility first, then the short-range enclosure), x fastest. valid = false: no file for the stage or not baked yet.
struct OccVolume {
  bool valid = false;
  V3 origin{};          // world position of cell (0,0,0)'s minimum corner
  float cell = 32;      // world units per cell
  uint32_t dims[3]{};
  uint32_t count = 0;   // cells per volume
};
// Game thread, once per frame from the first camera: loads the stage's .occ when the stage changes, re-bakes the volume
// on a worker thread when the camera leaves the middle of the current one, queues the GPU upload when a bake is done.
const OccVolume& occlusion_update(const Cam& cam, int stage, bool wanted);
// The capacity of the storage region (floats): both volumes together must fit (5 * 512 * 512, the old sky maps).
constexpr uint32_t kOccCapacity = 5u * 512u * 512u;
}  // namespace lighting
