#include "sys.h"
#include "sched.h"
#include "hw.h"
#include "gx_lle.h"
#include "input.h"
#include "settings_ui.h"
#include "first_run.h"
#include <windows.h>
#include <shellapi.h>
#include <cstring>
#include <cstdlib>
#include <string>
#include <io.h>
#include <cstdio>

extern void f_80003154(Cpu&);

double g_base_scale = 1.0;
bool g_quiet = false;
int g_boot_stage = 0;
bool g_play_stage = false;
bool g_boot_story = false;

// The extracted disc (folder with sys\main.dol): --disc wins; otherwise look next to / above the exe, so a plain
// double-click works from any working directory (play\shadow.exe finds ..\..\Shadow).
static std::string find_disc() {
  char exe[MAX_PATH];
  GetModuleFileNameA(nullptr, exe, MAX_PATH);
  std::string dir = exe;
  dir = dir.substr(0, dir.find_last_of("\\/"));
  const std::string cands[] = {dir + "\\Shadow", dir + "\\..\\Shadow", dir + "\\..\\..\\Shadow", "../Shadow"};
  for (auto& c : cands)
    if (GetFileAttributesA((c + "\\sys\\main.dol").c_str()) != INVALID_FILE_ATTRIBUTES) return c;
  return "../Shadow";
}

extern std::string g_disc_root;   // disc.cpp

int main(int argc, char** argv) {
  {   // #151: tabbed out / minimised, Windows 11 power throttling (EcoQoS: efficiency cores, low clocks) and timer
      // throttling slowed the game thread enough to starve the audio queue (stutter). This process opts out of both.
    PROCESS_POWER_THROTTLING_STATE pt{};
    pt.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    pt.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    pt.StateMask = 0;   // controlled + off = never throttled
    if (!getenv("TEST_ALLOW_THROTTLE")) SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof pt);   // tests: A/B
    timeBeginPeriod(1);
  }
  std::string root = find_disc();
  bool no_launcher = getenv("SHADOW_NO_LAUNCHER") != nullptr;
  bool builtin_launcher = false;   // --builtin-launcher: the old in-window launcher (launcher.cpp)
  bool with_launcher = false;      // --with-launcher: also open the launcher window (ShadowLauncher.exe / .pyw) as before
  settings::load();   // shadow_settings.ini; command-line flags below override it
  gxlle::g_render_height = settings::g.height;
  gxlle::g_widescreen = settings::g.widescreen;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--disc") && i + 1 < argc) root = argv[++i];
    else if (!strcmp(argv[i], "--trace-mmio")) g_trace_mmio = true;
    else if (!strcmp(argv[i], "--watch")) sys_set_watch(true);
    else if (!strcmp(argv[i], "--trace-sched")) g_trace_sched = true;
    else if (!strcmp(argv[i], "--trace-dsp")) hw::g_trace_dsp = true;
    else if (!strcmp(argv[i], "--trace-audio")) hw::g_audio_trace = true;
    else if (!strcmp(argv[i], "--trace-gx")) gxlle::g_trace = true;
    else if (!strcmp(argv[i], "--timescale") && i + 1 < argc) g_base_scale = atof(argv[++i]);
    else if (!strcmp(argv[i], "--trace-disc")) g_trace_disc = true;
    else if (!strcmp(argv[i], "--skip-movies")) g_skip_movies = true;
    else if (!strcmp(argv[i], "--mash")) { g_mash = true; g_skip_movies = true; }
    else if (!strcmp(argv[i], "--fast")) { g_mash = true; g_skip_movies = !getenv("TEST_PLAY_MOVIES"); }   // boot -> NEW GAME -> stage load, movies skipped
    else if (!strcmp(argv[i], "--watch-ea") && i + 1 < argc) g_watch_ea = (uint32_t)strtoul(argv[++i], nullptr, 16) & RT_RAM_MASK;
    else if (!strcmp(argv[i], "--press") && i + 1 < argc) input_add_script(argv[++i]);
    else if (!strcmp(argv[i], "--scale") && i + 1 < argc) settings::g.height = gxlle::g_render_height = 480 * atoi(argv[++i]);
    else if (!strcmp(argv[i], "--height") && i + 1 < argc) settings::g.height = gxlle::g_render_height = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--quiet")) g_quiet = true;
    else if (!strcmp(argv[i], "--no-launcher")) no_launcher = true;
    else if (!strcmp(argv[i], "--builtin-launcher")) builtin_launcher = true;
    else if (!strcmp(argv[i], "--with-launcher")) with_launcher = true;
    else if (!strcmp(argv[i], "--hdr")) settings::g.hdr = true;   // testing (not saved)
    else if (!strcmp(argv[i], "--stage") && i + 1 < argc) { g_boot_stage = atoi(argv[++i]); g_mash = true; g_skip_movies = !getenv("TEST_PLAY_MOVIES"); }   // TEST_PLAY_MOVIES: boot movies play (#149 skip test)
    // the launcher's Free Mode / Continue: stage N started the way Select Mode starts it, for players - boot screens
    // auto-confirmed only until the start (as --story), movies normal, memory card writes ON (--stage blocks them)
    else if (!strcmp(argv[i], "--play-stage") && i + 1 < argc) { g_boot_stage = atoi(argv[++i]); g_play_stage = true; g_mash = true; }
    else if (!strcmp(argv[i], "--widescreen")) settings::g.widescreen = gxlle::g_widescreen = true;
    else if (!strcmp(argv[i], "--story")) { g_boot_story = true; g_mash = true; }   // the launcher's Story button: continue the save's story
  }
  if (getenv("TEST_STORY")) { g_boot_story = true; g_boot_stage = 0; g_mash = true; }   // testing: wsassert's --stage copy, booted as --story
  g_time_scale = g_base_scale;
  // When run from a console/launcher without redirection, send the log to <exe dir>\logs\shadow-<time>.log so a
  // crash report survives the window closing.
  // Also when started by double-click: a Windows-subsystem program then has no stderr at all.
  HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
  if (_isatty(_fileno(stderr)) || err == nullptr || err == INVALID_HANDLE_VALUE) {
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string dir = exe;
    dir = dir.substr(0, dir.find_last_of("\\/")) + "\\logs";
    CreateDirectoryA(dir.c_str(), nullptr);
    SYSTEMTIME t;
    GetLocalTime(&t);
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\shadow-%04d%02d%02d-%02d%02d%02d.log", dir.c_str(), t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    fprintf(stderr, "logging to %s\n", path);
    if (!freopen(path, "w", stderr)) fprintf(stdout, "could not open log %s\n", path);
  }
  // The launcher (launcher.cpp): the window opens on it and the game boots when Play is pressed. Tests and tools
  // (--stage, --fast, --quiet, --no-launcher) go straight into the game.
  const bool plain_start = !no_launcher && !g_quiet && !g_mash && g_boot_stage == 0 && !g_boot_story;
  if (getenv("SHADOW_FORCE_LAUNCHER") || (plain_start && builtin_launcher)) {   // FORCE: tests (quiet window)
    g_disc_root = root;
    launcher::run();
  } else if (plain_start && with_launcher) {
    // #131 (user, 2026-10-20: "scrap launcher and move to the in-game one only"): started directly, the game boots and
    // opens no launcher; settings are in the in-game menu. --with-launcher opens the old launcher window next to it.
    // the launcher is its own window (launcher\shadow_launcher.pyw, Python + Tk): it edits shadow_settings.ini live while
    // the game runs. Opened next to the game when the game is started directly; a second copy just raises the first.
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string dir = exe;
    dir = dir.substr(0, dir.find_last_of("\\/"));
    // the launcher: ShadowLauncher.exe (tools/launcher_web, one exe, nothing to install) beside the game or in launcher\,
    // else the Python ones (forge_launcher.pyw, then shadow_launcher.pyw). --from-game: an open launcher stays the only one.
    std::string launcher_exe = dir + "\\ShadowLauncher.exe";
    if (GetFileAttributesA(launcher_exe.c_str()) == INVALID_FILE_ATTRIBUTES) launcher_exe = dir + "\\launcher\\ShadowLauncher.exe";
    std::string script = dir + "\\launcher\\forge_launcher.pyw";
    if (GetFileAttributesA(script.c_str()) == INVALID_FILE_ATTRIBUTES) script = dir + "\\launcher\\shadow_launcher.pyw";
    if (GetFileAttributesA(launcher_exe.c_str()) != INVALID_FILE_ATTRIBUTES) {
      ShellExecuteA(nullptr, "open", launcher_exe.c_str(), "--from-game", dir.c_str(), SW_SHOWNORMAL);
    } else if (GetFileAttributesA(script.c_str()) != INVALID_FILE_ATTRIBUTES) {
      const std::string args = "\"" + script + "\"";
      ShellExecuteA(nullptr, "open", "pythonw", args.c_str(), dir.c_str(), SW_SHOWNORMAL);
    }
  }
  // first start without a disc (release folder): ask for the player's own disc, check it, unpack to <exe>\Shadow
  if (!first_run::disc_present(root) && (root = first_run::run(g_quiet)).empty()) return 0;
  extern void profiler_start_if_requested();
  profiler_start_if_requested();
  if (!boot_init(root)) return 1;
  static Cpu cpu;
  memset(&cpu, 0, sizeof cpu);
  cpu.msr = 0x2030;
  cpu.budget = 4000;
  cpu.lr = 0;
  sched_init(cpu);
  sys_install_crash_handlers();
  logf("[boot] starting guest at 80003154\n");
  f_80003154(cpu);
  logf("[boot] guest returned\n");
  return 0;
}
