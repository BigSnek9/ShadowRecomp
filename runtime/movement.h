#pragma once
// Movement tuning (docs/gameplay/MOVEMENT-REWORK.md), phase 1: gravity / fall speed / air drag / jump / homing bounce.
#include <cstddef>
#include <cstdint>
#include <string>
struct Cpu;
namespace movement {
// once per frame, before Shadow's move step (frame_rate.cpp hle_PlayerMoveStep)
void frame(uint32_t player);
// Tweak menu (user, 2026-10-18): named presets in movement_presets.ini beside shadow.exe ("Original" = the game, read-only)
void cycle();                 // F1: Original -> each saved preset -> Original
const char* active_name();    // "Original", a preset name, or "Custom"
void toggle_overlay();        // F2: the in-game Tweak window
bool overlay_open();
void draw_overlay();          // inside the ImGui frame (settings_ui.cpp draw)
void draw_tweak();            // the menu body (overlay window and the built-in launcher card)
// #144 (user, 2026-10-20): no built-in Reworked; the player creates, names, saves, renames and deletes their own presets
// (in-game menu Movement > Preset, docs/launcher/IN-GAME-MENU-PRESETS.md). Index i = 0 .. preset_count() - 1.
constexpr size_t kMaxName = 24;
int preset_count();
const char* preset_name(int i);
int active_preset();                                      // -1 Original, -2 the sliders unsaved (Custom), else the index
bool preset_modified();                                   // the active preset's sliders differ from what is saved
void use_preset(int i);                                   // -1 Original, -2 Custom (the sliders as they are), else that one
std::string clean_name(const std::string& raw);           // trimmed, no [ ] ; = or control characters, at most kMaxName
const char* name_problem(const std::string& raw, int except = -1);   // nullptr = usable (unique, not Original / Custom)
std::string unique_name(const std::string& base);         // "My preset 1", "My preset 2" ... the first free one
int create_preset(const std::string& name);               // the current sliders (all 100% from Original); -1 = refused
bool rename_preset(int i, const std::string& name);
bool delete_preset(int i);                                // the active one: back to Original
bool save_into(int i);                                    // the current sliders into preset i (Save changes)
void ask_new();                                           // the on-screen keyboard (osk.h) asks the name, then create_preset
void ask_rename(int i);                                   // ... then rename_preset
void command(const std::string& arg);                     // INPUT_FILE "MVP new:<name>" ... (tests)
}
