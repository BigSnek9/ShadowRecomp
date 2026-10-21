// #135 bug capture: one key - F12, or pad Back + Start together - saves what is needed to see and replay
// a bug into <game>\bugs\<date>_<time>\ and shows a toast:
//   screen.png     the game frame (Aurora's frame capture of the next present; the menus drawn over it are not in it)
//   state.json     build, stage, Shadow's position / velocity, the camera, counters, flags of the moment
//   settings.ini   a copy of shadow_settings.ini
//   log.txt        the last 400 lines of this run's log
// tools/bug_replay.py <folder> turns a capture into a test scenario (same stage, Shadow put back at that spot, same
// settings). BUG_DIR=<folder> (tests) writes the captures there instead of <game>\bugs.
#include "sys.h"
#include "settings_ui.h"
#include "build_info.h"
#include "texture_pack.h"
#include "game.h"
#include <aurora/gfx.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

extern uint32_t g_plr_owner;      // frame_rate.cpp: the player object (position +0, velocity +0x18)
extern float g_last_cam[6];       // cam_recentre.cpp: the last 3D camera's position and look direction
void hud_text_show(const char* text, double seconds);   // settings_ui.cpp

namespace fs = std::filesystem;
namespace bug_capture {
namespace {
std::string g_dir;           // the capture in progress (waiting for its screenshot)
uint64_t g_started = 0;
bool ram(uint32_t a) { return a >= 0x80000000u && a < RT_RAM_END - 0x100; }
std::string exe_dir() {
  char exe[MAX_PATH];
  GetModuleFileNameA(nullptr, exe, MAX_PATH);
  std::string d = exe;
  return d.substr(0, d.find_last_of("\\/"));
}
std::string json_str(const std::string& s) {
  std::string o = "\"";
  for (char c : s) { if (c == '"' || c == '\\') o += '\\'; if ((unsigned char)c >= 0x20) o += c; }
  return o + "\"";
}
// the log this run writes to: stderr's file (the game's own logs\shadow-*.log, or the test harness's file)
std::vector<std::string> log_tail(size_t n) {
  fflush(stderr);
  std::vector<std::string> out;
  HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
  char path[MAX_PATH * 2] = {};
  if (!h || h == INVALID_HANDLE_VALUE || !GetFinalPathNameByHandleA(h, path, sizeof path, 0)) return out;
  std::string p = path;
  if (p.rfind("\\\\?\\", 0) == 0) p = p.substr(4);
  std::ifstream f(p, std::ios::binary);
  std::deque<std::string> q;
  std::string line;
  while (std::getline(f, line)) { q.push_back(line); if (q.size() > n) q.pop_front(); }
  out.assign(q.begin(), q.end());
  return out;
}
void write_state(const std::string& dir) {
  FILE* f = fopen((dir + "\\state.json").c_str(), "wb");
  if (!f) return;
  SYSTEMTIME t;
  GetLocalTime(&t);
  fprintf(f, "{\n  \"build\": %s,\n", json_str(build_info::text()).c_str());
  fprintf(f, "  \"time\": \"%04d-%02d-%02d %02d:%02d:%02d\",\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
  fprintf(f, "  \"stage\": %d,\n", (int)(int32_t)ld32(0x8057D748));
  const uint32_t o = g_plr_owner;
  if (ram(o)) {
    fprintf(f, "  \"player\": {\"pos\": [%.2f, %.2f, %.2f], \"vel\": [%.2f, %.2f, %.2f], \"object\": \"%08X\"},\n", ldf32(o), ldf32(o + 4), ldf32(o + 8),
            ldf32(o + 0x18), ldf32(o + 0x1C), ldf32(o + 0x20), o);
  } else {
    fprintf(f, "  \"player\": null,\n");
  }
  fprintf(f, "  \"camera\": {\"pos\": [%.2f, %.2f, %.2f], \"at\": [%.4f, %.4f, %.4f]},\n", g_last_cam[0], g_last_cam[1], g_last_cam[2],
          g_last_cam[3], g_last_cam[4], g_last_cam[5]);
  struct W { const char* n; uint32_t a; } words[] = {{"rings", 0x8057670C}, {"lives", 0x80576704}, {"score", 0x80576710}, {"hero_points", 0x80576718},
                                                     {"dark_points", 0x80576714}, {"hero_gauge", 0x805766C8}, {"dark_gauge", 0x805766D4}};
  fprintf(f, "  \"counters\": {");
  for (size_t i = 0; i < sizeof words / sizeof words[0]; ++i) fprintf(f, "%s\"%s\": %d", i ? ", " : "", words[i].n, (int)(int32_t)ld32(words[i].a));
  fprintf(f, "},\n");
  fprintf(f, "  \"flags\": {\"loading\": %s, \"cutscene\": %s, \"paused\": %s},\n", game::stage_boot_loading() ? "true" : "false",
          game::cutscene_active() ? "true" : "false", game::pause_menu_active() ? "true" : "false");
  // a custom stage's build sits in mods\ next to the exe: name what is there so the crew can find the project
  std::string mods;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(exe_dir() + "\\mods", ec))
    mods += (mods.empty() ? "" : ", ") + json_str(e.path().filename().string());
  fprintf(f, "  \"mods\": [%s]\n}\n", mods.c_str());
  fclose(f);
}
}  // namespace

void trigger() {
  if (!g_dir.empty()) return;   // one at a time
  SYSTEMTIME t;
  GetLocalTime(&t);
  char stamp[64];
  snprintf(stamp, sizeof stamp, "%04d-%02d-%02d_%02d-%02d-%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
  const char* env = getenv("BUG_DIR");
  const std::string root = env ? std::string(env) : exe_dir() + "\\bugs";
  const std::string dir = root + "\\" + stamp;
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) { logf("[bug] could not create %s\n", dir.c_str()); hud_text_show("Could not save the bug report (folder)", 3); return; }
  write_state(dir);
  fs::copy_file(exe_dir() + "\\shadow_settings.ini", dir + "\\settings.ini", fs::copy_options::overwrite_existing, ec);
  if (FILE* f = fopen((dir + "\\log.txt").c_str(), "wb")) {
    for (const auto& l : log_tail(400)) { fwrite(l.data(), 1, l.size(), f); fputc('\n', f); }
    fclose(f);
  }
  aurora_capture_frames(1, 0, 0, 1 << 15, 1 << 15, (dir + "\\screen_").c_str());   // the next presented frame
  g_dir = dir;
  g_started = sys_time_ns();
  logf("[bug] capture -> %s\n", dir.c_str());
  hud_text_show((std::string("Bug report saved: bugs\\") + stamp).c_str(), 4);
}

// every frame: once the frame capture wrote screen_000.ppm, it becomes screen.png
void tick() {
  if (g_dir.empty()) return;
  const std::string ppm = g_dir + "\\screen_000.ppm";
  std::error_code ec;
  const bool timeout = sys_time_ns() - g_started > 5000000000ull;
  if (!fs::exists(ppm, ec) || sys_time_ns() - g_started < 300000000ull) {   // written a frame or two later
    if (timeout) { logf("[bug] no screenshot (capture did not arrive)\n"); g_dir.clear(); }
    return;
  }
  FILE* f = fopen(ppm.c_str(), "rb");
  int w = 0, h = 0, mx = 0;
  if (f && fscanf(f, "P6 %d %d %d", &w, &h, &mx) == 3 && w > 0 && h > 0) {
    fgetc(f);
    std::vector<uint8_t> rgb(size_t(w) * h * 3), rgba(size_t(w) * h * 4);
    if (fread(rgb.data(), 1, rgb.size(), f) == rgb.size()) {
      for (size_t i = 0; i < size_t(w) * h; ++i) { rgba[i * 4] = rgb[i * 3]; rgba[i * 4 + 1] = rgb[i * 3 + 1]; rgba[i * 4 + 2] = rgb[i * 3 + 2]; rgba[i * 4 + 3] = 255; }
      std::vector<uint8_t> png;
      if (texture_pack::write_png(rgba, w, h, png)) {
        if (FILE* o = fopen((g_dir + "\\screen.png").c_str(), "wb")) { fwrite(png.data(), 1, png.size(), o); fclose(o); }
        fclose(f);
        f = nullptr;
        fs::remove(ppm, ec);
        fs::remove(g_dir + "\\screen_000.pfm", ec);
        logf("[bug] screenshot %d x %d saved\n", w, h);
        g_dir.clear();
        return;
      }
    }
  }
  if (f) fclose(f);
  if (timeout) { logf("[bug] screenshot could not be converted (kept the .ppm)\n"); g_dir.clear(); }
}
}  // namespace bug_capture
