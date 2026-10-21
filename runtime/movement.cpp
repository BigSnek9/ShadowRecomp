// Movement tuning, phase 1 (docs/gameplay/MOVEMENT-REWORK.md; constants from agents/claude/research/movement/README.md).
// Shadow only, every key a multiplier (percent, 100 = the game's own value). "Original" (mv_preset 0, or every key at
// 100) takes the untouched path: no hook changes anything and every written constant holds the value read from the game
// at first use, so it is bit-identical.
//   mv_gravity        gravity while Shadow is in the air / on the ground (his own force, not the shared constant)
//   mv_fall_max       terminal fall speed: Shadow's vertical air drag k = original / multiplier (terminal = 320 / k)
//   mv_air_drag       horizontal air drag k (how fast air speed bleeds off), Shadow only
//   mv_jump           jump start velocity and hold force (Shadow's row of the jump table 0x804B0558)
//   mv_homing_bounce  the bounce after a homing hit: start velocity and extra up-force (HomingJump constants)
//   mv_homing_time    how long that extra up-force lasts (0.75 s originally)
//   mv_homing_fall    extra gravity while the HomingJump behaviour runs (the "the fall takes ages" fix)
// Hooks (overrides.txt): fn_800A7700 (gravity add: the executer, player at +0x28), fn_80093728 (the integrator with
// drag: player in r3). The jump row and the HomingJump constants are written live (read every frame by the game).
#include "movement.h"
#include "settings_ui.h"
#include "osk.h"
#include "sys.h"
#include <imgui.h>
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

void f_800A7700(Cpu&);
void f_80093728(Cpu&);

namespace movement {
namespace {
constexpr uint32_t kShadowVt = 0x80521C50;
constexpr uint32_t kHomingJumpVt = 0x805207A0;
constexpr uint32_t kVertDrag = 0x805E4584, kHorizDrag = 0x805E4580;
constexpr uint32_t kJumpTable = 0x804B0558;
constexpr uint32_t kHjVel1 = 0x805F273C, kHjForce1 = 0x805F2740, kHjTime = 0x805F2738, kHjVel0 = 0x805F2744, kHjForce0 = 0x805F2748;

bool shadow(uint32_t p) { return p >= 0x80000000u && p < RT_RAM_END - 0x400 && ld32(p + 0x38) == kShadowVt; }
float f(uint32_t a) { return float(ldf32(a)); }
void put(uint32_t a, float v) { if (ld32(a) != [&] { uint32_t u; memcpy(&u, &v, 4); return u; }()) stf32(a, v); }

struct Mult { float gravity = 1, fall_max = 1, air_drag = 1, jump = 1, bounce = 1, htime = 1, hfall = 1;
              float max_speed = 1, accel = 1, turn = 1, air_control = 1; bool any = false; };
Mult current() {
  Mult m;
  const auto& s = settings::g;
  if (s.mv_preset == 0) return m;                               // Original
  // tuned: the sliders (a preset = a named set of slider values, loaded into them; mv_name says which)
  auto pct = [](int v, int lo, int hi) { return std::clamp(v, lo, hi) / 100.f; };
  m.gravity = pct(s.mv_gravity, 25, 400); m.fall_max = pct(s.mv_fall_max, 25, 400); m.air_drag = pct(s.mv_air_drag, 0, 400);
  m.jump = pct(s.mv_jump, 25, 300); m.bounce = pct(s.mv_homing_bounce, 0, 300); m.htime = pct(s.mv_homing_time, 0, 300);
  m.hfall = pct(s.mv_homing_fall, 25, 400);
  m.max_speed = pct(s.mv_max_speed, 50, 200); m.accel = pct(s.mv_accel, 25, 400); m.turn = pct(s.mv_turn, 25, 300);
  m.air_control = pct(s.mv_air_control, 0, 400);
  m.any = m.gravity != 1 || m.fall_max != 1 || m.air_drag != 1 || m.jump != 1 || m.bounce != 1 || m.htime != 1 || m.hfall != 1 ||
          m.max_speed != 1 || m.accel != 1 || m.turn != 1 || m.air_control != 1;
  return m;
}

struct Orig { bool read = false; float vdrag, hdrag, hjv1, hjf1, hjt, hjv0, hjf0; int jump_id = -1; float jump0, jump4;
              int run_id = -1; float run_mult = 1; } g_o;
// phase 2: the character table row (0x804F1834 + id * 0x2C, +4 = top-speed multiplier: top speed = 750 u/s x it) and the
// constants the Ground run state / the air updates read (the README (b), all proven)
constexpr uint32_t kCharTable = 0x804F1834, kAccel = 0x805F26AC, kTurnSlow = 0x805F268C, kTurnFast = 0x805F26C0,
                   kTurnSpan = 0x805F26C4, kAirFall = 0x805F25F8, kAirJump = 0x805F2784, kAirJump2 = 0x805F2780, kAirDash = 0x805F27B4;
Mult g_m;
uint32_t g_player = 0;

void read_originals() {
  if (g_o.read) return;
  g_o.vdrag = f(kVertDrag); g_o.hdrag = f(kHorizDrag);
  g_o.hjv1 = f(kHjVel1); g_o.hjf1 = f(kHjForce1); g_o.hjt = f(kHjTime); g_o.hjv0 = f(kHjVel0); g_o.hjf0 = f(kHjForce0);
  g_o.read = true;
  logf("[movement] originals: vertical air drag %g, horizontal air drag %g, homing bounce %g / %g for %g s (miss %g / %g)\n",
       g_o.vdrag, g_o.hdrag, g_o.hjv1, g_o.hjf1, g_o.hjt, g_o.hjv0, g_o.hjf0);
}
// the HomingJump behaviour is running: the executer's current behaviour (+0x30) has the HomingJump vtable
bool homing_jump(uint32_t p) {
  const uint32_t ex = ld32(p + 0x22C);
  for (uint32_t e : {ex, p + 0x22C}) {
    if (e < 0x80000000u || e >= RT_RAM_END) continue;
    const uint32_t b = ld32(e + 0x30);
    if (b >= 0x80000000u && b < RT_RAM_END && ld32(b) == kHomingJumpVt) return true;
  }
  return false;
}
// Object-driven movement stays vanilla (user, 2026-10-18): springs, dash panels / pads, dash rings, rails, light dash,
// vehicles, scripted control... Tuning applies only in Shadow's own behaviours (README (a): ids 0x00-0x06, 0x08-0x09,
// 0x10, 0x12, 0x18-0x1A); any other behaviour latches "vanilla", and the latch holds until he is back on the ground in
// an allowed behaviour, so a spring / pad arc is the original all the way to the landing (incl. the Fall after Launch).
bool allowed_id(uint32_t id) { return id <= 0x06 || id == 0x08 || id == 0x09 || id == 0x10 || id == 0x12 || (id >= 0x18 && id <= 0x1A); }
uint32_t behaviour(uint32_t p, uint32_t* vt = nullptr) {
  const uint32_t ex = ld32(p + 0x22C);
  if (ex < 0x80000000u || ex >= RT_RAM_END) return 0xFFFFFFFF;
  const uint32_t b = ld32(ex + 0x30);
  if (b < 0x80000000u || b >= RT_RAM_END) return 0xFFFFFFFF;
  if (vt) *vt = ld32(b);
  return ld32(b + 0x10);
}
bool g_vanilla = false;
bool update_vanilla(uint32_t p) {
  uint32_t vt = 0;
  const uint32_t id = behaviour(p, &vt);
  // dash panels / speed pads keep the Ground behaviour but lock the controls: +0x90 = 1 and bit 0x4000 of +0xAC while
  // the boost runs (seen in quiet runs, harness 'speedpad'); its Fall off an edge stays vanilla through the latch
  const bool boost = ld32(p + 0x90) != 0 || (ld32(p + 0xAC) & 0x4000);
  const bool ok = id != 0xFFFFFFFF && allowed_id(id) && !boost;
  const bool grounded = ld32(p + 0xA8) & 4;
  const bool was = g_vanilla;
  if (!ok) g_vanilla = true;
  else if (grounded) g_vanilla = false;
  static const bool log = getenv("MV_BEHLOG") != nullptr;
  static uint32_t last_id = 0xFFFFFFFE;
  static bool last_boost = false;
  if (log && (id != last_id || was != g_vanilla || boost != last_boost)) {
    last_boost = boost;
    logf("[movement] behaviour id 0x%X vt %08X grounded %d boost %d -> %s\n", id, vt, grounded ? 1 : 0, boost ? 1 : 0, g_vanilla ? "VANILLA" : "tuned");
    last_id = id;
  }
  return g_vanilla;
}
const Mult kNone{};
}  // namespace

// once per frame (frame_rate.cpp hle_PlayerMoveStep, before the move): the live constants
void ensure_presets();
void frame(uint32_t player) {
  ensure_presets();
  if (!shadow(player)) return;
  g_player = player;
  read_originals();
  g_m = update_vanilla(player) ? kNone : current();
  static bool was_any = false;
  if (!g_m.any && !was_any) return;                             // Original and nothing to restore: untouched
  was_any = g_m.any;
  // Shadow's jump row (per character id; read once, the first time he is seen)
  const int id = int(ld32(player + 0x64));
  if (id >= 0 && id < 32) {
    if (g_o.jump_id != id) { g_o.jump_id = id; g_o.jump0 = f(kJumpTable + id * 0xC); g_o.jump4 = f(kJumpTable + id * 0xC + 4);
      logf("[movement] jump row %d: start %g, hold force %g\n", id, g_o.jump0, g_o.jump4); }
    put(kJumpTable + id * 0xC, g_o.jump0 * g_m.jump);
    put(kJumpTable + id * 0xC + 4, g_o.jump4 * g_m.jump);
  }
  if (id >= 0 && id < 32) {   // top speed: Shadow's row of the character table
    if (g_o.run_id != id) { g_o.run_id = id; g_o.run_mult = f(kCharTable + id * 0x2C + 4);
      logf("[movement] character row %d: top-speed multiplier %g\n", id, g_o.run_mult); }
    put(kCharTable + id * 0x2C + 4, g_o.run_mult * g_m.max_speed);
  }
  put(kHjVel1, g_o.hjv1 * g_m.bounce); put(kHjForce1, g_o.hjf1 * g_m.bounce);
  put(kHjVel0, g_o.hjv0 * g_m.bounce); put(kHjForce0, g_o.hjf0 * g_m.bounce);
  put(kHjTime, g_o.hjt * g_m.htime);
  static uint64_t last_log = 0; const uint64_t now = sys_time_ns();
  if (now - last_log > 2000000000ull) { last_log = now;
    logf("[movement] preset %d gravity %.2f fall_max %.2f air_drag %.2f jump %.2f bounce %.2f homing_time %.2f homing_fall %.2f\n",
         settings::g.mv_preset ? 1 : 0, g_m.gravity, g_m.fall_max, g_m.air_drag, g_m.jump, g_m.bounce, g_m.htime, g_m.hfall); }
}

// phase 2 hooks: is this behaviour Shadow's, and the multipliers now (all 1 while object-driven movement is vanilla)
uint32_t player() { return g_player; }   // Shadow (this frame), for homing.cpp
bool phase2(uint32_t b, float& accel, float& turn, float& air) {
  if (b < 0x80000000u || b >= RT_RAM_END - 8 || !g_m.any) return false;
  const uint32_t p = ld32(b + 4);
  if (!shadow(p)) return false;
  accel = g_m.accel; turn = g_m.turn; air = g_m.air_control;
  return true;
}

// ---------------------------------------------------------------------------------------------------- Tweak menu
namespace {
struct KeyInfo { const char* key; const char* label; const char* group; int lo, hi; };
constexpr int kN = 11;
const KeyInfo kKeys[kN] = {
    {"mv_gravity", "Gravity", "Fall / weight", 25, 400},       {"mv_fall_max", "Top fall speed", "Fall / weight", 25, 400},
    {"mv_air_drag", "Air drag", "Air", 0, 400},                {"mv_jump", "Jump", "Jump", 25, 300},
    {"mv_homing_bounce", "Bounce height", "Homing", 0, 300},   {"mv_homing_time", "Bounce time", "Homing", 0, 300},
    {"mv_homing_fall", "Fall after the bounce", "Homing", 25, 400},
    {"mv_max_speed", "Top speed", "Ground", 50, 200},           {"mv_accel", "Acceleration", "Ground", 25, 400},
    {"mv_turn", "Turning", "Ground", 25, 300},                  {"mv_air_control", "Air control", "Air", 0, 400},
};
int settings::Values::* const kField[kN] = {
    &settings::Values::mv_gravity, &settings::Values::mv_fall_max, &settings::Values::mv_air_drag, &settings::Values::mv_jump,
    &settings::Values::mv_homing_bounce, &settings::Values::mv_homing_time, &settings::Values::mv_homing_fall,
    &settings::Values::mv_max_speed, &settings::Values::mv_accel, &settings::Values::mv_turn, &settings::Values::mv_air_control};
struct Preset { std::string name; int v[kN]; };
std::vector<Preset> g_presets;
bool g_presets_loaded = false;
bool g_overlay = false;

std::string presets_path() {
  char exe[MAX_PATH];
  GetModuleFileNameA(nullptr, exe, MAX_PATH);
  std::string p = exe;
  return p.substr(0, p.find_last_of("\\/")) + "\\movement_presets.ini";
}
void save_presets() {
  FILE* f = fopen(presets_path().c_str(), "w");
  if (!f) { logf("[movement] could not write %s\n", presets_path().c_str()); return; }
  fprintf(f, "; Shadow's movement presets (the Tweak menu: launcher and F2 in the game). Values are %% of the original.\n"
             "; 'Original' is the game itself and is not stored here.\n");
  for (const auto& p : g_presets) {
    fprintf(f, "\n[%s]\n", p.name.c_str());
    for (int i = 0; i < kN; ++i) fprintf(f, "%s=%d\n", kKeys[i].key, p.v[i]);
  }
  fclose(f);
}
void load_presets() {
  g_presets.clear();
  if (FILE* f = fopen(presets_path().c_str(), "r")) {
    char line[256];
    while (fgets(line, sizeof line, f)) {
      std::string l = line;
      while (!l.empty() && (l.back() == '\n' || l.back() == '\r' || l.back() == ' ')) l.pop_back();
      if (l.empty() || l[0] == ';') continue;
      if (l[0] == '[' && l.back() == ']') {
        Preset p; p.name = l.substr(1, l.size() - 2);
        for (int& v : p.v) v = 100;
        if (!p.name.empty() && p.name != "Original") g_presets.push_back(p);
        continue;
      }
      const size_t eq = l.find('=');
      if (eq == std::string::npos || g_presets.empty()) continue;
      for (int i = 0; i < kN; ++i)
        if (l.compare(0, eq, kKeys[i].key) == 0) g_presets.back().v[i] = std::clamp(atoi(l.c_str() + eq + 1), kKeys[i].lo, kKeys[i].hi);
    }
    fclose(f);
    // #144 (user): no built-in Reworked any more; the old first-run seed goes if nobody changed it (a tuned one stays)
    static const int kSeed[kN] = {135, 140, 100, 100, 80, 100, 180, 100, 100, 100, 100};
    const size_t n0 = g_presets.size();
    g_presets.erase(std::remove_if(g_presets.begin(), g_presets.end(), [](const Preset& q) {
                      return q.name == "Reworked" && std::equal(q.v, q.v + kN, kSeed); }), g_presets.end());
    if (g_presets.size() != n0) { save_presets(); logf("[movement] the old built-in Reworked preset removed (#144)\n"); }
  }   // no file: none yet (Original is the game; the player makes their own)
  logf("[movement] %zu preset(s) from %s\n", g_presets.size(), presets_path().c_str());
}
int find_preset(const std::string& n) {
  for (size_t i = 0; i < g_presets.size(); ++i) if (g_presets[i].name == n) return int(i);
  return -1;
}
void load_into_sliders(const Preset& p) {
  auto& s = settings::g;
  for (int i = 0; i < kN; ++i) s.*kField[i] = p.v[i];
  s.mv_preset = 1; s.mv_name = p.name;
}
void apply(int idx) {   // -1 = Original
  auto& s = settings::g;
  if (idx < 0 || idx >= int(g_presets.size())) { s.mv_preset = 0; s.mv_name.clear(); }
  else load_into_sliders(g_presets[idx]);
  settings::save();
  logf("[movement] preset -> %s\n", active_name());
}
bool modified() {
  const auto& s = settings::g;
  if (!s.mv_preset) return false;
  const int k = find_preset(s.mv_name);
  if (k < 0) return true;
  for (int i = 0; i < kN; ++i) if (s.*kField[i] != g_presets[k].v[i]) return true;
  return false;
}
// the readout next to each slider: the game's value and what the slider makes of it
std::string readout(int i, int pct) {
  const float p = pct / 100.f;
  const float vd = g_o.read ? g_o.vdrag : 1.f, hd = g_o.read ? g_o.hdrag : 3.f;
  const float j0 = g_o.jump_id >= 0 ? g_o.jump0 : 100.f, j4 = g_o.jump_id >= 0 ? g_o.jump4 : 270.f;
  const float b1 = g_o.read ? g_o.hjv1 : 200.f, f1 = g_o.read ? g_o.hjf1 : 180.f, t = g_o.read ? g_o.hjt : 0.75f;
  char b[128];
  switch (i) {
    case 0: snprintf(b, sizeof b, "%.0f   (original -320)", -320 * p); break;
    case 1: snprintf(b, sizeof b, "%.0f per s   (original %.0f)", 320 / vd * p, 320 / vd); break;
    case 2: snprintf(b, sizeof b, "drag %.2f   (original %.2f)", hd * p, hd); break;
    case 3: snprintf(b, sizeof b, "%.0f up, hold %.0f   (original %.0f, %.0f)", j0 * p, j4 * p, j0, j4); break;
    case 4: snprintf(b, sizeof b, "%.0f up, then %.0f   (original %.0f, %.0f)", b1 * p, f1 * p, b1, f1); break;
    case 5: snprintf(b, sizeof b, "%.2f s   (original %.2f s)", t * p, t); break;
    case 6: snprintf(b, sizeof b, "gravity x%.2f while bouncing   (original x1)", p); break;
    case 7: snprintf(b, sizeof b, "%.0f per s   (original %.0f)", 750 * g_o.run_mult * p, 750 * g_o.run_mult); break;
    case 8: snprintf(b, sizeof b, "%.0f per s to top speed   (original 3500)", 3500 * p); break;
    case 9: snprintf(b, sizeof b, "turn %.1f slow / %.1f fast   (original 15 / 2)", 15 * p, 2 * p); break;
    default: snprintf(b, sizeof b, "steer %.0f   (original 220)", 220 * p); break;
  }
  return b;
}
}  // namespace

void ensure_presets() {
  if (g_presets_loaded) return;
  g_presets_loaded = true;
  load_presets();
  auto& s = settings::g;   // mv_preset 0 = Original, else the sliders (mv_name: the preset they came from, or Custom)
  if (s.mv_preset && (s.mv_name.empty() || (s.mv_name != "Custom" && find_preset(s.mv_name) < 0))) s.mv_name = "Custom";
  if (s.mv_preset) s.mv_preset = 1;
}
// #144 the player's own presets (the in-game menu: Movement > Preset; docs/launcher/IN-GAME-MENU-PRESETS.md)
int preset_count() { ensure_presets(); return int(g_presets.size()); }
const char* preset_name(int i) { ensure_presets(); return i >= 0 && i < int(g_presets.size()) ? g_presets[i].name.c_str() : ""; }
int active_preset() {
  ensure_presets();
  const auto& s = settings::g;
  if (!s.mv_preset) return -1;
  const int k = find_preset(s.mv_name);
  return k >= 0 ? k : -2;
}
bool preset_modified() { ensure_presets(); return modified(); }
void use_preset(int i) {   // -1 Original, -2 the sliders as they are (Custom), else that preset
  ensure_presets();
  auto& s = settings::g;
  if (i == -2) { s.mv_preset = 1; s.mv_name = "Custom"; settings::save(); logf("[movement] preset -> Custom\n"); return; }
  apply(i);
}
std::string clean_name(const std::string& in) {
  std::string r;
  for (char ch : in) if ((unsigned char)ch >= 32 && !strchr("[];=", ch)) r += ch;
  while (!r.empty() && r.front() == ' ') r.erase(r.begin());
  while (!r.empty() && r.back() == ' ') r.pop_back();
  if (r.size() > kMaxName) r.resize(kMaxName);
  return r;
}
namespace { bool same_ci(const std::string& a, const char* b) { return _stricmp(a.c_str(), b) == 0; } }
const char* name_problem(const std::string& raw, int except) {
  ensure_presets();
  const std::string n = clean_name(raw);
  if (n.empty()) return "Type a name first";
  if (same_ci(n, "Original") || same_ci(n, "Custom")) return "That name is taken by the game";
  for (int i = 0; i < int(g_presets.size()); ++i) if (i != except && same_ci(g_presets[i].name, n.c_str())) return "A preset with that name exists";
  return nullptr;
}
std::string unique_name(const std::string& base) {
  ensure_presets();
  for (int k = 1;; ++k) { std::string n = base + " " + std::to_string(k); if (!name_problem(n, -1)) return n; }
}
int create_preset(const std::string& raw) {   // from the sliders as they are now; becomes the active preset
  if (name_problem(raw, -1)) return -1;
  auto& s = settings::g;
  if (!s.mv_preset) for (int i = 0; i < kN; ++i) s.*kField[i] = 100;   // from Original: a copy of the game (all 100%)
  Preset p{clean_name(raw), {}};
  for (int i = 0; i < kN; ++i) p.v[i] = s.*kField[i];
  g_presets.push_back(p);
  s.mv_preset = 1; s.mv_name = p.name;
  save_presets(); settings::save();
  logf("[movement] preset created: %s\n", p.name.c_str());
  return int(g_presets.size()) - 1;
}
bool rename_preset(int i, const std::string& raw) {
  ensure_presets();
  if (i < 0 || i >= int(g_presets.size()) || name_problem(raw, i)) return false;
  auto& s = settings::g;
  const std::string old = g_presets[i].name, n = clean_name(raw);
  g_presets[i].name = n;
  if (s.mv_preset && s.mv_name == old) s.mv_name = n;
  save_presets(); settings::save();
  logf("[movement] preset renamed: %s -> %s\n", old.c_str(), n.c_str());
  return true;
}
bool delete_preset(int i) {   // the active one: back to Original (the sliders are not touched)
  ensure_presets();
  if (i < 0 || i >= int(g_presets.size())) return false;
  auto& s = settings::g;
  const std::string n = g_presets[i].name;
  g_presets.erase(g_presets.begin() + i);
  if (s.mv_preset && s.mv_name == n) { s.mv_preset = 0; s.mv_name.clear(); }
  save_presets(); settings::save();
  logf("[movement] preset deleted: %s\n", n.c_str());
  return true;
}
bool save_into(int i) {   // the sliders as they are now into preset i (Save changes)
  ensure_presets();
  if (i < 0 || i >= int(g_presets.size())) return false;
  auto& s = settings::g;
  for (int k = 0; k < kN; ++k) g_presets[i].v[k] = s.*kField[k];
  s.mv_preset = 1; s.mv_name = g_presets[i].name;
  save_presets(); settings::save();
  logf("[movement] preset saved: %s\n", g_presets[i].name.c_str());
  return true;
}
// the menu's action rows: the on-screen keyboard asks for the name, then the preset is made / renamed
void ask_new() {
  osk::open("Name the new preset", unique_name("My preset"), kMaxName, [](const std::string& t) { return name_problem(t, -1); },
            [](bool ok, const std::string& t) { if (ok) create_preset(t); });
}
void ask_rename(int i) {
  if (i < 0 || i >= preset_count()) return;
  osk::open("Rename the preset", preset_name(i), kMaxName, [i](const std::string& t) { return name_problem(t, i); },
            [i](bool ok, const std::string& t) { if (ok) rename_preset(i, t); });
}
// INPUT_FILE "MVP <op>:<arg>" (tests; '_' in a name = space): new:<name>, use:<n|original|custom>, rename:<n>:<name>,
// delete:<n>, save:<n>, set:<slider index>:<pct>, list
void command(const std::string& a) {
  std::string op = a, rest;
  const size_t c = a.find(':');
  if (c != std::string::npos) { op = a.substr(0, c); rest = a.substr(c + 1); }
  auto nm = [](std::string x) { std::replace(x.begin(), x.end(), '_', ' '); return x; };
  if (op == "new") { if (create_preset(nm(rest)) < 0) logf("[movement] preset refused: %s\n", name_problem(nm(rest), -1)); }
  else if (op == "use") use_preset(rest == "original" ? -1 : rest == "custom" ? -2 : atoi(rest.c_str()));
  else if (op == "rename") { const size_t d = rest.find(':'); if (d == std::string::npos || !rename_preset(atoi(rest.c_str()), nm(rest.substr(d + 1)))) logf("[movement] rename refused\n"); }
  else if (op == "delete") delete_preset(atoi(rest.c_str()));
  else if (op == "ask-new") ask_new();
  else if (op == "ask-rename") ask_rename(atoi(rest.c_str()));
  else if (op == "save") save_into(atoi(rest.c_str()));
  else if (op == "set") { const size_t d = rest.find(':'); const int k = atoi(rest.c_str()); if (d != std::string::npos && k >= 0 && k < kN) { settings::g.*kField[k] = atoi(rest.c_str() + d + 1); logf("[movement] %s = %d\n", kKeys[k].key, settings::g.*kField[k]); } }
  ensure_presets();
  std::string l; for (int i = 0; i < int(g_presets.size()); ++i) l += (i ? ", " : "") + g_presets[i].name;
  logf("[movement] presets (%d): %s | active %s%s\n", int(g_presets.size()), l.c_str(), active_name(), modified() ? " (changed)" : "");
}
const char* active_name() {
  const auto& s = settings::g;
  if (!s.mv_preset) return "Original";
  return s.mv_name.empty() ? "Custom" : s.mv_name.c_str();
}
void cycle() {
  ensure_presets();
  const int k = settings::g.mv_preset ? find_preset(settings::g.mv_name) : -1;
  apply(k + 1 < int(g_presets.size()) ? k + 1 : -1);
}
void toggle_overlay() { g_overlay = !g_overlay; logf("[movement] tweak window %s\n", g_overlay ? "open" : "closed"); }
bool overlay_open() { return g_overlay; }

void draw_tweak() {
  ensure_presets();
  auto& s = settings::g;
  // preset picker
  std::string shown = std::string(active_name()) + (modified() ? "  (changed)" : "");
  ImGui::SetNextItemWidth(260);
  if (ImGui::BeginCombo("Preset", shown.c_str())) {
    if (ImGui::Selectable("Original  (the game, read-only)", !s.mv_preset)) apply(-1);
    for (size_t i = 0; i < g_presets.size(); ++i)
      if (ImGui::Selectable(g_presets[i].name.c_str(), s.mv_preset && s.mv_name == g_presets[i].name)) apply(int(i));
    ImGui::EndCombo();
  }
  // name + save / duplicate / delete
  static char name[64] = "";
  static std::string synced = "\x01";
  if (synced != s.mv_name) { synced = s.mv_name; snprintf(name, sizeof name, "%s", s.mv_name.c_str()); }
  ImGui::SetNextItemWidth(260);
  ImGui::InputText("Name", name, sizeof name);
  const std::string nm = name;
  ImGui::BeginDisabled(nm.empty() || nm == "Original");
  if (ImGui::Button("Save")) { if (find_preset(clean_name(nm)) >= 0) save_into(find_preset(clean_name(nm))); else create_preset(nm); }
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(!s.mv_preset);
  if (ImGui::Button("Duplicate")) {
    std::string base = std::string(active_name()) + " copy", n = base;
    for (int k = 2; name_problem(n, -1); ++k) n = base + " " + std::to_string(k);
    create_preset(n);
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  const int cur = s.mv_preset ? find_preset(s.mv_name) : -1;
  ImGui::BeginDisabled(cur < 0);
  if (ImGui::Button("Delete")) delete_preset(cur);
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("Reset all")) { for (int i = 0; i < kN; ++i) s.*kField[i] = 100; if (s.mv_preset) settings::save(); }
  if (!s.mv_preset) ImGui::TextDisabled("Original = the game untouched. Moving a slider starts a custom set.");
  // the sliders, grouped
  const char* groups[] = {"Ground", "Fall / weight", "Air", "Jump", "Homing"};
  for (const char* gname : groups) {
    ImGui::SeparatorText(gname);
    for (int i = 0; i < kN; ++i) {
      if (strcmp(kKeys[i].group, gname)) continue;
      ImGui::PushID(i);
      int v = s.*kField[i];
      ImGui::SetNextItemWidth(220);
      if (ImGui::SliderInt(kKeys[i].label, &v, kKeys[i].lo, kKeys[i].hi, "%d%%", ImGuiSliderFlags_AlwaysClamp)) {
        s.*kField[i] = v;
        if (!s.mv_preset) { s.mv_preset = 1; s.mv_name = "Custom"; }
      }
      if (ImGui::IsItemDeactivatedAfterEdit()) settings::save();
      ImGui::SameLine();
      ImGui::BeginDisabled(v == 100);
      if (ImGui::SmallButton("Reset")) { s.*kField[i] = 100; settings::save(); }
      ImGui::EndDisabled();
      ImGui::SameLine();
      ImGui::TextDisabled("%s", readout(i, s.mv_preset ? s.*kField[i] : 100).c_str());
      ImGui::PopID();
    }
  }
  ImGui::Spacing();
  ImGui::TextDisabled("Springs, dash panels, rails, light dash and vehicles always move as in the original.");
}

void draw_overlay() {
  if (!g_overlay) return;

  ImGui::SetNextWindowPos(ImVec2(40, 60), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(0.92f);
  bool open = true;
  if (ImGui::Begin("Movement tweak  (F2 to close, F1 next preset)", &open, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize)) draw_tweak();
  ImGui::End();
  if (!open) toggle_overlay();
}
}  // namespace movement

// fn_800A7700 (executer: the player at +0x28): adds gravity P+0x100 to the force P+0x24. For Shadow, add the
// difference his multiplier asks for (and the homing-fall multiplier while HomingJump runs).
void hle_GravityAdd(Cpu& c) {
  const uint32_t ex = c.r[3];
  f_800A7700(c);
  using namespace movement;
  if (!g_m.any) return;
  const uint32_t p = ld32(ex + 0x28);
  if (!shadow(p) || !(ld32(p + 0xA8) & 1)) return;              // bit 0: gravity on
  if (update_vanilla(p)) return;                                // a spring / pad / rail... began this frame
  float m = g_m.gravity;
  if (g_m.hfall != 1 && homing_jump(p)) m *= g_m.hfall;
  if (m == 1) return;
  for (int i = 0; i < 3; ++i) stf32(p + 0x24 + i * 4, float(ldf32(p + 0x24 + i * 4)) + float(ldf32(p + 0x100 + i * 4)) * (m - 1));
}
// fn_80093728 (player in r3): the integrator with linear drag; Shadow's air drags are scaled around his call only
void hle_IntegrateDrag(Cpu& c) {
  using namespace movement;
  if (!g_m.any || !g_o.read || !shadow(c.r[3]) || (g_m.fall_max == 1 && g_m.air_drag == 1) ||
      update_vanilla(c.r[3])) { f_80093728(c); return; }
  stf32(kVertDrag, g_o.vdrag / g_m.fall_max);
  stf32(kHorizDrag, g_o.hdrag * g_m.air_drag);
  f_80093728(c);
  stf32(kVertDrag, g_o.vdrag);
  stf32(kHorizDrag, g_o.hdrag);
}

// movement phase 2 (Shadow only; the constants are shared by every character, so they are scaled around HIS update and
// restored right after): ground acceleration + turning around the Ground run state fn_800831D8, air control around the
// Fall / Jump / JumpDash updates. Behaviour objects: B + 4 = the player.
void f_800831D8(Cpu&);
void f_80080828(Cpu&);
void f_800871D8(Cpu&);
void f_800882A4(Cpu&);
namespace movement { bool phase2(uint32_t b, float& accel, float& turn, float& air); }
using namespace movement;   // the phase-2 constants (kAccel ...) live in movement's file scope
namespace {
struct Scaled { uint32_t a; float orig; };
template <int N> void scaled_call(Cpu& c, void (*fn)(Cpu&), const uint32_t (&addr)[N], const float (&k)[N]) {
  Scaled s[N];
  for (int i = 0; i < N; ++i) { s[i] = {addr[i], (float)ldf32(addr[i])}; stf32(addr[i], s[i].orig * k[i]); }
  fn(c);
  for (int i = 0; i < N; ++i) stf32(s[i].a, s[i].orig);
}
}  // namespace
void hle_GroundRun(Cpu& c) {
  float accel, turn, air;
  if (!movement::phase2(c.r[3], accel, turn, air) || (accel == 1 && turn == 1)) { f_800831D8(c); return; }
  static const uint32_t a[4] = {kAccel, kTurnSlow, kTurnFast, kTurnSpan};
  const float k[4] = {accel, turn, turn, turn};
  scaled_call(c, f_800831D8, a, k);
}
void hle_FallUpdate(Cpu& c) {
  float accel, turn, air;
  if (!movement::phase2(c.r[3], accel, turn, air) || air == 1) { f_80080828(c); return; }
  static const uint32_t a[1] = {kAirFall};
  const float k[1] = {air};
  scaled_call(c, f_80080828, a, k);
}
void hle_JumpUpdate(Cpu& c) {
  float accel, turn, air;
  if (!movement::phase2(c.r[3], accel, turn, air) || air == 1) { f_800871D8(c); return; }
  static const uint32_t a[2] = {kAirJump, kAirJump2};
  const float k[2] = {air, air};
  scaled_call(c, f_800871D8, a, k);
}
void hle_JumpDashUpdate(Cpu& c) {
  float accel, turn, air;
  if (!movement::phase2(c.r[3], accel, turn, air) || air == 1) { f_800882A4(c); return; }
  static const uint32_t a[1] = {kAirDash};
  const float k[1] = {air};
  scaled_call(c, f_800882A4, a, k);
}
