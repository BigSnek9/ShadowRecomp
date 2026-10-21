#pragma once
// PC settings: edited in the launcher (launcher.cpp) before the game starts; saved in shadow_settings.ini next to the
// executable. In game there is no settings menu any more (only the F3 texture viewer and F11 fullscreen).
#include <string>
namespace settings {
struct Values {
  int height = 0;           // render resolution height in pixels (0 = Auto: match the window)
  int width = 0;            // chosen resolution width (display only; the render width follows the aspect)
  bool widescreen = false;  // #141 vanilla default: Original 4:3; true = 16:9 widescreen
  bool fullscreen = false;
  int fps_target = 60;      // game frame rate: 60 = original pacing, N = run the game at N fps, 0 = unlimited
  int cutscene_fps_cap = 60;  // #122: frame rate while a cutscene plays (they animate in 1/60 s steps); 0 = no cap
  bool show_fps = false;
  bool game_bloom = true;   // the game's own glow/bloom pass
  int game_bloom_quality = 0;       // 0 Original (the game's blocky upscale, #141 default), 1 Smooth (3x3 tent, linear filter)
  int game_bloom_strength = 100;   // % of the game's bloom (0 = none, 100 = original, up to 200)
  bool clip_ui_43 = true;   // widescreen: hide 2D drawn outside the centred 4:3 area (as on a 4:3 TV)
  bool cutscene_bars = true;// widescreen: show in-engine cutscenes as 4:3 with black bars
  bool gauges_new = false;  // #143: the PC gauges were removed; always false (field kept until the menu schema drops it)
  bool hud_169 = false;     // #143: option removed, always false (the centred 4:3 HUD); the edge placement code stays unused (gx_lle.cpp hud_side_for)
  bool skip_any_cutscene = false;   // allow skipping cutscenes that have not been seen yet
  bool warp_enemy_counts = false;   // #141 default off; #108: checkpoint warp screen: a red number per hexagon = mission enemies left nearest it
  bool unlock_all_stages = false;   // stage select: every stage available (this session only, never saved)
  int fov_percent = 100;    // field of view, % of the original (70-150)
  int cam_sens = 100;       // camera (C-stick / mouse) sensitivity, %
  bool invert_cam_x = false, invert_cam_y = false;
  int skin = 0;                     // #120 character skin (disc.cpp): 0 Shadow, 1 2P Yellow, 2 2P Green, 3 2P Orange, 4 Metal blue,
                                    // 5 Metal purple, 6 Super Shadow, 7 Evil Shadow, 8 Sonic; read at start
  bool cam_recentre = false;        // #141 default off; #121: L swings the camera behind Shadow (cam_recentre.cpp)
  int vol_master = 100, vol_sfx = 100, vol_music = 100, vol_dialogue = 100;   // % of the original (0-200), audio.cpp
  bool mute_background = false;     // #151: silence while the window is unfocused or minimised (audio.cpp)
  bool custom_textures = false;     // load edited textures from textures\Custom (texture_pack.cpp)
  int tex_match = 0;                // %: custom textures pulled toward the ORIGINAL texture's colours, live (texture_pack.cpp colour_match; 0 = off)
  // HDR output (Aurora gpu.cpp: FP16 scRGB swapchain + present-pass transform), all live via apply_hdr()
  bool hdr = false;
  int hdr_paper_white = 200;        // nits of the game's white
  int hdr_peak = 1000;              // display peak nits (highlight target + soft-clip limit)
  int hdr_highlight = 20;           // % of the way the brightest SDR parts are pushed toward the peak (inverse tone mapping)
  int hdr_saturation = 100;         // %
  int hdr_vibrance = 0;             // %
  int hdr_contrast = 100;           // %
  int hdr_black = 0;                // black level, thousandths (-20..20; <0 crushes, >0 lifts)
  int hdr_ui_white = 200;           // PC menus brightness, nits
  bool hdr_test = false;            // calibration pattern (session only)
  // #147 colour pass (Aurora present pass, SDR and HDR, live via apply_hdr()). All neutral = the pass is skipped (vanilla)
  int col_black = 0;                // black level, thousandths of white (-50..50; <0 crushes, >0 lifts)
  int col_white = 100;              // white level % (50..150)
  int col_contrast = 100;           // % around mid grey (50..150)
  int col_saturation = 100;         // % (0..200)
  int col_vibrance = 0;             // % (-100..100): dull colours change more than saturated ones
  int col_r = 100, col_g = 100, col_b = 100;   // channel gains % (50..150)
  int col_gamma = 100;              // % (50..200): >100 brightens the mid tones
  int col_exposure = 0;             // tenths of a stop (-20..20)
  // #148 contact AO (lighting_contact_ao.cpp, docs/lighting/AO-148.md): off by default, independent of the shadows
  bool ao = false;
  int ao_strength = 100;            // % of the tuned default (0..200)
  bool bug_key = false;            // #170: F12 / pad Back + Start saves a bug report (bug_capture.cpp); off by default (user)
  bool original_logic = true;       // #152 "Original game logic": on = the game's own code 1:1; off allows the Enhancement speed-ups
                                    // that change how it works (object_perf.cpp: far enemies update at 1/2 or 1/3 rate)
  // custom textures folder (texture_pack.cpp); empty = <exe dir>\textures\Custom
  std::string custom_textures_dir;
  std::string mv_name;              // movement: the loaded preset's name (movement_presets.ini), empty = Original
  // new lighting engine (in development: saved now, each part reads its settings as it lands). Everything is
  // time-based, never per frame.
  bool light = false;               // master switch: the modern lighting engine
  int light_quality = 1;            // 0 low, 1 medium, 2 high, 3 ultra
  bool light_shadows = true;        // real-time sun / sky shadows (shadow maps)
  int light_shadow_res = 1;         // per cascade: 0 1024, 1 2048, 2 4096, 3 4096 (capped)
  int light_shadow_dist = 100;      // % of the default shadow distance
  int draw_distance = 100;          // % of the authored screen-camera far plane; object LOD is separate
  int set_objects = 0;             // #158 Enhancement: SET objects per stage, 0 original 2,048 / 1 = 4,096 / 2 = 8,192 (set_capacity.cpp; next stage load)
  int draw_objects = 100;           // % object draw distance (SET create / update range, by class; object_distance.cpp), 100..300
  bool light_profiles = true;       // stage/area atmosphere profiles (live lighting_profiles.ini)
  int light_caster_margin = 500;     // world-space padding for screen-frustum sphere tests; capture off-screen casters
  int light_sky = 60;               // skylight strength %
  // movement tuning (movement.cpp, MOVEMENT-REWORK.md): preset 0 original / 1 reworked / 2 custom; the rest % of the original
  int mv_preset = 0, mv_gravity = 100, mv_fall_max = 100, mv_air_drag = 100, mv_jump = 100, mv_homing_bounce = 100,
      mv_homing_time = 100, mv_homing_fall = 100;
  int hm_mode = 0;                  // homing target pick: 0 Original, 1 Reworked (homing.cpp)
  int speed_ram = 0;                // #141 default off; #111 speed ram: running at 90%+ of top speed, enemies you bump take the hit (speed_ram.cpp)
  int boss_rework = 0;              // #126 "Hard bosses" (opt-in since #130): reworked patterns + phases (boss_ai.cpp); 0 = vanilla AI
  int boss_camera_lock = 1;         // outside its own stage: 0 = no boss takes the camera (#130), 1 = original locks (default, user 2026-10-21), 2 = one boss owns it
  int menu_tint = 0;                // #132: the in-game menu's colours, 0 dark / 1 light (game_menu.cpp tint())
  int boss_intro_camera = 0;        // #127: 1 = a boss placed outside its own stage plays its intro camera at battle start (boss_arena.cpp)
  int mv_max_speed = 100, mv_accel = 100, mv_turn = 100, mv_air_control = 100;   // movement phase 2 (ground / air), %
  int light_ao = 0;                 // ambient occlusion strength %; #143 (user): OFF by default, also when the lighting layer is switched on
  int light_ao_radius = 100;        // % of the default AO reach
  int light_ao_mode = 1;            // #113: 1 = screen-space GTAO (thin contact shading, default), 0 = the old world-space volume (baked)
  int light_ao_quality = 1;         // GTAO quality: 0 low (960x540, 2 slices), 1 medium (1280x720, 3), 2 high (1600x900, 4)
  int light_gi = 60;                // global illumination strength %
  int light_gi_bounces = 1;         // 1..3
  bool light_replace = false;       // the engine takes over: the game's own lighting (baked vertex light, GX lights) and its bloom are switched off
                                    // (user 2026-10-18: default OFF = the game's lighting + the real-time layer on top)
  int light_baked = 50;             // replace mode: % of the game's own baked light kept (0 = full replace: night streets / interiors go black)
  bool light_replace_bloom = false; // replace mode also removes the game's bloom / glow halos (off: they stay, e.g. Iron Jungle)
  bool light_objects = false;       // objects (rings, items, enemies, props) get the new shading too (off: only the map)
  bool light_object_shadows = true; // #A3: with light_objects off, static objects still receive the sun shadow on their own look
  bool light_blob = false;          // the game's own circle (blob) shadows while real-time shadows are on (light=0: always the game's)
  bool light_world_only = true;     // characters / enemies keep the game's own shading: no engine shading or shadow drawn ON them
  bool light_char_cast = true;      // characters / enemies cast a real-time shadow onto the world
  int light_look = 1;               // the real-time layer's look: 0 Stylized, 1 Original match (#143 user: the default), 2 Dramatic (on top of the stage profile)
  int light_exposure = 100, light_shadow_strength = 100, light_shadow_tint = 100, light_ambient = 100, light_saturation = 100;   // look sliders, %
  bool light_sun_custom = false;    // sun direction: false = the game's own (lowered to the max elevation), true = azimuth / elevation below
  int light_sun_az = 225;           // degrees, 0 = north (+z), clockwise
  int light_sun_elev = 45;          // degrees above the horizon
  int light_sun_maxelev = 45;       // auto mode: the game's sun is lowered to at most this (a sun straight overhead hides every shadow)
  int light_sun = 100;              // sun strength %
  bool light_emissive = false;      // #112: off by default (glowing objects look exactly like vanilla); on: the game's glowing light sources (found from its bloom) cast real light
  int light_emissive_str = 100;     // %
  int light_emissive_range = 100;   // % of the default light radius
  int light_glow_flicker = 0;       // glow lights: 0 = one steady glow (default), 100 = a subtle living shimmer
  bool parallax = false;            // #141 default off (vanilla); parallax occlusion mapping on surfaces with a height map (not active yet)
  int parallax_depth = 100;         // %
  int light_debug = 0;              // views (session only)
  int light_view = 0;               // debug view: 0 final, 1 vanilla only, 2 real-time layer only, 3 shadows only, 4 AO only (F4)
};
extern Values g;
void load();                // read the ini (call before command-line overrides)
void save();
void apply_startup();       // after the window exists
bool menu_open();           // game input is suppressed while true (no in-game menu any more: always false)
void draw();                // inside an Aurora frame, before aurora_end_frame()
bool set_value(const char* key, int v);   // tests: INPUT_FILE "SET key=value" (live, not saved)
void apply_hdr();           // push the HDR settings to Aurora (live; switching on/off takes a frame or two)
void apply_display();
void archive_lighting();        // #147: archived lighting parts held at off / neutral (docs/lighting/ARCHIVED.md); LIGHT_ARCHIVE=0 unlocks
void cycle_light_view();        // F4: final -> vanilla only -> real-time layer -> shadows -> AO -> final (a short note)
void cycle_movement_preset();   // F1: Original -> Reworked -> Custom (saved; a short on-screen note)       // apply g.height / g.width / g.widescreen
void init_fonts();          // before the first ImGui frame (launcher.cpp)
void launcher_window_size(int* w, int* h);   // the launcher window, in SDL points (launcher.cpp)
bool skip_present();        // FPS counter bookkeeping (always false now: the game itself runs at the target rate)
int display_refresh_hz();   // refresh rate of the display the window is on (60 if unknown)
}
namespace pause_menu {   // #149: the PC pause menu is retired (runtime/_retired/pause_menu.cpp); the game's own is always used.
bool culling();      // always false (stubs in settings_ui.cpp until gx_lle.cpp drops these calls)
bool cull_2d(float x0, float y0, float x1, float y1);
}
namespace launcher {         // launcher.cpp: the window opens on the launcher; returns when the player presses Play
void run();
}
namespace tex_inspector {    // texture viewer (F3, tex_inspector.cpp): textures drawn now + file names + paint
void toggle();
bool is_open();
void draw();
void paint(const char* key, bool on);   // INPUT_FILE "PAINT <key>" (testing)
void log_used(const char* label);       // INPUT_FILE "TEXLOG <label>": textures drawn in the next 1.5 s -> <exe dir>	exlog.txt (testing)
}
