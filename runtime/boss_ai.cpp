// Several bosses in one stage (#125, 2026-10-19). The CtrlUnit bosses (Black Bull, Heavy Dog, Blue Falcon,
// Egg Breaker, Black Doom, Devil Doom) all show their HP through the shared unit BossCtrlUnit_DispGauge (update
// fn_80290FC8: P = *(*(unit + 0) + 8), ratio = P+0x24 HP / P+0x08 max -> fn_800BE2DC, the one HUD boss gauge) and end
// the stage from their DEATH state with fn_80175EEC(stage manager, 6, 0), the same "clear" the mission path uses.
// With more than one boss:
//  * the gauge shows the fight: sum of every boss's HP / sum of their max (each boss's own unit would overwrite it);
//  * a boss's clear call (r4 = 6, caller in the boss code 0x80230000..0x80320000) is held back while another boss still
//    has HP; the last one's goes through.
// Bosses are known from their gauge unit (seen within the last 3 s); for tests every
// boss is killed in turn (BOSS_KILL_SEQ below). BOSS_AI_LOG=1 logs.
#include "sys.h"
#include "settings_ui.h"
#include "game.h"
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern uint32_t g_plr_owner;   // frame_rate.cpp: the player object
void f_80290FC8(Cpu&);   // BossCtrlUnit_DispGauge update (original)
void f_800BE2DC(Cpu&);   // HUD boss gauge: set ratio (f1)
void f_80175EEC(Cpu&);   // stage manager action (r3 manager, r4 action: 6 = clear)
void f_80290DF4(Cpu&);   // BossCtrlUnit_CheckEvent update (original)

namespace {
bool ram(uint32_t a) { return a >= 0x80000000u && a < RT_RAM_END - 0x40; }
bool own_boss_stage();
const char* boss_name(uint32_t p);
// #130: outside a boss's own stage, the parts of its vanilla AI that depend on its stage are replaced (Black Doom's
// stage-bound GET_COMBO teleport -> points round his placement). BOSS_RAW=1: not even that (research).
bool env_fix() { static const bool raw = getenv("BOSS_RAW") != nullptr; return !raw && !own_boss_stage(); }
bool rework() { static const bool off = getenv("BOSS_NO_REWORK") != nullptr; return !off && settings::g.boss_rework; }
struct Boss { uint32_t p; uint64_t seen; bool killed; bool decoy = false; };
std::vector<Boss> g_boss;
int g_stage = -1;
uint64_t g_stage_t0 = 0;
bool log_on() { static const bool l = getenv("BOSS_AI_LOG") != nullptr; return l; }
float hp(uint32_t p) { return float(ldf32(p + 0x24)); }
float hpmax(uint32_t p) { return float(ldf32(p + 0x08)); }
void stage_check() {
  const int s = (int)(int32_t)ld32(0x8057D748);
  if (s != g_stage) { g_stage = s; g_boss.clear(); g_stage_t0 = sys_time_ns(); }
}
// a boss whose gauge unit has not run for 3 s is gone (a restart after Shadow's death builds a new set: never touch the
// old objects' memory again)
void prune() {
  const uint64_t now = sys_time_ns();
  g_boss.erase(std::remove_if(g_boss.begin(), g_boss.end(), [&](const Boss& b) { return b.seen && now - b.seen > 3000000000ull; }), g_boss.end());
}
std::vector<Boss*> live_bosses() {
  prune();
  std::vector<Boss*> v;
  for (auto& b : g_boss) if (ram(b.p) && !b.decoy) v.push_back(&b);   // decoys are not bosses of their own
  return v;
}
bool g_had_decoys = false;
uint32_t g_mgr = 0;   // the stage manager (r3 of every stage action call)
// death / destroy mode id per boss class (VarParam vtable at P+0; ids measured with BOSS_MODE_TRACE + BOSS_MODE_FORCE)
uint32_t death_mode(uint32_t p) {
  switch (ld32(p)) {
    case 0x80549224u: return 8;    // HeavyDog0410VarParam: DESTROY
    case 0x80549AACu: return 8;    // HeavyDog0510VarParam (Blue Falcon): DESTROY (measured: same ids as Heavy Dog)
    case 0x8054E35Cu: return 11;   // BlackDoomVarParam: DEATH
    case 0x805474CCu: return 9;    // BlackBull0210VarParam: DEATH
    case 0x805484B8u: return 9;    // BlackBull0412VarParam: DEATH
    case 0x80559870u: case 0x805599B0u: case 0x80559AD8u: return 16;   // EggMeca 511 / 411 / 310: DEATH (measured, v3)
    default: return 0;
  }
}
// BOSS_KILL_SEQ=<s>[,<step>] (tests of the clear rule): from that stage time, every <step> s (default 4) the next boss
// with a known death mode is killed for real: HP 0 + its death mode requested (P+0x14); its own DEATH / DESTROY state
// then makes the clear call fn_80175EEC(manager, 6, 0) (measured: Heavy Dog from 80250F2C, Black Doom from 8028691C).
void test_kill(Cpu&) {
  static const char* env = getenv("BOSS_KILL_SEQ");
  if (!env) return;
  static const double t0 = atof(env), step = strchr(env, ',') ? atof(strchr(env, ',') + 1) : 4.0;
  static double next = -1;
  const double t = double(sys_time_ns() - g_stage_t0) * 1e-9;
  if (next < 0) next = t0;
  if (t < next) return;
  next = t + step;
  for (size_t i = 0; i < g_boss.size(); ++i) {
    Boss& b = g_boss[i];
    const uint32_t m = ram(b.p) ? death_mode(b.p) : 0;
    if (b.killed || !m) continue;
    stf32(b.p + 0x24, 0.f);
    st32(b.p + 0x14, m);
    b.killed = true;
    logf("[boss-ai] test: boss %u (P %08X) killed at %.1f s (death mode %u)\n", unsigned(i), b.p, t, m);
    return;
  }
}
// ---- mode-id research (#126): BOSS_MODE_TRACE=1 logs, per boss class, which ModeCtrlUnit update ran under which mode
// word (P+0x14), from the recompiler's per-function call counters (one boss of a class in the stage at a time).
struct Unit { uint32_t fn; const char* name; };
const Unit kHeavyDog[] = {{0x80250290, "WAIT"}, {0x80250C78, "START"}, {0x80250440, "CHASE"}, {0x80250614, "ESCAPE"},
    {0x80250B90, "GET_ATK"}, {0x80251054, "ATK_ROCKET"}, {0x802510F8, "ATK_BARRIER"}, {0x8025131C, "ATK_MINE"},
    {0x80250D94, "BELOW_HALF"}, {0x80250ED0, "DESTROY"}, {0x80250394, "EDIT"}, {0x802513C0, "MOVETEST"}};
const Unit kBlackDoom[] = {{0x8028522C, "WAIT"}, {0x80286608, "START"}, {0x8028558C, "GET_ATK"}, {0x802853A8, "GET_COMBO"},
    {0x80285750, "ATK_READY"}, {0x80285B84, "ATK_DISK"}, {0x80285EFC, "ATK_METEOR"}, {0x80286324, "ATK_BOOMERANG"},
    {0x80286708, "BELOW_HALF"}, {0x802868C0, "DEATH"}, {0x80285310, "EDIT"}, {0x80286A44, "MOVETEST"}};
const Unit kBlueFalcon[] = {{0x802526E0, "WAIT"}, {0x80252F8C, "START"}, {0x80252848, "CHASE"}, {0x80252B7C, "ESCAPE"},
    {0x80252EA4, "GET_ATK"}, {0x80253328, "ATK_ROCKET"}, {0x802533CC, "ATK_BARRIER"}, {0x802535BC, "ATK_MINE"},
    {0x802530D4, "BELOW_HALF"}, {0x8025321C, "DESTROY"}, {0x802527A4, "EDIT"}, {0x80253660, "MOVETEST"}};
struct Kit { const char* name; const Unit* units; int n; };
const Unit kBB0210[] = {{0x80234D78, "WAIT"}, {0x80236248, "START"}, {0x8023514C, "CHASE"}, {0x8023520C, "ESCAPE"},
    {0x802352EC, "DIZZY"}, {0x802353C8, "GET_COMBO"}, {0x802354F4, "GET_ATK"}, {0x802357B0, "BELOW_HALF"}, {0x8023599C, "DEATH"},
    {0x80235C98, "ATK_BREATH"}, {0x80235E50, "ATK_WAVERING"}, {0x802360A8, "ATK_CLOSERANGE"}, {0x802350B4, "EDIT"}, {0x80236394, "MOVETEST"}};
const Unit kBB0412[] = {{0x80244E28, "WAIT"}, {0x802465F4, "START"}, {0x80245004, "ESCAPE"}, {0x80245160, "GET_COMBO"},
    {0x80245254, "GET_ATK"}, {0x802454E4, "BELOW_HALF"}, {0x802456D0, "DEATH"}, {0x8024597C, "ATK_BREATH"}, {0x80245B48, "ATK_WAVERING"},
    {0x80245D90, "ATK_CLOSERANGE"}, {0x8024679C, "ATK_CHARGE"}, {0x80244F6C, "EDIT"}};
const Unit kEgg[] = {{0x802A9594, "WAIT"}, {0x802A9E20, "GET_COMBO"}, {0x802A9F6C, "GET_ATK"}, {0x802AA11C, "MOVE"},
    {0x802AA288, "DASH"}, {0x802AA4EC, "ATK_HAMMER"}, {0x802AA620, "ATK_SHOOT"}, {0x802AA8D4, "ATK_MOVE"}, {0x802AAA00, "ATK_BOMB"},
    {0x802AAB90, "ATK_LASER"}, {0x802AAD64, "ATK_JUMP"}, {0x802AB2E0, "START"}, {0x802AB3E8, "BELOW_HALF"}, {0x802AB5D8, "DEATH"},
    {0x802A9D88, "EDIT"}};   // unit list from the egg_breaker.cpp
const Kit kKits[] = {{"HeavyDog0410", kHeavyDog, 12}, {"BlackDoom", kBlackDoom, 12}, {"HeavyDog0510", kBlueFalcon, 12},
                     {"BlackBull0210", kBB0210, 14}, {"BlackBull0412", kBB0412, 12}, {"EggMeca", kEgg, 15}};
constexpr int kNKits = 6;
uint32_t cov(uint32_t fn) { return g_cov[(fn - 0x80000000u) >> 2]; }
void trace_modes(uint32_t p) {
  static const bool on = getenv("BOSS_MODE_TRACE") != nullptr;
  if (!on) return;
  static uint32_t last[kNKits][16];
  static int active = -1;
  if (getenv("BOSS_POS_SCAN")) {   // position research: float triplets near the record's x / z round P (2 s and 15 s in)
    static int scans = 0;
    static uint64_t t0 = sys_time_ns();
    const float sx = getenv("BOSS_POS_SCAN") ? float(atof(getenv("BOSS_POS_SCAN"))) : 0.f;
    if ((scans == 0 && sys_time_ns() - t0 > 2000000000ull) || (scans == 1 && sys_time_ns() - t0 > 15000000000ull)) {
      ++scans;
      (void)sx;
      const uint32_t q = ld32(p + 0x1C);
      for (uint32_t base : {p, q}) {
        if (!ram(base)) continue;
        for (uint32_t a = base - 0x1000; a < base + 0x1000; a += 4) {
          const float x = float(ldf32(a)), y = float(ldf32(a + 4)), z = float(ldf32(a + 8));
          if (std::fabs(x) < 3000 && std::fabs(z) < 3000 && y > 10 && y < 600 && std::fabs(x) > 5 && std::fabs(z) > 5)
            logf("[boss-pos] scan %d %s%+d (%.1f, %.1f, %.1f)\n", scans, base == p ? "P" : "Q", int(a - base), x, y, z);
        }
      }
    }
  }
  {   // position research: Egg Breaker's position is [VarParam+0x1C] (#127) - the same for these classes?
    static uint64_t next_pos = 0;
    if (sys_time_ns() > next_pos) {
      next_pos = sys_time_ns() + 2000000000ull;
      const uint32_t q = ld32(p + 0x1C);
      if (ram(q)) logf("[boss-mode] [P+0x1C]=%08X -> (%.1f, %.1f, %.1f)  P+0x1C+0x10 (%.1f, %.1f, %.1f)\n", q, ldf32(q), ldf32(q + 4), ldf32(q + 8),
                       ldf32(q + 0x10), ldf32(q + 0x14), ldf32(q + 0x18));
    }
  }
  // BOSS_MODE_FORCE=<from>,<to>: every 4 s write the next mode id into the REQUEST word P+0x14 (P+0x0C = current mode,
  // P+0x10 = previous; writing +0x0C does nothing) so the tracer names the unit for ids the boss never chose by itself.
  // BOSS_MODE_HP0=1: HP 0 with it (death / destroy tests)
  if (const char* f = getenv("BOSS_MODE_FORCE")) {
    static int k = atoi(f), to = strchr(f, ',') ? atoi(strchr(f, ',') + 1) : 15;
    static uint64_t next = sys_time_ns() + 8000000000ull;
    if (k <= to && sys_time_ns() > next) {
      next = sys_time_ns() + 4000000000ull;
      static const uint32_t off = getenv("BOSS_MODE_OFF") ? (uint32_t)strtoul(getenv("BOSS_MODE_OFF"), nullptr, 0) : 0x14;
      st32(p + off, uint32_t(k));
      if (getenv("BOSS_MODE_HP0")) stf32(p + 0x24, 0.f);
      logf("[boss-mode] force mode id %d\n", k);
      ++k;
    }
  }
  static uint32_t snap[0x80];
  int now_unit = -1;
  for (int k = 0; k < kNKits; ++k)
    for (int i = 0; i < kKits[k].n; ++i) {
      const uint32_t now = cov(kKits[k].units[i].fn);
      if (now != last[k][i]) { last[k][i] = now; now_unit = k * 100 + i; }
    }
  if (now_unit < 0 || now_unit == active) return;
  // the mode unit changed: which small words of P (and of the object P points back to) changed with it
  std::string diff;
  for (uint32_t o = 0; o < 0x200; o += 4) {
    const uint32_t w = ld32(p + o);
    if (w != snap[o / 4] && w < 64 && snap[o / 4] < 64) { char b[40]; snprintf(b, sizeof b, " +%03X %u->%u", o, snap[o / 4], w); diff += b; }
    snap[o / 4] = w;
  }
  const Kit& kit = kKits[now_unit / 100];
  logf("[boss-mode] %s -> %s  (P %08X)%s\n", kit.name, kit.units[now_unit % 100].name, p, diff.c_str());
  active = now_unit;
}

// ---- #126 boss AI framework: the boss's own kit (its mode units: animation, projectiles, damage, sound) driven by our
// decisions. A boss class gets a "brain" keyed by its VarParam vtable; each frame (from its gauge unit) the brain reads
// the current mode (P+0x0C) and the boss's own request (P+0x14) and may replace the request or move the boss.
// Phases by HP: 1 (100-67 %), 2 (67-34 %), 3 (< 34 %) - docs/bosses/BOSS-REWORK.md. BOSS_NO_REWORK=1: vanilla.
struct Brain {
  uint32_t p = 0;
  float home[3] = {};
  int ring = 0, pat = 0, phase = 1;
  uint32_t cur = 0xFFFFFFFFu;
  uint64_t t_mode = 0, t_last = 0;
  bool picked = false, hp_set = false;
  uint64_t t_storm = 0;   // Black Doom: last meteor storm
  int storm = 0;          // meteors left in the current storm
  float orbit = 0;   // Heavy Dog: angle on his strafing circle
  bool aim_set = false;
  float spread[2] = {};
};
std::vector<Brain> g_brains;
float arena_r() { static const float r = getenv("BOSS_ARENA_R") ? float(atof(getenv("BOSS_ARENA_R"))) : 600.f; return r; }
bool brain_log() { static const bool l = getenv("BOSS_BRAIN_LOG") != nullptr; return l; }
void set_pos(uint32_t p, const float v[3]) {
  const uint32_t q = ld32(p + 0x1C);
  if (!ram(q)) return;
  for (int i = 0; i < 3; ++i) stf32(q + i * 4, v[i]);
}
// Black Doom "Doom Gate" (BlackDoomVarParam 8054E35C). Ids: 2 WAIT, 3 GET_COMBO, 4 GET_ATK, 5 ATK_READY, 6 ATK_DISK,
// 7 ATK_METEOR, 8 ATK_BOOMERANG, 9 START (his appear), 10 BELOW_HALF, 11 DEATH. Position [P+0x1C].
// His own GET_COMBO -> START cycle is a teleport through the helper points his create builds only in 610 / 616 / 617;
// in another stage it does nothing useful. The brain replaces it: GET_COMBO -> a ring point of his arena
// (6 points at 0.7 R round where he was placed, heights +90 / +340), straight into WAIT. READY's attack choice follows the
// phase pattern; WAIT gets shorter each phase.
void bd_teleport(Brain& b, uint32_t p) {
  const uint64_t now = sys_time_ns();
  if (b.storm > 0) { st32(p + 0x14, 5); return; }   // meteor storm: no teleport between its meteors, READY again
  float v[3];
  if (b.phase == 3 && now - b.t_storm > 20000000000ull) {   // phase 3: a meteor storm from the arena's high centre, 3 in a row
    b.t_storm = now;
    b.storm = 3;
    v[0] = b.home[0]; v[1] = b.home[1] + 600.f; v[2] = b.home[2];
    if (brain_log()) logf("[boss-brain] Black Doom: meteor storm\n");
  } else {
    b.ring = (b.ring + 2 + int(now / 1000000ull % 3)) % 6;
    const float a = b.ring * 1.0471976f, r = 0.7f * arena_r();
    v[0] = b.home[0] + r * std::cos(a); v[1] = b.home[1] + ((b.ring & 1) ? 340.f : 90.f); v[2] = b.home[2] + r * std::sin(a);
  }
  set_pos(p, v);
  // WAIT, not START: START replays his intro (close-up camera + title card) at every teleport
  static const uint32_t appear = getenv("BD_APPEAR") ? uint32_t(atoi(getenv("BD_APPEAR"))) : 2;
  st32(p + 0x14, appear);
  if (brain_log()) logf("[boss-brain] Black Doom %08X phase %d: teleport to point %d (%.0f, %.0f, %.0f)\n", p, b.phase, b.ring, v[0], v[1], v[2]);
}
// READY's attack choice -> the phase pattern (or the storm's meteors). Called from the READY unit hook right after it
// chose (READY decides in its first frame, before the per-frame brain sees it) and from the brain as a fallback.
void bd_pick(Brain& b, uint32_t p, uint32_t req) {
  static const uint32_t pat1[] = {8, 6}, pat2[] = {8, 7, 6}, pat3[] = {7, 6, 8, 7};
  const uint32_t* pat = b.phase == 1 ? pat1 : b.phase == 2 ? pat2 : pat3;
  const int n = b.phase == 1 ? 2 : b.phase == 2 ? 3 : 4;
  uint32_t want;
  if (b.storm > 0) {
    want = 7;
    if (--b.storm == 0) b.t_storm = sys_time_ns();   // the 20 s until the next storm count from the end of this one
  } else {
    want = pat[b.pat++ % n];
  }
  if (want != req) st32(p + 0x14, want);
  b.picked = true;
  if (brain_log()) logf("[boss-brain] Black Doom phase %d: attack %u (his choice %u)\n", b.phase, want, req);
}
void black_doom(Brain& b, uint32_t p) {
  const uint32_t cur = ld32(p + 0x0C), req = ld32(p + 0x14);
  const uint64_t now = sys_time_ns();
  if (cur != b.cur) { b.cur = cur; b.t_mode = now; b.picked = false; }
  const float in_mode = float(now - b.t_mode) * 1e-9f;
  if (cur == 11 || cur == 10) return;   // DEATH / BELOW_HALF: his own
  if (req == 3 || cur == 3 || req == 4 || cur == 4) {   // GET_COMBO / GET_ATK: teleport to a ring point and appear there
    bd_teleport(b, p);
    return;
  }
  if (cur == 5 && !b.picked && (req == 6 || req == 7 || req == 8)) { bd_pick(b, p, req); return; }
  const float wait_s = b.phase == 1 ? 1.2f : b.phase == 2 ? 0.7f : 0.3f;
  if (cur == 2 && in_mode > wait_s && req == 2) st32(p + 0x14, 5);   // shorter WAIT -> READY
}
// Heavy Dog "Siege Engine" (HeavyDog0410VarParam 80549224). Ids: 2 WAIT, 3 CHASE, 4 ESCAPE, 5 GET_ATK, 6 START,
// 7 BELOW_HALF, 8 DESTROY, 9 ATK_ROCKET, 10 ATK_BARRIER, 11 ATK_MINE. Home (placement) P+0xEC;
// [P+0x1C] = world position (#130). Vanilla, outside stage 410 he only ever raised the barrier between CHASE / ESCAPE; the brain gives him his whole kit:
// after each CHASE / ESCAPE leg (or a short WAIT) the next attack of the phase pattern, and shorter legs per phase.
void heavy_dog(Brain& b, uint32_t p) {
  const uint32_t cur = ld32(p + 0x0C), req = ld32(p + 0x14);
  const uint64_t now = sys_time_ns();
  if (cur != b.cur) { b.cur = cur; b.t_mode = now; b.picked = false; }
  const float in_mode = float(now - b.t_mode) * 1e-9f;
  if (cur == 8 || cur == 7 || cur == 6) return;   // DESTROY / BELOW_HALF / START: his own
  // strafe: while CHASE / ESCAPE (and his attacks) he slides round his home on a circle of 0.8 R, faster each phase,
  // 120 above where he was placed; eased toward the circle point so his own motion and the strafe blend
  {
    const float dt = b.t_last ? std::min(float(now - b.t_last) * 1e-9f, 0.1f) : 0.f;
    b.t_last = now;
    static const bool no_strafe = getenv("HD_NO_STRAFE") != nullptr;
    if (!no_strafe && dt > 0) {
      if (b.orbit == 0) b.orbit = 0.001f;
      b.orbit += dt * (b.phase == 1 ? 0.25f : b.phase == 2 ? 0.4f : 0.6f);
      const float r = 0.8f * arena_r();
      // [P+0x1C] = his world position (#130: it was read as relative to the placement); circle round the placement
      const uint32_t q = ld32(p + 0x1C);
      if (!ram(q)) return;
      const float tx = b.home[0] + r * std::cos(b.orbit), ty = 120.f, tz = b.home[2] + r * std::sin(b.orbit);
      const float k = std::min(1.f, dt * 2.5f);
      const float x = float(ldf32(q)), y = float(ldf32(q + 4)), z = float(ldf32(q + 8));
      stf32(q, x + (tx - x) * k); stf32(q + 4, y + (ty - y) * k); stf32(q + 8, z + (tz - z) * k);
      static uint64_t lg = 0;
      if (brain_log() && now - lg > 2000000000ull) { lg = now; logf("[boss-brain] Heavy Dog strafe (%.0f, %.0f, %.0f) -> (%.0f, %.0f, %.0f)\n", x, y, z, tx, ty, tz); }
    }
  }
  static const uint32_t pat1[] = {9, 9, 11}, pat2[] = {9, 10, 9, 11}, pat3[] = {9, 11, 10, 9};
  const uint32_t* pat = b.phase == 1 ? pat1 : b.phase == 2 ? pat2 : pat3;
  const int n = b.phase == 1 ? 3 : 4;
  const float leg = b.phase == 1 ? 3.0f : b.phase == 2 ? 2.0f : 1.2f;    // CHASE / ESCAPE legs
  const float wait = b.phase == 1 ? 1.0f : b.phase == 2 ? 0.6f : 0.3f;   // WAIT
  const bool attack_now = !b.picked && (req == 5 || cur == 5 || (req == 10 && cur != 10) ||
                                        ((cur == 3 || cur == 4) && in_mode > leg) || (cur == 2 && in_mode > wait));
  if (!attack_now) return;
  const uint32_t want = pat[b.pat++ % n];
  st32(p + 0x14, want);
  b.picked = true;
  if (brain_log()) logf("[boss-brain] Heavy Dog phase %d: attack %u (he wanted %u, in mode %u for %.1f s)\n", b.phase, want, req, cur, in_mode);
}
Brain* brain_for(uint32_t p) {
  auto it = std::find_if(g_brains.begin(), g_brains.end(), [&](const Brain& b) { return b.p == p; });
  if (it != g_brains.end()) return &*it;
  if (g_brains.size() > 64) g_brains.erase(g_brains.begin());
  Brain nb; nb.p = p;
  const uint32_t q = ld32(p + 0x1C);
  if (ram(q)) for (int i = 0; i < 3; ++i) nb.home[i] = float(ldf32(q + i * 4));
  if (ld32(p) == 0x80549224u) for (int i = 0; i < 3; ++i) nb.home[i] = float(ldf32(p + 0xEC + i * 4));   // Heavy Dog: home P+0xEC
  g_brains.push_back(nb);
  return &g_brains.back();
}
// ---- generic pattern brain (#126 v3): bosses whose kit is "move legs + attack modes" - Blue Falcon, Black Bull 0210 /
// 0412. The brain picks the attack after each leg / wait from the phase pattern; their own movement stays.
struct Def {
  uint32_t vt; const char* name;
  uint32_t wait, chase, escape, get_atk, get_combo, start, below, death, dizzy;
  uint32_t pat[3][5]; int n[3];
  float leg[3], wait_s[3];
};
// ids measured (BOSS_MODE_TRACE + BOSS_MODE_FORCE, 2026-10-20). 0 = the boss has no such mode.
const Def kDefs[] = {
  // Blue Falcon (HeavyDog0510): 2 WAIT 3 CHASE 4 ESCAPE 5 GET_ATK 6 START 7 BELOW_HALF 8 DESTROY 9 ROCKET 10 BARRIER 11 MINE
  {0x80549AACu, "Blue Falcon", 2, 3, 4, 5, 0, 6, 7, 8, 0,
   {{9, 11, 0}, {9, 10, 9, 11, 0}, {9, 9, 11, 10, 0}}, {2, 4, 4}, {3.0f, 2.0f, 1.2f}, {1.0f, 0.6f, 0.3f}},
  // Black Bull 0210: 2 WAIT 3 CHASE 4 ESCAPE 5 DIZZY 6 GET_COMBO 7 GET_ATK 8 BELOW_HALF 9 DEATH 10 BREATH 11 WAVERING 13 CLOSE 14 START
  {0x805474CCu, "Black Bull", 2, 3, 4, 7, 6, 14, 8, 9, 5,
   {{10, 13, 0}, {10, 11, 13, 0}, {11, 10, 13, 11, 0}}, {2, 3, 4}, {2.5f, 1.8f, 1.0f}, {1.0f, 0.6f, 0.3f}},
  // Black Bull 0412 (Death Ruins): same ids where it has the unit; no CHASE unit
  {0x805484B8u, "Black Bull", 2, 0, 4, 7, 6, 14, 8, 9, 0,
   {{10, 13, 0}, {10, 11, 13, 0}, {11, 10, 13, 11, 0}}, {2, 3, 4}, {2.5f, 1.8f, 1.0f}, {1.0f, 0.6f, 0.3f}},
  // Egg Breaker (EggMeca 511 / 411 / 310, v3 measured): 2 WAIT 3 GET_COMBO 4 GET_ATK 5 MOVE (-> SHOOT) 6 DASH (-> JUMP)
  // 8 HAMMER 9 SHOOT 10 ATK_MOVE 11 BOMB 12 LASER 13 JUMP 14 START 15 BELOW_HALF 16 DEATH. Vanilla with a still player:
  // laser after laser. Hard: phase 1 laser / shoot / hammer, phase 2 adds bombs and the jump, phase 3 the charge too.
  {0x80559870u, "Egg Breaker", 2, 5, 0, 4, 3, 14, 15, 16, 0,
   {{12, 9, 8, 0}, {11, 12, 13, 9, 0}, {13, 11, 10, 12, 9}}, {3, 4, 5}, {3.0f, 2.0f, 1.2f}, {0.9f, 0.6f, 0.35f}},
  {0x805599B0u, "Egg Breaker", 2, 5, 0, 4, 3, 14, 15, 16, 0,
   {{12, 9, 8, 0}, {11, 12, 13, 9, 0}, {13, 11, 10, 12, 9}}, {3, 4, 5}, {3.0f, 2.0f, 1.2f}, {0.9f, 0.6f, 0.35f}},
  {0x80559AD8u, "Egg Breaker", 2, 5, 0, 4, 3, 14, 15, 16, 0,
   {{12, 9, 8, 0}, {11, 12, 13, 9, 0}, {13, 11, 10, 12, 9}}, {3, 4, 5}, {3.0f, 2.0f, 1.2f}, {0.9f, 0.6f, 0.35f}},
};
const Def* def_for(uint32_t vt) { for (const Def& d : kDefs) if (d.vt == vt) return &d; return nullptr; }
void pattern_brain(Brain& b, uint32_t p, const Def& d) {
  const uint32_t cur = ld32(p + 0x0C), req = ld32(p + 0x14);
  const uint64_t now = sys_time_ns();
  if (cur != b.cur) { b.cur = cur; b.t_mode = now; b.picked = false; }
  const float in_mode = float(now - b.t_mode) * 1e-9f;
  if (cur == d.death || cur == d.below || cur == d.start || (d.dizzy && cur == d.dizzy)) return;   // their own (DIZZY = punish window)
  const int ph = b.phase - 1;
  const bool decision = (d.get_atk && (req == d.get_atk || cur == d.get_atk)) || (d.get_combo && (req == d.get_combo || cur == d.get_combo));
  const bool leg_done = ((d.chase && cur == d.chase) || (d.escape && cur == d.escape)) && in_mode > d.leg[ph];
  const bool wait_done = cur == d.wait && in_mode > d.wait_s[ph];
  if (b.picked || !(decision || leg_done || wait_done)) return;
  const uint32_t want = d.pat[ph][b.pat++ % d.n[ph]];
  st32(p + 0x14, want);
  b.picked = true;
  if (brain_log()) logf("[boss-brain] %s phase %d: attack %u (in mode %u for %.1f s)\n", d.name, b.phase, want, cur, in_mode);
}

// ---- predictive aiming (#126 v3): while a ranged attack runs, the boss's copies of Shadow's position (measured per
// class) are moved to where he will be: position + velocity x lead (0.4 / 0.6 / 0.8 s by phase), plus a small random
// spread (up to 40 units) so it stays fair. BOSS_NO_AIM=1: off.
struct Aim { uint32_t vt; uint32_t off[3]; uint32_t modes[4]; };
const Aim kAims[] = {
  {0x80549224u, {0x17C, 0x188, 0x194}, {9, 11, 0, 0}},    // Heavy Dog: rockets, mines
  {0x80549AACu, {0x17C, 0x188, 0x194}, {9, 11, 0, 0}},    // Blue Falcon
  {0x8054E35Cu, {0x124, 0x130, 0x13C}, {6, 7, 8, 0}},     // Black Doom: disk, meteor, boomerang
  {0x805474CCu, {0x138, 0x144, 0x150}, {10, 11, 0, 0}},   // Black Bull 0210: breath, wavering
  {0x805484B8u, {0x138, 0x144, 0x150}, {10, 11, 0, 0}},   // Black Bull 0412 (offsets as 0210, assumed)
  {0x80559870u, {0x1CC, 0x1D8, 0x1E4}, {9, 11, 12, 0}},   // Egg Breaker: shoot, bomb, laser (copies measured, v3)
  {0x805599B0u, {0x1CC, 0x1D8, 0x1E4}, {9, 11, 12, 0}},
  {0x80559AD8u, {0x1CC, 0x1D8, 0x1E4}, {9, 11, 12, 0}},
};
void aim(Brain& b, uint32_t p) {
  static const bool off = getenv("BOSS_NO_AIM") != nullptr;
  if (off || !ram(g_plr_owner)) return;
  const Aim* a = nullptr;
  for (const Aim& x : kAims) if (x.vt == ld32(p)) a = &x;
  if (!a) return;
  const uint32_t cur = ld32(p + 0x0C);
  bool ranged = false;
  for (uint32_t m : a->modes) if (m && m == cur) ranged = true;
  if (!ranged) { b.aim_set = false; return; }
  const uint32_t o = g_plr_owner;
  if (!b.aim_set) {   // one spread per attack
    b.aim_set = true;
    const uint64_t r = sys_time_ns() * 6364136223846793005ull + p;
    b.spread[0] = float(int((r >> 33) % 81) - 40); b.spread[1] = float(int((r >> 45) % 81) - 40);
  }
  const float lead = b.phase == 1 ? 0.4f : b.phase == 2 ? 0.6f : 0.8f;
  float t[3];
  for (int i = 0; i < 3; ++i) t[i] = float(ldf32(o + i * 4)) + float(ldf32(o + 0x18 + i * 4)) * lead;
  t[0] += b.spread[0]; t[2] += b.spread[1];
  for (uint32_t off_ : a->off) for (int i = 0; i < 3; ++i) stf32(p + off_ + i * 4, t[i]);
  static const bool alog = getenv("BOSS_AIM_LOG") != nullptr;
  static uint64_t anext = 0;
  if (alog && sys_time_ns() > anext) {   // once a second while a ranged attack aims
    anext = sys_time_ns() + 1000000000ull;
    logf("[boss-aim] mode %u: player (%.0f, %.0f) moving (%.0f, %.0f) u/s -> aimed at (%.0f, %.0f): lead %.1f s, spread (%.0f, %.0f)\n", cur,
         ldf32(o), ldf32(o + 8), ldf32(o + 0x18), ldf32(o + 0x20), t[0], t[2], lead, b.spread[0], b.spread[1]);
  }
}
// research (BOSS_TARGET_SCAN=1): words near P / the boss's [P+0x1C] block that hold Shadow's position (the copy the
// boss aims at): every 2 s, triplets within 2 units of the player's live position
void target_scan(uint32_t p) {
  static const bool on = getenv("BOSS_TARGET_SCAN") != nullptr;
  if (!on) return;
  static uint64_t next = 0;
  if (sys_time_ns() < next) return;
  next = sys_time_ns() + 2000000000ull;

  const uint32_t o = g_plr_owner;
  if (!ram(o)) return;
  const float px = float(ldf32(o)), py = float(ldf32(o + 4)), pz = float(ldf32(o + 8));
  const uint32_t q = ld32(p + 0x1C);
  std::string hits;
  for (uint32_t base : {p, q}) {
    if (!ram(base)) continue;
    for (uint32_t a = base - 0x800; a < base + 0x800; a += 4) {
      if (std::fabs(float(ldf32(a)) - px) < 2 && std::fabs(float(ldf32(a + 4)) - py) < 30 && std::fabs(float(ldf32(a + 8)) - pz) < 2) {
        char b[32]; snprintf(b, sizeof b, " %s%+d", base == p ? "P" : "Q", int(a - base)); hits += b;
      }
    }
  }
  logf("[boss-target] %08X player (%.0f, %.0f, %.0f):%s\n", ld32(p), px, py, pz, hits.c_str());
}
// ---- #130 speed probe (BOSS_SPEED_LOG=1): every 10 s per boss: gauge updates per second (= its game frames), path
// length per second ([P+0x1C] position), mode changes and attack starts per minute. Custom stage vs own stage, 60 vs 165.
struct Speed { uint32_t p = 0; float last[3] = {}; bool has = false; double path = 0; int calls = 0, modes = 0, atks = 0; uint32_t cur = ~0u; uint64_t t0 = 0;
               double seg_path = 0, win_path = 0, seg_peak = 0; uint64_t seg_t0 = 0, win_t0 = 0; };
std::vector<Speed> g_speed;
bool is_attack(uint32_t vt, uint32_t m) {
  switch (vt) {
    case 0x80549224u: case 0x80549AACu: return m >= 9 && m <= 11;   // Heavy Dog / Blue Falcon: rocket, barrier, mine
    case 0x8054E35Cu: return m >= 6 && m <= 8;                      // Black Doom: disk, meteor, boomerang
    case 0x805474CCu: case 0x805484B8u: return m == 10 || m == 11 || m == 13;   // Black Bull: breath, wavering, close
    case 0x80559870u: case 0x805599B0u: case 0x80559AD8u: return m >= 8 && m <= 13;   // Egg Breaker: hammer .. jump
    default: return false;
  }
}
void speed_probe(uint32_t p) {
  static const bool on = getenv("BOSS_SPEED_LOG") != nullptr;
  if (!on) return;
  Speed* s = nullptr;
  for (auto& x : g_speed) if (x.p == p) s = &x;
  if (!s) { g_speed.push_back({}); s = &g_speed.back(); s->p = p; s->t0 = sys_time_ns(); }
  const uint32_t q = ld32(p + 0x1C);
  if (ram(q)) {
    const float v[3] = {float(ldf32(q)), float(ldf32(q + 4)), float(ldf32(q + 8))};
    if (s->has) {
      const float d = std::sqrt((v[0] - s->last[0]) * (v[0] - s->last[0]) + (v[1] - s->last[1]) * (v[1] - s->last[1]) + (v[2] - s->last[2]) * (v[2] - s->last[2]));
      if (d < 500.f) { s->path += d; s->seg_path += d; s->win_path += d; }   // a teleport is not movement
    }
    for (int i = 0; i < 3; ++i) s->last[i] = v[i];
    s->has = true;
  }
  ++s->calls;
  {   // per-mode legs: peak speed over 0.5 s windows
    const uint64_t now = sys_time_ns();
    if (!s->win_t0) s->win_t0 = now;
    if (now - s->win_t0 >= 500000000ull) { s->seg_peak = std::max(s->seg_peak, s->win_path / (double(now - s->win_t0) * 1e-9)); s->win_path = 0; s->win_t0 = now; }
  }
  if (const char* w = getenv("BOSS_WORDS")) {   // research: these P words (floats, hex offsets) once a second
    static uint64_t next = 0;
    if (sys_time_ns() > next) {
      next = sys_time_ns() + 1000000000ull;
      std::string o;
      for (const char* q = w; *q;) {
        char* e; const uint32_t off = (uint32_t)strtoul(q, &e, 16);
        char b[48]; snprintf(b, sizeof b, " +%X=%.1f", off, ldf32(p + off)); o += b;
        q = *e ? e + 1 : e;
      }
      char g[112]; snprintf(g, sizeof g, " g7A0 (%.1f, %.1f, %.1f) g720 (%.1f, %.1f, %.1f)", ldf32(0x805E07A0), ldf32(0x805E07A4),
                            ldf32(0x805E07A8), ldf32(0x805E0720), ldf32(0x805E0724), ldf32(0x805E0728));
      o += g;
      logf("[boss-words] mode %u%s\n", ld32(p + 0x0C), o.c_str());
      if (const char* f = getenv("BOSS_FIND")) {   // research: where x, z (e.g. the placement) sits near P
        const float fx = float(atof(f)), fz = strchr(f, ',') ? float(atof(strchr(f, ',') + 1)) : 0.f;
        std::string h;
        for (uint32_t a = p - 0x400; a < p + 0x1000; a += 4)
          if (std::fabs(float(ldf32(a)) - fx) < 2 && std::fabs(float(ldf32(a + 8)) - fz) < 2) { char b[24]; snprintf(b, sizeof b, " P%+d", int(a - p)); h += b; }
        logf("[boss-find] (%.0f, %.0f):%s\n", fx, fz, h.c_str());
      }
      if (getenv("BOSS_COV_DIFF")) {   // research: boss-range functions that ran in the last second (mode shown)
        static std::vector<uint32_t> prev;
        const uint32_t ranges[][2] = {{0x8024D000u, 0x80254800u}, {0x8026B000u, 0x8026E000u}, {0x80290000u, 0x80293000u}};
        std::string h;
        size_t k = 0;
        for (auto& r : ranges)
          for (uint32_t a = r[0]; a < r[1]; a += 4, ++k) {
            if (prev.size() <= k) prev.push_back(0);
            const uint32_t n = cov(a);
            if (n != prev[k]) { char b[24]; snprintf(b, sizeof b, " %08X+%u", a, n - prev[k]); h += b; prev[k] = n; }
          }
        logf("[boss-covdiff] mode %u:%s\n", ld32(p + 0x0C), h.c_str());
      }
      if (const char* f = getenv("BOSS_COVS")) {   // research: call counts of these functions (hex, comma separated)
        std::string h;
        for (const char* q = f; *q;) {
          char* e; const uint32_t fn = (uint32_t)strtoul(q, &e, 16);
          char b[32]; snprintf(b, sizeof b, " %08X=%u", fn, cov(fn)); h += b;
          q = *e ? e + 1 : e;
        }
        logf("[boss-covs]%s\n", h.c_str());
      }
      if (const char* f = getenv("BOSS_FINDV")) {   // research: single floats of this value near P (and in the boss object)
        const float v = float(atof(f));
        std::string h;
        for (uint32_t a = p - 0x400; a < p + 0x800; a += 4)
          if (std::fabs(float(ldf32(a)) - v) < 0.01f) { char b[24]; snprintf(b, sizeof b, " P%+d", int(a - p)); h += b; }
        logf("[boss-findv] %.1f:%s\n", v, h.c_str());
      }
    }
  }
  const uint32_t cur = ld32(p + 0x0C);
  if (cur != s->cur) {
    const uint64_t now = sys_time_ns();
    if (s->cur != ~0u) {
      ++s->modes; if (is_attack(ld32(p), cur)) ++s->atks;
      const double secs = s->seg_t0 ? double(now - s->seg_t0) * 1e-9 : 0;
      if (secs > 0.2) logf("[boss-leg] %s mode %u: %.1f s, avg %.0f u/s, peak %.0f u/s\n", boss_name(p), s->cur, secs, s->seg_path / secs, s->seg_peak);
    }
    s->cur = cur; s->seg_t0 = now; s->seg_path = 0; s->seg_peak = 0;
  }
  const double dt = double(sys_time_ns() - s->t0) * 1e-9;
  if (dt < 10.0) return;
  logf("[boss-speed] %s P %08X: %.0f updates/s, path %.0f u/s, %.1f mode changes/min, %.1f attacks/min (stage %d, rate %d)\n",
       boss_name(p), p, s->calls / dt, s->path / dt, s->modes * 60 / dt, s->atks * 60 / dt, (int)(int32_t)ld32(0x8057D748),
       (int)ld32(0x8057D6A0 + 0x50));
  s->calls = s->modes = s->atks = 0; s->path = 0; s->t0 = sys_time_ns();
}
Brain* brain_for(uint32_t p);
void bd_teleport(Brain& b, uint32_t p);
// vanilla AI outside the boss's stage (#130): only its stage-bound parts are replaced
bool bf_centre(uint32_t p, float& cx, float& cz);
// Blue Falcon's START swoop is written as a path round the world origin (510's ring centre) by something other than its
// START / Move units (measured: neither changes the position). Each frame of START the path point is moved to its ring
// centre: when the position is not the one written here last frame, the path wrote a new point.
void bf_start_fix(uint32_t p) {
  struct W { uint32_t p; float x, z; bool valid; };
  static std::vector<W> ws;
  if (ld32(p) != 0x80549AACu) return;
  auto it = std::find_if(ws.begin(), ws.end(), [&](const W& w) { return w.p == p; });
  if (it == ws.end()) { if (ws.size() > 16) ws.erase(ws.begin()); ws.push_back({p, 0, 0, false}); it = ws.end() - 1; }
  float cx, cz;
  if (ld32(p + 0x0C) != 6 || !bf_centre(p, cx, cz)) { it->valid = false; return; }
  const uint32_t q = ld32(p + 0x1C);
  const float x = float(ldf32(q)), z = float(ldf32(q + 8));
  if (it->valid && x == it->x && z == it->z) return;
  it->x = x + cx; it->z = z + cz; it->valid = true;
  stf32(q, it->x); stf32(q + 8, it->z);
}
// Its START is the intro swoop: a path round the world origin (510's ring centre) written by the generic motion /
// object update (not its START or Move units; measured), ending ~280 out there - in another stage that is wherever the
// world origin is, then a fast run back (#130: first CHASE 325 u/s, peak 490). Outside 510 with the intro camera
// skipped (boss_intro_camera 0) START ends at once, as Egg Breaker's does, and it starts on its ring at the point
// nearest its placement.
void bf_skip_start(uint32_t p) {
  if (ld32(p) != 0x80549AACu || ld32(p + 0x0C) != 6 || settings::g.boss_intro_camera || getenv("BF_KEEP_START")) return;
  float cx, cz;
  if (!bf_centre(p, cx, cz)) return;
  const float r = float(ldf32(p + 0x1F4));
  float dx = float(ldf32(p + 0xEC)) - cx, dz = float(ldf32(p + 0xF4)) - cz;
  const float d = std::sqrt(dx * dx + dz * dz);
  if (d > 1.f) { dx /= d; dz /= d; } else { dx = 0; dz = -1; }
  const uint32_t q = ld32(p + 0x1C);
  stf32(q, cx + dx * r); stf32(q + 8, cz + dz * r);
  st32(p + 0x14, 2);   // WAIT
  static bool said = false;
  if (!said) { said = true; logf("[boss-ai] Blue Falcon START skipped: on its ring at (%.0f, %.0f)\n", cx + dx * r, cz + dz * r); }
}
void vanilla_fix(uint32_t p) {
  bf_skip_start(p);
  if (getenv("BF_START_FIX")) bf_start_fix(p);   // research: accumulates (the path does not rewrite both axes each frame)
  if (rework() || !env_fix()) return;
  if (ld32(p) == 0x8054E35Cu && ld32(p + 0x14) == 3)   // Black Doom's GET_COMBO: teleport round his placement, not the stage's points
    if (Brain* b = brain_for(p)) bd_teleport(*b, p);
}
void brain_step(uint32_t p) {
  target_scan(p);
  static const bool off = getenv("BOSS_NO_REWORK") != nullptr;
  if (off || !settings::g.boss_rework) return;
  const uint32_t vt = ld32(p);
  const Def* d = def_for(vt);
  if (vt != 0x8054E35Cu && vt != 0x80549224u && !d) return;   // brains: Black Doom, Heavy Dog, Blue Falcon, Black Bull
  Brain* it = brain_for(p);
  if (!it) return;
  if (const char* f = getenv("BOSS_HP_FRAC")) {   // tests of the later phases: the boss starts at this share of its HP
    if (!it->hp_set && hpmax(p) > 0) { it->hp_set = true; stf32(p + 0x24, hpmax(p) * float(atof(f))); }
  }
  const float r = hpmax(p) > 0 ? hp(p) / hpmax(p) : 1.f;
  const int ph = r > 0.67f ? 1 : r > 0.34f ? 2 : 3;
  if (ph != it->phase) {
    if (brain_log()) logf("[boss-brain] P %08X phase %d -> %d\n", p, it->phase, ph);
    // a new phase opens with the boss's own "below half" move (Heavy Dog 7, Black Doom 10): his tell that it got harder
    if (ph > it->phase && r > 0) st32(p + 0x14, d ? d->below : vt == 0x8054E35Cu ? 10u : 7u);
  }
  it->phase = ph;
  if (vt == 0x8054E35Cu) black_doom(*it, p);
  if (vt == 0x80549224u) heavy_dog(*it, p);
  if (d) pattern_brain(*it, p, *d);
  aim(*it, p);
}
}  // namespace

namespace {
// ---- Black Doom's decoys (#126 v3): extra Black Dooms placed in the stage. Phase 1 (the real one above 67 % HP): parked
// high above the arena, out of the fight. From phase 2 they fight like him (teleports, attacks: their own brain) but cannot
// be hurt (HP held at max), are not on the HP bar, not in the clear rule and never own the camera. When the real one
// falls they fall with him (HP 0 + DEATH).
Brain* brain_for(uint32_t p);
void black_doom(Brain& b, uint32_t p);
void decoy_step(Boss& d) {
  const uint32_t p = d.p;
  const Boss* real = nullptr;
  for (const Boss& o : g_boss) if (!o.decoy && ram(o.p) && ld32(o.p) == 0x8054E35Cu && sys_time_ns() - o.seen < 3000000000ull) real = &o;
  if (!real || hp(real->p) <= 0) {   // the real one is gone: the decoys go with him
    if (!d.killed) { d.killed = true; stf32(p + 0x24, 0.f); st32(p + 0x14, 11); logf("[boss-ai] decoy P %08X falls with the real Black Doom\n", p); }
    return;
  }
  stf32(p + 0x24, hpmax(p));   // cannot be hurt
  Brain* b = brain_for(p);
  if (!b) return;
  const float r = hpmax(real->p) > 0 ? hp(real->p) / hpmax(real->p) : 1.f;
  b->phase = r > 0.67f ? 1 : r > 0.34f ? 2 : 3;
  if (b->phase == 1) {   // parked out of the fight
    const float v[3] = {b->home[0], b->home[1] + 2500.f, b->home[2]};
    set_pos(p, v);
    if (ld32(p + 0x0C) != 2) st32(p + 0x14, 2);
    return;
  }
  if (getenv("BOSS_NO_REWORK") || !settings::g.boss_rework) return;
  black_doom(*b, p);
}
}  // namespace

void hle_BossDispGauge(Cpu& c) {
  stage_check();
  const uint32_t unit = c.r[3];
  const uint32_t ctx = ram(unit) ? ld32(unit) : 0, p = ram(ctx) ? ld32(ctx + 8) : 0;
  if (!ram(p)) { f_80290FC8(c); return; }
  auto it = std::find_if(g_boss.begin(), g_boss.end(), [&](const Boss& b) { return b.p == p; });
  if (it == g_boss.end()) {
    Boss nb{p, 0, false};
    // #126 decoys: several Black Dooms in one stage = the first is the real one, the others are his decoys
    if (ld32(p) == 0x8054E35Cu)
      for (const Boss& o : g_boss) if (o.p != p && ld32(o.p) == 0x8054E35Cu && !o.decoy && ram(o.p)) { nb.decoy = true; g_had_decoys = true; }
    g_boss.push_back(nb);
    it = g_boss.end() - 1;
    if (it->decoy) logf("[boss-ai] Black Doom P %08X is a decoy of the first one\n", p);
    if (log_on()) logf("[boss-ai] boss %zu: P %08X HP %.0f / %.0f\n", g_boss.size() - 1, p, double(hp(p)), double(hpmax(p)));
  }
  it->seen = sys_time_ns();
  if (it->decoy) { decoy_step(*it); c.r[3] = 0; return; }
  trace_modes(p);
  speed_probe(p);
  vanilla_fix(p);
  brain_step(p);
  if (log_on()) {   // every HP change (hits)
    static std::vector<std::pair<uint32_t, float>> last;
    auto l = std::find_if(last.begin(), last.end(), [&](const auto& e) { return e.first == p; });
    if (l == last.end()) { last.push_back({p, hp(p)}); }
    else if (l->second != hp(p)) { logf("[boss-ai] P %08X HP %.1f -> %.1f\n", p, double(l->second), double(hp(p))); l->second = hp(p); }
  }
  test_kill(c);
  const auto bosses = live_bosses();
  if (bosses.size() < 2) { f_80290FC8(c); return; }
  float sum = 0, sum_max = 0;
  for (Boss* b : bosses) { sum += std::max(hp(b->p), 0.f); sum_max += std::max(hpmax(b->p), 0.f); }
  c.f[1] = c.ps1[1] = sum_max > 0 ? double(sum / sum_max) : 0.0;
  if (log_on()) {
    static uint64_t last = 0;
    if (sys_time_ns() - last > 2000000000ull) { last = sys_time_ns(); logf("[boss-ai] gauge: %zu bosses, HP %.0f / %.0f = %.3f\n", bosses.size(), double(sum), double(sum_max), c.f[1]); }
  }
  const uint32_t lr = c.lr;
  c.lr = 0x80290FECu;
  f_800BE2DC(c);
  c.lr = lr;
  c.r[3] = 0;
}

namespace { void hle_stage_action(Cpu& c) {
  const uint32_t action = c.r[4], from = c.lr;
  if (ram(c.r[3])) g_mgr = c.r[3];
  static const bool all_actions = getenv("BOSS_ALL_ACTIONS") != nullptr;   // research: every caller
  if (log_on() && (all_actions || (from >= 0x80230000u && from < 0x80320000u))) {
    static int n = 0;
    if (n++ < 60) logf("[boss-ai] stage action %u (r5 %u) from %08X\n", action, c.r[5], from);
  }
  if (action == 6 && from >= 0x80230000u && from < 0x80320000u) {
    stage_check();
    const auto bosses = live_bosses();
    int alive = 0;
    for (Boss* b : bosses) alive += hp(b->p) > 0.f;
    if ((bosses.size() >= 2 || g_had_decoys) && alive > 0) {
      static uint64_t last = 0;
      if (sys_time_ns() - last > 1000000000ull) {
        last = sys_time_ns();
        logf("[boss-ai] a boss is down (call from %08X): %d of %zu still standing - no clear yet\n", from, alive, bosses.size());
      }
      c.r[3] = 0;
      return;
    }
    if (bosses.size() >= 2 || g_had_decoys) logf("[boss-ai] last boss down (call from %08X): stage clear\n", from);
  }
  f_80175EEC(c);
}
}  // namespace
void hle_BossStageAction(Cpu& c) { hle_stage_action(c); }

// BossCtrlUnit_CheckEvent (fn_80290DF4, unit + 8 = the boss's P): a boss whose event flag P+0x1A is set puts the stage in
// event mode (action 3: Shadow waits, the boss camera / title runs); a boss without it ENDS event mode (action 4). With
// several bosses the others ended every event at once and it restarted next frame - Shadow stood frozen 60-97 % of the
// time. Now a boss without the flag leaves the event alone while another known boss still wants it.
void hle_BossCheckEvent(Cpu& c) {
  const uint32_t unit = c.r[3], p = ram(unit) ? ld32(unit + 8) : 0;
  if (ram(p) && ld8(p + 0x1A) == 0) {
    for (Boss* b : live_bosses())
      if (b->p != p && ld8(b->p + 0x1A) != 0) { c.r[3] = 0; return; }
  }
  f_80290DF4(c);
}

// Black Doom's attack units (BOOMERANG / DISK / METEOR) request GET_COMBO (3) when they finish; entering GET_COMBO for
// one frame restarts a non-610 stage (his stage-bound teleport). Right after the unit, before the mode switch, the
// brain turns that request into its own ring-point teleport.
void f_80286324(Cpu&);   // BlackDoomModeCtrlUnit_ATK_BOOMERANG update
void f_80285B84(Cpu&);   // ..._ATK_DISK
void f_80285EFC(Cpu&);   // ..._ATK_METEOR
namespace {
void bd_after_attack(uint32_t unit) {
  const uint32_t p = ram(unit) ? ld32(unit + 8) : 0;
  if (!ram(p) || ld32(p) != 0x8054E35Cu || !(rework() || env_fix())) return;
  if (ld32(p + 0x14) != 3) return;
  if (Brain* b = brain_for(p)) bd_teleport(*b, p);
}
}  // namespace
void hle_BDBoomerang(Cpu& c) { const uint32_t u = c.r[3]; f_80286324(c); bd_after_attack(u); }
void hle_BDDisk(Cpu& c) { const uint32_t u = c.r[3]; f_80285B84(c); bd_after_attack(u); }
void hle_BDMeteor(Cpu& c) { const uint32_t u = c.r[3]; f_80285EFC(c); bd_after_attack(u); }

// Boss camera lock (#126): each boss's FixLookCharCamera unit asks a yes / no question every frame (vtable slot 0x1C:
// "lock the camera on me now?"); with several bosses they fought over the camera (spinning / flipped views). Refusing
// every lock is not an option: a boss intro leaves the camera parked at its last shot and the lock is what brings it
// back (quiet run: camera stuck at one point for the whole fight). Rule outside a boss's own stage: ONE boss owns the
// camera - the first whose unit asks, until it is gone (3 s without its gauge) - the others are refused.
// boss_camera_lock = 1: every boss as in the original game.
void f_80231CE8(Cpu&);   // BlackBull0210CtrlUnit_FixLookCharCamera +0x1C
void f_8026D608(Cpu&);   // BlackBull0412CtrlUnit_FixLookCharCamera +0x1C
void f_802813F0(Cpu&);   // BlackDoomCtrlUnit_FixLookCharCamera +0x1C
void f_8026B7E4(Cpu&);   // HeavyDog0510CtrlUnit_FixLookCharCamera +0x1C (Blue Falcon)
namespace {
bool own_boss_stage() {
  const int s = (int)(int32_t)ld32(0x8057D748);
  for (int b : {210, 310, 410, 411, 412, 510, 511, 610, 611, 612, 613, 614, 615, 616, 617, 618, 710}) if (s == b) return true;
  return false;
}
uint32_t g_cam_owner = 0;   // P of the boss that owns the camera
void cam_lock(Cpu& c, void (*orig)(Cpu&), const char* who) {
  const uint32_t unit = c.r[3], p = ram(unit) ? ld32(unit + 8) : 0;
  if (settings::g.boss_camera_lock == 1 || own_boss_stage()) { orig(c); return; }
  if (settings::g.boss_camera_lock == 0) {   // #130: no boss keeps the camera
    // A boss intro that still plays (Black Bull's title card) leaves the camera parked on its last shot; only the
    // boss's own lock brings it out (refusing every lock = a parked camera for the whole fight, frame series). So the
    // lock could be let through for its first BOSS_CAM_GRACE s of "yes" (default 0: the intro camera is released at
    // its creation instead, hle_CamCreate below), then refused.
    struct Granted { uint32_t p; double s; uint64_t last; };
    static std::vector<Granted> granted;
    auto it = std::find_if(granted.begin(), granted.end(), [&](const Granted& g) { return g.p == p; });
    if (it == granted.end()) { if (granted.size() > 32) granted.erase(granted.begin()); granted.push_back({p, 0, 0}); it = granted.end() - 1; }
    static const double grace = getenv("BOSS_CAM_GRACE") ? atof(getenv("BOSS_CAM_GRACE")) : 0.0;
    if (it->s < grace) {   // the boss's own answer; its "yes" counts toward the grace
      orig(c);
      const uint64_t now = sys_time_ns();
      if (c.r[3] && it->last) it->s += std::min(0.1, double(now - it->last) * 1e-9);
      it->last = now;
      if (it->s >= grace && getenv("BOSS_CAM_LOG")) logf("[boss-cam] %s (P %08X) lock granted %.1f s, now refused\n", who, p, grace);
      return;
    }
    static bool said = false;
    if (!said && getenv("BOSS_CAM_LOG")) { said = true; logf("[boss-cam] %s asks for the camera: refused (boss_camera_lock 0)\n", who); }
    c.r[3] = 0;
    return;
  }
  for (const Boss& d : g_boss) if (d.decoy && d.p == p) { c.r[3] = 0; return; }   // decoys never own the camera
  bool owner_alive = false;
  for (Boss* b : live_bosses()) if (b->p == g_cam_owner && hp(b->p) > 0) owner_alive = true;
  if (!owner_alive) g_cam_owner = 0;
  if (!g_cam_owner || g_cam_owner == p) {
    orig(c);
    if (c.r[3] && !g_cam_owner && ram(p)) { g_cam_owner = p; logf("[boss-cam] %s (P %08X) owns the camera\n", who, p); }
    return;
  }
  c.r[3] = 0;   // another boss owns it
}
}  // namespace
void hle_BB0210CamLock(Cpu& c) { cam_lock(c, f_80231CE8, "BB0210"); }
void hle_BB0412CamLock(Cpu& c) { cam_lock(c, f_8026D608, "BB0412"); }
void hle_BDCamLock(Cpu& c) { cam_lock(c, f_802813F0, "BD"); }
void hle_BFCamLock(Cpu& c) { cam_lock(c, f_8026B7E4, "BF"); }
void f_802A5AC0(Cpu&);   // EggMecaCtrlUnit_FixLookCharCamera +0x1C (no default branch outside 310 / 411 / 511)
void hle_EggCamLock(Cpu& c) { cam_lock(c, f_802A5AC0, "Egg"); }   // #130: Egg Breaker under the same camera rule

// HP bar label with several bosses (#126): the boss gauge shows the whole fight (sum of HP); above it the name of the
// boss the camera follows (the owner, else the first one) and "+N" for the others. settings_ui.cpp overlay.
namespace {
const char* boss_name(uint32_t p) {
  switch (ld32(p)) {
    case 0x80549224u: return "HEAVY DOG";
    case 0x80549AACu: return "BLUE FALCON";
    case 0x8054E35Cu: return "BLACK DOOM";
    case 0x805474CCu: case 0x805484B8u: return "BLACK BULL";
    case 0x80559870u: case 0x805599B0u: case 0x80559AD8u: return "EGG BREAKER";
    case 0x8055B808u: return "DEVIL DOOM";
    default: return "BOSS";
  }
}
}  // namespace
void boss_hud_draw() {
  if (!game::in_stage() || game::pause_menu_active() || game::cutscene_active()) return;
  const auto bosses = live_bosses();
  int alive = 0;
  uint32_t shown = 0;
  for (Boss* b : bosses) if (hp(b->p) > 0) { ++alive; if (!shown || b->p == g_cam_owner) shown = b->p; }
  if (alive < 2 || !shown) return;
  char text[48];
  snprintf(text, sizeof text, "%s  +%d", boss_name(shown), alive - 1);
  const ImGuiViewport* v = ImGui::GetMainViewport();
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  ImFont* font = ImGui::GetFont();
  const float W = v->Size.x, H = v->Size.y, px = 0.032f * H;
  const ImVec2 sz = font->CalcTextSizeA(px, 1e9f, 0.f, text);
  const ImVec2 at(v->Pos.x + (W - sz.x) * 0.5f, v->Pos.y + 0.818f * H - sz.y * 0.5f);
  for (int dx = -1; dx <= 1; ++dx)
    for (int dy = -1; dy <= 1; ++dy)
      if (dx || dy) dl->AddText(font, px, ImVec2(at.x + dx * 2.f, at.y + dy * 2.f), IM_COL32(0, 0, 0, 220), text);
  dl->AddText(font, px, at, IM_COL32(255, 235, 200, 255), text);
}

// Black Doom's READY (fn_80285750) picks the attack in its first frame: the brain's pattern goes in right after it.
void f_80285750(Cpu&);
namespace {
void bd_after_ready(uint32_t unit) {
  const uint32_t p = ram(unit) ? ld32(unit + 8) : 0;
  if (!ram(p) || ld32(p) != 0x8054E35Cu || getenv("BOSS_NO_REWORK") || !settings::g.boss_rework) return;
  const uint32_t req = ld32(p + 0x14);
  if (req != 6 && req != 7 && req != 8) return;
  Brain* b = brain_for(p);
  if (b && !b->picked) bd_pick(*b, p, req);
}
}  // namespace
void hle_BDReady(Cpu& c) { const uint32_t u = c.r[3]; f_80285750(c); bd_after_ready(u); }

// ---- #130 Blue Falcon re-anchored (vanilla AI). Its arena is a ring round the WORLD ORIGIN (measured, stage 510):
//  - its init fn_80253A10 sets the ring radius VarParam+0x1F4 (= boss+0x22C) to the placement's distance from (0, 0)
//    (510: 240, the player starts at the centre; a placement 800 from the origin = an 800 ring);
//  - HeavyDog0510CtrlUnit_Move fn_8026BF54 keeps it on that ring in polar form (radius +0x1F8, angle +0x110);
//  - the shared HeavyDogCtrlUnit_GetStatus fn_8024D6AC takes its own angle (+0x110) and the player's (+0x1B8) round
//    (0, 0) as atan2(-x, -z) - CHASE closes that angle and attacks within VarParam+0xD0 (300) of the player;
//  - CHASE / ESCAPE aim their bombs round the zero vector 805E07A0.
// In a custom stage it raced to a ring round (0, 0) and fought the arena leash (no attacks after its first ESCAPE).
// Outside 510 all four use a centre of our choosing: the editor arena's centre for Blue Falcon (mods/boss_arena.txt,
// type B7) or else its placement (VarParam+0xEC, measured), and the vanilla 240 ring (0.8 x a smaller arena; BF_RING_R
// = another radius). Move runs with the position shifted by the centre (it reads no player position); GetStatus's two
// angles are taken again round the centre; the zero vector holds the centre during CHASE / ESCAPE only.
// BF_NO_RECENTRE=1: off.
extern "C" __declspec(dllimport) unsigned long __stdcall GetModuleFileNameA(void*, char*, unsigned long);
void f_80252848(Cpu&);   // HeavyDog0510ModeCtrlUnit_CHASE update
void f_80252B7C(Cpu&);   // HeavyDog0510ModeCtrlUnit_ESCAPE update
void f_8026BF54(Cpu&);   // HeavyDog0510CtrlUnit_Move update
void f_8024D6AC(Cpu&);   // HeavyDogCtrlUnit_GetStatus update (Heavy Dog + Blue Falcon)
namespace {
struct BfArena { bool has = false; float cx = 0, cz = 0, r = 0; };
BfArena boss_arena_of(unsigned want) {   // the editor arena of a boss type (B7 Blue Falcon, B6 Heavy Dog), once per stage
  static int stage = -1;
  static BfArena cache[2];
  const int slot = want == 0xB7 ? 0 : 1;
  BfArena& a = cache[slot];
  const int s = (int)(int32_t)ld32(0x8057D748);
  if (s == stage && a.r >= 0) return a;
  if (s != stage) { stage = s; cache[0].r = cache[1].r = -1; }
  a = BfArena{};
  std::string txt;
  if (const char* e = getenv("BOSS_ARENA")) txt = e;
  else {
    char exe[512] = {};
    GetModuleFileNameA(nullptr, exe, sizeof exe);
    std::string path = exe;
    path = path.substr(0, path.find_last_of("\/")) + "\mods\boss_arena.txt";
    if (FILE* f = fopen(path.c_str(), "rb")) { char b[4096]; size_t n = fread(b, 1, sizeof b - 1, f); fclose(f); txt.assign(b, n); }
  }
  for (char& ch : txt) if (ch == ';') ch = '\n';
  size_t at = 0;
  while (at < txt.size()) {
    size_t nl = txt.find('\n', at);
    const std::string line = txt.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
    at = nl == std::string::npos ? txt.size() : nl + 1;
    char shape[16] = {};
    unsigned type = 0;
    float cx, cz, x, y = 0;
    if (sscanf(line.c_str(), "%x %15s %f %f %f %f", &type, shape, &cx, &cz, &x, &y) < 5 || type != want) continue;
    a.has = true; a.cx = cx; a.cz = cz;
    a.r = strcmp(shape, "box") == 0 ? 0.5f * std::min(x, y) : x;
    break;
  }
  if (a.has) logf("[boss-ai] boss type %X centred on its editor arena (%.0f, %.0f) r %.0f\n", want, a.cx, a.cz, a.r);
  return a;
}
// P = Blue Falcon's VarParam outside 510 (else 0); cx / cz = its centre, and the ring radius set
bool bf_centre(uint32_t p, float& cx, float& cz) {
  static const bool off = getenv("BF_NO_RECENTRE") != nullptr;
  if (off || !ram(p) || ld32(p) != 0x80549AACu || !ram(ld32(p + 0x1C)) || !env_fix()) return false;
  // 510's layout (measured): it is placed ON its ring (240 out) with the player near the ring's centre. Elsewhere the
  // centre is the editor arena's (B7), else where the player stands when it is first seen (not 240 toward him: that left
  // him just past the centre, opposite it, so it crossed the ring at once - #130 legs), and its
  // polar state (radius +0x1F8, angle +0x110) starts from the placement - otherwise START left it ~550 out and the first
  // CHASE was a fast run in (#130: 190 vs 50 u/s CHASE legs).
  struct C { uint32_t p; float cx, cz, r; bool started; };
  static std::vector<C> cs;
  static const float ring = getenv("BF_RING_R") ? float(atof(getenv("BF_RING_R"))) : 240.f;
  auto it = std::find_if(cs.begin(), cs.end(), [&](const C& x) { return x.p == p; });
  if (it == cs.end()) {
    const BfArena a = boss_arena_of(0xB7);
    const float hx = float(ldf32(p + 0xEC)), hz = float(ldf32(p + 0xF4));
    C n{p, hx, hz, ring, false};
    if (a.has) {
      n.cx = a.cx; n.cz = a.cz;
      if (a.r > 0) n.r = std::min(ring, 0.8f * a.r);
    } else if (ram(g_plr_owner)) {
      n.cx = float(ldf32(g_plr_owner)); n.cz = float(ldf32(g_plr_owner + 8));   // the player at the centre, as in 510
    }
    if (cs.size() > 16) cs.erase(cs.begin());
    cs.push_back(n);
    it = cs.end() - 1;
    logf("[boss-ai] Blue Falcon P %08X: ring centre (%.0f, %.0f) r %.0f\n", p, n.cx, n.cz, n.r);
  }
  cx = it->cx; cz = it->cz;
  // the ring is the vanilla 240 the whole time, START included, as in 510 (letting it follow the START swoop left it
  // ~670 out: the first CHASE then ran ~400 u/s - speed on the ring = radius x its angular speed)
  stf32(p + 0x1F4, it->r);
  return true;
}
void bf_zero_centred(Cpu& c, void (*orig)(Cpu&)) {
  const uint32_t unit = c.r[3], p = ram(unit) ? ld32(unit + 0xC) : 0;
  float cx, cz;
  if (!bf_centre(p, cx, cz)) { orig(c); return; }
  uint32_t keep[3];
  for (int i = 0; i < 3; ++i) keep[i] = ld32(0x805E07A0u + i * 4);
  stf32(0x805E07A0u, cx); st32(0x805E07A4u, 0); stf32(0x805E07A8u, cz);
  orig(c);
  for (int i = 0; i < 3; ++i) st32(0x805E07A0u + i * 4, keep[i]);
}
}  // namespace
void hle_BFChase(Cpu& c) { bf_zero_centred(c, f_80252848); }
void hle_BFEscape(Cpu& c) { bf_zero_centred(c, f_80252B7C); }
void f_80252F8C(Cpu&);   // HeavyDog0510ModeCtrlUnit_START: its swoop is a path round the world origin (510's centre)
void hle_BFStart(Cpu& c) {
  const uint32_t unit = c.r[3], p = ram(unit) ? ld32(unit + 0xC) : 0;
  float cx, cz;
  (void)p; (void)cx; (void)cz;
  f_80252F8C(c);   // the shift here did nothing (its path is written elsewhere): bf_start_fix in the gauge hook does it
}
void hle_BFMove(Cpu& c) {
  const uint32_t unit = c.r[3], p = ram(unit) ? ld32(unit + 0xC) : 0;
  float cx, cz;
  if (!bf_centre(p, cx, cz)) { f_8026BF54(c); return; }
  const uint32_t q = ld32(p + 0x1C);
  const float bx = float(ldf32(q));
  stf32(q, float(ldf32(q)) - cx); stf32(q + 8, float(ldf32(q + 8)) - cz);
  f_8026BF54(c);
  stf32(q, float(ldf32(q)) + cx); stf32(q + 8, float(ldf32(q + 8)) + cz);
  if (getenv("BF_POS_LOG") && ld32(p + 0x0C) == 6) {
    static uint64_t next = 0;
    if (sys_time_ns() > next) { next = sys_time_ns() + 250000000ull; logf("[bf-pos] Move: x %.1f -> %.1f (flag114 %u)\n", bx, ldf32(q), ld8(p + 0x114)); }
  }
}
// ---- #130 Heavy Dog re-anchored (vanilla AI). His fight is laid out round the WORLD ORIGIN (measured, stage 410: he
// starts at (0, 0, 0) = his placement there, the player at z -330; CHASE steers to lanes x = +-150, ESCAPE to corners
// (+-500, +-500), he stays inside |x|, |z| <= 300). In a custom stage he started at the world origin whatever his
// placement and ran those lanes / corners there. Outside 410 the decisions run in a frame centred on his placement (or
// the editor arena of type B6): his position [VarParam+0x1C], his move target +0xF8 and the copies of the player
// position (+0x17C / +0x188 / +0x194) are shifted by the centre for the length of his movement mode units (WAIT, START,
// CHASE, ESCAPE, GET_ATK, BELOW_HALF) and his Move unit; the attack units (rocket / barrier / mine) run in world space
// so what they spawn appears where it should. He is put on his placement the first time he is seen.
// HD_NO_RECENTRE=1: off.
void f_80250290(Cpu&);   // HeavyDog0410ModeCtrlUnit_WAIT
void f_80250C78(Cpu&);   // ..._START
void f_80250440(Cpu&);   // ..._CHASE
void f_80250614(Cpu&);   // ..._ESCAPE
void f_80250B90(Cpu&);   // ..._GET_ATK
void f_80250D94(Cpu&);   // ..._BELOW_HALF
void f_8026D220(Cpu&);   // HeavyDog0410CtrlUnit_Move
namespace {
std::vector<uint32_t> g_hd_placed;
bool hd_centre(uint32_t p, float& cx, float& cz) {
  static const bool off = getenv("HD_NO_RECENTRE") != nullptr;
  if (off || !ram(p) || ld32(p) != 0x80549224u || !ram(ld32(p + 0x1C)) || !env_fix()) return false;
  const BfArena a = boss_arena_of(0xB6);
  cx = a.has ? a.cx : float(ldf32(p + 0xEC));
  cz = a.has ? a.cz : float(ldf32(p + 0xF4));
  if (std::find(g_hd_placed.begin(), g_hd_placed.end(), p) == g_hd_placed.end()) {   // first sight: onto his centre
    g_hd_placed.push_back(p);
    const uint32_t q = ld32(p + 0x1C);
    stf32(q, float(ldf32(q)) + cx); stf32(q + 8, float(ldf32(q + 8)) + cz);
    logf("[boss-ai] Heavy Dog P %08X re-anchored on (%.0f, %.0f)\n", p, cx, cz);
  }
  return true;
}
void hd_shift(uint32_t p, float cx, float cz) {   // world -> his frame (cx, cz) or back (-cx, -cz)
  const uint32_t q = ld32(p + 0x1C);
  for (uint32_t a : {q, p + 0xF8, p + 0x17C, p + 0x188, p + 0x194}) {
    stf32(a, float(ldf32(a)) - cx);
    stf32(a + 8, float(ldf32(a + 8)) - cz);
  }
}
void hd_framed(Cpu& c, void (*orig)(Cpu&), uint32_t p_off) {
  const uint32_t unit = c.r[3], p = ram(unit) ? ld32(unit + p_off) : 0;
  float cx, cz;
  if (getenv("HD_LOG")) {   // research: which hooks see which P
    static uint64_t next = 0;
    if (sys_time_ns() > next) { next = sys_time_ns() + 1000000000ull; logf("[hd] unit %08X +%X -> %08X vt %08X\n", unit, p_off, p, ram(p) ? ld32(p) : 0); }
  }
  if (!hd_centre(p, cx, cz)) { orig(c); return; }
  hd_shift(p, cx, cz);
  const float in_x = float(ldf32(p + 0xF8));
  orig(c);
  const float out_x = float(ldf32(p + 0xF8));
  hd_shift(p, -cx, -cz);
  if (getenv("HD_LOG") && orig == f_80250440) {
    static uint64_t next = 0;
    if (sys_time_ns() > next) {
      next = sys_time_ns() + 1000000000ull;
      logf("[hd] CHASE target x local in %.1f out %.1f world %.1f, pos x %.1f\n", in_x, out_x, ldf32(p + 0xF8), ldf32(ld32(p + 0x1C)));
    }
  }
}
}  // namespace
void hle_HDWait(Cpu& c) { hd_framed(c, f_80250290, 0xC); }
void hle_HDStart(Cpu& c) { hd_framed(c, f_80250C78, 0xC); }
void hle_HDChase(Cpu& c) { hd_framed(c, f_80250440, 0xC); }
void hle_HDEscape(Cpu& c) { hd_framed(c, f_80250614, 0xC); }
void hle_HDGetAtk(Cpu& c) { hd_framed(c, f_80250B90, 0xC); }
void hle_HDBelowHalf(Cpu& c) { hd_framed(c, f_80250D94, 0xC); }
void hle_HDMove(Cpu& c) { hd_framed(c, f_8026D220, 0xC); }
void hle_HDGetStatus(Cpu& c) {
  const uint32_t unit = c.r[3];
  f_8024D6AC(c);
  const uint32_t p = ram(unit) ? ld32(unit + 8) : 0;
  float cx, cz;
  if (!bf_centre(p, cx, cz) && !hd_centre(p, cx, cz)) return;   // Blue Falcon / Heavy Dog outside their stages
  const uint32_t q = ld32(p + 0x1C);
  const float px = float(ldf32(p + 0x17C)) - cx, py = float(ldf32(p + 0x180)), pz = float(ldf32(p + 0x184)) - cz;
  if (std::sqrt(px * px + py * py + pz * pz) >= 10.f) stf32(p + 0x1B8, std::atan2(-px, -pz));   // 10: its own near-centre test
  stf32(p + 0x110, std::atan2(-(float(ldf32(q)) - cx), -(float(ldf32(q + 8)) - cz)));
}
// #130 the parked camera after a boss intro: BossCtrlUnit_PlayCameraMotion's setup fn_80291E0C creates the intro
// camera (fn_801CED24, call at 80291FE4) and only its destructor fn_802917D4 releases it (fn_801CEAD4 on unit+0x14).
// Outside boss stages the intro is skipped (boss_arena.cpp: its update fn_8029176C answers "done" at once) but the
// unit lives on, so its camera stayed on the camera stack - with no boss lock the view was parked on the intro's
// first shot for the whole fight (Black Bull, Blue Falcon, Black Doom; frame series). There the intro camera is
// released as soon as it is made (same conditions as the skip). CAM_CREATE_LOG=1 logs every camera creation.
void f_801CED24(Cpu&);
void f_801CEAD4(Cpu&);   // camera handle: take it off the camera stack
void hle_CamCreate(Cpu& c) {
  static const bool on = getenv("CAM_CREATE_LOG") != nullptr;
  const uint32_t from = c.lr, handle = c.r[3];
  const bool intro = from == 0x80291FE8u && !own_boss_stage() && !settings::g.boss_intro_camera && !getenv("BOSS_KEEP_INTRO_CAM");
  if (on) logf("[cam-create] from %08X r3 %08X r4 %08X r8 %u\n", c.lr, c.r[3], c.r[4], c.r[8]);
  f_801CED24(c);
  if (intro) {
    const uint32_t r3 = c.r[3];
    Cpu saved = c;
    c.r[3] = handle;
    f_801CEAD4(c);
    c = saved;
    c.r[3] = r3;
    if (on) logf("[cam-create] boss intro camera released (skipped intro)\n");
  }
}

// ---- #125 Diablon in any stage (vanilla AI). Its intro / phase events place the actors from a static table of 611
// world positions (fn_802D513C fills 0x805584D4: 3 entries of 0x34 bytes - rotation +4, position A +0x10, position B
// +0x1C; B = (0, 5, 260) is where Shadow is put, A = (0,5,60) / (-50,5,0) / (50,5,0) the others; measured). In another
// stage Shadow landed under the floor and fell (a restart every ~10 s, quiet run). Outside 611 / 613 / 618 the table is
// re-anchored before each use (fn_802D3934): B = where Shadow stands now, A keeps its offset from B. DIABLON_RAW=1: off.
void f_802D3934(Cpu&);   // Boss::Diablon event: place the actors of table entry r4 (r3 = the table's page)
void hle_DiablonEventPlace(Cpu& c) {
  static float orig[3][6];
  static bool have = false;
  constexpr uint32_t kTab = 0x805584D4u;
  const int s = (int)(int32_t)ld32(0x8057D748);
  const bool own = s == 611 || s == 613 || s == 618;
  if (!own && !getenv("DIABLON_RAW") && ram(g_plr_owner)) {
    if (!have) {
      for (int e = 0; e < 3; ++e) for (int k = 0; k < 6; ++k) orig[e][k] = float(ldf32(kTab + e * 0x34 + 0x10 + k * 4));
      have = true;
    }
    const float px = float(ldf32(g_plr_owner)), py = float(ldf32(g_plr_owner + 4)), pz = float(ldf32(g_plr_owner + 8));
    for (int e = 0; e < 3; ++e) {
      const float* o = orig[e];   // o[0..2] = A, o[3..5] = B
      const float b[3] = {px, py, pz};
      for (int k = 0; k < 3; ++k) {
        stf32(kTab + e * 0x34 + 0x1C + k * 4, b[k]);
        stf32(kTab + e * 0x34 + 0x10 + k * 4, b[k] + (o[k] - o[3 + k]));
      }
    }
    static int said = 0;
    if (said++ < 3) logf("[boss-ai] Diablon event re-anchored on Shadow (%.0f, %.0f, %.0f)\n", px, py, pz);
  }
  f_802D3934(c);
}
// Its Parts::CameraControl (fn_80322680) drives the camera along 611's fixed points: in another stage the view ended
// on the ground for the rest of the fight (frame series). Outside 611 / 613 / 618 with boss_camera_lock 0 (no boss
// takes the camera, #130) the part is skipped and the player camera runs; 1 = original.
void f_80322680(Cpu&);   // Boss::Diablon::Parts::CameraControl update
void hle_DiablonCamera(Cpu& c) {
  const int s = (int)(int32_t)ld32(0x8057D748);
  const bool own = s == 611 || s == 613 || s == 618;
  if (own || settings::g.boss_camera_lock == 1 || getenv("DIABLON_RAW")) { f_80322680(c); return; }
  static bool said = false;
  if (!said) { said = true; logf("[boss-cam] Diablon camera part skipped (boss_camera_lock %d)\n", settings::g.boss_camera_lock); }
  c.r[3] = 0;
}
