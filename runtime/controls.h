#pragma once
// #169 controls (2026-10-21; user: "a controls menu and allow full rebind of every button over any input device"):
// every game button, both sticks, the settings-menu key and the bug report key can be bound to keyboard keys, mouse
// buttons and controller buttons / stick and trigger directions. Two profiles: Controller (any SDL gamepad) and Keyboard
// and mouse; each action holds a list of alternatives (any one of them presses it), an alternative may be a combination
// ("Back + Start"). Saved in shadow_settings.ini as bind_pad_<action> / bind_kb_<action>. The defaults are the controls
// as they were. The settings menu itself keeps its own fixed navigation (a bad binding can never lock it).
#include <cstdint>
#include <cstdio>
#include <string>
#include <SDL3/SDL.h>

namespace controls {
enum Action { A, B, X, Y, Z, L, R, START, DUP, DDOWN, DLEFT, DRIGHT, SUP, SDOWN, SLEFT, SRIGHT, CUP, CDOWN, CLEFT, CRIGHT,
              MENU, BUG, kNumActions };
enum Profile { Pad, Keys, kNumProfiles };
const char* action_label(int a);                   // "A (jump)", "Main stick up", ...
const char* profile_label(int p);                  // "Controller", "Keyboard and mouse"
std::string binding_text(int p, int a);            // "Space, Enter" / "Left stick up" / "Back + Start" / "Not set"

struct State {
  uint16_t buttons = 0;                            // PAD_* bits (game buttons only)
  int8_t sx = 0, sy = 0, cx = 0, cy = 0;           // sticks
  uint8_t lt = 0, rt = 0;                          // analog triggers
  bool menu_pad = false, menu_keys = false, bug = false;
};
// every input poll (not in --quiet): pad may be null, keys = SDL_GetKeyboardState
void poll(SDL_Gamepad* pad, const bool* keys, State& out);

// press-to-bind (the Controls page): the next new input of that profile replaces the action's bindings; Esc cancels,
// so does 8 s without input. Waits for everything to be released first (the press that started it does not count).
void begin_capture(int p, int a);
bool capturing();
bool capturing(int p, int a);
void cancel_capture();

void reset(int p, int a);                          // one action back to its default
void reset_all();
bool load(const char* key, const char* value);     // shadow_settings.ini line (true = it was a bind_ key)
void save(FILE* f);
}  // namespace controls
