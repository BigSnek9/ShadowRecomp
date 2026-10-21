// Custom stage runs -> stats (docs/custom-stages/STATS-FORMAT.md, format 1). Active only when the game folder has a
// custom stage build's mods/custom_stage.json (projectId, slot, stats path) and that slot's stage is being played.
// One JSON line is appended per run when it ends: mission complete (fn_8016B2A4) = clear, mission failed (fn_8016B270)
// = fail, leaving the stage otherwise = quit (or dead when the lives ran out). The route comes from the mission object:
// the factory fn_8016B384 (r5 = mission id) is hooked to remember object -> id, and the stage-table row of the stage
// (lbl_804C4A18 + row * 0x50, +0 stage number, route slots at +0x1C + slot * 0xC, mission id at +4; slot 0 dark,
// 1 neutral, 2 hero) maps the id to a route. Score / rings / lives from the HUD words (settings_ui kLiveWords);
// seconds = stage time counted here while not paused; rank: not read yet (null).
#include "game.h"
#include "sys.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include <windows.h>

void f_8016B384(Cpu&);
void f_8016B2A4(Cpu&);
void f_8016B270(Cpu&);

namespace custom_stats {
namespace {
namespace fs = std::filesystem;
struct Record { std::string route, type, faction; int slot = -1, count = 1; };
struct Sidecar { bool ok = false; std::string project, stats; int slot = -1; std::vector<Record> records; } g_side;
bool g_side_read = false;
std::map<uint32_t, int> g_missions;   // mission object -> mission id (this stage)
std::map<int, std::pair<uint32_t, int>> g_custom;   // custom mission id -> (record, count A)
struct Run {
  bool active = false, ended = false;
  int stage = -1, deaths = 0, last_lives = -1;
  double seconds = 0;
  uint64_t last_ns = 0;
  std::string route, result;
} g_run;

fs::path exe_dir() {
  wchar_t w[MAX_PATH];
  GetModuleFileNameW(nullptr, w, MAX_PATH);
  return fs::path(w).parent_path();
}
std::string field(const std::string& s, const char* key) {   // "key": "value" / "key": number (flat, first match)
  const std::string k = std::string("\"") + key + "\"";
  size_t i = s.find(k);
  if (i == std::string::npos) return {};
  i = s.find(':', i + k.size());
  if (i == std::string::npos) return {};
  ++i;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
  if (i < s.size() && s[i] == '"') {
    const size_t e = s.find('"', i + 1);
    return e == std::string::npos ? std::string() : s.substr(i + 1, e - i - 1);
  }
  size_t e = i;
  while (e < s.size() && (isdigit((unsigned char)s[e]) || s[e] == '-')) ++e;
  return s.substr(i, e - i);
}
void read_sidecar() {
  if (g_side_read) return;
  g_side_read = true;
  std::ifstream f(exe_dir() / "mods" / "custom_stage.json");
  if (!f) return;
  std::stringstream ss;
  ss << f.rdbuf();
  const std::string s = ss.str();
  g_side.project = field(s, "projectId");
  g_side.stats = field(s, "stats");
  const std::string slot = field(s, "slot");
  g_side.slot = slot.empty() ? -1 : atoi(slot.c_str());
  g_side.ok = !g_side.project.empty() && !g_side.stats.empty() && g_side.slot > 0;
  // "customRecords": [{"route": .., "slotIndex": n, "type": "rings" | "timer" | "goal" | "defeat", "count": n, "seconds": n, ..}]
  const size_t cr = s.find("\"customRecords\"");
  if (cr != std::string::npos) {
    const size_t a = s.find('[', cr), z = s.find(']', cr);
    for (size_t o = s.find('{', a); a != std::string::npos && o != std::string::npos && o < z;) {
      const size_t e = s.find('}', o);
      if (e == std::string::npos) break;
      const std::string obj = s.substr(o, e - o + 1);
      Record r;
      r.route = field(obj, "route"); r.type = field(obj, "type"); r.faction = field(obj, "faction");
      const std::string si = field(obj, "slotIndex"), cn = field(obj, "count"), sec = field(obj, "seconds");
      r.slot = si.empty() ? -1 : atoi(si.c_str());
      r.count = !cn.empty() ? atoi(cn.c_str()) : !sec.empty() ? atoi(sec.c_str()) : 1;
      if (r.slot >= 0 && r.slot <= 2) g_side.records.push_back(r);
      o = s.find('{', e);
    }
    logf("[custom-stats] %zu custom mission record(s) in the sidecar\n", g_side.records.size());
  }
  logf("[custom-stats] mods/custom_stage.json: project %s slot %d -> %s%s\n", g_side.project.c_str(), g_side.slot, g_side.stats.c_str(),
       g_side.ok ? "" : " (incomplete: no stats)");
}
uint32_t stage_row(int stage) {
  for (uint32_t r = 0; r < 64; ++r)
    if ((int32_t)ld32(0x804C4A18u + r * 0x50) == stage) return 0x804C4A18u + r * 0x50;
  return 0;
}
std::string route_of(uint32_t obj) {
  const auto it = g_missions.find(obj);
  if (it == g_missions.end()) return {};
  const uint32_t row = stage_row(int(ld32(0x80584720)));
  if (!row) return {};
  static const char* kRoute[3] = {"dark", "neutral", "hero"};
  for (int s = 0; s < 3; ++s)
    if ((int32_t)ld32(row + 0x1C + s * 0xC + 4) == it->second) return kRoute[s];
  return {};
}
bool counter_like(uint32_t o) {
  if (o < 0x80000000u || o >= RT_RAM_END - 0x40) return false;
  const int32_t a = (int32_t)ld32(o + 0x8);
  return a > 0 && a < 100000;
}
void write_line() {
  if (!g_side.ok) return;
  char when[32];
  const std::time_t t = std::time(nullptr);
  std::tm lt{};
  localtime_s(&lt, &t);
  std::strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", &lt);
  const bool clear = g_run.result == "clear";
  std::string goals;
  const uint32_t row = stage_row(g_run.stage);
  static const char* kRoute[3] = {"dark", "neutral", "hero"};
  static const bool dump = getenv("CUSTOM_STATS_DUMP") != nullptr;
  for (const auto& [obj, id] : g_missions) {
    if (dump) { char b[400]; int n = snprintf(b, sizeof b, "[custom-stats] mission %d object %08X:", id, obj); for (int k = 0; k < 16; ++k) n += snprintf(b + n, sizeof b - n, " %08X", ld32(obj + k * 4)); logf("%s\n", b); }
    std::string r;
    for (int s = 0; row && s < 3; ++s)
      if ((int32_t)ld32(row + 0x1C + s * 0xC + 4) == id) { r = kRoute[s]; break; }
    if (r.empty() || goals.find("\"" + r + "\"") != std::string::npos) continue;
    char b[96];
    // mission objects: +0 vtable, +4 mission id, +8 route slot (seen in dumps); the live counter is not read yet. target =
    // the custom record's count (ours), else unknown; count = the target for the cleared route, else unknown
    char t[16] = "null";
    if (const auto it = g_custom.find(id); it != g_custom.end()) snprintf(t, sizeof t, "%d", it->second.second);
    else if (r == "neutral") snprintf(t, sizeof t, "1");
    if (clear && r == g_run.route) snprintf(b, sizeof b, "\"%s\":{\"count\":%s,\"target\":%s}", r.c_str(), t, t);
    else snprintf(b, sizeof b, "\"%s\":{\"count\":null,\"target\":%s}", r.c_str(), t);
    goals += (goals.empty() ? "" : ",") + std::string(b);
  }
  char line[1024];
  snprintf(line, sizeof line,
           "{\"format\":1,\"time\":\"%s\",\"projectId\":\"%s\",\"build\":\"%s\",\"route\":%s,\"result\":\"%s\",\"score\":%d,\"rank\":null,"
           "\"seconds\":%.2f,\"rings\":%d,\"deaths\":%d,\"goals\":{%s}}",
           when, g_side.project.c_str(), exe_dir().filename().string().c_str(),
           g_run.route.empty() ? "null" : ("\"" + g_run.route + "\"").c_str(), g_run.result.c_str(), clear ? (int)ld32(0x80576710) : 0,
           g_run.seconds, (int)ld32(0x8057670C), g_run.deaths, goals.c_str());
  std::error_code ec;
  const fs::path p = exe_dir() / fs::u8path(g_side.stats);
  fs::create_directories(p.parent_path(), ec);
  if (FILE* f = _wfopen(p.c_str(), L"ab")) { fprintf(f, "%s\n", line); fclose(f); }
  logf("[custom-stats] %s\n", line);
}
void end_run(const char* result, const std::string& route) {
  if (!g_run.active || g_run.ended) return;
  g_run.ended = true;
  g_run.result = result;
  g_run.route = route;
  write_line();
}
}  // namespace

void read_sidecar_public() { read_sidecar(); }
void remember(int id, uint32_t rec, int count) { g_custom[id] = {rec, count}; }
const std::vector<Record>& records() { return g_side.records; }
int slot_stage() { return g_side.slot; }
uint32_t row_of(int stage) { return stage > 0 ? stage_row(stage) : 0; }

// once per frame (settings_ui draw): the run's time, deaths and its end when the stage is left without a result
void tick() {
  read_sidecar();
  if (!g_side.ok) return;
  const int stage = int(ld32(0x80584720));
  const bool playing = game::in_stage() && stage == g_side.slot;
  const uint64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  if (playing && (!g_run.active || g_run.ended) && !g_run.active) {
    g_run = Run{};
    g_run.active = true; g_run.stage = stage; g_run.last_ns = now; g_run.last_lives = (int)ld32(0x80576704);
    logf("[custom-stats] run started (stage %d)\n", stage);
  }
  if (!g_run.active) return;
  if (playing) {
    if (!game::pause_menu_active() && !g_run.ended) g_run.seconds += double(now - g_run.last_ns) * 1e-9;
    const int lives = (int)ld32(0x80576704);
    if (lives < g_run.last_lives) ++g_run.deaths;
    g_run.last_lives = lives;
  } else if (stage != g_run.stage || !game::in_stage()) {
    // left the stage: a clear / fail was written already; else quit (or game over)
    if (!g_run.ended) end_run(g_run.last_lives <= 0 ? "dead" : "quit", "");
    g_run.active = false;
    g_missions.clear();
  }
  g_run.last_ns = now;
}
}  // namespace custom_stats

using namespace custom_stats;
// fn_8016B384 (the mission factory: r5 = mission id) -> r3 = the mission list, the new object at its head
void hle_MissionCreate(Cpu& c) {
  const int id = (int)c.r[5];
  if (const auto it = g_custom.find(id); it != g_custom.end()) {   // nukkoro2.inf rewrote the slot's counts at boot: ours again
    st32(it->second.first + 8, (uint32_t)it->second.second);
    st32(it->second.first + 0xC, 0);
  }
  f_8016B384(c);
  // r3 = the mission list; the new mission object is its head
  const uint32_t obj = (c.r[3] >= 0x80000000u && c.r[3] < RT_RAM_END) ? ld32(c.r[3]) : 0;
  if (obj >= 0x80000000u && obj < RT_RAM_END) g_missions[obj] = id;
}
// fn_8016B2A4 (r3 = the mission object): mission complete
void hle_MissionComplete(Cpu& c) {
  const std::string r = route_of(c.r[3]);
  f_8016B2A4(c);
  end_run("clear", r);
}
// fn_8016B270 (r3 = the mission object): mission failed
void hle_MissionFail(Cpu& c) {
  const std::string r = route_of(c.r[3]);
  f_8016B270(c);
  end_run("fail", r);
}

void custom_stats_tick() { custom_stats::tick(); }

// fn_8016C764 (r3 = the mission map: id -> {type, param, count A, count B}): after the game's 41 records, the custom
// stage's goals that its slot cannot do natively (missions-phase7.md section 4 step 3): record id 41 + i through
// fn_8016C508(map, id, type, param), its count A through fn_8016C3D8(map, id) (+8 / +0xC), and the slot patch: the stage
// row's route slot (+0x1C + slot * 0xC, mission id at +4) gets the new id. Types: goal 1 (Goal), defeat 3 (Enemy;
// param 0 GUN, 2 Black Arms), timer 4 (Timer, count = seconds), rings 7 (Ring).
void f_8016C764(Cpu&);
void f_8016C508(Cpu&);
void f_8016C3D8(Cpu&);
uint32_t g_mission_map = 0;   // warp_hud.cpp (#108) looks mission types up in it
void hle_MissionMapBuild(Cpu& c) {
  const uint32_t map = c.r[3];
  g_mission_map = map;
  f_8016C764(c);
  custom_stats::read_sidecar_public();
  const auto& recs = custom_stats::records();
  if (recs.empty()) return;
  const Cpu saved = c;
  const uint32_t row = custom_stats::row_of(custom_stats::slot_stage());

  for (size_t i = 0; i < recs.size(); ++i) {
    const auto& r = recs[i];
    const uint32_t id = 41 + (uint32_t)i;
    const int type = r.type == "goal" ? 1 : r.type == "defeat" ? 3 : r.type == "timer" ? 4 : r.type == "rings" ? 7 : -1;
    if (type < 0) { logf("[custom-stats] custom record %s: unknown goal type %s\n", r.route.c_str(), r.type.c_str()); continue; }
    const uint32_t param = type == 3 ? ((r.faction == "gun" || r.faction == "GUN" || (r.faction.empty() && r.route == "dark")) ? 0u : 2u) : 0u;
    c.r[3] = map; c.r[4] = id; c.r[5] = (uint32_t)type; c.r[6] = param;
    f_8016C508(c);
    c.r[3] = map; c.r[4] = id;
    f_8016C3D8(c);
    const uint32_t rec = c.r[3];
    if (rec >= 0x80000000u && rec < RT_RAM_END) { st32(rec + 8, (uint32_t)(r.count > 1 ? r.count : 1)); st32(rec + 0xC, 0); custom_stats::remember(id, rec, r.count > 1 ? r.count : 1); }
    if (row) st32(row + 0x1C + (uint32_t)r.slot * 0xC + 4, id);
    logf("[custom-stats] custom mission record id %u: %s route (slot %d) type %d param %u count %d (record %08X, row %08X)\n", id,
         r.route.c_str(), r.slot, type, param, r.count, rec, row);
    c = saved;
  }
  c = saved;
}

// custom_stage_rules.cpp (#101 time limit): a mission object of the stage being played (0 = none yet)
uint32_t custom_stats_first_mission() { return custom_stats::g_missions.empty() ? 0 : custom_stats::g_missions.begin()->first; }
// warp_hud.cpp (#108): this stage's mission objects and their ids, and the mission map (id -> {type, param, count A, B})
int custom_stats_missions(uint32_t* objs, int* ids, int max) {
  int n = 0;
  for (const auto& [o, id] : custom_stats::g_missions) {
    if (n >= max) break;
    objs[n] = o; ids[n] = id; ++n;
  }
  return n;
}
uint32_t custom_stats_mission_map() { return g_mission_map; }
// stage_workshop.cpp (logic VM mission complete / fail): the mission object of a route (0 dark, 1 neutral, 2 hero) in
// this stage; any mission object when that route has none; 0 = none yet
uint32_t custom_stats_route_mission(int route) {
  static const char* kRoute[3] = {"dark", "neutral", "hero"};
  uint32_t any = 0;
  for (const auto& [o, id] : custom_stats::g_missions) {
    if (!any) any = o;
    if (route >= 0 && route < 3 && custom_stats::route_of(o) == kRoute[route]) return o;
  }
  return any;
}
