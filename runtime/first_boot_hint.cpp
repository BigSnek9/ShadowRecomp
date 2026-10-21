// #155 first-boot hint (2026-10-21; user: "a small temporary pop-up to inform them they have a menu"): 2 s after
// the game starts, "Press Esc or Back for settings" shows for 5 s (hud_text_show, drawn by settings::draw) - on every
// start until the settings menu has been opened once. That is remembered by the file shadow_menu_seen next to
// shadow.exe (not in shadow_settings.ini: the game rewrites that file with its known keys only). FIRST_HINT=0: never.
#include "sys.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

void hud_text_show(const char* text, double seconds);   // settings_ui.cpp

namespace first_boot_hint {
namespace {
std::string marker() {
  char exe[MAX_PATH];
  GetModuleFileNameA(nullptr, exe, MAX_PATH);
  std::string d = exe;
  return d.substr(0, d.find_last_of("\\/")) + "\\shadow_menu_seen";
}
int g_seen = -1;   // -1 not checked yet
bool g_shown = false;
uint64_t g_t0 = 0;
}  // namespace

// once per presented frame (gx_lle.cpp, next to engine_limits::frame)
void frame() {
  if (g_shown) return;
  if (g_seen < 0) {
    const char* e = getenv("FIRST_HINT");
    g_seen = (e && e[0] == '0') || GetFileAttributesA(marker().c_str()) != INVALID_FILE_ATTRIBUTES ? 1 : 0;
    g_t0 = sys_time_ns();
  }
  if (g_seen || sys_time_ns() - g_t0 < 2000000000ull) return;
  hud_text_show("Press Esc or Back for settings", 5.0);
  g_shown = true;
  logf("[menu] first-boot hint shown\n");
}

// the settings menu opened (game_menu.cpp open_menu): no hint from now on
void menu_opened() {
  if (g_seen == 1) return;
  g_seen = 1;
  g_shown = true;
  const std::string m = marker();
  FILE* f = fopen(m.c_str(), "wb");
  if (f) { fputs("the settings menu has been opened (#155: no more first-boot hint)\n", f); fclose(f); }
  logf("[menu] first-boot hint off from now on (%s %s)\n", m.c_str(), f ? "written" : "NOT written");
  hud_text_show("", 0);   // the hint goes as the menu opens
}
}  // namespace first_boot_hint
