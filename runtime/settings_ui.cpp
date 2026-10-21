// PC settings: storage (shadow_settings.ini) and the in-game overlays that remain (FPS counter, fullscreen hint, PC
// pause menu / gauges, F3 texture viewer). The settings themselves are edited in the launcher (launcher.cpp).
#include "settings_ui.h"
#include "game_menu.h"
#include "bug_capture.h"
#include "controls.h"
#include "movement.h"
void custom_stats_tick();   // custom_stats.cpp
#include "gx_lle.h"
#include "cheats.h"
#include "input.h"
#include "game.h"
#include <map>
#include "sys.h"
#include <imgui.h>
void warp_hud_draw();   // warp_hud.cpp (#108)
void boss_hud_draw();   // boss_ai.cpp (#126): boss name over the shared HP bar with several bosses
#include <SDL3/SDL.h>
#include <aurora/gfx.h>
#include <algorithm>
#include <vector>
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <string>
#include <algorithm>
#include <cmath>

extern bool g_movie_skip_jumped;      // hw.cpp
extern bool g_movie_user_skip;        // hw.cpp (#149)
extern uint64_t g_movie_start_ns, g_movie_skip_for, g_movie_skip_ns;   // hw.cpp: the playback a skip request belongs to
extern uint64_t g_last_movie_read_ns;

void controls_changed();   // #169 (end of this file)
namespace settings {
Values g;

static std::string ini_path() {
  char exe[MAX_PATH];
  GetModuleFileNameA(nullptr, exe, MAX_PATH);
  std::string p = exe;
  return p.substr(0, p.find_last_of("\\/")) + "\\shadow_settings.ini";
}

// the lighting engine's settings, saved by name
static const struct { const char* key; int Values::*field; } kLightInts[] = {
    {"draw_distance", &Values::draw_distance}, {"draw_objects", &Values::draw_objects}, {"set_objects", &Values::set_objects},   // #158
    {"game_bloom_quality", &Values::game_bloom_quality}, {"game_bloom_strength", &Values::game_bloom_strength}, {"light_view", &Values::light_view}, {"light_look", &Values::light_look}, {"light_exposure", &Values::light_exposure}, {"light_shadow_strength", &Values::light_shadow_strength},
    {"light_shadow_tint", &Values::light_shadow_tint}, {"light_ambient", &Values::light_ambient}, {"light_saturation", &Values::light_saturation},
    {"light_caster_margin", &Values::light_caster_margin},
    {"light_quality", &Values::light_quality},           {"light_shadow_res", &Values::light_shadow_res},
    {"light_shadow_dist", &Values::light_shadow_dist},   {"light_sky", &Values::light_sky},
    {"light_ao", &Values::light_ao},                     {"light_ao_radius", &Values::light_ao_radius},
    {"light_ao_mode", &Values::light_ao_mode}, {"light_ao_quality", &Values::light_ao_quality},
    {"light_gi", &Values::light_gi},                     {"light_gi_bounces", &Values::light_gi_bounces},
    {"light_emissive_str", &Values::light_emissive_str}, {"light_emissive_range", &Values::light_emissive_range},
    {"light_glow_flicker", &Values::light_glow_flicker},
    {"col_black", &Values::col_black}, {"col_white", &Values::col_white}, {"col_contrast", &Values::col_contrast},   // #147 colour pass
    {"col_saturation", &Values::col_saturation}, {"col_vibrance", &Values::col_vibrance}, {"col_r", &Values::col_r},
    {"ao_strength", &Values::ao_strength},   // #148
    {"col_g", &Values::col_g}, {"col_b", &Values::col_b}, {"col_gamma", &Values::col_gamma}, {"col_exposure", &Values::col_exposure},
    {"light_baked", &Values::light_baked},
    {"parallax_depth", &Values::parallax_depth}, {"light_sun", &Values::light_sun}, {"light_sun_az", &Values::light_sun_az},
    {"light_sun_elev", &Values::light_sun_elev}, {"light_sun_maxelev", &Values::light_sun_maxelev},
    // movement tuning (movement.cpp)
    {"mv_preset", &Values::mv_preset}, {"mv_gravity", &Values::mv_gravity}, {"mv_fall_max", &Values::mv_fall_max},
    {"mv_air_drag", &Values::mv_air_drag}, {"mv_jump", &Values::mv_jump}, {"mv_homing_bounce", &Values::mv_homing_bounce},
    {"mv_homing_time", &Values::mv_homing_time}, {"mv_homing_fall", &Values::mv_homing_fall},
    {"hm_mode", &Values::hm_mode}, {"cutscene_fps_cap", &Values::cutscene_fps_cap}, {"speed_ram", &Values::speed_ram}, {"boss_intro_camera", &Values::boss_intro_camera}, {"menu_tint", &Values::menu_tint}, {"boss_rework", &Values::boss_rework}, {"boss_camera_lock", &Values::boss_camera_lock}, {"mv_max_speed", &Values::mv_max_speed}, {"mv_accel", &Values::mv_accel}, {"mv_turn", &Values::mv_turn}, {"mv_air_control", &Values::mv_air_control},
};
static const struct { const char* key; bool Values::*field; } kLightBools[] = {
    {"light_profiles", &Values::light_profiles},
    {"light_replace_bloom", &Values::light_replace_bloom}, {"light_blob", &Values::light_blob}, {"light_objects", &Values::light_objects}, {"light_object_shadows", &Values::light_object_shadows}, {"light_world_only", &Values::light_world_only}, {"light_char_cast", &Values::light_char_cast},
    {"light", &Values::light}, {"light_replace", &Values::light_replace}, {"light_sun_custom", &Values::light_sun_custom}, {"light_shadows", &Values::light_shadows}, {"light_emissive", &Values::light_emissive},
    {"parallax", &Values::parallax}, {"ao", &Values::ao}, {"mute_background", &Values::mute_background}, {"original_logic", &Values::original_logic}, {"bug_key", &Values::bug_key},
};

// ---- live settings: the launcher (launcher\shadow_launcher.pyw) is a separate window that rewrites the ini; the game
// follows within ~0.3 s (draw() -> poll_live). The game's own save() records its write time so it is not re-read.
static uint64_t ini_mtime() {
  WIN32_FILE_ATTRIBUTE_DATA d;
  if (!GetFileAttributesExA(ini_path().c_str(), GetFileExInfoStandard, &d)) return 0;
  return ((uint64_t)d.ftLastWriteTime.dwHighDateTime << 32) | d.ftLastWriteTime.dwLowDateTime;
}
static uint64_t s_ini_seen = 0;
// Session-only switches (cheats, unlock every stage) live in shadow_cheats.ini next to the ini: the launcher writes
// cheatN=0/1 and unlock_all_stages=0/1 and resets them to 0 whenever it starts, so they never stay on by accident.
static std::string cheats_path() {
  std::string p = ini_path();
  return p.substr(0, p.find_last_of("\\/")) + "\\shadow_cheats.ini";
}
static void poll_cheats() {
  static uint64_t seen = ~0ull;
  WIN32_FILE_ATTRIBUTE_DATA d;
  if (!GetFileAttributesExA(cheats_path().c_str(), GetFileExInfoStandard, &d)) return;
  const uint64_t t = ((uint64_t)d.ftLastWriteTime.dwHighDateTime << 32) | d.ftLastWriteTime.dwLowDateTime;
  if (t == seen) return;
  seen = t;
  FILE* f = fopen(cheats_path().c_str(), "r");
  if (!f) return;
  char line[128];
  while (fgets(line, sizeof line, f)) {
    int v;
    char key[64];
    if (sscanf(line, "%63[^=]=%d", key, &v) != 2) continue;
    if (!strcmp(key, "unlock_all_stages")) g.unlock_all_stages = v != 0;
    else if (cheats::by_key(key) >= 0) cheats::enabled(cheats::by_key(key)) = v != 0;
    else if (!strncmp(key, "cheat", 5) && isdigit((unsigned char)key[5])) {
      const int i = atoi(key + 5);
      if (i >= 0 && i < cheats::COUNT) cheats::enabled(i) = v != 0;
    }
  }
  fclose(f);
  logf("[settings] the launcher changed the cheats / stage unlock\n");
}
// Live values: the launcher's "Player" tab sets counters in the running game (shadow_live.ini: key=value,seq - a key is
// applied once per new seq; the first read after start only records them, so stale commands never fire) and the game
// publishes the current ones back (shadow_state.ini) so the sliders follow what happens in play.
static const struct { const char* key; uint32_t addr; } kLiveWords[] = {
    {"lives", 0x80576704}, {"rings", 0x8057670C}, {"score", 0x80576710}, {"dark_score", 0x80576714},
    {"hero_score", 0x80576718}, {"hero_gauge", 0x805766C8}, {"dark_gauge", 0x805766D4},
};
static std::string side_path(const char* name) {
  std::string p = ini_path();
  return p.substr(0, p.find_last_of("\\/")) + "\\" + name;
}
static void poll_live_commands() {
  static std::map<std::string, int> applied;
  static bool first = true;
  FILE* f = fopen(side_path("shadow_live.ini").c_str(), "r");
  if (!f) { first = false; return; }
  char line[128];
  while (fgets(line, sizeof line, f)) {
    char key[64];
    int v = 0, seq = 0;
    if (sscanf(line, "%63[^=]=%d,%d", key, &v, &seq) != 3) continue;
    auto it = applied.find(key);
    if (it != applied.end() && it->second >= seq) continue;
    applied[key] = seq;
    if (first) continue;
    for (const auto& w : kLiveWords)
      if (!strcmp(key, w.key)) { st32(w.addr, (uint32_t)v); logf("[settings] launcher set %s = %d\n", key, v); }
  }
  fclose(f);
  first = false;
}
static void publish_state() {
  static int32_t last[16];
  static bool had = false;
  int32_t cur[16];
  int n = 0;
  for (const auto& w : kLiveWords) cur[n++] = (int32_t)ld32(w.addr);
  cur[n++] = game::in_stage() ? 1 : 0;
  if (had && !memcmp(cur, last, sizeof(int32_t) * n)) return;
  memcpy(last, cur, sizeof(int32_t) * n);
  had = true;
  const std::string tmp = side_path("shadow_state.tmp"), dst = side_path("shadow_state.ini");
  FILE* f = fopen(tmp.c_str(), "w");
  if (!f) return;
  n = 0;
  for (const auto& w : kLiveWords) fprintf(f, "%s=%d\n", w.key, cur[n++]);
  fprintf(f, "in_stage=%d\n", cur[n]);
  fclose(f);
  MoveFileExA(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING);
}

static void poll_live() {
  static uint64_t last = 0;
  const uint64_t now = sys_time_ns();
  if (now - last < 300000000ull) return;
  last = now;
  poll_cheats();
  poll_live_commands();
  publish_state();
  const uint64_t t = ini_mtime();
  if (!s_ini_seen) { s_ini_seen = t; return; }
  if (!t || t == s_ini_seen) return;
  s_ini_seen = t;
  const Values old = g;
  load();
  logf("[settings] the launcher changed shadow_settings.ini - applied\n");
  gxlle::g_game_bloom = g.game_bloom && !(g.light && g.light_replace && g.light_replace_bloom);
  if (g.hdr != old.hdr || g.hdr_paper_white != old.hdr_paper_white || g.hdr_peak != old.hdr_peak ||
      g.hdr_highlight != old.hdr_highlight || g.hdr_saturation != old.hdr_saturation || g.hdr_vibrance != old.hdr_vibrance ||
      g.hdr_contrast != old.hdr_contrast || g.hdr_black != old.hdr_black || g.hdr_ui_white != old.hdr_ui_white ||
      g.col_black != old.col_black || g.col_white != old.col_white || g.col_contrast != old.col_contrast ||
      g.col_saturation != old.col_saturation || g.col_vibrance != old.col_vibrance || g.col_r != old.col_r ||
      g.col_g != old.col_g || g.col_b != old.col_b || g.col_gamma != old.col_gamma || g.col_exposure != old.col_exposure)
    apply_hdr();
  if (g.height != old.height || g.width != old.width || g.widescreen != old.widescreen) {
    const bool sz = g.height != old.height || g.width != old.width;
    gxlle::g_render_height = g.height;
    const bool aspect_changed = gxlle::g_widescreen != g.widescreen;
    gxlle::g_widescreen = g.widescreen;
    gxlle::apply_display_settings();
    if (sz && g.width > 0 && g.height > 0) gxlle::resize_window_pixels(g.width, g.height);
    else if (aspect_changed) gxlle::fit_window_to_aspect();
  }
  if (g.fullscreen != old.fullscreen && g.fullscreen != gxlle::is_fullscreen()) gxlle::toggle_fullscreen();
}

// One ini key (load, and INPUT_FILE "SET key=value" in tests). False = unknown key.
static bool set_key(const char* key, int v) {
  if (!strcmp(key, "height")) g.height = v;
  else if (!strcmp(key, "width")) g.width = v;
  else if (!strcmp(key, "scale")) g.height = 480 * v;   // older ini
  else if (!strcmp(key, "widescreen")) g.widescreen = v != 0;
  else if (!strcmp(key, "fullscreen")) g.fullscreen = v != 0;
  else if (!strcmp(key, "fps_target")) g.fps_target = v;
  else if (!strcmp(key, "show_fps")) g.show_fps = v != 0;
  else if (!strcmp(key, "game_bloom")) g.game_bloom = v != 0;
  else if (!strcmp(key, "clip_ui_43")) g.clip_ui_43 = v != 0;
  else if (!strcmp(key, "cutscene_bars")) g.cutscene_bars = v != 0;
  else if (!strcmp(key, "hud_169")) {}   // #143 removed (the 4:3-centred HUD stays): old ini lines are ignored
  else if (!strcmp(key, "pause_new")) {}   // #149: the PC pause menu is retired (old ini lines are ignored)
  else if (!strcmp(key, "gauges_new")) {}   // #143 the PC gauges were removed: old ini lines are ignored
  else if (!strcmp(key, "skip_any_cutscene")) g.skip_any_cutscene = v != 0;
  else if (!strcmp(key, "warp_enemy_counts")) g.warp_enemy_counts = v != 0;
  else if (!strcmp(key, "fov_percent")) g.fov_percent = v;
  else if (!strcmp(key, "cam_sens")) g.cam_sens = v;
  else if (!strcmp(key, "invert_cam_x")) g.invert_cam_x = v != 0;
  else if (!strcmp(key, "invert_cam_y")) g.invert_cam_y = v != 0;
  else if (!strcmp(key, "cam_recentre")) g.cam_recentre = v != 0;
  else if (!strcmp(key, "skin")) g.skin = v;
  else if (!strcmp(key, "custom_textures")) g.custom_textures = v != 0;
  else if (!strcmp(key, "tex_match")) g.tex_match = std::clamp(v, 0, 100);
  else if (!strcmp(key, "vol_master")) g.vol_master = v;
  else if (!strcmp(key, "vol_sfx")) g.vol_sfx = v;
  else if (!strcmp(key, "vol_music")) g.vol_music = v;
  else if (!strcmp(key, "vol_dialogue")) g.vol_dialogue = v;
  else if (!strcmp(key, "hdr")) g.hdr = v != 0;
  else if (!strcmp(key, "hdr_paper_white")) g.hdr_paper_white = v;
  else if (!strcmp(key, "hdr_peak")) g.hdr_peak = v;
  else if (!strcmp(key, "hdr_highlight")) g.hdr_highlight = v;
  else if (!strcmp(key, "hdr_saturation")) g.hdr_saturation = v;
  else if (!strcmp(key, "hdr_vibrance")) g.hdr_vibrance = v;
  else if (!strcmp(key, "hdr_contrast")) g.hdr_contrast = v;
  else if (!strcmp(key, "hdr_black")) g.hdr_black = v;
  else if (!strcmp(key, "hdr_ui_white")) g.hdr_ui_white = v;
  else if (!strcmp(key, "hdr_test")) g.hdr_test = v != 0;   // session only (not saved)
  else if (!strcmp(key, "light_debug")) g.light_debug = v;  // session only (not saved)
  else {
    for (const auto& f : kLightInts)
      if (!strcmp(key, f.key)) { g.*f.field = v; return true; }
    for (const auto& f : kLightBools)
      if (!strcmp(key, f.key)) { g.*f.field = v != 0; return true; }
    return false;
  }
  return true;
}

void load() {
  if (getenv("TEST_UNLOCK_ALL")) g.unlock_all_stages = true;   // testing (the option itself is never saved)
  if (const char* t = getenv("TEST_CHEATS")) {   // testing: comma-separated named cheat keys switched on (cheat_infinite_rings,...)
    std::string s = t;
    for (size_t i = 0; i <= s.size();) {
      size_t j = s.find(',', i);
      if (j == std::string::npos) j = s.size();
      const int id = cheats::by_key(s.substr(i, j - i).c_str());
      if (id >= 0) { cheats::enabled(id) = true; logf("[settings] test cheat on: %s\n", s.substr(i, j - i).c_str()); }
      i = j + 1;
    }
  }
  FILE* f = fopen(ini_path().c_str(), "r");
  if (!f) return;
  char line[1024];
  while (fgets(line, sizeof line, f)) {
    if (!strncmp(line, "bind_", 5)) {   // #169 controls: bind_pad_<action> / bind_kb_<action> (controls.cpp)
      char* eq = strchr(line, '=');
      if (eq) { *eq = 0; controls::load(line, eq + 1); }
      continue;
    }
    if (!strncmp(line, "mv_name=", 8)) {   // movement: the loaded preset's name (text)
      std::string p = line + 8;
      while (!p.empty() && (p.back() == '\n' || p.back() == '\r')) p.pop_back();
      g.mv_name = p;
      continue;
    }
    if (!strncmp(line, "custom_textures_dir=", 20)) {   // the one text setting (a folder path, UTF-8)
      std::string p = line + 20;
      while (!p.empty() && (p.back() == '\n' || p.back() == '\r')) p.pop_back();
      g.custom_textures_dir = p;
      continue;
    }
    char key[64];
    int v;
    if (sscanf(line, "%63[^=]=%d", key, &v) != 2) continue;
    set_key(key, v);
  }
  fclose(f);
  archive_lighting();
}

// #147 (user): the lighting layer is the real-time sun shadows on the game's own lighting, nothing else for now. The other
// parts (Replace, sky / sun relight, GI, glow lights, Looks and look sliders, old AO, debug views) stay in the code but are
// held at off / neutral here, whatever the ini says. docs/lighting/ARCHIVED.md. LIGHT_ARCHIVE=0 (dev) unlocks them.
void archive_lighting() {
  static const bool unlocked = [] { const char* e = getenv("LIGHT_ARCHIVE"); return e && *e == '0'; }();
  if (unlocked) return;
  g.light_replace = false; g.light_replace_bloom = false; g.light_baked = 50;
  g.light_look = 1; g.light_exposure = g.light_shadow_strength = g.light_shadow_tint = g.light_ambient = g.light_saturation = 100;
  g.light_sun_custom = false; g.light_sun = 100; g.light_sky = 60;
  g.light_gi = 0; g.light_emissive = false;
  g.light_ao = 0;   // #148: a new AO from fresh; the menu shows a greyed "Work in progress" box
  g.light_view = 0; g.light_debug = 0;
}

// Tests: change a setting live without saving (INPUT_FILE "SET key=value"); applies the live parts.
bool set_value(const char* key, int v) {
  if (!set_key(key, v)) return false;
  archive_lighting();
  gxlle::g_game_bloom = g.game_bloom && !(g.light && g.light_replace && g.light_replace_bloom);
  apply_hdr();
  return true;
}

uint64_t g_mv_hint_until = 0;
uint64_t g_view_hint_until = 0;
void cycle_light_view() {
  if (!(getenv("LIGHT_ARCHIVE") && *getenv("LIGHT_ARCHIVE") == '0')) return;   // #147: the debug views are archived
  g.light_view = (g.light_view + 1) % 5;
  save();
  g_view_hint_until = sys_time_ns() + 2500000000ull;
  logf("[light] F4 -> view %d\n", g.light_view);
}
void cycle_movement_preset() {
  movement::cycle();   // Original -> each saved preset (movement_presets.ini) -> Original; saves
  g_mv_hint_until = sys_time_ns() + 2500000000ull;
}
void save() {
  FILE* f = fopen(ini_path().c_str(), "w");
  if (!f) return;
  fprintf(f, "width=%d\nheight=%d\nwidescreen=%d\nfullscreen=%d\nfps_target=%d\nshow_fps=%d\ngame_bloom=%d\n", g.width, g.height,
          (int)g.widescreen, (int)g.fullscreen, g.fps_target, (int)g.show_fps, (int)g.game_bloom);
  fprintf(f, "clip_ui_43=%d\ncutscene_bars=%d\nskip_any_cutscene=%d\nhud_169=%d\nwarp_enemy_counts=%d\n", (int)g.clip_ui_43, (int)g.cutscene_bars,
          (int)g.skip_any_cutscene, (int)g.hud_169, (int)g.warp_enemy_counts);
  fprintf(f, "fov_percent=%d\ncam_sens=%d\ninvert_cam_x=%d\ninvert_cam_y=%d\n", g.fov_percent, g.cam_sens, (int)g.invert_cam_x,
          (int)g.invert_cam_y);
  fprintf(f, "cam_recentre=%d\nskin=%d\n", (int)g.cam_recentre, g.skin);
  fprintf(f, "custom_textures=%d\ngauges_new=%d\n", (int)g.custom_textures, (int)g.gauges_new);
  fprintf(f, "tex_match=%d\n", g.tex_match);
  fprintf(f, "vol_master=%d\nvol_sfx=%d\nvol_music=%d\nvol_dialogue=%d\n", g.vol_master, g.vol_sfx, g.vol_music,
          g.vol_dialogue);
  fprintf(f, "hdr=%d\nhdr_paper_white=%d\nhdr_peak=%d\nhdr_highlight=%d\nhdr_saturation=%d\n", (int)g.hdr,
          g.hdr_paper_white, g.hdr_peak, g.hdr_highlight, g.hdr_saturation);
  fprintf(f, "hdr_vibrance=%d\nhdr_contrast=%d\nhdr_black=%d\nhdr_ui_white=%d\n", g.hdr_vibrance, g.hdr_contrast,
          g.hdr_black, g.hdr_ui_white);
  for (const auto& b : kLightBools) fprintf(f, "%s=%d\n", b.key, (int)(g.*b.field));
  for (const auto& i : kLightInts) fprintf(f, "%s=%d\n", i.key, g.*i.field);
  fprintf(f, "custom_textures_dir=%s\n", g.custom_textures_dir.c_str());
  fprintf(f, "mv_name=%s\n", g.mv_name.c_str());
  controls::save(f);   // #169
  fclose(f);
  s_ini_seen = ini_mtime();
}

void apply_startup() {
  gxlle::g_game_bloom = g.game_bloom && !(g.light && g.light_replace && g.light_replace_bloom);
  if (g.fullscreen && !gxlle::is_fullscreen() && !gxlle::g_launcher_mode) gxlle::toggle_fullscreen();   // launcher: on Play
  apply_hdr();
}

void apply_hdr() {
  AuroraHdrSettings h{};
  h.enabled = g.hdr;
  h.testPattern = g.hdr_test;
  h.paperWhiteNits = (float)g.hdr_paper_white;
  h.peakNits = (float)std::max(g.hdr_peak, g.hdr_paper_white);
  h.highlight = g.hdr_highlight / 100.0f;
  h.saturation = g.hdr_saturation / 100.0f;
  h.vibrance = g.hdr_vibrance / 100.0f;
  h.contrast = g.hdr_contrast / 100.0f;
  h.blackLevel = g.hdr_black / 1000.0f;
  h.uiWhiteNits = (float)g.hdr_ui_white;
  // #147 colour pass: SDR and HDR; all neutral = skipped
  h.colBlack = std::clamp(g.col_black, -50, 50) / 4000.0f;   // +-50 = +-1.25 % linear (about +-11 % of an sRGB step at black)
  h.colWhite = std::clamp(g.col_white, 50, 150) / 100.0f;
  h.colContrast = std::clamp(g.col_contrast, 50, 150) / 100.0f;
  h.colSaturation = std::clamp(g.col_saturation, 0, 200) / 100.0f;
  h.colVibrance = std::clamp(g.col_vibrance, -100, 100) / 100.0f;
  h.colGain[0] = std::clamp(g.col_r, 50, 150) / 100.0f;
  h.colGain[1] = std::clamp(g.col_g, 50, 150) / 100.0f;
  h.colGain[2] = std::clamp(g.col_b, 50, 150) / 100.0f;
  h.colGamma = std::clamp(g.col_gamma, 50, 200) / 100.0f;
  h.colExposure = std::clamp(g.col_exposure, -20, 20) / 10.0f;
  aurora_set_hdr(&h);
}

bool menu_open() { return movement::overlay_open() || game_menu::is_open(); }   // the F2 movement window / #132 in-game menu own input while open

// ---- frame limiter / FPS counter
static uint64_t s_last_present = 0;
static double s_fps = 0;
bool skip_present() {
  uint64_t now = sys_time_ns();
  static uint64_t window_start = 0, frames = 0;
  if (!window_start) window_start = now;
  ++frames;
  if (now - window_start >= 1000000000ull) {
    s_fps = frames * 1e9 / (double)(now - window_start);
    static const bool log_fps = getenv("FPS_LOG") != nullptr;
    if (log_fps) {
      // frame-timing block of the game's main loop (lbl_8057D6A0): +0x0C elapsed VIs, +0x34 cap, +0x50 base rate
      char words[200];
      int o = 0;
      for (uint32_t off = 0; off < 0x60; off += 4) o += snprintf(words + o, sizeof words - o, "%X ", ld32(0x8057D6A0 + off));
      logf("[fps] %.1f | gp+translate %.0f ms, aurora %.0f ms, present %.0f ms, frame-wait %.0f ms per second | %s\n", s_fps,
           gxlle::g_prof_ns[0] / 1e6, gxlle::g_prof_ns[1] / 1e6, gxlle::g_prof_ns[2] / 1e6, gxlle::g_prof_ns[3] / 1e6, words);
      gxlle::g_prof_ns[0] = gxlle::g_prof_ns[1] = gxlle::g_prof_ns[2] = gxlle::g_prof_ns[3] = 0;
    }
    // MEM_DUMP_AT=<seconds>,<file>: write all of MEM1 to a file once (diffing game state between runs)
    static const char* dump_env = getenv("MEM_DUMP_AT");
    static uint64_t boot = now;
    static bool dumped = false;
    if (dump_env && !dumped && (now - boot) / 1e9 >= atof(dump_env)) {
      dumped = true;
      if (const char* fn = strchr(dump_env, ',')) {
        if (FILE* df = fopen(fn + 1, "wb")) { fwrite(g_ram, 1, g_mem1_size, df); fclose(df); logf("[mem] dumped MEM1 to %s\n", fn + 1); }
      }
    }
    // MEM_LOG=<hex addr>,<bytes>: dump a RAM range once a second (finding cheat addresses)
    static const char* mem_env = getenv("MEM_LOG");
    if (mem_env) {
      uint32_t a = (uint32_t)strtoul(mem_env, nullptr, 16), len = 256;
      if (const char* c = strchr(mem_env, ',')) len = (uint32_t)strtoul(c + 1, nullptr, 0);
      for (uint32_t off = 0; off < len; off += 32) {
        char line[200];
        int o = snprintf(line, sizeof line, "[mem] %08X:", a + off);
        for (uint32_t w = 0; w < 32 && off + w < len; w += 4) o += snprintf(line + o, sizeof line - o, " %08X", ld32(a + off + w));
        logf("%s\n", line);
      }
    }
    frames = 0;
    window_start = now;
  }
  s_last_present = now;
  return false;
}

int display_refresh_hz() {
  SDL_DisplayID id = SDL_GetPrimaryDisplay();
  if (gxlle::g_window) id = SDL_GetDisplayForWindow(gxlle::g_window);
  const SDL_DisplayMode* m = SDL_GetCurrentDisplayMode(id);
  return m && m->refresh_rate > 1 ? (int)(m->refresh_rate + 0.5f) : 60;
}

void draw() {
  poll_live();
  if(game::stage_boot_loading()) {
    const auto* viewport=ImGui::GetMainViewport();auto* draw=ImGui::GetForegroundDrawList();
    const ImVec2 origin=viewport->Pos,size=viewport->Size;
    draw->AddRectFilled(origin,ImVec2(origin.x+size.x,origin.y+size.y),IM_COL32(16,19,27,255));
    const char* text="Loading stage...";const ImVec2 measured=ImGui::CalcTextSize(text);
    draw->AddText(ImVec2(origin.x+(size.x-measured.x)*.5f,origin.y+size.y*.5f),IM_COL32(237,239,245,255),text);
    return;
  }
  {
    // scale the UI with the window (the default ImGui font is tiny on 1440p/4K)
    float h = ImGui::GetMainViewport()->Size.y;
    float sc = std::max(1.0f, std::round(h / 900.0f * 4.0f) / 4.0f);   // ~1x at 900 px tall, steps of 0.25
    static float applied = 0;
    if (sc != applied) {
      ImGui::GetIO().FontGlobalScale = sc;
      ImGuiStyle st;
      ImGui::StyleColorsDark(&st);
      st.ScaleAllSizes(sc);
      st.WindowRounding = 6 * sc;
      st.FrameRounding = 3 * sc;
      ImGui::GetStyle() = st;
      applied = sc;
    }
  }
  {
    // #149 "Skip any cutscene": START (Tab) while a movie streams asks hw.cpp to jump that movie to its tail (checked
    // every frame: the disc reads come in bursts and would miss a press)
    // P0 (user: New Game's movie skipped by itself): only a press made DURING this playback, 0.5 s after it started,
    // and tied to it (hw.cpp drops it if another movie starts); presses from before the movie (title / menus) are dropped
    static uint64_t s_seen_start = 0;
    if (g_movie_start_ns != s_seen_start) { s_seen_start = g_movie_start_ns; input_take_start_press(); }
    if (g.skip_any_cutscene && g_last_movie_read_ns && sys_time_ns() - g_last_movie_read_ns < 1000000000ull &&
        sys_time_ns() - g_movie_start_ns > 500000000ull && input_take_start_press()) {
      g_movie_user_skip = true; g_movie_skip_for = g_movie_start_ns; g_movie_skip_ns = sys_time_ns();
      logf("[skip] START during a movie: skip requested\n");
    }
    // a skipped movie's stream was jumped to its tail: until it ends the decoder shows garbage blocks - cover them
    if (g_movie_skip_jumped) {
      if (sys_time_ns() - g_last_movie_read_ns > 400000000ull) g_movie_skip_jumped = false;
      else ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), ImGui::GetMainViewport()->Size, IM_COL32(0, 0, 0, 255));
    }
  }
  ::warp_hud_draw();   // warp_hud.cpp (#108): enemy counts on the checkpoint warp screen
  ::boss_hud_draw();   // boss_ai.cpp (#126)
  if (g.show_fps) {
    ImGui::SetNextWindowPos(ImVec2(8, 8));
    ImGui::SetNextWindowBgAlpha(0.45f);
    ImGui::Begin("##fps", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    ImGui::Text("%.0f FPS", s_fps);
    ImGui::End();
  }
  {
    extern uint64_t g_view_hint_until;   // F4 lighting view note
    if (sys_time_ns() < g_view_hint_until) {
      static const char* vn[] = {"final", "vanilla only", "real-time layer only", "shadows only", "AO only"};
      const ImGuiViewport* v = ImGui::GetMainViewport();
      ImGui::SetNextWindowPos(ImVec2(v->Size.x * 0.5f, 80.0f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
      ImGui::SetNextWindowBgAlpha(0.6f);
      ImGui::Begin("##viewhint", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
      ImGui::Text("Lighting view: %s  (F4 to change)", vn[std::clamp(g.light_view, 0, 4)]);
      ImGui::End();
    }
    // F1 movement preset note (cycle_movement_preset)
    extern uint64_t g_mv_hint_until;
    if (sys_time_ns() < g_mv_hint_until) {
      const ImGuiViewport* v = ImGui::GetMainViewport();
      ImGui::SetNextWindowPos(ImVec2(v->Size.x * 0.5f, 40.0f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
      ImGui::SetNextWindowBgAlpha(0.6f);
      ImGui::Begin("##mvhint", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
      ImGui::Text("Movement: %s  (F1 next, F2 tweak)", movement::active_name());
      ImGui::End();
    }
  }
  {
    // after entering fullscreen, say how to leave it (Esc is the controller's START button)
    static bool was_fs = false;
    static uint64_t hint_until = 0;
    bool fs = gxlle::is_fullscreen();
    if (fs && !was_fs) hint_until = sys_time_ns() + 5000000000ull;
    was_fs = fs;
    if (fs && sys_time_ns() < hint_until) {
      const ImGuiViewport* v = ImGui::GetMainViewport();
      ImGui::SetNextWindowPos(ImVec2(v->Size.x * 0.5f, v->Size.y - 40.0f), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
      ImGui::SetNextWindowBgAlpha(0.6f);
      ImGui::Begin("##fshint", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
      ImGui::TextUnformatted("Fullscreen - press F11 or Alt+Enter to leave");
      ImGui::End();
    }
  }
  tex_inspector::draw();   // F3 texture viewer (tex_inspector.cpp)
  movement::draw_overlay();   // F2 movement Tweak window (movement.cpp)
  ::custom_stats_tick();   // custom stage run stats (custom_stats.cpp)
  {   // logic VM Show text (stage_workshop ACT 13): big centred text near the top for its seconds
    extern std::string g_hud_text; extern uint64_t g_hud_text_until;
    if (!g_hud_text.empty() && sys_time_ns() < g_hud_text_until) {
      const ImGuiViewport* v = ImGui::GetMainViewport();
      ImGui::SetNextWindowPos(ImVec2(v->Size.x * 0.5f, v->Size.y * 0.22f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
      ImGui::SetNextWindowBgAlpha(0.45f);
      ImGui::Begin("##hudtext", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
      ImGui::SetWindowFontScale(std::max(1.5f, v->Size.y / 480.f));
      ImGui::TextUnformatted(g_hud_text.c_str());
      ImGui::End();
    }
    extern std::string g_hud_values[3];   // logic VM "Show value on HUD" lines (stage_workshop.cpp)
    if (!g_hud_values[0].empty() || !g_hud_values[1].empty() || !g_hud_values[2].empty()) {
      const ImGuiViewport* v = ImGui::GetMainViewport();
      ImGui::SetNextWindowPos(ImVec2(v->Size.x * 0.03f, v->Size.y * 0.30f), ImGuiCond_Always, ImVec2(0.f, 0.f));
      ImGui::SetNextWindowBgAlpha(0.45f);
      ImGui::Begin("##hudvalues", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
      ImGui::SetWindowFontScale(std::max(1.3f, v->Size.y / 560.f));
      for (const auto& line : g_hud_values)
        if (!line.empty()) ImGui::TextUnformatted(line.c_str());
      ImGui::End();
    }
  }
  bug_capture::tick();   // #135
  game_menu::draw();   // #132 last: over every other overlay
}

// Display changes (launcher): render height / aspect, window size, saved.
void apply_display() {
  gxlle::g_render_height = g.height;
  bool aspect_changed = gxlle::g_widescreen != g.widescreen;
  gxlle::g_widescreen = g.widescreen;
  gxlle::apply_display_settings();
  if (g.width > 0 && g.height > 0) gxlle::resize_window_pixels(g.width, g.height);   // windowed only
  else if (aspect_changed) gxlle::fit_window_to_aspect();
  save();
}
}  // namespace settings

// logic VM Show text (stage_workshop ACT 13) -> drawn in settings::draw
namespace settings { std::string g_hud_text; uint64_t g_hud_text_until = 0; std::string g_hud_values[3]; }
void hud_text_show(const char* text, double seconds) { settings::g_hud_text = text ? text : ""; settings::g_hud_text_until = sys_time_ns() + (uint64_t)(seconds * 1e9); }

namespace pause_menu {   // #149: retired (runtime/_retired/pause_menu.cpp) - nothing is culled, the game's own pause draws
bool culling() { return false; }
bool cull_2d(float, float, float, float) { return false; }
}  // namespace pause_menu

// #169 controls.cpp: a binding changed (press-to-bind, reset) - saved with the other settings
void controls_changed() { settings::save(); }
