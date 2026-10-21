#pragma once
// #132 in-game settings menu: Xbox 360-style blades over the running game. Look:
// docs/launcher/IN-GAME-MENU-STYLE.md; content: the runtime/menu_schema.h. The game is never paused; while the
// menu is open its input goes to the menu (settings::menu_open()).
#include <string>

namespace game_menu {
// raw controls this poll (input.cpp builds it from the pad / keyboard before the game sees them)
struct In {
  bool back, a, b, x, y, lb, rb, lt, rt, up, down, left, right, start;   // pad (sticks folded into the directions; x: #144 on-screen keyboard space)
  bool esc, enter, bksp, q, e, pgup, pgdn, r, kup, kdown, kleft, kright, f5;   // keyboard
};
void input(const In& in);            // every input poll
bool is_open();                      // open, or still animating closed
void draw();                         // inside the ImGui frame (settings::draw)
void init_fonts();                   // with the other fonts, before the atlas is built
void command(const std::string& s);  // INPUT_FILE "MENU <cmd>" (tests): open, close, up, down, left, right, a, b, y,
                                     // page <n>, next, prev, section <n>, focus <key>
}  // namespace game_menu
