// #59 per-enemy AI hook (docs/stage-workshop/research/enemy-ai-59.md): the enemy task update fn_801A6EC4 calls the
// control module's AI / decision update ctrl->vt[0x14](dt) through bctrl at 0x801A7130 (r31 = the task, enemy =
// *(*(task + 4)), ctrl = enemy + 4). callhooks.txt routes that call here, so one function sees every enemy's decision.
// Per SET record (the SetManager table: entry + 0xC = instance, instance + 0x30 = the enemy object):
//   WS_AI=stand:12,40      records 12 and 40 skip their AI (they stand; damage / animation still run). PROVEN: the state
//   machine runs from ctrl->vt[0x18] (0x801A7170, WS_AI_SITE=18, default); skipping vt[0x14] (0x801A7130) changes nothing
// ENEMY_AI_LOG=1: one line per active enemy per second (enemy, record index, class, dt).
#include "sys.h"
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <unordered_map>
#include <fstream>
#include <map>
#include <sstream>
#include <windows.h>

namespace {
std::set<uint32_t> g_stand;
bool g_parsed = false;
void parse() {
  if (g_parsed) return;
  g_parsed = true;
  const char* e = getenv("WS_AI");
  if (!e) return;
  std::string s = e;
  if (s.rfind("stand:", 0) == 0) s = s.substr(6);
  for (size_t i = 0; i < s.size();) {
    const size_t j = s.find(',', i);
    g_stand.insert((uint32_t)atoi(s.substr(i, j == std::string::npos ? std::string::npos : j - i).c_str()));
    if (j == std::string::npos) break;
    i = j + 1;
  }
  logf("[enemy-ai] %zu record(s) set to stand\n", g_stand.size());
}
// instance -> record index, rebuilt once per frame-ish (by frame counter) from the SetManager table
std::unordered_map<uint32_t, uint32_t> g_inst;
uint32_t g_built = ~0u;
int record_of(uint32_t enemy) {
  const uint32_t frame = ld32(0x80584720) ^ (uint32_t)(sys_time_ns() / 100000000ull);   // ~10 rebuilds a second
  if (frame != g_built) {
    g_built = frame;
    g_inst.clear();
    const uint32_t m = ld32(0x805EE6D4);
    if (m >= 0x80000000u && m < RT_RAM_END) {
      const uint32_t n = ld32(m), tab = ld32(m + 0x18);
      if (n < 8192 && tab >= 0x80000000u && tab + n * 0x20 <= RT_RAM_END)
        for (uint32_t i = 0; i < n; ++i) {
          const uint32_t inst = ld32(tab + i * 0x20 + 0xC);
          if (inst >= 0x80000000u && inst < RT_RAM_END - 0x40) {
            g_inst[inst] = i;
            const uint32_t e = ld32(inst + 0x30);   // the enemy object the AI task updates (instance + 0x30, seen in game)
            if (e >= 0x80000000u && e < RT_RAM_END) g_inst.emplace(e, i);
          }
        }
    }
  }
  static const bool scan = getenv("ENEMY_AI_SCAN") != nullptr;
  if (scan) {   // probe: which SET instance holds a pointer to this enemy (and at which offset)
    static int n = 0;
    for (const auto& [inst, idx] : g_inst)
      for (uint32_t o = 0; o < 0x800 && n < 20; o += 4)
        if (ld32(inst + o) == enemy) { logf("[enemy-ai] instance %08X (record %u) +%03X -> enemy %08X\n", inst, idx, o, enemy); ++n; }
  }
  const auto it = g_inst.find(enemy);
  return it == g_inst.end() ? -1 : (int)it->second;
}
}  // namespace

void enemy_ai_mid_begin(Cpu& c);   // below: #58 / #59 overrides (ignore / chase)
void enemy_ai_mid_end();
void hle_EnemyAiUpdate(Cpu& c, uint32_t target) {
  parse();
  const uint32_t task = c.r[31];
  const uint32_t holder = ld32(task + 4);
  const uint32_t enemy = holder >= 0x80000000u && holder < RT_RAM_END ? ld32(holder) : 0;
  static const bool log = getenv("ENEMY_AI_LOG") != nullptr;
  int rec = -2;
  if (log || !g_stand.empty()) { rec = record_of(enemy); if (rec < 0) rec = record_of(task); if (rec < 0) rec = record_of(holder); }
  if (log) {
    static std::unordered_map<uint32_t, uint64_t> last;
    const uint64_t now = sys_time_ns();
    uint64_t& l = last[enemy];
    if (now - l > 1000000000ull) {
      l = now;
      logf("[enemy-ai] enemy %08X record %d ctrl %08X update %08X dt %.4f%s\n", enemy, rec, c.r[3], target, c.f[1],
           rec >= 0 && g_stand.count((uint32_t)rec) ? " STAND" : "");
    }
  }
  static const int site = getenv("WS_AI_SITE") ? atoi(getenv("WS_AI_SITE")) : 18;
  if (site == 14 && rec >= 0 && g_stand.count((uint32_t)rec)) return;   // no decision this frame: the enemy stands
  { extern void enemy_ai_getter_probe(Cpu&); enemy_ai_getter_probe(c); }   // test probe (AI_GETTER_PROBE)
  enemy_ai_mid_begin(c);   // #58 / #59: state changes made in this call belong to this enemy
  call_addr(c, target);
  enemy_ai_mid_end();
}

// the neighbouring ctrl calls of the same task update (pre-update vt[0x10] at 0x801A7110, vt[0x18] at 0x801A7170):
// WS_AI_SITE=10 / 14 / 18 picks which one 'stand' skips (default 14), to find where the state machine runs
static bool skip_here(Cpu& c, int site) {
  static const int want = getenv("WS_AI_SITE") ? atoi(getenv("WS_AI_SITE")) : 18;
  if (site != want || g_stand.empty()) return false;
  const uint32_t holder = ld32(c.r[31] + 4);
  const uint32_t enemy = holder >= 0x80000000u && holder < RT_RAM_END ? ld32(holder) : 0;
  const int rec = record_of(enemy);
  static const bool dbg = getenv("ENEMY_AI_LOG") != nullptr;
  static uint64_t last = 0;
  if (dbg && sys_time_ns() - last > 1000000000ull) {
    last = sys_time_ns();
    logf("[enemy-ai] skip check site %d enemy %08X record %d stand-set %zu -> %d\n", site, enemy, rec, g_stand.size(),
         (int)(rec >= 0 && g_stand.count((uint32_t)rec)));
  }
  return rec >= 0 && g_stand.count((uint32_t)rec);
}
void hle_EnemyAiPre(Cpu& c, uint32_t target) { parse(); if (!skip_here(c, 10)) call_addr(c, target); }
void hle_EnemyAiMid(Cpu& c, uint32_t target) {
  parse();
  if (skip_here(c, 18)) return;
  enemy_ai_mid_begin(c);
  call_addr(c, target);
  enemy_ai_mid_end();
}

// the logic VM (ACT 19 / 20: object AI on / off) sets a record to stand or not, on top of WS_AI
void enemy_ai_stand(uint32_t record, bool on) { parse(); if (on) g_stand.insert(record); else g_stand.erase(record); }
// 'stand' also skips the move module's update (move->vt[0x0C](dt) at 0x801A7190): an enemy that already walks keeps
// going on its last goal otherwise (stopping the state machine alone only works before its first decision)
void hle_EnemyMoveUpdate(Cpu& c, uint32_t target) {
  parse();
  if (!g_stand.empty()) {
    const uint32_t holder = ld32(c.r[31] + 4);
    const uint32_t enemy = holder >= 0x80000000u && holder < RT_RAM_END ? ld32(holder) : 0;
    const int rec = record_of(enemy);
    if (rec >= 0 && g_stand.count((uint32_t)rec)) return;
  }
  call_addr(c, target);
}

// ---- #58 / #59 AI override menu (the editor writes mods/enemy_ai.json {"format":1,"records":{"<index>":"stand|ignore|chase"}}):
//   stand   the record stands (as WS_AI stand: no decision, no movement; still takes hits)
//   ignore  never notices the player: AI state changes INTO Caution / Found / Chase / Attack states are vetoed
//   chase   always chases: idle state changes are redirected to its class's ChaseTarget state, and an idle enemy is
//           pushed into it (its class's ChaseTarget getter, chase_getter); while chasing, GoHome is vetoed
// States: fn_8019F944(stateMachine, newState) (stage_workshop.cpp hle_EnemyStateChange calls enemy_ai_state_filter); the
// enemy being updated is noted in hle_EnemyAiMid (the state machine runs inside that call). ENEMY_AI_LOG=1 logs overrides.
void f_8019F944(Cpu&);
namespace {
std::map<uint32_t, int> g_mode;   // record -> 2 ignore / 3 chase (stand goes into g_stand)
int g_mode_stage = -1;
uint32_t g_cur_enemy = 0;
std::unordered_map<uint32_t, uint32_t> g_sm_of;   // enemy -> its AI state machine (seen in a state change)
std::unordered_map<uint32_t, uint32_t> g_chase_for;   // enemy vtable -> the state 'chase' pushes it into (0 = none)

std::string rtti(uint32_t obj) {
  if (obj < 0x80000000u || obj >= RT_RAM_END) return {};
  const uint32_t vt = ld32(obj);
  if (vt < 0x80000000u || vt >= RT_RAM_END) return {};
  const uint32_t ti = ld32(vt);
  if (ti < 0x80000000u || ti >= RT_RAM_END) return {};
  const uint32_t np = ld32(ti);
  if (np < 0x80000000u || np >= RT_RAM_END) return {};
  std::string n;
  for (uint32_t a = np; n.size() < 96; ++a) {
    const uint8_t ch = ld8(a);
    if (!ch) break;
    if (ch < 32 || ch > 126) return {};
    n += char(ch);
  }
  return n;
}
bool has(const std::string& s, std::initializer_list<const char*> ks) {
  for (auto k : ks)
    if (s.find(k) != std::string::npos) return true;
  return false;
}
bool aggressive(const std::string& n) { return has(n, {"Caution", "Found", "Chase", "Attack", "Fire", "Shoot", "Aim"}); }
bool idle(const std::string& n) { return has(n, {"Wait", "Patrol", "Home", "Wander", "Stand", "Lost", "Return", "Search"}); }
std::string prefix(const std::string& n) { const size_t i = n.rfind('_'); return i == std::string::npos ? n : n.substr(0, i); }

// the state 'chase' uses for this enemy: its class's ChaseTarget, else Caution (the class token = the enemy object's RTTI
// name without namespaces, e.g. BkSoldier -> EnemyAIState_BkSoldierAI_Caution)
// the state 'chase' pushes an enemy into, per SET class (list 0, type byte): the class's own lazy singleton getter
// (found by calling every lazy state getter once, AI_GETTER_PROBE; addresses are fixed in the game): ChaseTarget where the
// class has one, else Caution (which leads to the attack). Other classes: not supported (logged once).
uint32_t chase_getter(uint32_t type) {
  switch (type) {
    case 0x8D: return 0x8017FCE8u;   // Bk Soldier: EnemyAIState_BkSoldierAI_ChaseTarget
    case 0x64: return 0x80187108u;   // GUN Soldier: GunSoldierAI_ChaseTarget
    case 0x8C: return 0x8028E528u;   // Bk Giant: BkGiantAI_ChaseTarget
    case 0x8E: return 0x80180D7Cu;   // Bk Wing Large: BkWingLargeAI_Caution
    case 0x8F: return 0x80181A08u;   // Bk Wing Small: BkWingSmallAI_Caution
    case 0x65: return 0x801842D0u;   // GUN Beetle: GunBeetleAI_Caution
    case 0x92: return 0x802B308Cu;   // Bk Chaos: BkChaosAI_Caution
    default: return 0;
  }
}
uint32_t chase_state_for(Cpu& c, uint32_t enemy) {
  const int rec = record_of(enemy);
  const uint32_t m = ld32(0x805EE6D4);
  if (rec < 0 || m < 0x80000000u || m >= RT_RAM_END) return 0;
  const uint32_t r = ld32(ld32(m + 0x18) + uint32_t(rec) * 0x20);
  if (r < 0x80000000u || r >= RT_RAM_END) return 0;
  const uint32_t type = ld8(r + 0x21), getter = ld8(r + 0x20) == 0 ? chase_getter(type) : 0;
  if (!getter) {
    static std::set<uint32_t> told;
    if (told.insert(type).second) logf("[enemy-ai] chase: class 00/%02X not supported (stays Normal)\n", type);
    return 0;
  }
  Cpu t = c;
  call_addr(t, getter);   // constructs the singleton the first time
  return t.r[3];
}

void load_modes() {
  const int stage = (int)(int32_t)ld32(0x8057D748);
  if (stage == g_mode_stage) return;
  g_mode_stage = stage;
  g_mode.clear();
  char exe[MAX_PATH];
  GetModuleFileNameA(nullptr, exe, MAX_PATH);
  std::string dir = exe;
  dir = dir.substr(0, dir.find_last_of("\\/"));
  std::string s;
  if (const char* ej = getenv("ENEMY_AI_JSON")) s = ej;   // tests: the json itself (scenarios can't write mods/)
  else {
    std::ifstream f(dir + "\\mods\\enemy_ai.json");
    if (!f) return;
    std::stringstream ss;
    ss << f.rdbuf();
    s = ss.str();
  }
  size_t i = s.find("\"records\"");
  if (i == std::string::npos) return;
  int n = 0;
  for (i = s.find('{', i); i != std::string::npos;) {   // "<index>": "<mode>" pairs
    const size_t k0 = s.find('"', i + 1); if (k0 == std::string::npos) break;
    const size_t k1 = s.find('"', k0 + 1); if (k1 == std::string::npos) break;
    const size_t v0 = s.find('"', k1 + 1); if (v0 == std::string::npos) break;
    const size_t v1 = s.find('"', v0 + 1); if (v1 == std::string::npos) break;
    const uint32_t rec = (uint32_t)atoi(s.substr(k0 + 1, k1 - k0 - 1).c_str());
    const std::string mode = s.substr(v0 + 1, v1 - v0 - 1);
    if (mode == "stand") g_stand.insert(rec);
    else if (mode == "ignore") g_mode[rec] = 2;
    else if (mode == "chase") g_mode[rec] = 3;
    ++n;
    i = s.find_first_of(",}", v1);
    if (i == std::string::npos || s[i] == '}') break;
  }
  logf("[enemy-ai] mods/enemy_ai.json: %d override(s) for stage %d\n", n, stage);
}
int mode_of(uint32_t enemy) {
  if (g_mode.empty() || !enemy) return 0;
  const int rec = record_of(enemy);
  const auto it = rec >= 0 ? g_mode.find((uint32_t)rec) : g_mode.end();
  return it == g_mode.end() ? 0 : it->second;
}
}  // namespace

// stage_workshop.cpp hle_EnemyStateChange: 0 = let the change happen, 1 = veto it, 2 = c.r[4] was redirected
int enemy_ai_state_filter(Cpu& c) {
  parse();
  load_modes();
  if (!g_cur_enemy) {   // the change came from outside the AI update (messages, damage): the enemy from the owner object
    static std::unordered_map<uint32_t, uint32_t> enemy_of_owner;
    const uint32_t owner = ld32(c.r[3]);
    auto it = enemy_of_owner.find(owner);
    if (it == enemy_of_owner.end()) {
      uint32_t found = 0;
      if (owner >= 0x80000000u && owner < RT_RAM_END - 0x200) {
        if (record_of(owner) >= 0) found = owner;
        for (uint32_t k = 0; !found && k < 0x200; k += 4) {
          const uint32_t p = ld32(owner + k);
          if (p >= 0x80000000u && p < RT_RAM_END && record_of(p) >= 0) found = p;
        }
      }
      it = enemy_of_owner.emplace(owner, found).first;
    }
    if (!it->second) return 0;
    const uint32_t keep = g_cur_enemy;
    g_cur_enemy = it->second;
    const int r = enemy_ai_state_filter(c);
    g_cur_enemy = keep;
    return r;
  }
  g_sm_of[g_cur_enemy] = c.r[3];
  const std::string to = rtti(c.r[4]);
  static const bool log = getenv("ENEMY_AI_LOG") != nullptr;
  if (log) logf("[enemy-ai] state change enemy %08X (vt %08X) -> %s at %08X\n", g_cur_enemy, ld32(g_cur_enemy), to.c_str(), c.r[4]);
  const int m = mode_of(g_cur_enemy);
  if (!m) return 0;
  if (m == 2 && aggressive(to)) {
    static int n = 0;
    if (log || n++ < 10) logf("[enemy-ai] ignore: enemy %08X record %d stays (vetoed -> %s)\n", g_cur_enemy, record_of(g_cur_enemy), to.c_str());
    return 1;
  }
  if (m == 3 && idle(to)) {
    if (const uint32_t st = chase_state_for(c, g_cur_enemy)) {
      // ChaseTarget drops to GoHome when the target leaves the enemy's home area (fn_8017FE80 -> fn_801A68C0): already
      // chasing = stay (a redirect would re-enter ChaseTarget every frame and reset it)
      if (ld32(c.r[3] + 4) == st) return 1;
      if (log) logf("[enemy-ai] chase: enemy %08X record %d %s -> %s\n", g_cur_enemy, record_of(g_cur_enemy), to.c_str(), rtti(st).c_str());
      c.r[4] = st;
      return 2;
    }
  }
  return 0;
}
// around the AI state machine update (hle_EnemyAiMid): note the enemy; push an idle 'chase' enemy into ChaseTarget
void enemy_ai_mid_begin(Cpu& c) {
  load_modes();
  const uint32_t holder = ld32(c.r[31] + 4);
  g_cur_enemy = holder >= 0x80000000u && holder < RT_RAM_END ? ld32(holder) : 0;
  if (mode_of(g_cur_enemy) != 3) return;
  auto sm = g_sm_of.find(g_cur_enemy);
  if (sm == g_sm_of.end()) {   // not seen in a state change yet: the state machine {owner, state, previous, lock} inside the enemy
    for (uint32_t q = g_cur_enemy; q < g_cur_enemy + 0x1000 && q + 8 < RT_RAM_END; q += 4)
      if (rtti(ld32(q + 4)).rfind("EnemyAIState_", 0) == 0) { sm = g_sm_of.emplace(g_cur_enemy, q).first; break; }
    if (sm == g_sm_of.end()) { g_sm_of.emplace(g_cur_enemy, 0); return; }
  }
  if (!sm->second) return;
  const std::string cur = rtti(ld32(sm->second + 4));
  if (!idle(cur)) return;
  const uint32_t st = chase_state_for(c, g_cur_enemy);
  if (!st || ld8(sm->second + 0xC)) return;
  Cpu t = c;
  t.r[3] = sm->second;
  t.r[4] = st;
  f_8019F944(t);
  static int n = 0;
  if (n++ < 20) logf("[enemy-ai] chase: enemy %08X record %d pushed %s -> %s\n", g_cur_enemy, record_of(g_cur_enemy), cur.c_str(), rtti(st).c_str());
}
void enemy_ai_mid_end() { g_cur_enemy = 0; }
// probe (AI_GETTER_PROBE=<file of hex addresses>): call each lazy singleton getter once and log the RTTI name it returns
void enemy_ai_getter_probe(Cpu& c) {
  static bool done = false;
  const char* f = getenv("AI_GETTER_PROBE");
  if (done || !f) return;
  done = true;
  std::ifstream in(f);
  std::string line;
  while (std::getline(in, line)) {
    const uint32_t a = (uint32_t)strtoul(line.c_str(), nullptr, 16);
    if (!a) continue;
    Cpu t = c;
    call_addr(t, a);
    logf("[enemy-ai] getter %08X -> %08X %s\n", a, t.r[3], rtti(t.r[3]).c_str());
  }
}
