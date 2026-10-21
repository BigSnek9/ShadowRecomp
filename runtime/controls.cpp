// #169 controls: rebinding of every game input (controls.h). Bindings, defaults, the poll that turns devices into the
// game's pad, press-to-bind, ini load / save.
#include "controls.h"
#include "input.h"
#include "sys.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

void controls_changed();   // settings_ui.cpp: save the settings (a binding changed)

namespace controls {
namespace {
// one input: a keyboard key (scancode), a mouse button (1..5), a gamepad button, or a gamepad axis direction
enum Kind : uint8_t { None, Key, Mouse, Button, AxisPos, AxisNeg };
struct In { Kind k = None; int code = 0; };
struct Alt { In a, b; };   // b set = a combination (both held)
using Binds = std::vector<Alt>;
Binds g_binds[kNumProfiles][kNumActions];
bool g_init = false;

const char* kNames[kNumActions] = {"a", "b", "x", "y", "z", "l", "r", "start", "dpad_up", "dpad_down", "dpad_left", "dpad_right",
                                   "stick_up", "stick_down", "stick_left", "stick_right", "cstick_up", "cstick_down", "cstick_left",
                                   "cstick_right", "menu", "bug"};
const char* kLabels[kNumActions] = {"A (jump)", "B (action)", "X (weapon)", "Y (weapon / car)", "Z (lock-on)", "L (camera)",
                                    "R (camera)", "START (pause)", "D-pad up", "D-pad down", "D-pad left", "D-pad right",
                                    "Move up", "Move down", "Move left", "Move right", "Camera up", "Camera down", "Camera left",
                                    "Camera right", "Settings menu", "Bug report (System page switch)"};

In K(SDL_Scancode s) { return {Key, int(s)}; }
In Bt(SDL_GamepadButton b) { return {Button, int(b)}; }
In Ax(SDL_GamepadAxis a, bool pos) { return {pos ? AxisPos : AxisNeg, int(a)}; }
Binds one(In a) { return {Alt{a, {}}}; }
Binds def(int p, int a) {   // the controls as they were before #169 (input.cpp)
  if (p == Pad) switch (a) {
    case A: return one(Bt(SDL_GAMEPAD_BUTTON_SOUTH)); case B: return one(Bt(SDL_GAMEPAD_BUTTON_EAST));
    case X: return one(Bt(SDL_GAMEPAD_BUTTON_WEST)); case Y: return one(Bt(SDL_GAMEPAD_BUTTON_NORTH));
    case Z: return one(Bt(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER));
    case L: return {Alt{Ax(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, true), {}}, Alt{Bt(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER), {}}};
    case R: return one(Ax(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, true));
    case START: return one(Bt(SDL_GAMEPAD_BUTTON_START));
    case DUP: return one(Bt(SDL_GAMEPAD_BUTTON_DPAD_UP)); case DDOWN: return one(Bt(SDL_GAMEPAD_BUTTON_DPAD_DOWN));
    case DLEFT: return one(Bt(SDL_GAMEPAD_BUTTON_DPAD_LEFT)); case DRIGHT: return one(Bt(SDL_GAMEPAD_BUTTON_DPAD_RIGHT));
    case SUP: return one(Ax(SDL_GAMEPAD_AXIS_LEFTY, false)); case SDOWN: return one(Ax(SDL_GAMEPAD_AXIS_LEFTY, true));
    case SLEFT: return one(Ax(SDL_GAMEPAD_AXIS_LEFTX, false)); case SRIGHT: return one(Ax(SDL_GAMEPAD_AXIS_LEFTX, true));
    case CUP: return one(Ax(SDL_GAMEPAD_AXIS_RIGHTY, false)); case CDOWN: return one(Ax(SDL_GAMEPAD_AXIS_RIGHTY, true));
    case CLEFT: return one(Ax(SDL_GAMEPAD_AXIS_RIGHTX, false)); case CRIGHT: return one(Ax(SDL_GAMEPAD_AXIS_RIGHTX, true));
    case MENU: return one(Bt(SDL_GAMEPAD_BUTTON_BACK));
    case BUG: return {Alt{Bt(SDL_GAMEPAD_BUTTON_BACK), Bt(SDL_GAMEPAD_BUTTON_START)}};
  }
  else switch (a) {
    case A: return {Alt{K(SDL_SCANCODE_SPACE), {}}, Alt{K(SDL_SCANCODE_RETURN), {}}, Alt{K(SDL_SCANCODE_KP_ENTER), {}}};
    case B: return {Alt{K(SDL_SCANCODE_LSHIFT), {}}, Alt{K(SDL_SCANCODE_X), {}}, Alt{K(SDL_SCANCODE_BACKSPACE), {}}};
    case X: return one(K(SDL_SCANCODE_C)); case Y: return one(K(SDL_SCANCODE_V));
    case Z: return {Alt{K(SDL_SCANCODE_R), {}}, Alt{K(SDL_SCANCODE_LCTRL), {}}};
    case L: return one(K(SDL_SCANCODE_Q)); case R: return one(K(SDL_SCANCODE_E));
    case START: return one(K(SDL_SCANCODE_TAB));
    case DUP: return one(K(SDL_SCANCODE_T)); case DDOWN: return one(K(SDL_SCANCODE_G));
    case DLEFT: return one(K(SDL_SCANCODE_F)); case DRIGHT: return one(K(SDL_SCANCODE_H));
    case SUP: return one(K(SDL_SCANCODE_W)); case SDOWN: return one(K(SDL_SCANCODE_S));
    case SLEFT: return one(K(SDL_SCANCODE_A)); case SRIGHT: return one(K(SDL_SCANCODE_D));
    case CUP: return one(K(SDL_SCANCODE_UP)); case CDOWN: return one(K(SDL_SCANCODE_DOWN));
    case CLEFT: return one(K(SDL_SCANCODE_LEFT)); case CRIGHT: return one(K(SDL_SCANCODE_RIGHT));
    case MENU: return one(K(SDL_SCANCODE_ESCAPE));
    case BUG: return one(K(SDL_SCANCODE_F12));
  }
  return {};
}
void init() {
  if (g_init) return;
  g_init = true;
  for (int p = 0; p < kNumProfiles; ++p) for (int a = 0; a < kNumActions; ++a) g_binds[p][a] = def(p, a);
}

// ---- text forms: ini "k:44,b:0&b:6" and names for the menu
std::string enc(const In& i) {
  const char* t = i.k == Key ? "k" : i.k == Mouse ? "m" : i.k == Button ? "b" : i.k == AxisPos ? "a+" : i.k == AxisNeg ? "a-" : "";
  return std::string(t) + ":" + std::to_string(i.code);
}
In dec(const std::string& s) {
  const size_t c = s.find(':');
  if (c == std::string::npos) return {};
  const std::string t = s.substr(0, c);
  const int code = atoi(s.c_str() + c + 1);
  return {t == "k" ? Key : t == "m" ? Mouse : t == "b" ? Button : t == "a+" ? AxisPos : t == "a-" ? AxisNeg : None, code};
}
std::string name(const In& i) {
  switch (i.k) {
    case Key: { const char* n = SDL_GetScancodeName(SDL_Scancode(i.code)); return n && *n ? n : "Key " + std::to_string(i.code); }
    case Mouse: return i.code == 1 ? "Left mouse button" : i.code == 2 ? "Middle mouse button" : i.code == 3 ? "Right mouse button"
                     : "Mouse button " + std::to_string(i.code);
    case Button: {
      static const char* n[] = {"Pad A", "Pad B", "Pad X", "Pad Y", "Back", "Guide", "Start", "Left stick press", "Right stick press",
                                "LB", "RB", "D-pad up", "D-pad down", "D-pad left", "D-pad right", "Misc", "Right paddle 1",
                                "Left paddle 1", "Right paddle 2", "Left paddle 2", "Touchpad"};
      return i.code >= 0 && i.code < int(sizeof n / sizeof n[0]) ? n[i.code] : "Pad button " + std::to_string(i.code);
    }
    case AxisPos: case AxisNeg: {
      const bool pos = i.k == AxisPos;
      switch (i.code) {
        case SDL_GAMEPAD_AXIS_LEFTX: return pos ? "Left stick right" : "Left stick left";
        case SDL_GAMEPAD_AXIS_LEFTY: return pos ? "Left stick down" : "Left stick up";
        case SDL_GAMEPAD_AXIS_RIGHTX: return pos ? "Right stick right" : "Right stick left";
        case SDL_GAMEPAD_AXIS_RIGHTY: return pos ? "Right stick down" : "Right stick up";
        case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: return "LT";
        case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: return "RT";
      }
      return "Axis " + std::to_string(i.code) + (pos ? "+" : "-");
    }
    default: return "";
  }
}

// ---- reading one input now: 0..1 (axes analog, everything else 0 or 1)
uint32_t g_mouse = 0;
float level(const In& i, SDL_Gamepad* pad, const bool* keys) {
  switch (i.k) {
    case Key: return keys && i.code > 0 && i.code < SDL_SCANCODE_COUNT && keys[i.code] ? 1.f : 0.f;
    case Mouse: return i.code >= 1 && i.code <= 5 && (g_mouse & SDL_BUTTON_MASK(i.code)) ? 1.f : 0.f;
    case Button: return pad && SDL_GetGamepadButton(pad, SDL_GamepadButton(i.code)) ? 1.f : 0.f;
    case AxisPos: case AxisNeg: {
      if (!pad) return 0;
      const float v = SDL_GetGamepadAxis(pad, SDL_GamepadAxis(i.code)) / 32767.f * (i.k == AxisPos ? 1 : -1);
      return v > 0.10f ? std::min(1.f, v) : 0.f;   // the old 10 % deadzone
    }
    default: return 0;
  }
}
float alt_level(const Alt& a, SDL_Gamepad* pad, const bool* keys) {
  const float x = level(a.a, pad, keys);
  return a.b.k == None ? x : std::min(x, level(a.b, pad, keys) > 0 ? 1.f : 0.f);
}
float act(int p, int a, SDL_Gamepad* pad, const bool* keys) {   // the strongest alternative
  float v = 0;
  for (const Alt& x : g_binds[p][a]) v = std::max(v, alt_level(x, pad, keys));
  return v;
}
bool is_analog(const Alt& a) { return a.b.k == None && (a.a.k == AxisPos || a.a.k == AxisNeg); }

// ---- press-to-bind
int g_cap_p = -1, g_cap_a = -1;
bool g_cap_released = false;
uint64_t g_cap_t = 0;
bool held_any(int p, SDL_Gamepad* pad, const bool* keys) {
  if (p == Keys) {
    if (g_mouse & 0x1F) return true;
    for (int s = 1; keys && s < SDL_SCANCODE_COUNT; ++s) if (keys[s]) return true;
    return false;
  }
  if (!pad) return false;
  for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b) if (SDL_GetGamepadButton(pad, SDL_GamepadButton(b))) return true;
  for (int x = 0; x < SDL_GAMEPAD_AXIS_COUNT; ++x) if (std::abs(SDL_GetGamepadAxis(pad, SDL_GamepadAxis(x))) > 16000) return true;
  return false;
}
In first_held(int p, SDL_Gamepad* pad, const bool* keys) {
  if (p == Keys) {
    for (int s = 1; keys && s < SDL_SCANCODE_COUNT; ++s) if (keys[s]) return K(SDL_Scancode(s));
    for (int m = 1; m <= 5; ++m) if (g_mouse & SDL_BUTTON_MASK(m)) return {Mouse, m};
    return {};
  }
  if (!pad) return {};
  for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b) if (SDL_GetGamepadButton(pad, SDL_GamepadButton(b))) return Bt(SDL_GamepadButton(b));
  for (int x = 0; x < SDL_GAMEPAD_AXIS_COUNT; ++x) {
    const int v = SDL_GetGamepadAxis(pad, SDL_GamepadAxis(x));
    if (std::abs(v) > 20000) return Ax(SDL_GamepadAxis(x), v > 0);
  }
  return {};
}
void capture_step(SDL_Gamepad* pad, const bool* keys) {
  if (g_cap_p < 0) return;
  if (keys && keys[SDL_SCANCODE_ESCAPE] && g_cap_released) { logf("[controls] binding cancelled\n"); cancel_capture(); return; }
  if (sys_time_ns() - g_cap_t > 8000000000ull) { logf("[controls] binding timed out\n"); cancel_capture(); return; }
  const bool any = held_any(g_cap_p, pad, keys) || (g_cap_p == Pad && keys && keys[SDL_SCANCODE_ESCAPE]);
  if (!g_cap_released) { if (!any) g_cap_released = true; return; }
  const In i = first_held(g_cap_p, pad, keys);
  if (i.k == None) return;
  g_binds[g_cap_p][g_cap_a] = one(i);
  logf("[controls] %s: %s = %s\n", profile_label(g_cap_p), kLabels[g_cap_a], name(i).c_str());
  g_cap_p = g_cap_a = -1;
  ::controls_changed();
}
}  // namespace

const char* action_label(int a) { return a >= 0 && a < kNumActions ? kLabels[a] : ""; }
const char* profile_label(int p) { return p == Pad ? "Controller" : "Keyboard and mouse"; }
std::string binding_text(int p, int a) {
  init();
  std::string s;
  for (const Alt& x : g_binds[p][a]) {
    if (!s.empty()) s += ", ";
    s += name(x.a);
    if (x.b.k != None) s += " + " + name(x.b);
  }
  return s.empty() ? "Not set" : s;
}

void poll(SDL_Gamepad* pad, const bool* keys, State& o) {
  init();
  float mx = 0, my = 0;
  g_mouse = SDL_GetMouseState(&mx, &my);
  capture_step(pad, keys);
  o = State{};
  if (g_cap_p >= 0) return;   // binding: the game gets nothing (the menu is open anyway)
  static const uint16_t bits[] = {PAD_A, PAD_B, PAD_X, PAD_Y, PAD_Z, PAD_L, PAD_R, PAD_START, PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT};
  for (int a = A; a <= DRIGHT; ++a)
    for (int p = 0; p < kNumProfiles; ++p) if (act(p, a, pad, keys) >= (a == L || a == R ? 170.f / 255 : 0.5f)) o.buttons |= bits[a];
  // analog triggers: the strongest L / R binding (a key or button = fully pressed)
  o.lt = uint8_t(std::lround(std::max(act(Pad, L, pad, keys), act(Keys, L, pad, keys)) * 255));
  o.rt = uint8_t(std::lround(std::max(act(Pad, R, pad, keys), act(Keys, R, pad, keys)) * 255));
  // sticks: controller profile analog; the keyboard profile (digital) overrides when used, diagonals normalised
  auto stick = [&](int up, int down, int left, int right, int8_t& x, int8_t& y) {
    const float px = act(Pad, right, pad, keys) - act(Pad, left, pad, keys), py = act(Pad, up, pad, keys) - act(Pad, down, pad, keys);
    x = int8_t(std::clamp(int(px * 127.f), -127, 127));
    y = int8_t(std::clamp(int(py * 127.f), -127, 127));
    const float kx = act(Keys, right, pad, keys) - act(Keys, left, pad, keys), ky = act(Keys, up, pad, keys) - act(Keys, down, pad, keys);
    if (kx != 0 || ky != 0) {
      const float len = std::sqrt(kx * kx + ky * ky);
      x = int8_t(kx / len * 100); y = int8_t(ky / len * 100);
    }
  };
  stick(SUP, SDOWN, SLEFT, SRIGHT, o.sx, o.sy);
  stick(CUP, CDOWN, CLEFT, CRIGHT, o.cx, o.cy);
  o.menu_pad = act(Pad, MENU, pad, keys) >= 0.5f;
  o.menu_keys = act(Keys, MENU, pad, keys) >= 0.5f;
  o.bug = act(Pad, BUG, pad, keys) >= 0.5f || act(Keys, BUG, pad, keys) >= 0.5f;
}

void begin_capture(int p, int a) {
  init();
  if (p < 0 || p >= kNumProfiles || a < 0 || a >= kNumActions) return;
  g_cap_p = p; g_cap_a = a; g_cap_released = false; g_cap_t = sys_time_ns();
  logf("[controls] press an input for %s (%s)\n", kLabels[a], profile_label(p));
}
bool capturing() { return g_cap_p >= 0; }
bool capturing(int p, int a) { return g_cap_p == p && g_cap_a == a; }
void cancel_capture() { g_cap_p = g_cap_a = -1; }
void reset(int p, int a) {
  init();
  if (p < 0 || p >= kNumProfiles || a < 0 || a >= kNumActions) return;
  g_binds[p][a] = def(p, a);
  ::controls_changed();
}
void reset_all() {
  for (int p = 0; p < kNumProfiles; ++p) for (int a = 0; a < kNumActions; ++a) g_binds[p][a] = def(p, a);
  g_init = true;
  logf("[controls] all controls back to their defaults\n");
  ::controls_changed();
}

bool load(const char* key, const char* value) {
  init();
  int p = -1;
  const char* rest = nullptr;
  if (!strncmp(key, "bind_pad_", 9)) { p = Pad; rest = key + 9; }
  else if (!strncmp(key, "bind_kb_", 8)) { p = Keys; rest = key + 8; }
  else return false;
  for (int a = 0; a < kNumActions; ++a) {
    if (strcmp(rest, kNames[a])) continue;
    Binds b;
    std::string v = value;
    while (!v.empty() && (v.back() == '\n' || v.back() == '\r' || v.back() == ' ')) v.pop_back();
    for (size_t i = 0; i <= v.size() && !v.empty();) {
      size_t j = v.find(',', i);
      if (j == std::string::npos) j = v.size();
      const std::string tok = v.substr(i, j - i);
      const size_t amp = tok.find('&');
      Alt x{dec(tok.substr(0, amp)), amp == std::string::npos ? In{} : dec(tok.substr(amp + 1))};
      if (x.a.k != None) b.push_back(x);
      i = j + 1;
    }
    g_binds[p][a] = b;   // "bind_kb_x=" (empty) = not bound
    return true;
  }
  return true;   // an unknown action: ignore the line
}
void save(FILE* f) {
  init();
  for (int p = 0; p < kNumProfiles; ++p)
    for (int a = 0; a < kNumActions; ++a) {
      std::string s;
      for (const Alt& x : g_binds[p][a]) {
        if (!s.empty()) s += ",";
        s += enc(x.a);
        if (x.b.k != None) s += "&" + enc(x.b);
      }
      fprintf(f, "bind_%s_%s=%s\n", p == Pad ? "pad" : "kb", kNames[a], s.c_str());
    }
}
}  // namespace controls
