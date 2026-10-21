#pragma once
// #148 contact AO: ambient occlusion on this frame's own depth, applied when the 3D scene is complete (the HUD marker).
// docs/lighting/AO-148.md. Settings ao (0/1, default off) / ao_strength (%). lighting_contact_ao.cpp.
namespace contact_ao {
// the scene is complete, the HUD starts: proj = the scene's GX projection p0..p5 + viewport (left, top, width, height)
// on the 640x480 screen in 0..1 units (gx_lle.cpp FxBoundary), or null when not known this frame
void run(const float* proj);
}  // namespace contact_ao
