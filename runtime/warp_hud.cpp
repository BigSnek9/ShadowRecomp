// Checkpoint warp screen (SavePointSelect, docs/stage-workshop/research/hexagon-hud-107.md): #107 / #108.
// The checkpoint's update fn_80117D6C (r3 = the checkpoint; +0x48 state, +0x50 flag word) opens the screen when the
// state is 2, flag bit 1 is set (the player's command 0xD at a touched checkpoint, from button X) and fn_80117494 agrees.
//   WARP_LOG=1    fn_80117494's four conditions each time it is asked (why the screen does / does not open)
//   WARP_OPEN=<s> test: s seconds after the first checkpoint update, set bit 1 on a checkpoint in state 2 (the game's own
//                 path then opens the screen if fn_80117494 agrees); WARP_FORCE=1 also makes fn_80117494 agree
#include "sys.h"
#include "settings_ui.h"
#include <imgui.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>

void f_80117D6C(Cpu&);
void f_80117494(Cpu&);
void f_80117578(Cpu&);
void f_8020261C(Cpu&);
void f_80074470(Cpu&);
void f_80117514(Cpu&);
void f_802E5810(Cpu&);
void f_80330CB0(Cpu&);
void f_8016F3A0(Cpu&);
void f_80079808(Cpu&);

void hle_SavePointUpdate(Cpu& c) {
  static const double open_at = getenv("WARP_OPEN") ? atof(getenv("WARP_OPEN")) : -1;
  if (open_at >= 0) {
    static uint64_t t0 = 0;
    static uint64_t last = 0;
    const uint64_t now = sys_time_ns();
    if (!t0) t0 = now;
    const uint32_t obj = c.r[3];
    if ((now - t0) * 1e-9 >= open_at && now - last > 1000000000ull && obj >= 0x80000000u && obj < RT_RAM_END - 0x60 &&
        ld32(obj + 0x48) == 2) {
      last = now;
      st32(obj + 0x50, ld32(obj + 0x50) | 2);
      logf("[warp] test: checkpoint %08X state 2 -> open request (flag bit 1)\n", obj);
    }
  }
  f_80117D6C(c);
}

void hle_WarpAllowed(Cpu& c) {
  static const bool log = getenv("WARP_LOG") != nullptr;
  static const bool force = getenv("WARP_FORCE") != nullptr;
  if (log) {
    const Cpu keep = c;
    const uint32_t b84 = ld8(0x8057D6A0u + 0x84);
    f_80117578(c);
    const uint32_t other = c.r[3];
    f_8020261C(c);
    const uint32_t blocked = c.r[3] & 0xFF;
    c = keep;
    f_80074470(c);
    const uint32_t mode = c.r[3] >= 0x80000000u && c.r[3] < RT_RAM_END ? ld32(c.r[3] + 8) : 0xFFFFFFFFu;
    c = keep;
    f_80117514(c);
    f_802E5810(c);
    const uint32_t busy = c.r[3] & 0xFF;
    c = keep;
    f_80330CB0(c);
    const uint32_t open_now = c.r[3] & 0xFF;   // r13+0x57F9: set by fn_80330CC4 when its value (r13+0x57FC) is 5-8
    const uint32_t avail_val = ld32(0x805EB440u + 0x57FC);
    c = keep;
    f_80079808(c);
    f_8016F3A0(c);
    const int32_t count = (int32_t)c.r[3];   // the game: fn_8016F3A0(fn_80079808()) must be > 0
    c = keep;
    f_80117494(c);
    static int n = 0;
    if (n++ < 30)
      logf("[warp] allowed? %u  (byte84 %u, other %08X blocks %u, game mode %u, busy %u) | fn_80330CB0 %u (value %d), fn_8016F3A0 %d%s\n",
           c.r[3] & 0xFF, b84, other, blocked, mode, busy, open_now, (int32_t)avail_val, count, force ? " - forced" : "");
  } else {
    f_80117494(c);
  }
  if (force) c.r[3] = 1;
}

// ---- #108: the warp screen's per-frame input / update fn_80331388 (r3 = the screen: +0x34 cursor (hexagon), +0x38
// count, +0x3C the checkpoint the player stands on, +0x40 + 8 * i = hexagon i's entry: +0 checkpoint id)
void f_80331388(Cpu&);
void f_8016C3D8(Cpu&);
int custom_stats_missions(uint32_t* objs, int* ids, int max);   // custom_stats.cpp
uint32_t custom_stats_mission_map();
namespace warp_hud {
uint32_t g_screen = 0;
uint64_t g_seen_ns = 0;
struct Shown { int n = 0, cursor = 0, faction = -1; int count[8] = {}; } g_shown;   // what the overlay draws (game thread fills it)

// the active "defeat" mission's faction (mission map type 3, param 0 GUN / 1 Eggman / 2 Black Arms): of this stage's
// defeat missions the one with the most kills so far (the HUD counter the player is filling); -1 = none
int active_faction(Cpu& c) {
  uint32_t objs[16]; int ids[16];
  const int n = custom_stats_missions(objs, ids, 16);
  const uint32_t map = custom_stats_mission_map();
  if (!map) return -1;
  int best = -1; int32_t best_kills = -1;
  for (int i = 0; i < n; ++i) {
    Cpu t = c;
    t.r[3] = map; t.r[4] = (uint32_t)ids[i];
    f_8016C3D8(t);
    const uint32_t rec = t.r[3];
    if (rec < 0x80000000u || rec >= RT_RAM_END) continue;
    if (ld32(rec) != 3) continue;
    const int32_t kills = objs[i] >= 0x80000000u && objs[i] < RT_RAM_END ? (int32_t)ld32(objs[i] + 8) : 0;
    if (kills > best_kills) { best_kills = kills; best = (int)ld32(rec + 4); }
  }
  return best;
}
bool in_faction(uint32_t cls, int f) {
  if (f == 0) return cls >= 0x64 && cls < 0x70;   // GUN
  if (f == 1) return cls >= 0x78 && cls < 0x80;   // Eggman
  if (f == 2) return cls >= 0x8C && cls < 0xA0;   // Black Arms
  return false;
}
// per hexagon: the alive enemies of the active faction whose nearest checkpoint is that hexagon's checkpoint
void count(Cpu& c, uint32_t screen) {
  Shown sh;
  sh.n = std::clamp((int32_t)ld32(screen + 0x38), 0, 8);
  sh.cursor = (int32_t)ld32(screen + 0x34);
  sh.faction = active_faction(c);
  const uint32_t m = ld32(0x805EE6D4);
  if (sh.faction < 0 || m < 0x80000000u || m >= RT_RAM_END) { g_shown = sh; return; }
  const uint32_t nrec = std::min<uint32_t>(ld32(m), 8192), tab = ld32(m + 0x18);
  struct Cp { int id; float x, y, z; };
  Cp cps[16]; int ncp = 0;
  auto rec_ok = [](uint32_t r) { return r >= 0x80000000u && r < RT_RAM_END; };
  for (uint32_t i = 0; i < nrec && ncp < 16; ++i) {   // checkpoints: id = the instance's +0x4C, else the record's first parameter
    const uint32_t e = tab + i * 0x20, rec = ld32(e);
    if (!rec_ok(rec) || ld8(rec + 0x20) != 0 || ld8(rec + 0x21) != 5) continue;
    const uint32_t inst = ld32(e + 0xC), prm = ld32(rec + 0x28);
    const int id = rec_ok(inst) ? (int32_t)ld32(inst + 0x4C) : rec_ok(prm) ? (int32_t)ld32(prm) : -1;
    cps[ncp++] = {id, float(ldf32(rec)), float(ldf32(rec + 4)), float(ldf32(rec + 8))};
  }
  for (uint32_t i = 0; i < nrec; ++i) {
    const uint32_t e = tab + i * 0x20, rec = ld32(e);
    if (!rec_ok(rec) || ld8(rec + 0x20) != 0 || !in_faction(ld8(rec + 0x21), sh.faction) || (ld32(rec + 0x18) & 8)) continue;
    const float x = float(ldf32(rec)), y = float(ldf32(rec + 4)), z = float(ldf32(rec + 8));
    int best = -1; float bd = 1e30f;
    for (int k = 0; k < ncp; ++k) {
      const float d = (cps[k].x - x) * (cps[k].x - x) + (cps[k].y - y) * (cps[k].y - y) + (cps[k].z - z) * (cps[k].z - z);
      if (d < bd) { bd = d; best = cps[k].id; }
    }
    for (int h = 0; h < sh.n; ++h)
      if ((int32_t)ld32(screen + 0x40 + h * 8) == best) ++sh.count[h];
  }
  g_shown = sh;
  static const bool log = getenv("WARP_LOG") != nullptr;
  static uint64_t last = 0;
  if (log && sys_time_ns() - last > 1000000000ull) {
    last = sys_time_ns();
    logf("[warp] enemy counts (faction %d, %d checkpoints):", sh.faction, ncp);
    for (int h = 0; h < sh.n; ++h) logf(" cp%d=%d", (int32_t)ld32(screen + 0x40 + h * 8), sh.count[h]);
    logf("  | checkpoint ids:");
    for (int k = 0; k < ncp; ++k) logf(" %d", cps[k].id);
    logf("\n");
  }
}
}  // namespace warp_hud

// settings_ui.cpp overlay: the red numbers on the hexagons while the warp screen is up. The row is centred on the screen,
// one hexagon every 0.0874 screen heights at 49.8 % height (measured at 16:9 and 4:3, 6 checkpoints, Westopolis).
void warp_hud_draw() {
  using namespace warp_hud;
  if (!settings::g.warp_enemy_counts || sys_time_ns() - g_seen_ns > 150000000ull) return;
  const Shown sh = g_shown;
  if (sh.faction < 0 || sh.n <= 0) return;
  const ImGuiViewport* v = ImGui::GetMainViewport();
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const float W = v->Size.x, H = v->Size.y;
  const float step = 0.0874f * H, y = 0.498f * H + 0.030f * H, px = 0.034f * H;
  ImFont* font = ImGui::GetFont();
  for (int h = 0; h < sh.n; ++h) {
    char b[8];
    snprintf(b, sizeof b, "%d", sh.count[h]);
    const ImVec2 sz = font->CalcTextSizeA(px, 1e9f, 0.f, b);
    const ImVec2 p(v->Pos.x + W * 0.5f + (h - (sh.n - 1) * 0.5f) * step - sz.x * 0.5f, v->Pos.y + y - sz.y * 0.5f);
    const ImU32 col = sh.count[h] > 0 ? IM_COL32(255, 40, 40, 255) : IM_COL32(170, 170, 170, 200);
    for (int dx = -1; dx <= 1; ++dx)
      for (int dy = -1; dy <= 1; ++dy)
        if (dx || dy) dl->AddText(font, px, ImVec2(p.x + dx * 2.f, p.y + dy * 2.f), IM_COL32(0, 0, 0, 220), b);
    dl->AddText(font, px, p, col, b);
  }
}
void hle_WarpScreenUpdate(Cpu& c) {
  warp_hud::g_screen = c.r[3];
  warp_hud::g_seen_ns = sys_time_ns();
  static const bool log = getenv("WARP_LOG") != nullptr;
  static bool once = false;
  if (log && !once) {
    once = true;
    const uint32_t s = c.r[3];
    logf("[warp] screen %08X: cursor %d count %d current %d entries", s, (int32_t)ld32(s + 0x34), (int32_t)ld32(s + 0x38), (int32_t)ld32(s + 0x3C));
    for (int i = 0; i < 8; ++i) logf(" [%d %d]", (int32_t)ld32(s + 0x40 + i * 8), (int32_t)ld32(s + 0x44 + i * 8));
    logf("\n");
    const uint32_t m = ld32(0x805EE6D4);
    const uint32_t n = ld32(m), tab = ld32(m + 0x18);
    for (uint32_t i = 0; i < n && i < 8192; ++i) {
      const uint32_t e = tab + i * 0x20, rec = ld32(e);
      if (rec < 0x80000000u || rec >= RT_RAM_END || ld8(rec + 0x20) != 0 || ld8(rec + 0x21) != 5) continue;
      const uint32_t inst = ld32(e + 0xC);
      logf("[warp] checkpoint record %u inst %08X inst+4C %d pos (%.0f,%.0f,%.0f) words", i, inst,
           inst >= 0x80000000u && inst < RT_RAM_END ? (int32_t)ld32(inst + 0x4C) : -99, ldf32(rec), ldf32(rec + 4), ldf32(rec + 8));
      for (int k = 0x18; k < 0x40; k += 4) logf(" %08X", ld32(rec + k));
      logf("\n");
    }
  }
  if (settings::g.warp_enemy_counts) warp_hud::count(c, c.r[3]);
  f_80331388(c);
}
