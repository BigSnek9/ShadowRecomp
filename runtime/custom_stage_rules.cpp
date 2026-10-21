// Custom stage rules (2026-10-19; tickets #102 / #101 / #99). Active only in a custom stage build: the game folder
// has mods/custom_stage.json (written by the Stage Workshop build, custom_stage.py) and the stage being played is its slot.
//   #102 voice: 'none' (the default) skips the borrowed slot's mission reminder lines: Mission::Message / CountMessage /
//        TimerMessage speak through fn_802EEAF0 (-> talk request fn_801FAF00); placed hint balls / talk triggers still speak.
//   #101 mods/stage_rules.json {startRings, startLives, timeLimit}: rings are set whenever a new player controller starts
//        (stage start and every restart), lives once per entry into the stage. RAM words from cheats.cpp: rings 8057670C,
//        lives 80576704, stage timer f32 8057D734 (counts up). timeLimit: the stage timer reaching it fails the mission
//        (fn_8016B270 through custom_stats' hle_MissionFail, so the run is recorded as a fail), once per attempt.
// Hooks: overrides.txt fn_802EEAF0 -> hle_CsMissionSpeak, fn_8009EC2C (Player::Controller update) -> hle_CsPlayerUpdate.
// Log lines start with "[stage-rules]" (quiet-run proofs: tests/RESULTS.md).
#include "sys.h"
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <windows.h>

namespace speed_ram { void frame(Cpu& c); }
void f_802EEAF0(Cpu&);
void f_8009EC2C(Cpu&);
void hle_MissionFail(Cpu&);
void hle_MissionComplete(Cpu&);
uint32_t custom_stats_first_mission();

namespace {
struct Rules {
  bool read = false, active = false;
  int slot = -1;
  std::string voice = "none";
  int startRings = -1, startLives = -1, timeLimit = -1;
} g;

std::filesystem::path exe_dir() {
  wchar_t w[MAX_PATH];
  GetModuleFileNameW(nullptr, w, MAX_PATH);
  return std::filesystem::path(w).parent_path();
}
std::string slurp(const std::filesystem::path& p) {
  std::ifstream f(p);
  if (!f) return {};
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}
// the value after "key": in a flat JSON text (string without quotes, or a number); "" when absent
std::string field(const std::string& s, const char* key) {
  const std::string k = std::string("\"") + key + "\"";
  size_t i = s.find(k);
  if (i == std::string::npos) return {};
  i = s.find(':', i + k.size());
  if (i == std::string::npos) return {};
  ++i;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) ++i;
  if (i < s.size() && s[i] == '"') {
    const size_t e = s.find('"', i + 1);
    return e == std::string::npos ? std::string() : s.substr(i + 1, e - i - 1);
  }
  size_t e = i;
  while (e < s.size() && (isdigit((unsigned char)s[e]) || s[e] == '-' || s[e] == '.')) ++e;
  return s.substr(i, e - i);
}
int num(const std::string& s, const char* key) { const std::string v = field(s, key); return v.empty() ? -1 : atoi(v.c_str()); }

void read() {
  if (g.read) return;
  g.read = true;
  const std::string cs = slurp(exe_dir() / "mods" / "custom_stage.json");
  if (cs.empty()) return;
  g.slot = num(cs, "slot");
  const std::string v = field(cs, "voice");
  if (!v.empty()) g.voice = v;
  const std::string rules = slurp(exe_dir() / "mods" / "stage_rules.json");
  if (!rules.empty()) {
    g.startRings = num(rules, "startRings"); g.startLives = num(rules, "startLives"); g.timeLimit = num(rules, "timeLimit");
  }
  g.active = g.slot > 0;
  logf("[stage-rules] custom stage slot %d: voice %s, startRings %d, startLives %d, timeLimit %d\n", g.slot, g.voice.c_str(), g.startRings,
       g.startLives, g.timeLimit);
}
bool in_slot() { read(); return g.active && (int)ld32(0x80584720) == g.slot; }

uint32_t g_last_ctrl = 0;      // the player controller the rings were last set for
bool g_lives_done = false;     // lives set for this entry into the stage
uint32_t g_time_ctrl = 0;      // the attempt (player controller) the time limit already failed
int g_frames_out = 0;          // frames of controller updates outside the slot (resets the entry)
}  // namespace

// #102: fn_802EEAF0(message, ...) speaks the message's line; skipped in the custom stage's slot unless voice = 'slot'
void hle_CsMissionSpeak(Cpu& c) {
  if (in_slot() && g.voice != "slot") {
    static int n = 0;
    if (n++ < 20) logf("[stage-rules] mission voice line skipped (voice %s)\n", g.voice.c_str());
    return;
  }
  f_802EEAF0(c);
}

// #102 (proven path): the slot's lines heard in a custom stage (Westopolis 0x1B7 / 0x1B8 at +10.7 / +30.7 s) are NOT
// Mission::Message: they come from the player's hint routine fn_800A78FC -> fn_802F2A9C -> fn_802F2D34 -> the talk
// request fn_801FAF00. In the custom slot (voice none) a talk request whose caller two frames up is fn_800A78FC is
// skipped (r3 = 0, as fn_802F2D34's own no-line path); hint balls / talk triggers use other callers and still speak.
// CS_TALK_LOG=1 logs every talk request with its callers.
void f_801FAF00(Cpu&);
void hle_CsTalk(Cpu& c) {
  const uint32_t f1 = ld32(c.r[1]), f2 = f1 >= 0x80000000u && f1 < RT_RAM_END ? ld32(f1) : 0;
  const uint32_t up1 = f1 >= 0x80000000u && f1 < RT_RAM_END ? ld32(f1 + 4) : 0, up2 = f2 >= 0x80000000u && f2 < RT_RAM_END ? ld32(f2 + 4) : 0;
  static const bool log = getenv("CS_TALK_LOG") != nullptr;
  if (log)
    logf("[stage-rules] talk request lr %08X up %08X %08X line %X stage %u\n", c.lr, up1, up2, c.r[4], ld32(0x80584720));
  if (up2 >= 0x800A78FCu && up2 < 0x800A7CF8u && in_slot() && g.voice != "slot") {
    static int n = 0;
    if (n++ < 20) logf("[stage-rules] slot voice line %X skipped (player hint, voice %s)\n", c.r[4], g.voice.c_str());
    c.r[3] = 0;
    return;
  }
  f_801FAF00(c);
}

// #101: after the player controller's own update (once per frame per player while a stage runs)
void hle_CsPlayerUpdate(Cpu& c) {
  const uint32_t ctrl = c.r[3];
  f_8009EC2C(c);
  speed_ram::frame(c);   // #111 (speed_ram.cpp): once per player update, every stage
  // testing: TEST_CLEAR_AFTER=<s> completes a mission of the stage being played once, s seconds after the player first
  // updates (the game's own mission complete fn_8016B2A4: results, story advance, save) - makes real story saves
  static const double clear_after = getenv("TEST_CLEAR_AFTER") ? atof(getenv("TEST_CLEAR_AFTER")) : 0;
  if (clear_after > 0) {
    static uint64_t t0 = 0;
    static bool done = false;
    if (!t0) t0 = sys_time_ns();
    if (!done && (sys_time_ns() - t0) * 1e-9 >= clear_after) {
      const uint32_t m = custom_stats_first_mission();
      if (m) {
        done = true;
        const Cpu keep = c;
        c.r[3] = m;
        hle_MissionComplete(c);
        c = keep;
        logf("[test] mission %08X completed (TEST_CLEAR_AFTER)\n", m);
      }
    }
  }
  if (!in_slot()) { if (++g_frames_out > 30) g_lives_done = false; return; }
  g_frames_out = 0;
  if (g.startRings >= 0 && ctrl != g_last_ctrl) {
    g_last_ctrl = ctrl;
    st32(0x8057670C, (uint32_t)g.startRings);
    logf("[stage-rules] start rings -> %d (player %08X)\n", g.startRings, ctrl);
  }
  if (g.timeLimit > 0 && g_time_ctrl != ctrl) {
    const uint32_t u = ld32(0x8057D734);
    float t;
    memcpy(&t, &u, 4);
    if (t >= (float)g.timeLimit) {
      const uint32_t m = custom_stats_first_mission();
      if (m) {
        g_time_ctrl = ctrl;
        const Cpu keep = c;
        c.r[3] = m;
        hle_MissionFail(c);
        c = keep;
        logf("[stage-rules] time limit %d s reached (timer %.1f): mission %08X failed\n", g.timeLimit, t, m);
      }
    }
  }
  if (g.startLives > 0 && !g_lives_done) {
    g_lives_done = true;
    st32(0x80576704, (uint32_t)g.startLives);
    logf("[stage-rules] start lives -> %d\n", g.startLives);
  }
}
