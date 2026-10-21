#pragma once
// #144 on-screen keyboard: names for the player's movement presets, typed with a pad, a mouse or the
// keyboard. One card over the in-game menu (docs/launcher/IN-GAME-MENU-PRESETS.md); the menu (game_menu.cpp) opens it,
// hands it the raw controls while it is open and draws it in its own colours.
//   pad: d-pad / stick move, A types the key, B deletes (B on an empty name = cancel), X space, Y shift, Start done,
//        Back cancel, LB / RB move the caret
//   keyboard: type, Enter done, Esc cancel, Backspace, Left / Right the caret
//   mouse: click a key; click the name to put the caret there
#include <functional>
#include <string>
#include <imgui.h>

namespace osk {
struct Keys {   // raw state this poll (held), the caller fills it from the pad / keyboard
  bool up, down, left, right, a, b, x, y, start, back, lb, rb;   // pad
  bool enter, esc, bksp, kleft, kright, kup, kdown;               // keyboard (letters come as text input)
};
struct Style {   // the menu's tint (game_menu.cpp Tint) as colours, and its fonts
  ImU32 panel, text, dim, line, track, focus, accent, keycap, scrim;
  ImFont* font = nullptr;   // body
  ImFont* bold = nullptr;   // title + key labels
};
// check(name): nullptr = fine, else the reason shown under the field (Done is refused while there is one)
using Check = std::function<const char*(const std::string&)>;
using Done = std::function<void(bool ok, const std::string& text)>;
void open(const std::string& title, const std::string& initial, size_t max_len, Check check, Done done);
bool active();
void close();                     // = cancel
bool input(const Keys& k);        // every input poll while active(); true = it used the controls
void draw(ImDrawList* dl, ImVec2 a, ImVec2 b, float u, const Style& st);   // inside the menu's frame; a..b = the area to cover
void command(const std::string& s);   // tests (INPUT_FILE "MENU osk:<cmd>"): type:<text> ('_' = space), key:<label>, move:<dir>, a, b, x, y, done, cancel
}  // namespace osk
