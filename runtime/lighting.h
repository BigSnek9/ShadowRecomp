#pragma once
// The lighting engine (LIGHTING-ENGINE.md): real-time shadows, world-space AO / sky light / GI, lights from the
// game's bloom, parallax. Called by gx_lle.cpp; settings in settings::g.light*.
#include <cstdint>

namespace lighting {
// Register shader contributions before Aurora starts warming cached pipelines. No GPU work is done here.
void register_builtin_modules();
// gx_lle: a new Aurora frame started (capture of the 3D scene begins when the engine is on)
void frame_start();
// gx_lle: the 3D scene of this frame is complete, the HUD starts. proj = the scene camera (FxBoundary: GX projection
// p0..p5 and the viewport l, t, w, h in 0..1), nullptr when unknown.
void before_hud(uint64_t frame, const float* proj);
// widescreen.cpp: the RenderWare camera that renders the screen (landscape view window), updated every frame
void note_screen_camera(uint32_t cam);
// widescreen.cpp: whether to report screen cameras (0x04 FIFO markers) - the engine is on and a stage runs
bool wants_camera();
// gx_lle, in FIFO order: a screen camera begins its update; ltm = its world matrix rows right, up, at, pos
void camera_begin(const float ltm[12]);
// gx_lle: an additive 3D glow sprite of this frame (centre / size in view space, colour 0..1) - lighting_glow.cpp
void glow_add(const float view[3], const float colour[3], float radius);
// #105: the direction shadows fall this frame (world, from the sun into the scene) and how far they reach (the outer
// cascades); false while the sun / shadows are off
bool shadow_sweep(float dir[3], float* reach);
}  // namespace lighting
