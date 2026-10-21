// #121: L recentres the camera behind Shadow (Zelda style). The game's own camera is steered with the C stick
// until it looks along Shadow's facing, so its collision / smoothing still run and nothing snaps back afterwards.
// Fixed and scripted cameras ignore the C stick: when the camera does not turn in the first 0.25 s the recentre
// stops (it never fights them). Z+L stays the Hero gauge cheat (no recentre while Z is held); the player's own
// C stick input cancels it. Setting cam_recentre (default on). CAM_LOG=1 logs it.
#include "ppc_rt.h"
#include "sys.h"
#include "game.h"
#include "settings_ui.h"
#include "input.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>

extern uint32_t g_plr_owner;   // frame_rate.cpp: the player object the move step last moved

namespace {
constexpr float kPi = 3.14159265f;
float g_cam_at[3] = {0, 0, 1};
uint64_t g_cam_ns = 0;
struct Run {
  bool on = false;
  uint64_t t0 = 0;
  float start_err = 0;   // |error| when it started (the "did the camera turn" test)
  float last_err = 0;
  int sign = 1;          // C stick right -> camera yaw direction (learned from the first frames)
  bool sign_known = false;
} g_run;
float wrap(float a) { while (a > kPi) a -= 2 * kPi; while (a < -kPi) a += 2 * kPi; return a; }
bool ram(uint32_t a) { return a >= 0x80000000u && a < RT_RAM_END - 0x400; }
}

// widescreen.cpp hle_RwCameraBeginUpdate, screen cameras: the frame matrix the scene is drawn through
float g_last_cam[6] = {};   // #135 bug capture: the last 3D camera's position and look direction (copied: the frame is reused)
void cam_recentre_camera(uint32_t frm) {
  if (!ram(frm)) return;
  // the same camera draws the 2D overlays with an identity frame: keep the 3D view only
  if (ldf32(frm + 0x70) == 0 && ldf32(frm + 0x74) == 0 && ldf32(frm + 0x78) == 1 && ldf32(frm + 0x80) == 0 &&
      ldf32(frm + 0x84) == 0 && ldf32(frm + 0x88) == 0) return;
  for (int i = 0; i < 3; ++i) g_cam_at[i] = float(ldf32(frm + 0x70 + i * 4));   // LTM at
  for (int i = 0; i < 3; ++i) { g_last_cam[i] = float(ldf32(frm + 0x80 + i * 4)); g_last_cam[3 + i] = g_cam_at[i]; }
  g_cam_ns = sys_time_ns();
  static const bool log = getenv("CAM_LOG") != nullptr;
  static uint64_t last = 0;
  static int n = 0;
  if (log && (g_cam_ns - last > 500000000ull || n < 3)) {
    n = g_cam_ns - last > 500000000ull ? 0 : n + 1;
    if (!n) last = g_cam_ns;
    logf("[cam-recentre] camera frm %08X at (%.3f,%.3f,%.3f) pos (%.1f,%.1f,%.1f)\n", frm, double(g_cam_at[0]), double(g_cam_at[1]),
         double(g_cam_at[2]), ldf32(frm + 0x80), ldf32(frm + 0x84), ldf32(frm + 0x88));
  }
}

// input.cpp input_poll, after the camera options: user_b = the player's buttons; cx = the C stick the game will read
void cam_recentre_input(uint16_t user_b, int8_t& cx, int8_t cy) {
  static const bool log = getenv("CAM_LOG") != nullptr;
  static bool was_l = false;
  const bool l = (user_b & PAD_L) != 0;
  const bool edge = l && !was_l;
  was_l = l;
  const uint64_t now = sys_time_ns();
  const uint32_t p = g_plr_owner;
  // starts in play (in_stage); then only a menu, a cutscene, the pause menu or a lost camera stops it (in_stage blinks
  // off on slow frames and while the move step pauses, #116)
  const bool can = settings::g.cam_recentre && (g_run.on || game::in_stage()) && !game::menus_active() &&
                   !game::cutscene_active() && !game::pause_menu_active() && ram(p) && ld32(p + 0x38) == 0x80521C50 &&
                   now - g_cam_ns < 500000000ull;
  if (edge && can && !(user_b & PAD_Z)) {
    g_run = Run{};
    g_run.on = true;
    g_run.t0 = now;
  }
  if (!g_run.on) return;
  if (!can || std::abs(int(cx)) > 40 || std::abs(int(cy)) > 40 || now - g_run.t0 > 1500000000ull) {
    if (log) logf("[cam-recentre] stop (%s)\n", !can ? "not in play" : now - g_run.t0 > 1500000000ull ? "timeout" : "C stick");
    g_run.on = false;
    return;
  }
  const float hx = g_cam_at[0], hz = g_cam_at[2];
  if (hx * hx + hz * hz < 1e-4f) { g_run.on = false; return; }   // looking straight down / up: no yaw
  const float cam = std::atan2(hx, hz);
  const float face = wrap(float(ldf32(p + 0x10)) + kPi);   // P+0x10 yaw 0 = facing -z (proven: running velocity)
  const float err = wrap(face - cam);
  const float t = float(now - g_run.t0) * 1e-9f;
  if (log && t == 0.f)
    logf("[cam-recentre] start at (%.3f,%.3f,%.3f) face raw %08X %08X %08X vel (%.1f,%.1f)\n", double(g_cam_at[0]),
         double(g_cam_at[1]), double(g_cam_at[2]), ld32(p + 0x10), ld32(p + 0x14), ld32(p + 0xC), ldf32(p + 0x18), ldf32(p + 0x20));
  if (t == 0.f || !g_run.start_err) g_run.start_err = std::abs(err) + 1e-6f;
  if (std::abs(err) < 2.f * kPi / 180.f) {
    if (log) logf("[cam-recentre] done t=%.2f err %.1f deg\n", double(t), double(err * 180 / kPi));
    g_run.on = false;
    return;
  }
  // which way the C stick turns this camera: the error must shrink once the stick is held
  if (!g_run.sign_known && t > 0.12f) {
    const float moved = std::abs(err) - g_run.start_err;
    if (moved < -0.03f) g_run.sign_known = true;
    else if (moved > 0.03f) { g_run.sign = -g_run.sign; g_run.sign_known = true; g_run.start_err = std::abs(err); g_run.t0 = now - 120000000ull; }
  }
  if (t > 0.3f && !g_run.sign_known) {   // a fixed / scripted camera: the stick does nothing
    if (log) logf("[cam-recentre] stop (camera does not turn)\n");
    g_run.on = false;
    return;
  }
  // ease out: full speed far away, proportional in the last ~35 degrees
  const float k = std::clamp(std::abs(err) / (35.f * kPi / 180.f), 0.35f, 1.f);
  cx = int8_t(std::clamp(int(std::lround(127.f * k)) * (err > 0 ? 1 : -1) * g_run.sign, -127, 127));
  if (log) logf("[cam-recentre] t=%.2f cam %.1f face %.1f err %.1f cx %d\n", double(t), double(cam * 180 / kPi),
                double(face * 180 / kPi), double(err * 180 / kPi), int(cx));
  g_run.last_err = err;
}
