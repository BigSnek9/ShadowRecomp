#pragma once
// #143 the game's textures as PNG in <game>\textures\Original (in-game menu, System page). texture_extract.cpp.
#include <string>
namespace texture_extract {
void ensure_folders();      // at start: textures\Custom + textures\Original (+ README.txt) next to the game
void start();               // extract on a worker thread (no-op while one runs)
int state();                // 0 idle, 1 reading, 2 writing, 3 done, 4 failed
std::string status();       // one line for the menu
void open_folder();         // Explorer on <game>\textures
}  // namespace texture_extract
