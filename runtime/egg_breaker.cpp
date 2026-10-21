// Egg Breaker in any stage (#127, 2026-10-19). The user (Playground): "Egg Breaker AI doesn't work, he just
// menacingly looks at me". Cause (static, docs/bosses/BOSS-SUPPORT.md 1.3): EggMecaModeCtrlUnit_WAIT fn_802A9594 picks
// the next mode only for stages 310 / 411 / 511 (default: nothing - he waits for ever) and ..._ATK_SHOOT fn_802AA620
// has its timings / shot choice per stage too. Fix: while those two run, the current stage number (0x8057D6A0 + 0xA8)
// reads as the boss's OWN stage (EggMeca310 / 411 / 511 by vtable), so his own stage's decisions run; restored at once.
// Not touched: EggMecaCtrlUnit_AdjustCircleArea (its clamp is centred on the world origin; the editor's arena leash
// is boss_arena.cpp) and FixLookCharCamera fn_802A5AC0 (no default branch: nothing outside 310 / 411 / 511).
// EGG_NO_AI=1: off (vanilla behaviour). EGG_LOG=1: logs each mode change of each Egg Breaker.
#include "sys.h"
#include <cstdlib>
#include <string>
#include <unordered_map>

void f_802A9594(Cpu&);   // EggMecaModeCtrlUnit_WAIT update (r3 unit, unit+8 = the boss)
void f_802AA620(Cpu&);   // EggMecaModeCtrlUnit_ATK_SHOOT update

namespace {
constexpr uint32_t kStage = 0x8057D6A0u + 0xA8;
bool ram(uint32_t a) { return a >= 0x80000000u && a < RT_RAM_END; }
uint32_t own_stage(uint32_t boss) {
  if (!ram(boss)) return 0;
  // unit+8 is the boss's VarParam (EggMecaVarParam vtable 80559870 = EggMeca511's vtable 80559850 + 0x20, measured);
  // 411 / 310 assumed to follow the same layout (+0x20 after EggMeca411 80559990 / EggMeca310 80559AB8)
  switch (ld32(boss)) {
    case 0x80559870u: return 511;
    case 0x805599B0u: return 411;
    case 0x80559AD8u: return 310;
    default: return 0;
  }
}
struct StageAs {
  uint32_t keep = 0;
  bool on = false;
  explicit StageAs(uint32_t boss) {
    static const bool off = getenv("EGG_NO_AI") != nullptr;
    const uint32_t s = own_stage(boss);
    keep = ld32(kStage);
    on = !off && s && keep != 310 && keep != 411 && keep != 511;
    static const uint32_t as = getenv("EGG_AS") ? (uint32_t)atoi(getenv("EGG_AS")) : 0;   // research: another stage's logic
    if (on) st32(kStage, as ? as : s);
  }
  ~StageAs() { if (on) st32(kStage, keep); }
};
void log_mode(uint32_t boss) {
  static const bool log = getenv("EGG_LOG") != nullptr;
  if (!log || !ram(boss)) return;
  static std::unordered_map<uint32_t, uint32_t> last;
  const uint32_t m = ld32(boss + 0x14);
  auto it = last.find(boss);
  if (it != last.end() && it->second == m) return;
  last[boss] = m;
  logf("[egg] %08X (EggMeca%u) mode -> %u  pos (%.0f, %.0f, %.0f)\n", boss, own_stage(boss), m,
       (double)ldf32(boss + 0xFC), (double)ldf32(boss + 0x100), (double)ldf32(boss + 0x104));
  static int dumped = 0;
  if (dumped++ < 1)   // research: which words of this object point at an object with a 0x8055xxxx / 0x8054xxxx word at +0 or +0x18
    for (uint32_t o = 0; o < 0x400; o += 4) {
      const uint32_t w = ld32(boss + o);
      if (!ram(w) || !ram(w + 0x20)) continue;
      const uint32_t a = ld32(w), b = ld32(w + 0x18);
      if ((a >> 20) == 0x805 || (b >> 20) == 0x805) logf("[egg] +%03X -> %08X (+0 %08X, +0x18 %08X)\n", o, w, a, b);
    }
  if (dumped == 1) logf("[egg] object +0 %08X +0x18 %08X\n", ld32(boss), ld32(boss + 0x18));
  if (dumped == 1) {
    const uint32_t xf = ld32(boss + 0x1C);
    logf("[egg] vp+0x1C -> %08X (vp %+d):", xf, (int)(xf - boss));
    for (uint32_t o = 0; o < 0x40; o += 4) logf(" %.1f", (double)ldf32(xf + o));
    logf("\n");
  }
}
}  // namespace

// EGG_LOG=1 research: which Egg Breaker units ran in the last ~0.3 s (the runtime's per-function call counters), logged
// when the set changes. Called from boss_arena::step (every boss base update).
// ---- #127 laps on the user's path. The build writes mods/boss_path.txt, one line per boss with a path:
//   <type hex> <speed units/s> x z x z ...     (a closed loop, game units; BOSS_PATH=<one line> replaces the file)
// While Egg Breaker is between attacks (mode 0 WAIT / 2 MOVE) he walks the loop: his position (VarParam+0x1C ->
// x y z, = boss+0x290) moves toward the next point at that speed and he faces along it (his yaw +0x10 there and his
// turn target VarParam+0x1F4). Attacks (approach / hammer / shoot / laser ...) move him as the game does; after one he
// heads back to the point he was going to. EGG_PATH_LOG=1 logs the lap progress.
#include <cmath>
#include <fstream>
#include <sstream>
#include <vector>
#include <windows.h>
namespace {
struct Path { uint32_t type = 0; float speed = 300; std::vector<float> xz; };
std::vector<Path> g_paths;
int g_path_stage = -1;
std::unordered_map<uint32_t, size_t> g_next;   // boss -> index of the point it is heading for
std::unordered_map<uint32_t, bool> g_aim;      // VarParam -> its WAIT is about to decide (turn to the player)
void load_paths() {
  const int stage = (int)(int32_t)ld32(kStage);
  if (stage == g_path_stage) return;
  g_path_stage = stage;
  g_paths.clear();
  g_next.clear();
  std::string s;
  if (const char* e = getenv("BOSS_PATH")) s = e;
  else {
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string dir = exe;
    dir = dir.substr(0, dir.find_last_of("\\/"));
    std::ifstream f(dir + "\\mods\\boss_path.txt");
    if (!f) return;
    std::stringstream ss;
    ss << f.rdbuf();
    s = ss.str();
  }
  std::istringstream in(s);
  std::string line;
  while (std::getline(in, line, line.find(';') != std::string::npos ? ';' : '\n')) {
    std::istringstream l(line);
    std::string type;
    Path p;
    if (!(l >> type >> p.speed)) continue;
    p.type = (uint32_t)strtoul(type.c_str(), nullptr, 16);
    for (float v; l >> v;) p.xz.push_back(v);
    if (p.xz.size() >= 4) g_paths.push_back(p);
  }
  logf("[egg] %zu boss path(s) for stage %d\n", g_paths.size(), stage);
}
uint32_t egg_type(uint32_t boss) {   // SET type of an EggMeca object (00/B9 310, 00/BA 411, 00/BB 511)
  switch (ld32(boss + 0x18)) {
    case 0x80559850u: return 0xBB;
    case 0x80559990u: return 0xBA;
    case 0x80559AB8u: return 0xB9;
    default: return 0;
  }
}
void drive(uint32_t boss) {
  load_paths();
  if (g_paths.empty()) return;
  const uint32_t type = egg_type(boss), vp = boss + 0x38;
  const Path* p = nullptr;
  for (const Path& x : g_paths) if (x.type == type) p = &x;
  if (!p || !ram(ld32(vp + 0x1C))) return;
  const uint32_t mode = ld32(vp + 0x14);
  if (mode != 0 && mode != 2) return;
  const uint32_t xf = ld32(vp + 0x1C);
  const float x = (float)ldf32(xf), z = (float)ldf32(xf + 8);
  if (x == 0.f && z == 0.f) return;   // #130: not placed yet - the first lap point was picked from the origin
  const size_t n = p->xz.size() / 2;
  auto it = g_next.find(boss);
  if (it == g_next.end()) {   // start at the nearest point
    size_t best = 0; float bd = 1e30f;
    for (size_t i = 0; i < n; ++i) { const float dx = p->xz[2 * i] - x, dz = p->xz[2 * i + 1] - z, d = dx * dx + dz * dz; if (d < bd) { bd = d; best = i; } }
    it = g_next.emplace(boss, best).first;
  }
  float tx = p->xz[2 * it->second], tz = p->xz[2 * it->second + 1];
  float dx = tx - x, dz = tz - z, d = std::sqrt(dx * dx + dz * dz);
  if (d < 20.f) {
    it->second = (it->second + 1) % n;
    tx = p->xz[2 * it->second]; tz = p->xz[2 * it->second + 1];
    dx = tx - x; dz = tz - z; d = std::sqrt(dx * dx + dz * dz);
    static const bool log = getenv("EGG_PATH_LOG") != nullptr;
    if (log) logf("[egg] %08X lap -> point %zu (%.0f, %.0f)  yaw now %.2f target %.2f (want %.2f)\n", boss, it->second, (double)tx, (double)tz,
                  (double)ldf32(xf + 0x10), (double)ldf32(vp + 0x1F4), (double)std::atan2(dx, dz));
  }
  if (d < 1e-3f) return;
  float dt = (float)ldf32(0x8057D6A0u + 0x20);   // frame time (the boss units read it there)
  if (!(dt > 0.f && dt < 0.1f)) dt = 1.f / 60;
  // #130 test: EGG_SLIDE=1 = the old 300 u/s push; default: no push - the walk motion's own root motion moves him
  static const bool slide = getenv("EGG_SLIDE") != nullptr;
  if (slide) {
    const float step = std::fmin(p->speed * dt, d);
    stf32(xf, x + dx / d * step);
    stf32(xf + 8, z + dz / d * step);
  }
  // facing: yaw at [VarParam+0x1C]+0x10, the player's convention (0 = facing -z, + toward -x). His Move unit only turns
  // him while flag VarParam+0x1B5 is set (MOVE / attacks), so while lapping the yaw is turned here (max 6 rad/s).
  static const float yaw_add = getenv("EGG_YAW") ? (float)atof(getenv("EGG_YAW")) : 0.f;   // research: convention check
  // #130: his attack choice (WAIT, fn_802A9A80) needs him FACING the player: the game keeps that target yaw in
  // VarParam+0x1F4 - never overwritten here; in the last 0.35 s of each WAIT (g_aim, set by hle_EggWait) he turns to it,
  // the rest of the time he faces along the lap
  const bool aim = g_aim.count(vp) && g_aim[vp];
  const float want = aim ? (float)ldf32(vp + 0x1F4) : std::atan2(-dx, -dz) + yaw_add, cur = (float)ldf32(xf + 0x10);
  float diff = std::remainder(want - cur, 6.2831853f);
  const float turn = (aim ? 12.f : 6.f) * dt;   // aiming: a quick turn to the player before the decision
  diff = std::fmax(-turn, std::fmin(turn, diff));
  stf32(xf + 0x10, std::remainder(cur + diff, 6.2831853f));
  static const bool plog = getenv("EGG_PATH_LOG") != nullptr;
  static uint64_t last = 0;
  if (plog && sys_time_ns() - last > 500000000ull) { last = sys_time_ns(); logf("[egg] pos %.1f %.1f yaw %.3f\n", (double)ldf32(xf), (double)ldf32(xf + 8), (double)ldf32(xf + 0x10)); }
}
}  // namespace

namespace egg_breaker {
void trace(uint32_t boss) {
  drive(boss);
  static const bool dump = getenv("EGG_POSDUMP") != nullptr;   // research: the boss object 0..0x800 at update 200 and 700
  if (dump) {
    static std::unordered_map<uint32_t, int> upd;
    const int k = ++upd[boss];
    if (k == 200 || k == 700)
      for (uint32_t o = 0; o < 0x800; o += 4) { const float f = (float)ldf32(boss + o);
        if (f == f && f > -1e5f && f < 1e5f && (f > 1.f || f < -1.f)) logf("[eggdump] %08X u%d +%03X %.1f\n", boss, k, o, (double)f); }
  }
  static const bool log = getenv("EGG_LOG") != nullptr;
  if (!log) return;
  static const struct { uint32_t fn; const char* name; } kUnits[] = {
    {0x802A9594, "WAIT"}, {0x802A9E20, "GET_COMBO"}, {0x802A9F6C, "GET_ATK"}, {0x802AA11C, "MOVE"}, {0x802AA288, "DASH"},
    {0x802AA4EC, "ATK_HAMMER"}, {0x802AA620, "ATK_SHOOT"}, {0x802AA8D4, "ATK_MOVE"}, {0x802AAA00, "ATK_BOMB"},
    {0x802AAB90, "ATK_LASER"}, {0x802AAD64, "ATK_JUMP"}, {0x802AB2E0, "START"}, {0x802AB3E8, "BELOW_HALF"},
    {0x802AB5D8, "DEATH"}, {0x802A9D88, "EDIT"}, {0x802A6048, "u.Move"}, {0x802A215C, "u.Laser"}, {0x802A3918, "u.IronBall"},
    {0x802A1738, "u.Bomb"}, {0x8029FDD0, "u.ShockWave"}, {0x802A0BB4, "u.RollUpper"}, {0x8029F4D0, "u.AdjustCircle"},
    {0x802A5AC0, "u.FixLookCam"}, {0x8029176C, "u.PlayCamMotion"}, {0x80290DF4, "u.CheckEvent"}};
  constexpr size_t n = sizeof kUnits / sizeof kUnits[0];
  static uint32_t prev[n] = {}, frames = 0;
  static std::string last;
  if (++frames % 20) return;
  std::string now;
  for (size_t i = 0; i < n; ++i) {
    const uint32_t v = g_cov[(kUnits[i].fn - 0x80000000u) >> 2];
    if (v != prev[i]) { now += ' '; now += kUnits[i].name; }
    prev[i] = v;
  }
  if (now != last) { logf("[egg] running:%s  (t %.2f)\n", now.c_str(), (double)ldf32(0x8057D734)); last = now; }
}
}  // namespace egg_breaker

// Walk motion while lapping: the mode units set their motion in their constructors as f_804165D8(motion, 0, -1, -1);
// f_80417568(motion, [VarParam+0x44] + index); f_80416148(motion, 0, f1 = [r2-0xF98]) with motion = [VarParam+0x3C]
// (MOVE fn_802A90A0: index 15 = walk; WAIT 13 = stand, DASH 19, ATK_MOVE 18, START 14). WAIT plays 'stand' each time
// it is entered, so on every entry into WAIT with a lap path the walk motion is set again.
void f_804165D8(Cpu&);
void f_80417568(Cpu&);
void f_80416148(Cpu&);
namespace {
bool has_path(uint32_t vp) {
  load_paths();
  const uint32_t s = own_stage(vp), type = s == 511 ? 0xBB : s == 411 ? 0xBA : s == 310 ? 0xB9 : 0;
  for (const Path& x : g_paths) if (x.type == type) return true;
  return false;
}
void play_motion(Cpu& c, uint32_t vp, uint32_t index) {
  const Cpu keep = c;
  const uint32_t m = ld32(vp + 0x3C);
  c.r[3] = m; c.r[4] = 0; c.r[5] = 0xFFFFFFFFu; c.r[6] = 0xFFFFFFFFu; f_804165D8(c);
  c.r[3] = m; c.r[4] = (ld32(vp + 0x44) + index) & 0xFFFF; f_80417568(c);
  c.r[3] = m; c.r[4] = 0; c.f[1] = c.ps1[1] = ldf32(keep.r[2] + 0xFFFFF068u); f_80416148(c);
  c = keep;
}
}  // namespace

void hle_EggWait(Cpu& c) {
  const uint32_t unit = c.r[3], boss = ld32(c.r[3] + 8);
  g_aim[boss] = (float)ldf32(unit + 0xC) < 0.35f;   // the WAIT timer (unit+0xC) runs down to the decision (#130: 0.8 s left no time to walk)
  { StageAs s(boss); f_802A9594(c); }
  log_mode(boss);
  static std::unordered_map<uint32_t, uint64_t> last_wait;   // VarParam -> time of its last WAIT update
  const uint64_t now = sys_time_ns(), prev = last_wait[boss];
  last_wait[boss] = now;
  if (now - prev > 100000000ull && ram(ld32(boss + 0x3C)) && ld32(boss + 0x14) == 0 && has_path(boss) && !getenv("EGG_NO_WALK"))
    play_motion(c, boss, 15);   // just (re)entered WAIT and he laps: walk instead of stand
}
void hle_EggShoot(Cpu& c) {
  const uint32_t boss = ld32(c.r[3] + 8);
  { StageAs s(boss); f_802AA620(c); }
  log_mode(boss);
}

// #127 "remove the forced camera lock-on to him when his battle starts": his START mode (fn_802AB2E0) waits for his
// start motion (motion 3, ~3 s) and holds the intro camera (BossCtrlUnit_PlayCameraMotion, name card, no HUD) until
// then. Outside 310 / 411 / 511 and with settings key boss_intro_camera = 0 (default) START ends at once (mode 2, what
// START itself sets when the motion is done), so the fight begins with the player's own camera.
#include "settings_ui.h"
void f_802AB2E0(Cpu&);
void hle_EggStart(Cpu& c) {
  const uint32_t vp = ld32(c.r[3] + 8);
  const uint32_t stage = ld32(kStage);
  if (!settings::g.boss_intro_camera && own_stage(vp) && stage != 310 && stage != 411 && stage != 511) {
    st32(vp + 0x14, 2);
    c.r[3] = 0;
    return;
  }
  f_802AB2E0(c);
}
