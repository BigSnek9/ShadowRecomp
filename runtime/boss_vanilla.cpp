// #130 vanilla boss AI anywhere - Black Bull 0210 (2026-10-20; Blue Falcon / Heavy Dog / Black Doom in
// boss_ai.cpp). Its AI is kept; only its stage dependency is moved: CHASE flies to one of 8 points of a table at
// 0x80583CC8 (12 bytes each, x y z; filled once by fn_80236478; next index at VarParam+0x18C, the chosen point copied
// to VarParam+0xFC, home = VarParam+0x108, distance to the point VarParam+0x114, leg over when < VarParam+0x7C). The
// points are stage 210's arena round the WORLD ORIGIN (+-680), so in another stage it flew off toward them (or into a
// wall) and hovered there. Outside stage 210 the table is rebuilt round the boss card's arena for 00/B4
// (mods/boss_arena.txt; scaled to its radius) or round the boss's home (its placement) at half size (vanilla's +-680
// square only fits a stage built for it: in the 1200-wide blank stage it flew into the walls); stage 210 gets the
// original table back. A CHASE leg that stops getting closer for 1.5 s (a wall, or circling a scaled point it cannot
// turn into) ends as vanilla ends it (WAIT).
// BB_ANCHOR_LOG=1 logs the table and the stuck legs. Black Bull 0412 has no CHASE (it hovers at its placement and
// returns there after a charge): nothing to move.
#include "sys.h"
#include <cmath>
#include <cstdlib>

namespace boss_arena { bool arena_of(uint32_t type, float& cx, float& cz, float& r); }

namespace bb0210 {
namespace {
constexpr uint32_t kTable = 0x80583CC8u;
float g_orig[8][3];
bool g_saved = false, g_moved = false;
float g_cx = 0, g_cz = 0, g_scale = 1;
bool log_on() { static const bool on = getenv("BB_ANCHOR_LOG") != nullptr; return on; }
}  // namespace

// called from Black Bull 0210's START / WAIT / CHASE updates (boss.cpp) with its VarParam
void anchor(uint32_t vp) {
  if (!g_saved) {
    float r = 0;
    for (int i = 0; i < 8; ++i)
      for (int k = 0; k < 3; ++k) { g_orig[i][k] = (float)ldf32(kTable + i * 12 + k * 4); r = std::fmax(r, std::fabs(g_orig[i][k])); }
    if (!(r > 1.f && r < 1e5f)) return;   // not filled yet
    g_saved = true;
    if (log_on())
      for (int i = 0; i < 8; ++i) logf("[bb0210] table %d (%.1f, %.1f, %.1f)\n", i, g_orig[i][0], g_orig[i][1], g_orig[i][2]);
  }
  const bool own = ld32(0x80584720) == 210;
  float cx = 0, cz = 0, scale = 1, ar = 0;
  if (!own) {
    float rmax = 0;
    for (auto& p : g_orig) rmax = std::fmax(rmax, std::sqrt(p[0] * p[0] + p[2] * p[2]));
    if (boss_arena::arena_of(0xB4, cx, cz, ar) && rmax > 0) scale = ar / rmax;
    else { cx = (float)ldf32(vp + 0x108); cz = (float)ldf32(vp + 0x110); scale = 0.5f; }
  }
  if (own ? !g_moved : (g_moved && cx == g_cx && cz == g_cz && scale == g_scale)) return;
  for (int i = 0; i < 8; ++i) {
    stf32(kTable + i * 12, own ? g_orig[i][0] : cx + g_orig[i][0] * scale);
    stf32(kTable + i * 12 + 4, g_orig[i][1]);
    stf32(kTable + i * 12 + 8, own ? g_orig[i][2] : cz + g_orig[i][2] * scale);
  }
  g_moved = !own; g_cx = cx; g_cz = cz; g_scale = scale;
  if (log_on()) logf("[bb0210] CHASE points %s round (%.0f, %.0f) x %.2f%s\n", own ? "restored" : "moved", cx, cz, scale, ar > 0 ? " (boss card arena)" : "");
}

// after the CHASE update: a leg that has not got 5 units closer in 1.5 s ends (request WAIT + the flag CHASE sets)
void chase_guard(uint32_t vp) {
  if (ld32(0x80584720) == 210) return;
  static uint32_t s_vp = 0; static float s_best = 0; static uint64_t s_t = 0;
  const float d = (float)ldf32(vp + 0x114);
  const uint64_t now = sys_time_ns();
  if (vp != s_vp || ld32(vp + 0x0C) != 3 || d < s_best - 5.f) { s_vp = vp; s_best = d; s_t = now; return; }
  if (now - s_t < 1500000000ull) return;
  st32(vp + 0x14, 2);
  st8(vp + 0x172, 1);
  s_t = now;
  if (log_on()) logf("[bb0210] CHASE leg stuck %.0f from its point -> WAIT\n", d);
}
}  // namespace bb0210
