// Widescreen for the 3D camera, done the RenderWare way.
//
// Shadow renders with RenderWare. A camera's view window (RwCamera +0x68: half-width/half-height of the view
// plane at distance 1) defines both its projection and the frustum planes used to cull objects on the CPU. Widening
// only the GPU projection showed the extra area but objects there were still culled against the 4:3 frustum
// (pop-in at the sides). Here RwCameraSetViewWindow (fn_80485950) is overridden and the window width is scaled by
// output aspect / (4/3), so projection and culling widen together. The game calls it several times per frame, so
// aspect changes apply immediately.
//
// Screen cameras have landscape windows (the game's are ~1.14-1.32 wide, compensating for the console's non-square
// pixels); render-to-texture cameras (shadows, reflections) are square and left alone. The game sometimes passes a
// camera's own stored window back in; that value is already widened, so each camera remembers the last window it
// was given by us and such re-submissions pass through unchanged (no compounding).
#include "sys.h"
#include "gx_lle.h"
#include "settings_ui.h"
#include "game.h"
#include "lighting.h"
#include "draw_distance.h"
#include "stage_workshop.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

void f_80485950(Cpu&);   // RwCameraSetViewWindow (recompiled original)

static float ldf(uint32_t ea) { uint32_t u = ld32(ea); float f; memcpy(&f, &u, 4); return f; }
static void stf(uint32_t ea, float f) { uint32_t u; memcpy(&u, &f, 4); st32(ea, u); }

namespace {
struct Widened { float x_in, y_in, x_out, y_out; };
std::unordered_map<uint32_t, Widened> g_cams;
Widened g_scene = {0, 0, 0, 0};   // screen camera -> its original window and what we gave it last
// The FOV setting is for the gameplay camera: cutscenes keep their authored framing.
// #87: and only while a stage is played - menus / loading screens set the same camera, and a scene's subtitles are
// sized from whatever window it last held when the scene starts (FOV 150% on the title gave 1.5x cutscene text)
float current_fov() {
  if (game::cutscene_active() || (game::menus_active() && !game::in_stage())) return 1.0f;
  static const bool log = getenv("FOV_LOG") != nullptr;   // #117 proof: moments the old gate (player step only) dropped the FOV
  if (log && !game::in_stage()) {
    static uint64_t last = 0; const uint64_t now = sys_time_ns();
    if (now - last > 1000000000ull) { last = now; logf("[fov] kept %d%% in a stage while the player step paused (old gate: 100%%)\n", settings::g.fov_percent); }
  }
  return std::clamp(settings::g.fov_percent, 70, 150) / 100.0f;
}
// Every window this hook produced recently (any camera) and the game's original window it came from. The game
// keeps copies of the windows it was given and re-sends them (the stage camera gets its stored window again every
// frame; a camera can alternate two). A re-sent window is rebuilt from its original with the CURRENT factors: no
// compounding (that exploded the view / blacked out cutscenes at FOV != 100%), live FOV/aspect changes apply at
// once, and in 4:3 cutscenes a stored widened window turns back into the original.
struct Made { float xo, yo, xi, yi; };
Made g_made[64];
unsigned g_made_n = 0;
const Made* made(float x, float y) {
  if (x == 0 && y == 0) return nullptr;
  for (auto& m : g_made) if (m.xo == x && m.yo == y) return &m;
  return nullptr;
}
void remember(float xo, float yo, float xi, float yi) {
  if (xo == xi && yo == yi) return;   // unchanged windows need no mapping
  if (!made(xo, yo)) g_made[g_made_n++ % 64] = {xo, yo, xi, yi};
}
// Screen cameras have landscape windows (render-to-texture ones are square).
bool screen_window(float x, float y) { float r = y > 0.0f ? x / y : 0.0f; return r > 1.05f && r < 1.6f; }
}

// Also the field-of-view setting (settings "Field of view", percent): both axes of the view window scale together,
// so projection and frustum culling follow (a bigger window = a wider field of view).
void hle_RwCameraSetViewWindow(Cpu& c) {
  static const bool off = getenv("NO_CAM_HOOK") != nullptr;   // bisecting
  if (off) { f_80485950(c); return; }
  uint32_t cam = c.r[3], vw = c.r[4];
  float k = gxlle::widescreen_camera_factor();   // output aspect / (4/3); 1 when not widescreen
  float fov = current_fov();
  if (!vw) { f_80485950(c); return; }
  static const bool skip_special = getenv("WS_SKIP_SPECIAL") != nullptr;   // experiment: render-tree camera node window
  if (skip_special && c.lr == 0x80271B44u) { f_80485950(c); return; }
  static const char* sp_scale = getenv("WS_SPECIAL_SCALE");   // experiment: which draws use that window
  if (sp_scale && c.lr == 0x80271B44u) {
    float s = (float)atof(sp_scale), ox = ldf(vw), oy = ldf(vw + 4);
    stf(vw, ox * s); stf(vw + 4, oy * s);
    f_80485950(c);
    stf(vw, ox); stf(vw + 4, oy);
    return;
  }
  float x = ldf(vw), y = ldf(vw + 4);
  static const bool trace = getenv("CAM_TRACE") != nullptr;
  if (trace && game::in_stage()) {
    static int n = 0;
    if (n++ < 400) logf("[cam] cam=%08X vw=%08X%s x=%g y=%g k=%g fov=%g\n", cam, vw, vw == cam + 0x68 ? "(own)" : "", x, y, k, fov);
  }
  float xi = x, yi = y;
  const Made* hit = made(x, y);
  if (hit) { xi = hit->xi; yi = hit->yi; }   // a window we produced, sent again
  else if (!screen_window(x, y)) { g_cams.erase(cam); f_80485950(c); return; }
  float wx = xi * k * fov, wy = yi * fov;
  if (const float lf = stage_workshop::live_fov(); lf > 0 && yi > 0) {   // live game view: vertical FOV in degrees
    const float s = std::tan(lf * 0.00872664626f) / yi; wx = xi * k * s; wy = yi * s;
  }
  if (trace && game::in_stage()) {
    static int n = 0;
    if (n++ < 400) logf("[cam]   -> %s in=(%g,%g) out=(%g,%g) frame %llu lr=%08X\n", hit ? "re-sent" : "original", xi, yi, wx, wy,
                        (unsigned long long)gxlle::xfb_count(), c.lr);
  }
  g_cams[cam] = {xi, yi, wx, wy};
  g_scene = {xi, yi, wx, wy};   // the last screen camera window: the marker projection pairs with it (#89)
  lighting::note_screen_camera(cam);
  remember(wx, wy, xi, yi);
  if (wx == x && wy == y) { f_80485950(c); return; }
  stf(vw, wx);
  stf(vw + 4, wy);
  f_80485950(c);          // stores the window + reciprocals, marks the frustum dirty
  // restore the caller's struct (not the camera's own) - to the ORIGINAL window when the game handed back one of ours
  // (#87: the event camera's copy held a FOV-widened window, and the cutscene subtitles were sized from it: 1.5x text
  // at FOV 150%, the second line below the picture). In the original game such a copy always holds the 4:3 window.
  if (vw != cam + 0x68) { stf(vw, hit ? xi : x); stf(vw + 4, hit ? yi : y); }
}

// RwCameraBeginUpdate (fn_804854DC, calls camera +0x18): the camera's frame matrix (LTM) is current from here on, and
// everything the game draws until EndUpdate is seen through it. The lighting engine needs that exact matrix for the
// frame's 3D scene - reading the camera later is wrong (the game reuses it for 2D), reading it in SetViewWindow lags a
// frame in motion. For a screen camera (landscape view window, see g_cams) the LTM goes into the GX FIFO as a private
// command (0x04 + right, up, at, pos: 12 floats), so gx_lle hands it to the engine exactly in drawing order.
void f_804854DC(Cpu&);   // RwCameraBeginUpdate (recompiled original)
void f_804854B4(Cpu&);   // RwCameraEndUpdate
void f_804856C8(Cpu&);   // RwCameraFrustumTestSphere
void hle_RwCameraEndUpdate(Cpu& c) {
  const uint32_t cam = c.r[3];
  f_804854B4(c);
  draw_distance::finish_camera(cam, c); // the extended frustum existed only for this render interval
}

// Admit nearby off-screen geometry to the normal render path, where GPU clipping still uses the exact camera.
// Those draws can then be captured as shadow casters instead of vanishing at a screen edge. This does NOT spawn
// unloaded objects, bypass gameplay activation distances, or disable the camera's real clipping planes.
void hle_RwCameraFrustumTestSphere(Cpu& c) {
  const uint32_t cam = c.r[3], sphere = c.r[4];
  static uint64_t eligibility_frame = ~0ull;
  static bool eligible = false;
  if (eligibility_frame != gxlle::xfb_count()) {
    eligibility_frame = gxlle::xfb_count();
    eligible = settings::g.light && settings::g.light_shadows && game::in_stage() &&
        !game::cutscene_active() && settings::g.light_caster_margin > 0;
  }
  const bool extend = eligible && g_cams.count(cam) && ld32(cam + 0x14) == 1;
  if (!extend) { f_804856C8(c); return; }
  f_804856C8(c);
  if (c.r[3] != 0 || sphere < 0x80000000u || sphere > 0x817FFFF0u) return;
  const float radius = ldf32(sphere + 12);
  if (!std::isfinite(radius) || radius < 0 || radius > 10000) return;
  const Cpu result = c;
  const uint32_t stack = c.r[1] - 32;
  if (stack < 0x80000000u || stack > 0x817FFFE0u) { c = result; return; }
  st32(stack, c.r[1]);
  for (int i = 0; i < 3; ++i) st32(stack + 8 + i * 4, ld32(sphere + i * 4));
  stf32(stack + 20, radius + std::clamp(settings::g.light_caster_margin, 0, 2000));
  c.r[1] = stack; c.r[3] = cam; c.r[4] = stack + 8;
  f_804856C8(c);
  bool admitted = c.r[3] != 0;
  // #105: a caster further off-screen whose SHADOW reaches the view: the sphere swept along the shadow direction (from
  // the sun into the scene) up to the shadow cascades' reach. Without it a tall building towards the sun only casts once
  // it is within the margin, and its long shadow pops in over the road.
  float sdir[3], reach = 0;
  static uint64_t swept_kept = 0;
  if (!admitted && lighting::shadow_sweep(sdir, &reach)) {
    for (int k = 1; k <= 4 && !admitted; ++k) {
      const float t = reach * k / 4.f;
      for (int i = 0; i < 3; ++i) stf32(stack + 8 + i * 4, ldf32(sphere + i * 4) + sdir[i] * t);
      stf32(stack + 20, radius + std::clamp(settings::g.light_caster_margin, 0, 2000));
      c.r[1] = stack; c.r[3] = cam; c.r[4] = stack + 8;
      f_804856C8(c);
      admitted = c.r[3] != 0;
    }
    swept_kept += admitted;
  }
  c = result;
  if (admitted) c.r[3] = 1; // intersecting, never claim wholly inside (downstream clipping stays active)
  static const bool trace = getenv("CULL_LOG") != nullptr;
  if (trace) {
    static uint64_t checks = 0, kept = 0, last = 0;
    ++checks; kept += admitted;
    const uint64_t now = sys_time_ns();
    if (now - last > 2000000000ull) {
      logf("[shadow-cull] %llu rejected spheres checked, %llu additional casters kept (%llu by their shadow), margin %d\n",
           (unsigned long long)checks, (unsigned long long)kept, (unsigned long long)swept_kept, settings::g.light_caster_margin);
      last = now; checks = kept = swept_kept = 0;
    }
  }
}

void hle_RwCameraBeginUpdate(Cpu& c) {
  const uint32_t cam = c.r[3];
  if (g_cams.count(cam)) { draw_distance::refresh_camera(cam, c); stage_workshop::live_camera(cam, ld32(cam + 4)); }
  f_804854DC(c);
  if (g_cams.count(cam)) stage_workshop::live_check(ld32(cam + 4));
  if (g_cams.count(cam)) { extern void cam_recentre_camera(uint32_t); cam_recentre_camera(ld32(cam + 4)); }   // #121
  if (!lighting::wants_camera() || !g_cams.count(cam)) return;
  const uint32_t frm = ld32(cam + 4);
  if (frm < 0x80000000u || frm >= RT_RAM_END) return;
  hw_wgp_write(0x04, 1);
  for (int row = 0; row < 4; ++row)   // RwMatrix: right +0x00, up +0x10, at +0x20, pos +0x30 (+0x0C etc. = flags/pad)
    for (int i = 0; i < 3; ++i) hw_wgp_write(ld32(frm + 0x50 + row * 0x10 + i * 4), 4);
}

// A field-of-view change from the menus applies at once even for cameras the game does not set again: live cameras
// get their window again. "Live" = still a RenderWare camera (RwObject type 4) that still holds exactly the window we
// gave it - a freed / reused camera fails that, so this never writes into memory that is something else now.
void widescreen_refresh(Cpu& c) {
  static float applied_fov = -1;
  float fov = current_fov();
  if (applied_fov < 0) applied_fov = fov;
  if (fov == applied_fov) return;
  applied_fov = fov;
  float k = gxlle::widescreen_camera_factor();
  for (auto& [cam, w] : g_cams) {
    if ((ld32(cam) >> 24) != 4 || ldf(cam + 0x68) != w.x_out || ldf(cam + 0x6C) != w.y_out) continue;
    w.x_out = w.x_in * k * fov;
    w.y_out = w.y_in * fov;
    remember(w.x_out, w.y_out, w.x_in, w.y_in);
    stf(cam + 0x68, w.x_out);
    stf(cam + 0x6C, w.y_out);
    Cpu saved = c;
    c.r[3] = cam;
    c.r[4] = cam + 0x68;
    f_80485950(c);
    c = saved;
  }
}

// #89 markers on screen (lock-on marks, enemy HP bars and the other world-anchored 2D markers): every one is placed by
// fn_8000F654, which projects the world point with the game camera's view values (camera +0x24, read through
// fn_8000F7BC, its only caller). The 16:9 HUD then squeezes the 2D quad to the centred 4:3 area, which is right only
// when that projection used the 4:3 window, and the scene's FOV must match too, or the markers drift off their
// targets. Seen in quiet runs (tests hud89-soldier*): the values stay the original 4:3 ones, so after the squeeze fix in
// gx_lle (untextured bars were left unsqueezed) the markers sit on their targets at FOV 100%. The FOV setting never
// reaches them: while fn_8000F654 runs, fn_8000F7BC hands back the view angles scaled by the scene's FOV factor, and
// the camera's values are put back right after. (MARKER_LOG=1 logs them.)
void f_8000F654(Cpu&);
void f_8000F7BC(Cpu&);
static uint32_t g_marker_vals = 0;   // the camera values changed for this projection (0 = none)
static uint32_t g_marker_old[2];
void hle_MarkerProject(Cpu& c) {
  f_8000F654(c);
  if (g_marker_vals) { st32(g_marker_vals, g_marker_old[0]); st32(g_marker_vals + 4, g_marker_old[1]); g_marker_vals = 0; }
}
void hle_MarkerViewValues(Cpu& c) {
  f_8000F7BC(c);
  const uint32_t p = c.r[3];
  if (p < 0x80000000u || p + 8 > RT_RAM_END) return;
  static const bool log = getenv("MARKER_LOG") != nullptr;
  const float k = gxlle::widescreen_camera_factor();
  const float ax = ldf(p), ay = ldf(p + 4);
  if (log) {
    static int n = 0;
    if (n++ < 60 || n % 200 == 0)
      logf("[marker] values %g %g tan/2 %g %g scene in (%g,%g) out (%g,%g) k %g\n", ax, ay, std::tan(ax * 0.5f), std::tan(ay * 0.5f),
           g_scene.x_in, g_scene.y_in, g_scene.x_out, g_scene.y_out, k);
  }
  // the scene's FOV scale (FOV setting / live game view; out = in * k * fov horizontally, in * fov vertically)
  const float fov = g_scene.y_in > 0 ? g_scene.y_out / g_scene.y_in : 1.0f;
  if (g_marker_vals || std::fabs(fov - 1.0f) < 1e-4f || !(fov > 0.2f && fov < 5.0f) || !(ax > 0 && ax < 3.1f && ay > 0 && ay < 3.1f)) return;
  g_marker_vals = p;
  g_marker_old[0] = ld32(p);
  g_marker_old[1] = ld32(p + 4);
  stf(p, 2.0f * std::atan(std::tan(ax * 0.5f) * fov));   // the values are full view angles (tan of the half = window)
  stf(p + 4, 2.0f * std::atan(std::tan(ay * 0.5f) * fov));
}

// #87 game text (subtitles, talk window, menus) is RenderWare Rt2d (docs/stage-workshop/research/subtitles-87.md): Rt2d
// copied the main camera's view window ONCE at boot (G = 0x805E1950, +0x248 / +0x24C) and sizes text by that cached
// window / the window the camera projects with now. Our FOV setting would resize every line, so for each print
// (fn_80049BC0) the cache gets the same FOV factor this hook applied to that camera: the text keeps the game's own size
// and layout (#115: never the 16:9 factor, see below). Cached values put back and the view matrix rebuilt (dirty flag +0x318) around it.
void f_80049BC0(Cpu&);
void hle_TextPrint(Cpu& c) {
  const uint32_t G = 0x805E1950u, cam = ld32(G + 0x34C);
  auto it = g_cams.find(cam);
  // #115: only the FOV factor (out / in vertically, the same on both axes). The 16:9 factor must stay out: the boot
  // cache already holds the widened width, and scaling it again stretched Rt2d's plane sideways - left-anchored lines
  // (pause mission text, mission start box, talk window) slid far left of their boxes while centred ones looked right.
  const float f = it == g_cams.end() || it->second.y_in <= 0 ? 1.0f : it->second.y_out / it->second.y_in;
  if (!(f > 0.2f && f < 5.0f) || std::fabs(f - 1.0f) < 1e-4f) { f_80049BC0(c); return; }   // FOV 100%: untouched
  const uint32_t wx = ld32(G + 0x248), wy = ld32(G + 0x24C);
  stf(G + 0x248, ldf(G + 0x248) * f);
  stf(G + 0x24C, ldf(G + 0x24C) * f);
  st32(G + 0x318, 0);
  f_80049BC0(c);
  st32(G + 0x248, wx);
  st32(G + 0x24C, wy);
  st32(G + 0x318, 0);
}
