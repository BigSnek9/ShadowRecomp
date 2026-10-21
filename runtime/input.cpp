// Host input -> GameCube controller state (SDL3 gamepad, keyboard, optional mouse look).
#include "input.h"
#include "sys.h"
#include "gx_lle.h"
#include "settings_ui.h"
#include "movement.h"
#include "osk.h"
#include "game_menu.h"
#include "bug_capture.h"
#include "controls.h"
#include <SDL3/SDL.h>
#include <aurora/gfx.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>

namespace {
SDL_Gamepad* g_pad = nullptr;
bool g_mouse_look = false;

int8_t axis8(int16_t v) {          // SDL axis -> signed 8 bit around 0 with a small deadzone
  float f = v / 32768.0f;
  if (std::fabs(f) < 0.10f) return 0;
  return (int8_t)std::clamp((int)(f * 127.0f), -127, 127);
}
}  // namespace

static void ensure_gamepad() {
  if (g_pad && SDL_GamepadConnected(g_pad)) return;
  if (g_pad) { SDL_CloseGamepad(g_pad); g_pad = nullptr; }
  int n = 0;
  SDL_JoystickID* ids = SDL_GetGamepads(&n);
  if (ids && n > 0) {
    g_pad = SDL_OpenGamepad(ids[0]);
    if (g_pad) logf("[input] gamepad connected: %s\n", SDL_GetGamepadName(g_pad));
  }
  SDL_free(ids);
}

// ---- scripted input for automated testing:  --press <seconds>:<A|B|X|Y|S|U|D|L|R>:<duration>
struct Scripted { double t0, dur; uint16_t buttons; };
static std::vector<Scripted> g_script;
void input_add_script(const char* spec) {
  double t = 0, d = 0.2;
  char b = 'A';
  if (sscanf(spec, "%lf:%c:%lf", &t, &b, &d) < 2) return;
  uint16_t m = 0;
  switch (b) {
    case 'A': m = PAD_A; break; case 'B': m = PAD_B; break; case 'X': m = PAD_X; break; case 'Y': m = PAD_Y; break;
    case 'S': m = PAD_START; break; case 'U': m = PAD_UP; break; case 'D': m = PAD_DOWN; break;
    case 'L': m = PAD_LEFT; break; case 'R': m = PAD_RIGHT; break;
  }
  g_script.push_back({t, d, m});
}
bool g_mash = false;
uint16_t g_auto_buttons = 0;
uint16_t g_user_buttons = 0;
static uint64_t g_start_press_ns = 0;   // last START press (consumed by input_take_start_press within 250 ms)
bool input_take_start_press() {
  bool v = g_start_press_ns && sys_time_ns() - g_start_press_ns < 250000000ull;
  if (v) g_start_press_ns = 0;
  return v;
}
static uint16_t mash_buttons() {
  if (!g_mash) return 0;
  static uint64_t t0 = sys_time_ns();
  double now = (sys_time_ns() - t0) / 1e9;
  if (now < (g_boot_stage > 0 ? 2.0 : 6.0)) return 0;   // --stage: start sooner (the card check is done by then)
  static const double until = getenv("MASH_UNTIL") ? atof(getenv("MASH_UNTIL")) : 1e9;   // testing: stop mashing at T s
  if (now > until) return 0;
  uint16_t m = (fmod(now, 0.8) < 0.12) ? PAD_A : 0;
  if (fmod(now, 3.0) > 1.5 && fmod(now, 3.0) < 1.65) m |= PAD_START;   // "PRESS START" title
  return m;
}

static uint16_t movie_skip_buttons() {
  // while a movie is streaming from disc (or just stopped), tap START: 150 ms down every 600 ms
  if (!g_skip_movies || !g_last_movie_read_ns) return 0;
  uint64_t now = sys_time_ns();
  if (now - g_last_movie_read_ns > 4000000000ull) return 0;
  // (a probe holding START/B/Y/X/Z/A for 1.2 s each did not skip the first-time story movie E1001;
  //  hw.cpp jumps the stream to its tail instead)
  return ((now / 1000000ull) % 600 < 150) ? PAD_START : 0;
}

// INPUT_FILE=<path> (testing): a line like "A 150" or "DOWN 120" written to the file presses that button for the
// given milliseconds; the file is emptied once read. Lets a test drive menus step by step between screenshots.
extern uint32_t g_watch_ea, g_watch_mask;   // rt_sys.cpp
static uint64_t g_file_stick_until = 0;   // INPUT_FILE "STICK": left stick held forward until then
static uint64_t g_file_cam_until = 0;     // INPUT_FILE "CAM": C stick held right (camera turns) until then
static float g_tp[3];
static int g_tp_frames = 0;
static void tp_apply() {   // input-file TP: re-applied for a few frames (a spring holding the player would win otherwise)
  extern uint32_t g_plr_owner;
  const uint32_t o = g_plr_owner;
  if (g_tp_frames <= 0 || o < 0x80000000u || o >= RT_RAM_END - 0x400) return;
  --g_tp_frames;
  for (int i = 0; i < 3; ++i) {
    uint32_t u; memcpy(&u, &g_tp[i], 4);
    st32(o + i * 4, u); st32(o + 0xD0 + i * 4, u); st32(o + 0x18 + i * 4, 0); st32(o + 0x24 + i * 4, 0);
  }
}
static uint16_t file_buttons() {
  static const char* path = getenv("INPUT_FILE");
  if (!path) return 0;
  static uint64_t until = 0, last_check = 0;
  static uint16_t held = 0;
  uint64_t now = sys_time_ns();
  if (now < until) return held;
  held = 0;
  if (now - last_check < 50000000ull) return 0;
  last_check = now;
  FILE* f = fopen(path, "rb");
  if (!f) return 0;
  char name[32] = {0}, arg[260] = {0};
  int ms = 150;
  int n = fscanf(f, "%31s %259s", name, arg);
  fclose(f);
  if (n < 1) return 0;
  if (FILE* w = fopen(path, "wb")) fclose(w);
  if (!strcmp(name, "DUMP")) {   // "DUMP <file>": write MEM1 to a file (diffing game state)
    if (FILE* df = fopen(arg, "wb")) { fwrite(g_ram, 1, g_mem1_size, df); fclose(df); logf("[input-file] dumped MEM1 to %s\n", arg); }
    return 0;
  }
  if (!strcmp(name, "HDRSTAT")) {   // log overbright statistics of the next frame (Aurora gpu.cpp)
    aurora_hdr_request_stats();
    return 0;
  }
  if (!strcmp(name, "FRAMES")) {   // "FRAMES n,x,y,w,h": that region of the next n frames -> work\frame_NNN.ppm (exact frames)
    int n = 0, x = 0, y = 0, w = 0, h = 0;
    if (sscanf(arg, "%d,%d,%d,%d,%d", &n, &x, &y, &w, &h) == 5) {
      aurora_capture_frames(n, x, y, w, h, "work\\frame_");
      logf("[input-file] capturing %d frames (%d,%d %dx%d)\n", n, x, y, w, h);
    }
    return 0;
  }
  if (!strcmp(name, "SET")) {   // "SET key=value": any ini setting, live, not saved (settings_ui.cpp set_value)
    char key[64];
    int v;
    if (sscanf(arg, "%63[^=]=%d", key, &v) == 2)
      logf("[input-file] set %s=%d %s\n", key, v, settings::set_value(key, v) ? "" : "(unknown key)");
    return 0;
  }
  if (!strcmp(name, "COV")) {   // "COV reset" / "COV <file>": function coverage counters (rt_core.cpp; tools/cov_diff.py)
    extern void cov_reset();
    extern int cov_dump(const char*);
    if (!strcmp(arg, "reset")) { cov_reset(); logf("[input-file] coverage reset\n"); }
    else logf("[input-file] coverage: %d functions -> %s\n", cov_dump(arg), arg);
    return 0;
  }
  if (!strcmp(name, "STICK")) {   // "STICK <ms>": hold the left stick forward (testing)
    g_file_stick_until = now + (uint64_t)atoi(arg) * 1000000ull;
    logf("[input-file] stick forward %s ms\n", arg);
    return 0;
  }
  if (!strcmp(name, "TP")) {   // "TP x,y,z": the player to that point, at rest, held there 12 frames (testing: #124 spring arcs)
    if (sscanf(arg, "%f,%f,%f", &g_tp[0], &g_tp[1], &g_tp[2]) == 3) { g_tp_frames = 12; logf("[input-file] tp %.1f %.1f %.1f\n", double(g_tp[0]), double(g_tp[1]), double(g_tp[2])); }
    return 0;
  }
  if (!strcmp(name, "CAM")) {   // "CAM <ms>": hold the C stick right - the camera turns (testing camera-motion bugs)
    g_file_cam_until = now + (uint64_t)atoi(arg) * 1000000ull;
    return 0;
  }
  if (!strcmp(name, "WATCH")) {   // "WATCH <hex ea>[:mask]" / "WATCH off": like --watch-ea, from now on; a mask logs only writes changing those bits
    g_watch_ea = strcmp(arg, "off") ? (uint32_t)strtoul(arg, nullptr, 16) & RT_RAM_MASK : 0xFFFFFFFFu;
    const char* colon = strchr(arg, ':');
    g_watch_mask = colon ? (uint32_t)strtoul(colon + 1, nullptr, 16) : 0;
    logf("[input-file] watch %s\n", arg);
    return 0;
  }
  if (!strcmp(name, "MENU")) { game_menu::command(arg); return 0; }
  if (!strcmp(name, "OSK")) { std::string a = arg; std::replace(a.begin(), a.end(), ':', ' '); osk::command(a); return 0; }   // #144 "OSK type:My_name" / key:Done / move:down / a / b / done / cancel (tests)
  if (!strcmp(name, "MVP")) { movement::command(arg); return 0; }   // #144 "MVP new:My_preset" / use:0 / rename:0:Name / delete:0 / save:0 / set:3:150 (tests)
  if (!strcmp(name, "BUG")) { bug_capture::trigger(); return 0; }   // #135 "BUG": a bug capture now (testing)   // #132 "MENU open" / "MENU page:1" / "MENU focus:<key>" ... (testing)
  if (!strcmp(name, "FPS")) { settings::g.fps_target = atoi(arg); logf("[input-file] fps %s\n", arg); return 0; }   // like the menu (not saved)
  if (!strcmp(name, "FOV")) { settings::g.fov_percent = atoi(arg); logf("[input-file] fov %s\n", arg); return 0; }   // like the slider (not saved)
  if (!strcmp(name, "TEXMATCH")) { settings::g.tex_match = atoi(arg); logf("[input-file] tex_match %s\n", arg); return 0; }   // custom texture colour match % (not saved)
  if (!strcmp(name, "TEXVIEW")) { tex_inspector::toggle(); return 0; }   // F3 texture viewer (testing)
  if (!strcmp(name, "PAINT")) { tex_inspector::paint(arg, true); return 0; }    // "PAINT <tex1_ key>" (testing)
  if (!strcmp(name, "KEY")) {   // "KEY F1" / "KEY F2" (testing: the movement keys without keyboard focus)
    if (!strcmp(arg, "F1")) settings::cycle_movement_preset();
    else if (!strcmp(arg, "F2")) movement::toggle_overlay();
    else if (!strcmp(arg, "F4")) settings::cycle_light_view();
    return 0;
  }
  if (!strcmp(name, "TEXLOG")) { tex_inspector::log_used(arg); return 0; }       // "TEXLOG <label>" (testing)
  if (n >= 2) ms = atoi(arg);
  struct { const char* n; uint16_t m; } map[] = {{"A", PAD_A}, {"B", PAD_B}, {"X", PAD_X}, {"Y", PAD_Y}, {"START", PAD_START},
      {"Z", PAD_Z}, {"L", PAD_L}, {"R", PAD_R}, {"UP", PAD_UP}, {"DOWN", PAD_DOWN}, {"LEFT", PAD_LEFT}, {"RIGHT", PAD_RIGHT}};
  for (auto& e : map) if (!strcmp(e.n, name)) held = e.m;
  until = now + (uint64_t)ms * 1000000ull;
  logf("[input-file] %s %d ms\n", name, ms);
  return held;
}

static uint16_t scripted_buttons() {
  static uint64_t t0 = sys_time_ns();
  double now = (sys_time_ns() - t0) / 1e9;
  uint16_t m = 0;
  for (auto& s : g_script) if (now >= s.t0 && now < s.t0 + s.dur) m |= s.buttons;
  return m;
}

PadState input_poll() {
  PadState s{};
  s.stick_x = s.stick_y = s.c_x = s.c_y = 0;
  ensure_gamepad();
  uint16_t b = 0;
  int8_t sx = 0, sy = 0, cx = 0, cy = 0;
  uint8_t lt = 0, rt = 0;

  // --quiet (tests): the user's own pad / keyboard / mouse are ignored (their play and the test do not mix)
  // #169 controls (controls.cpp): the pad and the keyboard / mouse through the player's bindings (the defaults are the
  // mapping that was here: pad A/B/X/Y, RB = Z, LT or LB = L, RT = R; keyboard WASD, arrows, Space, Shift, C, V, Tab ...)
  const bool* k = g_quiet ? nullptr : SDL_GetKeyboardState(nullptr);
  controls::State cs;
  if (!g_quiet) {
    controls::poll(g_pad, k, cs);
    b |= cs.buttons;
    sx = cs.sx; sy = cs.sy; cx = cs.cx; cy = cs.cy;
    lt = cs.lt; rt = cs.rt;
  }

  // F3: texture viewer (textures drawn now, their file names, paint one in the game)
  {
    static bool was = false;
    bool now = k && k[SDL_SCANCODE_F3];
    if (now && !was) tex_inspector::toggle();
    was = now;
  }
  // F1: movement preset Original -> each saved preset -> Original (movement.cpp)
  {
    static bool was = false;
    bool now = k && k[SDL_SCANCODE_F1];
    if (now && !was) settings::cycle_movement_preset();
    was = now;
  }
  // F4: lighting debug view (final / vanilla only / real-time layer / shadows / AO)
  {
    static bool was = false;
    bool now = k && k[SDL_SCANCODE_F4];
    if (now && !was) settings::cycle_light_view();
    was = now;
  }
  // F2: the movement Tweak window (movement.cpp; the game gets no input while it is open)
  {
    static bool was = false;
    bool now = k && k[SDL_SCANCODE_F2];
    if (now && !was) movement::toggle_overlay();
    was = now;
  }
  // F11 or Alt+Enter: toggle fullscreen (edge-triggered)
  {
    static bool was = false;
    bool now = k && (k[SDL_SCANCODE_F11] || ((k[SDL_SCANCODE_LALT] || k[SDL_SCANCODE_RALT]) && k[SDL_SCANCODE_RETURN]));
    if (now && !was) gxlle::toggle_fullscreen();
    was = now;
    if (now) b &= ~PAD_A;   // Alt+Enter must not also press A
  }

  // fast-forward for testing: hold the backtick key (4x). A --timescale option sets the base speed.
  {
    extern double g_base_scale;
    g_time_scale = (k && k[SDL_SCANCODE_GRAVE]) ? 4.0 * g_base_scale : g_base_scale;
  }

  // mouse look on the C stick (hold the right mouse button)
  float mx = 0, my = 0;
  SDL_MouseButtonFlags mb = SDL_GetRelativeMouseState(&mx, &my);
  (void)mb;
  if ((mb & SDL_BUTTON_RMASK) && !g_quiet) {
    cx = (int8_t)std::clamp((int)(cx + mx * 6.0f), -127, 127);
    cy = (int8_t)std::clamp((int)(cy - my * 6.0f), -127, 127);
  }

  // camera options: sensitivity and inversion on the C stick (pad, arrows and mouse look alike)
  {
    float sens = std::clamp(settings::g.cam_sens, 25, 300) / 100.0f;
    int x = (int)std::lround(cx * sens), y = (int)std::lround(cy * sens);
    if (settings::g.invert_cam_x) x = -x;
    if (settings::g.invert_cam_y) y = -y;
    cx = (int8_t)std::clamp(x, -127, 127);
    cy = (int8_t)std::clamp(y, -127, 127);
  }
  {   // #132 the in-game settings menu: the raw pad / keyboard (before the game's mapping), then it owns them while open
    game_menu::In mi{};
    if (g_pad && !g_quiet) {
      auto btn = [&](SDL_GamepadButton k2) { return SDL_GetGamepadButton(g_pad, k2); };
      const int ly = SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_LEFTY), lx = SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_LEFTX);
      mi.back = btn(SDL_GAMEPAD_BUTTON_BACK);   // #169: replaced below by the "Settings menu" binding while the menu is closed
      mi.start = btn(SDL_GAMEPAD_BUTTON_START);
      mi.a = btn(SDL_GAMEPAD_BUTTON_SOUTH); mi.b = btn(SDL_GAMEPAD_BUTTON_EAST); mi.x = btn(SDL_GAMEPAD_BUTTON_WEST); mi.y = btn(SDL_GAMEPAD_BUTTON_NORTH);
      mi.lb = btn(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER); mi.rb = btn(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
      mi.lt = SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000;
      mi.rt = SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000;
      mi.up = btn(SDL_GAMEPAD_BUTTON_DPAD_UP) || ly < -20000; mi.down = btn(SDL_GAMEPAD_BUTTON_DPAD_DOWN) || ly > 20000;
      mi.left = btn(SDL_GAMEPAD_BUTTON_DPAD_LEFT) || lx < -20000; mi.right = btn(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || lx > 20000;
    }
    if (k) {
      mi.esc = k[SDL_SCANCODE_ESCAPE]; mi.enter = k[SDL_SCANCODE_RETURN] || k[SDL_SCANCODE_KP_ENTER]; mi.bksp = k[SDL_SCANCODE_BACKSPACE];
      mi.q = k[SDL_SCANCODE_Q]; mi.e = k[SDL_SCANCODE_E]; mi.pgup = k[SDL_SCANCODE_PAGEUP]; mi.pgdn = k[SDL_SCANCODE_PAGEDOWN];
      mi.r = k[SDL_SCANCODE_R]; mi.kup = k[SDL_SCANCODE_UP]; mi.kdown = k[SDL_SCANCODE_DOWN];
      mi.kleft = k[SDL_SCANCODE_LEFT]; mi.kright = k[SDL_SCANCODE_RIGHT];
    }
    // #169 the "Settings menu" binding opens the menu; once open, Back / Esc also still close it (and cancel lists)
    if (!settings::menu_open()) { mi.back = cs.menu_pad; mi.esc = cs.menu_keys; }
    else { mi.back = mi.back || cs.menu_pad; mi.esc = mi.esc || cs.menu_keys; }
    game_menu::input(mi);
    {   // #135 bug capture: F12, or pad Back + Start together (#170: off by default)
      static bool was = false;
      const bool now = cs.bug;   // #169 the "Bug report" binding (default F12, pad Back + Start)
      if (now && !was && settings::g.bug_key) bug_capture::trigger();   // #170: only with "Bug report key" on (System page)
      was = now;
    }
  }
  if (settings::menu_open()) {   // the menu owns keyboard/mouse/pad while open
    b = 0; sx = sy = cx = cy = 0; lt = rt = 0;
  }
  b |= scripted_buttons();
  {   // INPUT_FILE presses act like the player's pad
    tp_apply();
    uint16_t fb = file_buttons();
    static uint16_t prev_fb = 0;
    uint16_t edge = fb & ~prev_fb;
    prev_fb = fb;
    (void)edge;
    b |= fb;
  }
  {   // a button still held when a menu closes (the B that closed it...) must not reach the game as a fresh press
    static bool was_open = false;
    static uint16_t swallow = 0;
    bool open = settings::menu_open();
    if (was_open && !open) swallow = b;
    swallow &= b;   // released buttons work again
    b &= ~swallow;
    was_open = open;
  }
  if (sys_time_ns() < g_file_stick_until) sy = 110;
  if (sys_time_ns() < g_file_cam_until) cx = 90;
  {
    extern void cam_recentre_input(uint16_t, int8_t&, int8_t);   // cam_recentre.cpp: #121 L recentres the camera
    cam_recentre_input(b, cx, cy);
  }
  const uint16_t user_b = b;   // the player's own buttons (not the automatic movie-skip / --fast presses)
  g_user_buttons = user_b;
  b |= movie_skip_buttons();
  b |= mash_buttons();
  b |= g_auto_buttons;
  {   // START edge for gameplay hooks (skip-any-cutscene): only presses the player made
    static bool was = false;
    bool now = (user_b & PAD_START) != 0;
    if (now && !was) g_start_press_ns = sys_time_ns();
    was = now;
  }
  s.buttons = b;
  {
    static const bool trace = getenv("INPUT_TRACE") != nullptr;
    static uint16_t last = 0;
    if (trace && b != last) { logf("[input] buttons %04X (menu_open=%d)\n", b, (int)settings::menu_open()); last = b; }
  }
  s.stick_x = sx; s.stick_y = sy; s.c_x = cx; s.c_y = cy;
  s.trig_l = lt; s.trig_r = rt;
  return s;
}
