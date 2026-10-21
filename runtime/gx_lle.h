#pragma once
#include <cstdint>
struct SDL_Window;

namespace gxlle {
extern bool g_trace;
void init_window();
void present_frame();          // end the current Aurora frame, pump window events, start the next
void present_frame_events();   // pump window events only
bool ui_frame(void (*draw)());  // launcher: events + one ImGui-only frame; false = window closed
void ui_resume_game();          // launcher done: game window size / fullscreen, then the frame the game's first present expects
extern bool g_launcher_mode;    // set by the launcher before init_window: compact window, fullscreen deferred to Play
void toggle_fullscreen();       // F11 / Alt+Enter
extern int g_render_height;    // render height in pixels (0 = window height)
extern bool g_widescreen;      // --widescreen
extern bool g_game_bloom;      // settings: draw the game's bloom/glow pass
extern uint64_t g_prof_ns[4];  // FPS_LOG profiling (ns): gp_run, Aurora processing, present
uint64_t xfb_count();          // frames displayed so far (hang watchdog)
int effective_render_height(); // render height actually in use
void apply_display_settings(); // re-apply g_render_height / g_widescreen (settings UI)
bool is_fullscreen();
float widescreen_camera_factor(); // output aspect / (4/3) when widescreen, else 1 (3D camera widening)
void fit_window_to_aspect();   // windowed: resize width to the current aspect
void resize_window_pixels(int w, int h);   // windowed: client area of w x h pixels (clamped to the desktop)
extern ::SDL_Window* g_window;   // the game window (SDL)
extern bool g_bp_status;
extern bool g_gp_no_halt;      // frame_rate.cpp: custom frame rate - a long-parked GP passes its breakpoint
void resume_gp();
void gp_run();
void print_stats();
void pe_finish();
void pe_token();
}
