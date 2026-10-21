// Boss arena leash (#123, 2026-10-19). A boss placed in another stage keeps its own AI: Blue Falcon chases the
// player over the whole stage (quiet run: it flew 2,500 units from its arena to the start in 15 s). The editor's Boss
// card stores an arena per boss (o.boss.arena: circle / box / ring); the build writes them to mods/boss_arena.txt, one
// line per boss:  <type hex> circle <cx> <cz> <r>  |  <type hex> box <cx> <cz> <width> <depth>  |  <type hex> ring <cx> <cz> <r> <inner>
// (game units, x / z; the height is left to the boss). After each boss base update (boss_slots.cpp hle_BossBaseUpdate)
// the boss's live position is pushed back inside its arena. Live position offsets (measured, BOSS_POSDUMP=1):
//   Blue Falcon (00/B7, vtable 80549A78): +0x248 (x, y, z)    Black Bull 0210 (00/B4, vtable 805473B8): +0xFC and +0x108
//   Egg Breaker 310 / 411 / 511 (00/B9 / BA / BB): +0x290 (#127)
// Bosses not listed (Egg Breaker clamps itself round its stage's own centre) are left alone. BOSS_ARENA=<one line> (tests)
// replaces the file. BOSS_ARENA_LOG=1 logs each push once a second.
#include "sys.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <windows.h>

namespace {
struct Arena { uint32_t type; int shape; float cx, cz, a, b; };   // shape 0 circle (a = r), 1 box (a, b = width, depth), 2 ring (a = r, b = inner)
struct Kind { uint32_t vtable, type; uint32_t offs[2]; int n; };
const Kind kKinds[] = {{0x80549A78u, 0xB7, {0x248, 0}, 1}, {0x805473B8u, 0xB4, {0xFC, 0x108}, 2},
                       {0x80559850u, 0xBB, {0x290, 0}, 1}, {0x80559990u, 0xBA, {0x290, 0}, 1}, {0x80559AB8u, 0xB9, {0x290, 0}, 1}};   // Egg Breaker 511 / 411 / 310: boss+0x290 (#127)
std::vector<Arena> g_arenas;
int g_stage = -1;

void load() {
  const int stage = (int)(int32_t)ld32(0x8057D748);
  if (stage == g_stage) return;
  g_stage = stage;
  g_arenas.clear();
  std::string s;
  if (const char* e = getenv("BOSS_ARENA")) s = e;
  else {
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string dir = exe;
    dir = dir.substr(0, dir.find_last_of("\\/"));
    std::ifstream f(dir + "\\mods\\boss_arena.txt");
    if (!f) return;
    std::stringstream ss;
    ss << f.rdbuf();
    s = ss.str();
  }
  std::istringstream in(s);
  std::string line;
  while (std::getline(in, line, line.find(';') != std::string::npos ? ';' : '\n')) {
    std::istringstream l(line);
    std::string type, shape;
    Arena a{};
    if (!(l >> type >> shape >> a.cx >> a.cz >> a.a)) continue;
    a.type = (uint32_t)strtoul(type.c_str(), nullptr, 16);
    a.shape = shape == "box" ? 1 : shape == "ring" ? 2 : 0;
    if (a.shape) l >> a.b;
    g_arenas.push_back(a);
  }
  logf("[boss-arena] %zu arena(s) for stage %d\n", g_arenas.size(), stage);
}
}  // namespace

namespace egg_breaker { void trace(uint32_t boss); }   // egg_breaker.cpp

namespace boss_arena {
// the editor arena for a boss type (circle / ring: r; box: half the shorter side) - boss_vanilla.cpp re-anchors with it
bool arena_of(uint32_t type, float& cx, float& cz, float& r) {
  load();
  for (const Arena& x : g_arenas)
    if (x.type == type) { cx = x.cx; cz = x.cz; r = x.shape == 1 ? std::fmin(x.a, x.b) / 2 : x.a; return r > 0; }
  return false;
}
void step(uint32_t boss) {
  if ((ld32(boss + 0x18) & 0xFFFFF000u) == 0x80559000u) egg_breaker::trace(boss);   // EggMeca310 / 411 / 511 (EGG_LOG research)
  load();
  if (g_arenas.empty()) return;
  const uint32_t vt = ld32(boss + 0x18);   // the vtable (as boss_slots.cpp reads it)
  const Kind* k = nullptr;
  for (const Kind& x : kKinds) if (x.vtable == vt) k = &x;
  if (!k) return;
  const Arena* a = nullptr;
  for (const Arena& x : g_arenas) if (x.type == k->type) a = &x;
  if (!a) return;
  for (int i = 0; i < k->n; ++i) {
    const uint32_t p = boss + k->offs[i];
    const float x = (float)ldf32(p), z = (float)ldf32(p + 8);
    if (!std::isfinite(x) || !std::isfinite(z) || std::fabs(x) > 1e6f || std::fabs(z) > 1e6f) continue;
    if (k->type >= 0xB9 && k->type <= 0xBB && x == 0.f && z == 0.f) continue;   // Egg Breaker before his SET position is set (#130: was snapped to the arena edge)
    float nx = x, nz = z;
    if (a->shape == 1) {
      nx = std::fmin(std::fmax(x, a->cx - a->a / 2), a->cx + a->a / 2);
      nz = std::fmin(std::fmax(z, a->cz - a->b / 2), a->cz + a->b / 2);
    } else {
      const float dx = x - a->cx, dz = z - a->cz, d = std::sqrt(dx * dx + dz * dz);
      const float lo = a->shape == 2 ? a->b : 0.f;
      if (d > a->a && d > 0) { nx = a->cx + dx / d * a->a; nz = a->cz + dz / d * a->a; }
      else if (d < lo && d > 0) { nx = a->cx + dx / d * lo; nz = a->cz + dz / d * lo; }
    }
    if (nx == x && nz == z) continue;
    stf32(p, nx);
    stf32(p + 8, nz);
    static const bool log = getenv("BOSS_ARENA_LOG") != nullptr;
    static uint64_t last = 0;
    if (log && sys_time_ns() - last > 1000000000ull) {
      last = sys_time_ns();
      logf("[boss-arena] %02X %08X +%X (%.0f, %.0f) -> (%.0f, %.0f)\n", k->type, boss, k->offs[i], x, z, nx, nz);
    }
  }
}
}  // namespace boss_arena

// #127 (user, Playground): "remove the forced camera lock-on to him when his battle starts". A boss's intro is
// BossCtrlUnit_PlayCameraMotion (update fn_8029176C: r3 = 1 while its camera motion plays, 0 = done; START waits for
// it). Outside the boss stages it is skipped (reports done at once, the camera stays the player's) unless settings key
// boss_intro_camera = 1. In the boss stages themselves nothing changes.
#include "settings_ui.h"
void f_8029176C(Cpu&);
void hle_BossCamMotion(Cpu& c) {
  const int stage = (int)(int32_t)ld32(0x8057D748);
  const bool boss_stage = stage == 210 || stage == 310 || (stage >= 410 && stage <= 412) || stage == 510 || stage == 511 ||
                          (stage >= 610 && stage <= 618) || stage == 710;
  if (boss_stage || settings::g.boss_intro_camera) { f_8029176C(c); return; }
  c.r[3] = 0;
}
