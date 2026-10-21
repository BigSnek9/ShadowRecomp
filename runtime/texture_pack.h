#pragma once
// Custom textures from <exe dir>\textures\Custom (settings "Load custom textures"); see texture_pack.cpp.
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
namespace texture_pack {
void tick();                 // once per frame: follows the setting, reloads when the folder changes
int count();                 // custom textures registered right now
std::string folder_path();
std::filesystem::path textures_dir();   // <exe dir>\textures
// a multi-piece UI folder (Pause menu, HUD gauges, UI text...): textures\Custom\UI\<name>, textures\Custom\<name>,
// else textures\Custom\<any folder>\<name> (e.g. Custom\Shared\Pause menu)
std::filesystem::path ui_dir(const wchar_t* name);
bool read_png(const std::filesystem::path& file, int& w, int& h, std::vector<uint8_t>& rgba);   // any PNG -> RGBA8
bool write_png(const std::vector<uint8_t>& rgba, int w, int h, std::vector<uint8_t>& png);      // RGBA8 -> PNG bytes
}
