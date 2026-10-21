#pragma once
#include <cstdint>
struct Cpu;
namespace stage_workshop {
void frame_begin(Cpu& c);   // at the frame start (frame_rate.cpp hle_FramePace): logic + VM object operations
void before_player(uint32_t owner);
void after_player(uint32_t owner, float dt);
void node_enter(uint32_t obj);   // #35 capture: task-tree node run by fn_8004EC68 (game_hooks.cpp hud_element_draw)
void node_leave();
void memory_report(int stage);
// lighting L1: the player's position (the player object the movement step last ran for); false = none known
bool player_position(float out[3]);
// live game view (LIVE-VIEW.md): camera frame override at RwCameraBeginUpdate (widescreen.cpp), FOV (deg, 0 = game's)
void live_camera(uint32_t cam, uint32_t frm);
void live_check(uint32_t frm);
float live_fov();   // [mem] lines at --stage "player active"
}
