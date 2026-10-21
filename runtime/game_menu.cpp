// #132 in-game settings menu. The only settings UI (the launcher is gone): Xbox 360-style blades that
// slide out over the running game - never paused, the right half of the screen stays clear. Look and timings follow
// the docs/launcher/IN-GAME-MENU-STYLE.md (mockups agents/claude/out/menu132); the content is the generated
// runtime/menu_schema.h (pages > sections > items, shown_if dependencies) plus our own System page (restart).
// Drawn with ImGui's foreground draw list only (rects, rounded rects, lines, circles, text, alpha). Controls: pad Back
// or Esc opens / closes; D-pad / stick / arrows move and change; A / Enter; B / Backspace; LB RB (Q E) page; LT RT
// (PgUp PgDn) section; Y (R) default; mouse: hover, click, wheel. MENU_TINT=light: the light tint.
#include "game_menu.h"
#include "menu_schema.h"
#include "settings_ui.h"
#include "cheats.h"
#include "sys.h"
#include "gx_lle.h"
#include "build_info.h"
#include "texture_extract.h"
#include "movement.h"
#include "osk.h"
#include "controls.h"
#include <imgui.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace ms = menu_schema;

namespace first_boot_hint { void menu_opened(); }   // first_boot_hint.cpp (#155)
namespace game_menu {
namespace {

// ---------------------------------------------------------------- fonts (built at the display's scale)
ImFont* f_title = nullptr;   // Bahnschrift: page title
ImFont* f_text = nullptr;    // Segoe UI: rows, values, help, hints
ImFont* f_semi = nullptr;    // Segoe UI Semibold: spines, chips, section labels, help name

ImFont* or_default(ImFont* f) { return f ? f : ImGui::GetIO().Fonts->Fonts[0]; }
ImVec2 tsize(ImFont* f, float px, const char* s, const char* e = nullptr) { return or_default(f)->CalcTextSizeA(px, FLT_MAX, 0.f, s, e); }

// ---------------------------------------------------------------- time and easing
double now_s() { return double(sys_time_ns()) * 1e-9; }
float clamp01(float x) { return std::clamp(x, 0.f, 1.f); }
float fl(float px, float floor_px) { return std::max(px, floor_px); }   // the spec's text floors (readable at 480p)
float cubic_out(float x) { x = clamp01(x); return 1.f - (1.f - x) * (1.f - x) * (1.f - x); }
float quad_out(float x) { x = clamp01(x); return 1.f - (1.f - x) * (1.f - x); }

// ---------------------------------------------------------------- colours
struct Tint { ImVec4 panel, spine, text, dim, line, track, focus, scrim, keycap; };
ImVec4 rgb(int r, int g, int b, float a = 1.f) { return ImVec4(r / 255.f, g / 255.f, b / 255.f, a); }
const ImVec4 kAccent = rgb(0xE5, 0x39, 0x4B);
const Tint kDark{rgb(0x10, 0x11, 0x17, .96f), rgb(0x18, 0x1A, 0x22, .94f), rgb(0xF2, 0xF3, 0xF7), rgb(0x9B, 0xA0, 0xAD),
                 rgb(255, 255, 255, .09f), rgb(255, 255, 255, .16f), rgb(0xE5, 0x39, 0x4B, .24f), rgb(0, 0, 0, .18f), rgb(0x3A, 0x3D, 0x48)};
const Tint kLight{rgb(0xF4, 0xF5, 0xF8, .97f), rgb(0xE2, 0xE4, 0xEB, .94f), rgb(0x14, 0x15, 0x1A), rgb(0x5C, 0x61, 0x70),
                  rgb(0, 0, 0, .09f), rgb(0, 0, 0, .14f), rgb(0xE5, 0x39, 0x4B, .15f), rgb(0, 0, 0, .10f), rgb(0x5C, 0x61, 0x70)};
const Tint& tint() {   // settings key menu_tint (live; Graphics > Interface); MENU_TINT=light / dark overrides (tests)
  static const char* env = getenv("MENU_TINT");
  const bool light = env ? !strcmp(env, "light") : settings::g.menu_tint == 1;
  return light ? kLight : kDark;
}
ImU32 C(const ImVec4& c, float a = 1.f) { return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, c.w * a)); }

// ---------------------------------------------------------------- model
// our own page after the schema's: restart, version, textures. #170: when the schema has a "system" page (its settings,
// e.g. the bug report key) that page IS the System page: our rows first, then its sections
const int kSchemaSystem = [] { for (int p = 0; p < ms::kNumPages; ++p) if (!strcmp(ms::kPages[p].id, "system")) return p; return -1; }();
const int kSystemPage = kSchemaSystem >= 0 ? kSchemaSystem : ms::kNumPages;
int n_pages() { return kSchemaSystem >= 0 ? ms::kNumPages : ms::kNumPages + 1; }
// #169 the Controls page: a schema page with no sections (id "controls"); its rows are the bindings (controls.cpp)
const int kControlsPage = [] { for (int p = 0; p < ms::kNumPages; ++p) if (!strcmp(ms::kPages[p].id, "controls")) return p; return -1; }();
const char* page_title(int p) { return p < ms::kNumPages ? ms::kPages[p].title : "System"; }
enum class RowKind { Section, Item, Restart, Info, Extract, OpenTex, Note, Bind, BindReset, PresetNew, PresetSave, PresetRename, PresetDelete };   // Preset*: #144 (Shadow)
struct Row { RowKind kind; int idx; float y, h; };   // y: top within the list content

const ms::Item* find_item(const char* key) {
  for (const auto& it : ms::kItems) if (!strcmp(it.key, key)) return &it;
  return nullptr;
}
// #144 the Movement preset row lists the player's own presets (movement.h), not the schema's options:
// 0 Original, 1..n the presets, n + 1 Custom (the sliders, not saved)
bool is_preset(const ms::Item& it) { return !strcmp(it.key, "mv_preset"); }
int n_opts(const ms::Item& it) { return is_preset(it) ? movement::preset_count() + 2 : it.n_options; }
const char* opt_label(const ms::Item& it, int o) {
  if (!is_preset(it)) return it.options[o].label;
  return o == 0 ? "Original" : o <= movement::preset_count() ? movement::preset_name(o - 1) : "Custom (not saved)";
}
int preset_opt() { const int a = movement::active_preset(); return a == -1 ? 0 : a == -2 ? movement::preset_count() + 1 : a + 1; }
void use_opt(int o) { const int n = movement::preset_count(); o = std::clamp(o, 0, n + 1); movement::use_preset(o == 0 ? -1 : o > n ? -2 : o - 1); logf("[menu] mv_preset = %s\n", movement::active_name()); }
double g_del_armed = -10;   // Delete: the first A arms it for 3 s, the second deletes
int raw_value(const ms::Item& it) {
  using settings::g;
  if (it.store == ms::Store::Cheats) return it.cheat < 0 ? (g.unlock_all_stages ? 1 : 0) : (cheats::enabled(it.cheat) ? 1 : 0);
  if (it.store == ms::Store::Live) return int(int32_t(ld32(it.addr)));
  if (it.bfield) return g.*it.bfield ? 1 : 0;
  if (it.ifield) return g.*it.ifield;
  return 0;
}
int option_index(const ms::Item& it) {
  if (is_preset(it)) return preset_opt();
  if (!it.options) return -1;
  if (it.kind == ms::Kind::Resolution) {
    const int w = settings::g.*it.ifield, h = settings::g.*it.ifield2;
    for (int i = 0; i < it.n_options; ++i) if (it.options[i].value == w && it.options[i].value2 == h) return i;
    return w == 0 ? 0 : -1;
  }
  const int v = raw_value(it);
  for (int i = 0; i < it.n_options; ++i) if (it.options[i].value == v) return i;
  return -1;
}
bool shown(const ms::Item& it) {
  if (!it.shown_if) return true;
  const ms::Item* d = find_item(it.shown_if);
  if (!d) return true;
  const int v = raw_value(*d);
  if (is_preset(*d)) return v != 0;   // #144 the sliders: any preset or Custom (mv_preset 1), not Original
  return it.shown_if_value < 0 ? v != 0 : v == it.shown_if_value;
}
bool is_switch(const ms::Item& it) { return it.kind == ms::Kind::Toggle || it.kind == ms::Kind::Session; }
bool is_list(const ms::Item& it) { return it.kind == ms::Kind::Choice || it.kind == ms::Kind::Fps || it.kind == ms::Kind::Resolution; }
bool has_default(const ms::Item& it) { return it.store != ms::Store::Live; }
int nice_step(int range, int parts) {
  const int raw = std::max(1, range / parts);
  for (int s : {1, 2, 5, 10, 20, 25, 50, 100, 200, 250, 500, 1000, 2000, 5000, 10000, 25000, 50000, 100000, 250000, 1000000})
    if (s >= raw) return s;
  return raw;
}

// ---------------------------------------------------------------- applying a change
double g_save_at = 0;   // settings::save() a moment after the last change (sliders held down)
float g_knob_from[ms::kNumItems];
double g_knob_t[ms::kNumItems];
std::string g_toast;
double g_toast_until = 0;
void after_change(const ms::Item& it) {
  logf("[menu] %s = %d\n", it.key, raw_value(it));
  if (it.store != ms::Store::Ini) return;
  const std::string k = it.key;
  if (k == "resolution" || k == "widescreen") settings::apply_display();
  else if (k == "fullscreen") {
    const bool want = settings::g.fullscreen;
    if (want != gxlle::is_fullscreen()) gxlle::toggle_fullscreen();
  } else if (k.rfind("hdr", 0) == 0 || k.rfind("col_", 0) == 0) settings::apply_hdr();   // col_: #147 colour pass, live
  if (k == "original_logic" && settings::g.original_logic) {
    // #166 (user): Original game logic puts the distance sliders back to the original, at once - the view distance is
    // live and object_distance::apply() (every frame) restores each object's own range byte, so the game's range check
    // despawns objects that are now too far the normal way
    settings::g.draw_objects = 100;
    settings::g.draw_distance = 100;
    logf("[menu] original game logic on: view distance and object distance back to 100%%\n");
  }
  if (k == "game_bloom" || k == "light" || k == "light_replace" || k == "light_replace_bloom")   // #143: the renderer's copy, live
    gxlle::g_game_bloom = settings::g.game_bloom && !(settings::g.light && settings::g.light_replace && settings::g.light_replace_bloom);
  g_save_at = now_s() + 0.4;
  settings::archive_lighting();   // #147 the archived lighting parts stay off whatever was changed
}
void set_raw(const ms::Item& it, int v) {
  using settings::g;
  const int i = int(&it - ms::kItems);
  if (is_switch(it)) { g_knob_from[i] = raw_value(it) ? 1.f : 0.f; g_knob_t[i] = now_s(); }
  if (it.store == ms::Store::Cheats) {
    if (it.cheat < 0) g.unlock_all_stages = v != 0;
    else cheats::enabled(it.cheat) = v != 0;
    after_change(it);
    return;
  }
  if (it.store == ms::Store::Live) { st32(it.addr, uint32_t(std::clamp(v, it.lo, it.hi))); after_change(it); return; }
  if (it.bfield) g.*it.bfield = v != 0;
  else if (it.ifield) g.*it.ifield = v;
  after_change(it);
}
void set_option(const ms::Item& it, int i) {
  if (is_preset(it)) { use_opt(i); return; }
  i = std::clamp(i, 0, it.n_options - 1);
  if (it.kind == ms::Kind::Resolution) {
    settings::g.*it.ifield = it.options[i].value;
    settings::g.*it.ifield2 = it.options[i].value2;
    after_change(it);
  } else {
    set_raw(it, it.options[i].value);
  }
}
void set_default(const ms::Item& it) {
  if (!has_default(it) || it.wip) return;
  if (is_preset(it)) { use_opt(0); return; }   // Y on the preset row: Original
  if (it.kind == ms::Kind::Resolution) { settings::g.*it.ifield = 0; settings::g.*it.ifield2 = 0; after_change(it); return; }
  if (it.kind == ms::Kind::Path) { settings::g.*it.sfield = ""; after_change(it); return; }
  set_raw(it, it.store == ms::Store::Cheats ? 0 : it.def);
}
void change(const ms::Item& it, int dir) {
  if (it.wip) return;   // #147 work in progress: shown greyed, not changeable
  if (is_switch(it)) { set_raw(it, dir > 0 ? 1 : 0); return; }
  if (is_list(it)) { const int i = option_index(it); set_option(it, (i < 0 ? 0 : i) + dir); return; }
  if (it.kind == ms::Kind::Slider) {
    const int step = nice_step(it.hi - it.lo, 20);   // 5 % of the range, rounded to a tidy number
    set_raw(it, std::clamp(raw_value(it) + dir * step, it.lo, it.hi));
    return;
  }
  if (it.kind == ms::Kind::Live) {
    const int step = it.step ? it.step : it.scale ? std::max(1, it.scale / 20) : nice_step(it.hi - it.lo, 100);   // #150: the schema's exact step first
    set_raw(it, std::clamp(raw_value(it) + dir * step, it.lo, it.hi));
  }
}
std::mutex g_pick_mutex;
std::string g_picked;
bool g_pick_ready = false;
void SDLCALL on_folder_picked(void*, const char* const* list, int) {
  std::lock_guard lk(g_pick_mutex);
  if (list && list[0]) { g_picked = list[0]; g_pick_ready = true; }
}
void restart_game() {   // the same exe with the same command line, then this one ends (settings saved first)
  settings::save();
  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring cmd = GetCommandLineW();
  STARTUPINFOW si{};
  si.cb = sizeof si;
  PROCESS_INFORMATION pi{};
  if (CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    logf("[menu] restart: new game started, this one ends\n");
    sys_exit_guarded(0);   // #153: no teardown (it can block on the GPU driver)
  }
  g_toast = "Could not restart - please close and start the game again.";
  g_toast_until = now_s() + 4;
}

// ---------------------------------------------------------------- state
bool g_want = false;          // open (target)
float g_tl = 0;               // open timeline, ms 0..220
double g_tl_at = 0;           // when g_tl was last advanced
int g_page = 0, g_prev_page = 0, g_page_dir = 1;
double g_page_t = -10;
int g_focus = -1;             // index into the page's rows (an Item / Restart row)
float g_focus_y = 0, g_focus_from = 0;
double g_focus_t = -10;
float g_scroll = 0, g_scroll_from = 0, g_scroll_to = 0;
double g_scroll_t = -10;
float g_scroll_dur = 0.16f;
bool g_list = false;          // a choice list is open on the focused row
int g_list_sel = 0;
int g_boot[ms::kNumItems];    // values when the menu first opened (restart_game items: tag "restart to apply" once changed)
bool g_boot_set = false;
ImVec2 g_last_mouse{-1, -1};
In g_inject{};              // MENU pad:<btn> / key:<k> (tests): pressed for one poll, released the next
int g_inject_polls = 0;
bool g_click_up = false;    // MENU click: the button is released on the next frame

std::vector<std::string> wrap(ImFont* f, float px, const std::string& s, float first, float rest);
float note_px(float u) { return fl(18 * u, 14); }
float note_w(float u) { return 780 * u - 96 * u - 20 * u; }   // the panel's text width (the fallback panel is wider: the banner is then a little tall)
std::vector<Row> rows_of(int page, float u) {
  std::vector<Row> r;
  float y = 0;
  auto add = [&](RowKind k, int idx, float h) { r.push_back({k, idx, y, h}); y += h; };
  const float sec_h = 43 * u, item_h = 62 * u;
  if (page == kControlsPage) {   // #169: reset, then one row per action and profile (idx = profile * actions + action)
    add(RowKind::Section, -5, sec_h); add(RowKind::BindReset, 0, item_h);
    for (int p = 0; p < controls::kNumProfiles; ++p) {
      add(RowKind::Section, -3 - p, sec_h);
      for (int a = 0; a < controls::kNumActions; ++a) add(RowKind::Bind, p * controls::kNumActions + a, item_h);
    }
    return r;
  }
  if (page == kSystemPage) {
    add(RowKind::Section, -1, sec_h); add(RowKind::Restart, 0, item_h); add(RowKind::Info, 0, item_h);
    add(RowKind::Section, -2, sec_h); add(RowKind::Extract, 0, item_h); add(RowKind::OpenTex, 0, item_h);   // #143
    if (kSchemaSystem < 0) return r;   // #170: else the schema's System sections follow
  }
  for (int s = 0; s < ms::kNumSections; ++s) {
    if (ms::kSections[s].page != page) continue;
    bool any = false;
    for (int i = 0; i < ms::kNumItems; ++i) {
      if (ms::kItems[i].section != s || !shown(ms::kItems[i])) continue;
      if (!any) {
        add(RowKind::Section, s, sec_h); any = true;
        if (const char* n = ms::kSections[s].note) {   // #145 the section's banner line(s)
          const int lines = std::max(1, int(wrap(f_text, note_px(u), n, note_w(u), note_w(u)).size()));
          add(RowKind::Note, s, lines * note_px(u) * 1.35f + 22 * u);
        }
      }
      add(RowKind::Item, i, item_h);
      if (is_preset(ms::kItems[i])) {   // #144: New / Save changes / Rename / Delete
        const int a = movement::active_preset();
        add(RowKind::PresetNew, 0, item_h);
        if (a >= 0 && movement::preset_modified()) add(RowKind::PresetSave, 0, item_h);
        if (a >= 0) { add(RowKind::PresetRename, 0, item_h); add(RowKind::PresetDelete, 0, item_h); }
      }
    }
  }
  return r;
}
bool focusable(const Row& r) { return r.kind != RowKind::Section && r.kind != RowKind::Note && !(r.kind == RowKind::Item && ms::kItems[r.idx].wip); }
int focus_from(const std::vector<Row>& rows, int i) { for (; i < int(rows.size()); ++i) if (focusable(rows[i])) return i; return -1; }   // the first selectable row from i (skips banners / wip)
int first_focus(const std::vector<Row>& rows) { for (int i = 0; i < int(rows.size()); ++i) if (focusable(rows[i])) return i; return -1; }
const ms::Item* focused_item(const std::vector<Row>& rows) {
  return g_focus >= 0 && g_focus < int(rows.size()) && rows[g_focus].kind == RowKind::Item ? &ms::kItems[rows[g_focus].idx] : nullptr;
}
int section_of_row(const std::vector<Row>& rows, int i) {   // the section label row a row belongs to
  for (; i >= 0; --i) if (rows[i].kind == RowKind::Section) return i;
  return -1;
}

// layout of the current frame
struct Layout {
  float W, H, u;
  bool fallback;
  float spine_w, x0, panel_w;    // panel from x0, panel_w wide
  float content_l, row_l, row_r;
  float title_y, sub_y, chips_y, list_top, list_bottom, help_top;
  float tabs_h;
};
Layout layout() {
  Layout L{};
  const ImVec2 sz = ImGui::GetMainViewport()->Size;
  L.W = sz.x; L.H = sz.y;
  float u = L.H / 1080.f;
  const float spine = n_pages() >= 6 ? 40.f : n_pages() >= 4 ? 48.f : 56.f;   // more pages: narrower spines keep the right half clear (Shadow; 7 pages since #140)
  static const bool force_fb = getenv("MENU_FALLBACK") != nullptr;
  L.fallback = force_fb || L.H < 600 || (n_pages() * spine + 780) * u > 0.6f * L.W;
  if (L.fallback) u = L.H / 720.f;
  L.u = u;
  L.spine_w = spine * u;
  L.tabs_h = L.fallback ? 46 * u : 0;
  L.x0 = L.fallback ? 0 : n_pages() * L.spine_w;
  L.panel_w = L.fallback ? L.W : 780 * u;
  L.content_l = L.x0 + 48 * u;
  L.row_l = L.x0 + 32 * u;
  L.row_r = L.x0 + L.panel_w - 32 * u;
  L.title_y = L.fallback ? L.tabs_h + 22 * u : 64 * u;
  L.sub_y = L.title_y + 58 * u;
  if (L.fallback) L.sub_y = L.title_y + 7 * u;   // fallback: no subtitle line and a 34 u title (Shadow: more rows at 480)
  L.chips_y = L.sub_y + 41 * u;
  L.help_top = L.H - 177 * u;
  L.list_bottom = L.help_top - 8 * u;
  return L;
}
// the list starts under the page's chips (one or two lines): the same rule draw_page draws them with
float chips_lines(const Layout& L, int page);
Layout full_layout() {
  Layout L = layout();
  const int lines = int(chips_lines(L, -1));
  const float chip_h = std::max(34 * L.u, fl(17 * L.u, 14) * 1.35f + 6 * L.u);
  L.list_top = L.chips_y + lines * chip_h + (lines - 1) * 6 * L.u + 18 * L.u;
  return L;
}

// set the focus (animated box) and keep it in view
void set_focus(const std::vector<Row>& rows, int i, const Layout& L, bool animate = true) {
  if (i < 0 || i >= int(rows.size())) return;
  g_focus_from = g_focus_y;
  g_focus = i;
  if (rows[i].kind == RowKind::Item) logf("[menu] focus %s\n", ms::kItems[rows[i].idx].key);
  g_focus_t = animate ? now_s() : -10;
  g_list = false;
  const float view = L.list_bottom - L.list_top;
  const Row& r = rows[i];
  float to = g_scroll_to;
  if (r.y - to < 0) to = r.y;
  if (r.y + r.h - to > view) to = r.y + r.h - view;
  const int s = section_of_row(rows, i);   // keep the section label (and its banner) in view when the first row is focused
  if (s >= 0 && focus_from(rows, s + 1) == i && rows[s].y < to && r.y + r.h - rows[s].y <= view) to = rows[s].y;
  if (focus_from(rows, i + 1) < 0 && !rows.empty()) {   // nothing selectable below (a banner, a greyed row): show it all
    const float end = rows.back().y + rows.back().h;
    if (end - to > view) to = std::min(r.y, end - view);
  }
  if (to != g_scroll_to) { g_scroll_from = g_scroll; g_scroll_to = std::max(0.f, to); g_scroll_t = now_s(); g_scroll_dur = 0.09f; }
}
void move_focus(int dir, const Layout& L) {
  const auto rows = rows_of(g_page, L.u);
  if (rows.empty()) return;
  int i = g_focus;
  for (int k = 0; k < int(rows.size()); ++k) {
    i += dir;
    if (i < 0 || i >= int(rows.size())) return;
    if (focusable(rows[i])) { set_focus(rows, i, L); return; }
  }
}
void jump_section(int dir, const Layout& L) {
  const auto rows = rows_of(g_page, L.u);
  const int cur = section_of_row(rows, std::max(0, g_focus));
  int i = cur;
  for (;;) {
    i += dir;
    if (i < 0 || i >= int(rows.size())) return;
    if (rows[i].kind == RowKind::Section) break;
  }
  if (const int f = focus_from(rows, i + 1); f >= 0) set_focus(rows, f, L);
  g_scroll_from = g_scroll; g_scroll_to = rows[i].y; g_scroll_t = now_s(); g_scroll_dur = 0.16f;   // label at the top
}
void set_page(int p, const Layout& L) {
  p = (p + n_pages()) % n_pages();
  if (p == g_page) return;
  g_page_dir = (p > g_page) ? 1 : -1;
  g_prev_page = g_page;
  g_page = p;
  logf("[menu] page %s\n", page_title(p));
  g_page_t = now_s();
  g_scroll = g_scroll_from = g_scroll_to = 0;
  const auto rows = rows_of(p, L.u);
  g_focus = first_focus(rows);
  g_focus_y = g_focus_from = g_focus >= 0 ? rows[g_focus].y : 0;
  g_focus_t = -10;
  g_list = false;
}
void activate(const Layout& L) {
  const auto rows = rows_of(g_page, L.u);
  if (g_focus < 0 || g_focus >= int(rows.size())) return;
  if (rows[g_focus].kind == RowKind::Restart) { restart_game(); return; }
  if (rows[g_focus].kind == RowKind::Bind) { controls::begin_capture(rows[g_focus].idx / controls::kNumActions, rows[g_focus].idx % controls::kNumActions); return; }   // #169
  if (rows[g_focus].kind == RowKind::BindReset) { controls::reset_all(); return; }
  if (rows[g_focus].kind == RowKind::Extract) { texture_extract::start(); return; }
  if (rows[g_focus].kind == RowKind::OpenTex) { texture_extract::open_folder(); return; }
  if (rows[g_focus].kind == RowKind::PresetNew) { movement::ask_new(); return; }   // #144
  if (rows[g_focus].kind == RowKind::PresetSave) { movement::save_into(movement::active_preset()); return; }
  if (rows[g_focus].kind == RowKind::PresetRename) { movement::ask_rename(movement::active_preset()); return; }
  if (rows[g_focus].kind == RowKind::PresetDelete) {
    if (now_s() - g_del_armed > 3) { g_del_armed = now_s(); logf("[menu] delete armed\n"); return; }
    g_del_armed = -10; const int a = movement::active_preset(); movement::delete_preset(a);
    const auto r2 = rows_of(g_page, L.u);   // the rows under the preset row are gone: focus the preset row
    for (int i = 0; i < int(r2.size()); ++i) if (r2[i].kind == RowKind::Item && is_preset(ms::kItems[r2[i].idx])) set_focus(r2, i, L, false);
    return;
  }
  const ms::Item* it = focused_item(rows);
  if (!it || it->wip) return;
  if (is_switch(*it)) set_raw(*it, raw_value(*it) ? 0 : 1);
  else if (is_list(*it)) { g_list = true; g_list_sel = std::max(0, option_index(*it)); }
  else if (it->kind == ms::Kind::Path) {
    const std::string def = settings::g.*it->sfield;
    SDL_ShowOpenFolderDialog(on_folder_picked, nullptr, gxlle::g_window, def.empty() ? nullptr : def.c_str(), false);
  }
}
void open_menu(bool on) {
  if (on == g_want) return;
  g_want = on;
  if (!on && osk::active()) osk::close();   // #144
  g_tl_at = now_s();
  g_list = false;
  if (on) {
    if (!g_boot_set) { for (int i = 0; i < ms::kNumItems; ++i) g_boot[i] = raw_value(ms::kItems[i]); g_boot_set = true; }
    const Layout L = layout();
    if (g_focus < 0) { const auto rows = rows_of(g_page, L.u); g_focus = first_focus(rows); g_focus_y = g_focus >= 0 ? rows[g_focus].y : 0; }
    logf("[menu] open (page %s)\n", page_title(g_page));
    first_boot_hint::menu_opened();   // #155: no more first-boot hint
  } else {
    if (g_save_at) { settings::save(); g_save_at = 0; }
    logf("[menu] closed\n");
    // #156 asked "Restart to apply?" here; user 2026-10-21: no pop-up - the changed row's tag ("restart to apply") and its
    // help line ("Takes effect after you restart the game") say it instead.
    std::string what;
    for (int i = 0; g_boot_set && i < ms::kNumItems; ++i)
      if (ms::kItems[i].restart == 2 && raw_value(ms::kItems[i]) != g_boot[i]) what += (what.empty() ? "" : ", ") + std::string(ms::kItems[i].label);
    if (!what.empty()) logf("[menu] changed, takes effect after a restart: %s\n", what.c_str());
  }
}

// input repeat: first press, then after 350 ms every 70 ms
struct Held { bool on = false; double t0 = 0, last = 0; };
bool rep(Held& h, bool now) {
  const double t = now_s();
  if (!now) { h.on = false; return false; }
  if (!h.on) { h.on = true; h.t0 = h.last = t; return true; }
  if (t - h.t0 >= 0.35 && t - h.last >= 0.07) { h.last = t; return true; }
  return false;
}
bool edge(bool& prev, bool now) { const bool e = now && !prev; prev = now; return e; }

float chips_lines(const Layout& L, int page) {   // page -1 = the current one
  const auto rows = rows_of(page < 0 ? g_page : page, L.u);
  const float u = L.u, max_x = L.x0 + L.panel_w - 48 * u;
  float cx = L.content_l;
  int line = 0;
  for (const Row& r : rows) {
    if (r.kind != RowKind::Section) continue;
    const float w = tsize(f_semi, fl(17 * u, 14), r.idx >= 0 ? ms::kSections[r.idx].title : r.idx == -2 ? "Textures" : r.idx == -3 ? "Controller" : r.idx == -4 ? "Keyboard and mouse" : r.idx == -5 ? "All controls" : "Game").x + 24 * u;
    if (cx + w > max_x) { if (line == 1) break; ++line; cx = L.content_l; }
    cx += w + 8 * u;
  }
  return float(line + 1);
}

// ---------------------------------------------------------------- drawing helpers
void text(ImDrawList* dl, ImFont* f, float px, ImVec2 p, ImU32 c, const char* s, const char* e = nullptr) { dl->AddText(or_default(f), px, p, c, s, e); }
float text_spaced(ImDrawList* dl, ImFont* f, float px, ImVec2 p, ImU32 c, const char* s, float spacing) {   // letter spacing
  float x = p.x;
  for (const char* q = s; *q; ++q) {
    if (dl) text(dl, f, px, ImVec2(x, p.y), c, q, q + 1);
    x += tsize(f, px, q, q + 1).x + spacing;
  }
  return x - p.x - spacing;
}
std::string upper(const char* s) { std::string r = s; for (char& ch : r) ch = char(toupper((unsigned char)ch)); return r; }
// bottom-to-top text: drawn level at `start`, its vertices then turned 90 degrees anticlockwise about `start`
void text_vertical(ImDrawList* dl, ImFont* f, float px, ImVec2 start, ImU32 c, const char* s, float spacing) {
  const int v0 = dl->VtxBuffer.Size;
  text_spaced(dl, f, px, start, c, s, spacing);
  for (int i = v0; i < dl->VtxBuffer.Size; ++i) {
    ImVec2& p = dl->VtxBuffer[i].pos;
    const float dx = p.x - start.x, dy = p.y - start.y;
    p = ImVec2(start.x + dy, start.y - dx);
  }
}
void tri(ImDrawList* dl, ImVec2 c, float s, int dir, ImU32 col) {   // ◀ (dir -1) / ▶ (dir 1)
  if (dir > 0) dl->AddTriangleFilled(ImVec2(c.x - s * .45f, c.y - s * .55f), ImVec2(c.x + s * .55f, c.y), ImVec2(c.x - s * .45f, c.y + s * .55f), col);
  else dl->AddTriangleFilled(ImVec2(c.x + s * .45f, c.y - s * .55f), ImVec2(c.x - s * .55f, c.y), ImVec2(c.x + s * .45f, c.y + s * .55f), col);
}
// word wrap: lines of at most `first` px (first line) / `rest` px (the others)
std::vector<std::string> wrap(ImFont* f, float px, const std::string& s, float first, float rest) {
  std::vector<std::string> out;
  std::string line;
  size_t i = 0;
  while (i < s.size()) {
    size_t j = s.find(' ', i);
    if (j == std::string::npos) j = s.size();
    const std::string word = s.substr(i, j - i);
    const std::string cand = line.empty() ? word : line + " " + word;
    if (!line.empty() && tsize(f, px, cand.c_str()).x > (out.empty() ? first : rest)) { out.push_back(line); line = word; }
    else line = cand;
    i = j + 1;
  }
  if (!line.empty()) out.push_back(line);
  return out;
}
std::string value_text(const ms::Item& it) {
  char b[128];
  if (is_preset(it)) return std::string(movement::active_name()) + (movement::preset_modified() ? "  (changed)" : "");
  switch (it.kind) {
    case ms::Kind::Slider: snprintf(b, sizeof b, it.fmt ? it.fmt : "%d", raw_value(it)); return b;
    case ms::Kind::Choice: case ms::Kind::Fps: case ms::Kind::Resolution: {
      const int i = option_index(it);
      if (i >= 0) return it.options[i].label;
      if (it.kind == ms::Kind::Resolution) { snprintf(b, sizeof b, "%d x %d", settings::g.*it.ifield, settings::g.*it.ifield2); return b; }
      snprintf(b, sizeof b, "%d", raw_value(it)); return b;
    }
    case ms::Kind::Path: { const std::string& s = settings::g.*it.sfield; return s.empty() ? "textures\\Custom (default)" : s; }
    case ms::Kind::Live: {
      const int v = raw_value(it);
      if (it.scale) snprintf(b, sizeof b, "%d%%", v * 100 / it.scale); else snprintf(b, sizeof b, "%d", v);
      return b;
    }
    default: return "";
  }
}
std::string fit_left(ImFont* f, float px, std::string s, float w) {   // "...end of a long path"
  if (tsize(f, px, s.c_str()).x <= w) return s;
  while (s.size() > 4 && tsize(f, px, ("..." + s).c_str()).x > w) s.erase(0, 1);
  return "..." + s;
}

// ---------------------------------------------------------------- one page's panel content
struct Hit { ImVec2 a, b; int kind; int idx; };   // mouse targets of this frame: 0 row, 1 chip, 2 spine/tab, 3 ◀, 4 ▶, 5 slider track
std::vector<Hit> g_hits;

void draw_page(ImDrawList* dl, const Layout& L, int page, float alpha, float xoff, bool active) {
  const Tint& T = tint();
  const float u = L.u;
  auto X = [&](float x) { return x + xoff; };
  // header
  text(dl, f_title, (L.fallback ? 34 : 46) * u, ImVec2(X(L.content_l), L.title_y), C(T.text, alpha), page_title(page));
  if (!L.fallback) text(dl, f_text, fl(19 * u, 14), ImVec2(X(L.content_l), L.sub_y), C(T.dim, alpha), "The game keeps running \xC2\xB7 Back closes");
  const auto rows = rows_of(page, u);
  // section chips (wrapping to two lines; "..." jumps on)
  const int cur_sec = active ? section_of_row(rows, std::max(0, g_focus)) : -1;
  float cx = L.content_l, cy = L.chips_y;
  const float chip_px = fl(17 * u, 14), chip_h = std::max(34 * u, chip_px * 1.35f + 6 * u), max_x = L.x0 + L.panel_w - 48 * u;
  int line = 0;
  for (int i = 0; i < int(rows.size()); ++i) {
    if (rows[i].kind != RowKind::Section) continue;
    const char* name = rows[i].idx >= 0 ? ms::kSections[rows[i].idx].title : rows[i].idx == -2 ? "Textures" : rows[i].idx == -3 ? "Controller" : rows[i].idx == -4 ? "Keyboard and mouse" : rows[i].idx == -5 ? "All controls" : "Game";
    const float w = tsize(f_semi, chip_px, name).x + 24 * u;
    if (cx + w > max_x) {
      if (line == 1) {   // no room: the "..." chip
        const float ew = tsize(f_semi, chip_px, "...").x + 24 * u;
        dl->AddRect(ImVec2(X(cx), cy), ImVec2(X(cx + ew), cy + chip_h), C(T.line, alpha * 2.f), 16 * u, 0, std::max(1.f, u));
        text(dl, f_semi, chip_px, ImVec2(X(cx + 12 * u), cy + (chip_h - chip_px * 1.3f) * .5f), C(T.dim, alpha), "...");
        if (active) g_hits.push_back({ImVec2(X(cx), cy), ImVec2(X(cx + ew), cy + chip_h), 1, i});
        break;
      }
      ++line; cx = L.content_l; cy += chip_h + 6 * u;
    }
    const bool on = i == cur_sec;
    dl->AddRect(ImVec2(X(cx), cy), ImVec2(X(cx + w), cy + chip_h), on ? C(kAccent, alpha) : C(T.line, alpha * 2.f), 16 * u, 0, on ? 2 * u : std::max(1.f, u));
    text(dl, f_semi, chip_px, ImVec2(X(cx + 12 * u), cy + (chip_h - chip_px * 1.3f) * .5f), C(on ? T.text : T.dim, alpha), name);
    if (active) g_hits.push_back({ImVec2(X(cx), cy), ImVec2(X(cx + w), cy + chip_h), 1, i});
    cx += w + 8 * u;
  }
  const float list_top = cy + chip_h + 18 * u;
  const float list_bottom = L.help_top - 8 * u;
  // rows
  dl->PushClipRect(ImVec2(L.x0, list_top), ImVec2(L.x0 + L.panel_w, list_bottom), true);
  const float scroll = active ? g_scroll : 0;
  const float base = list_top - scroll;
  // focus box (behind the row it is on)
  const float fy = active && g_focus >= 0 ? base + g_focus_y : -1;
  if (active && g_focus >= 0) {
    const Row& fr = rows[std::min(g_focus, int(rows.size()) - 1)];
    const ImVec2 a(X(L.row_l), fy), b(X(L.row_r), fy + fr.h);
    dl->AddRectFilled(a, b, C(T.focus, alpha), 8 * u);
    dl->AddRect(a, b, C(kAccent, alpha), 8 * u, 0, 2 * u);
    dl->AddRectFilled(ImVec2(a.x, a.y + 10 * u), ImVec2(a.x + 6 * u, b.y - 10 * u), C(kAccent, alpha), 3 * u);
  }
  for (int i = 0; i < int(rows.size()); ++i) {
    const Row& r = rows[i];
    const float y = base + r.y;
    if (y + r.h < list_top || y > list_bottom) continue;
    if (y + r.h > list_bottom + 0.5f) continue;   // only whole rows above the help line (a label goes with its first row)
    if (r.kind == RowKind::Section) {
      if (i + 1 < int(rows.size()) && base + rows[i + 1].y + rows[i + 1].h > list_bottom) continue;   // no orphan label at the bottom
      const std::string t = upper(r.idx >= 0 ? ms::kSections[r.idx].title : r.idx == -2 ? "Textures" : r.idx == -3 ? "Controller" : r.idx == -4 ? "Keyboard and mouse" : r.idx == -5 ? "All controls" : "Game");
      text_spaced(dl, f_semi, fl(17 * u, 14), ImVec2(X(L.content_l), y + 18 * u), C(T.dim, alpha), t.c_str(), 1.4f * u);
      continue;
    }
    if (r.kind == RowKind::Note) {   // #145 banner: a quiet box under the section label
      const float np = note_px(u);
      const auto lines = wrap(f_text, np, ms::kSections[r.idx].note, note_w(u), note_w(u));
      dl->AddRectFilled(ImVec2(X(L.row_l), y + 4 * u), ImVec2(X(L.row_r), y + r.h - 6 * u), C(T.line, alpha), 6 * u);
      dl->AddRectFilled(ImVec2(X(L.row_l), y + 4 * u), ImVec2(X(L.row_l) + 4 * u, y + r.h - 6 * u), C(kAccent, alpha * .8f), 2 * u);
      for (size_t k = 0; k < lines.size(); ++k) text(dl, f_text, np, ImVec2(X(L.content_l), y + 11 * u + k * np * 1.35f), C(T.dim, alpha), lines[k].c_str());
      continue;
    }
    const bool focus = active && i == g_focus;
    const float mid = y + r.h * .5f;
    if (active && !(r.kind == RowKind::Item && ms::kItems[r.idx].wip)) g_hits.push_back({ImVec2(X(L.row_l), std::max(y, list_top)), ImVec2(X(L.row_r), std::min(y + r.h, list_bottom)), 0, i});
    // separator under the row (not under the focused one)
    if (i + 1 < int(rows.size()) && rows[i + 1].kind != RowKind::Section && !focus && !(active && i + 1 == g_focus))
      dl->AddLine(ImVec2(X(L.row_l), y + r.h), ImVec2(X(L.row_r), y + r.h), C(T.line, alpha), std::max(1.f, u));
    const float label_px = 26 * u, value_px = 24 * u, vr = X(L.row_r - 44 * u);
    if (r.kind == RowKind::Restart) {
      text(dl, f_text, label_px, ImVec2(X(L.content_l), mid - label_px * .68f), C(T.text, alpha), "Restart the game");
      const char* v = "Settings marked \"after a restart\" take effect";
      text(dl, f_text, 19 * u, ImVec2(vr - tsize(f_text, 19 * u, v).x, mid - 19 * u * .68f), C(T.dim, alpha), v);
      continue;
    }
    if (r.kind == RowKind::Extract || r.kind == RowKind::OpenTex) {   // #143 textures
      const bool ex = r.kind == RowKind::Extract;
      text(dl, f_text, label_px, ImVec2(X(L.content_l), mid - label_px * .68f), C(T.text, alpha), ex ? "Extract the game's textures" : "Open the textures folder");
      std::string v = ex ? texture_extract::status() : std::string("textures\\Custom and Original");
      if (ex && v.empty()) v = "To textures\\Original";
      text(dl, f_text, 19 * u, ImVec2(vr - tsize(f_text, 19 * u, v.c_str()).x, mid - 19 * u * .68f), C(focus ? T.text : T.dim, alpha), v.c_str());
      continue;
    }
    if (r.kind >= RowKind::PresetNew) {   // #144 the preset actions
      const bool armed = r.kind == RowKind::PresetDelete && now_s() - g_del_armed < 3;
      const std::string nm = movement::active_preset() >= 0 ? movement::preset_name(movement::active_preset()) : "";
      const std::string lab = r.kind == RowKind::PresetNew ? "New preset from these sliders..." : r.kind == RowKind::PresetSave ? "Save changes"
                            : r.kind == RowKind::PresetRename ? "Rename..." : armed ? "Press A again to delete \"" + nm + "\"" : "Delete";
      text(dl, f_text, label_px, ImVec2(X(L.content_l), mid - label_px * .68f), armed ? C(kAccent, alpha) : C(T.text, alpha), lab.c_str());
      const std::string v = r.kind == RowKind::PresetNew ? "Name it with the keyboard" : r.kind == RowKind::PresetSave ? "Into \"" + nm + "\"" : nm;
      text(dl, f_text, 19 * u, ImVec2(vr - tsize(f_text, 19 * u, v.c_str()).x, mid - 19 * u * .68f), C(focus ? T.text : T.dim, alpha), v.c_str());
      continue;
    }
    if (r.kind == RowKind::Bind || r.kind == RowKind::BindReset) {   // #169 controls
      const int p = r.idx / controls::kNumActions, a = r.idx % controls::kNumActions;
      const bool cap = r.kind == RowKind::Bind && controls::capturing(p, a);
      const char* lab = r.kind == RowKind::BindReset ? "Reset all controls to default" : controls::action_label(a);
      const std::string v = r.kind == RowKind::BindReset ? std::string("Controller and keyboard")
                          : cap ? std::string(p == controls::Pad ? "Press a controller button or stick... (Esc cancels)" : "Press a key or mouse button... (Esc cancels)")
                          : controls::binding_text(p, a);
      text(dl, f_text, label_px, ImVec2(X(L.content_l), mid - label_px * .68f), C(T.text, alpha), lab);
      text(dl, f_text, 19 * u, ImVec2(vr - tsize(f_text, 19 * u, v.c_str()).x, mid - 19 * u * .68f), cap ? C(kAccent, alpha) : C(focus ? T.text : T.dim, alpha), v.c_str());
      continue;
    }
    if (r.kind == RowKind::Info) {   // #137 the build number
      text(dl, f_text, label_px, ImVec2(X(L.content_l), mid - label_px * .68f), C(T.text, alpha), "Version");
      const char* v = build_info::text();
      text(dl, f_text, value_px, ImVec2(vr - tsize(f_text, value_px, v).x, mid - value_px * .68f), C(focus ? T.text : T.dim, alpha), v);
      continue;
    }
    const ms::Item& it = ms::kItems[r.idx];
    const float ia = it.wip ? alpha * .45f : alpha;   // #147 work in progress: greyed
    text(dl, f_text, label_px, ImVec2(X(L.content_l), mid - label_px * .68f), C(it.wip ? T.dim : T.text, ia), it.label);
    float label_end = X(L.content_l) + tsize(f_text, label_px, it.label).x;
    if (it.experimental || it.wip) {   // #145 / #147 tags: amber Experimental, grey Work in progress
      const char* tag = it.wip ? "Work in progress" : "Experimental";
      const ImVec4 tc = it.wip ? T.dim : rgb(0xF2, 0xB1, 0x34);
      const float tp = fl(15 * u, 12), tw = tsize(f_text, tp, tag).x + 14 * u, tx = label_end + 10 * u, th = std::max(22 * u, tp * 1.45f);
      dl->AddRect(ImVec2(tx, mid - th * .5f), ImVec2(tx + tw, mid + th * .5f), C(tc, alpha), 10 * u, 0, std::max(1.f, u));
      text(dl, f_text, tp, ImVec2(tx + 7 * u, mid - tp * .66f), C(tc, alpha), tag);
      label_end = tx + tw;
    }
    if (it.restart) {   // tag: 1 = from the next stage, 2 = after a restart (filled once changed since the menu first opened)
      const bool pending = it.restart == 2 && g_boot_set && raw_value(it) != g_boot[r.idx];
      const char* tag = it.restart == 1 ? "next stage" : pending ? "restart to apply" : "after a restart";
      const float tp = fl(15 * u, 12), tw = tsize(f_text, tp, tag).x + 14 * u, tx = label_end + 10 * u, th = std::max(22 * u, tp * 1.45f);
      if (pending) dl->AddRectFilled(ImVec2(tx, mid - th * .5f), ImVec2(tx + tw, mid + th * .5f), C(kAccent, alpha * .25f), 10 * u);
      dl->AddRect(ImVec2(tx, mid - th * .5f), ImVec2(tx + tw, mid + th * .5f), C(kAccent, alpha), 10 * u, 0, std::max(1.f, u));
      text(dl, f_text, tp, ImVec2(tx + 7 * u, mid - tp * .66f), C(kAccent, alpha), tag);
      label_end = tx + tw;
    }
    float vl = vr;   // left edge of the value control (for the ◀ arrow)
    const ImU32 vcol = C(focus ? T.text : T.dim, it.wip ? ia : alpha);
    if (is_switch(it)) {
      const float w = 58 * u, h = 32 * u, k = 24 * u;
      const int idx = r.idx;
      const float on = raw_value(it) ? 1.f : 0.f;
      const float e = cubic_out(float((now_s() - g_knob_t[idx]) / 0.12));
      const float pos = g_knob_t[idx] > 0 ? g_knob_from[idx] + (on - g_knob_from[idx]) * e : on;
      const ImVec2 a(vr - w, mid - h * .5f), b(vr, mid + h * .5f);
      dl->AddRectFilled(a, b, on > .5f ? C(kAccent, ia) : C(T.track, ia), h * .5f);
      const float kx = a.x + 4 * u + k * .5f + pos * (w - 8 * u - k);
      dl->AddCircleFilled(ImVec2(kx, mid), k * .5f, C(rgb(255, 255, 255), ia), 24);
      vl = a.x;
    } else if (it.kind == ms::Kind::Slider || it.kind == ms::Kind::Live) {
      const std::string v = value_text(it);
      const float vw = std::max(64 * u, tsize(f_text, value_px, v.c_str()).x);
      text(dl, f_text, value_px, ImVec2(vr - tsize(f_text, value_px, v.c_str()).x, mid - value_px * .68f), vcol, v.c_str());
      if (it.kind == ms::Kind::Slider) {
        const float tr = vr - vw - 18 * u, tl = tr - 200 * u;
        const float f = it.hi > it.lo ? clamp01(float(raw_value(it) - it.lo) / float(it.hi - it.lo)) : 0.f;
        dl->AddRectFilled(ImVec2(tl, mid - 4 * u), ImVec2(tr, mid + 4 * u), C(T.track, alpha), 4 * u);
        dl->AddRectFilled(ImVec2(tl, mid - 4 * u), ImVec2(tl + (tr - tl) * f, mid + 4 * u), C(kAccent, alpha), 4 * u);
        dl->AddCircleFilled(ImVec2(tl + (tr - tl) * f, mid), 10 * u, C(rgb(255, 255, 255), alpha), 24);
        if (active) g_hits.push_back({ImVec2(tl - 10 * u, mid - 16 * u), ImVec2(tr + 10 * u, mid + 16 * u), 5, i});
        vl = tl - 10 * u;
      } else {
        vl = vr - vw;
      }
    } else {
      const float room = vr - label_end - 40 * u;
      std::string v = value_text(it);
      if (it.kind == ms::Kind::Path) v = fit_left(f_text, value_px, v, room);
      const float w = tsize(f_text, value_px, v.c_str()).x;
      text(dl, f_text, value_px, ImVec2(vr - w, mid - value_px * .68f), vcol, v.c_str());
      vl = vr - w;
    }
    if (focus && it.kind != ms::Kind::Path) {   // ◀ ▶ on the focused row
      const float s = 13 * u;
      tri(dl, ImVec2(vl - 20 * u, mid), s, -1, C(kAccent, alpha));
      tri(dl, ImVec2(X(L.row_r) - 24 * u, mid), s, 1, C(kAccent, alpha));
      if (active) {
        g_hits.push_back({ImVec2(vl - 32 * u, mid - 16 * u), ImVec2(vl - 8 * u, mid + 16 * u), 3, i});
        g_hits.push_back({ImVec2(X(L.row_r) - 36 * u, mid - 16 * u), ImVec2(X(L.row_r) - 10 * u, mid + 16 * u), 4, i});
      }
    }
  }
  dl->PopClipRect();
  // the open choice list, next to the focused row
  if (active && g_list) {
    const ms::Item* it = focused_item(rows);
    if (it && is_list(*it)) {
      const float rh = 46 * u, w = 380 * u, px = 22 * u;
      const int n = n_opts(*it), vis = std::min(n, std::max(3, int((list_bottom - list_top - 16 * u) / rh)));
      int first = std::clamp(g_list_sel - vis / 2, 0, std::max(0, n - vis));
      const float h = vis * rh + 16 * u;
      const float lo = list_top, hi = std::max(list_top, list_bottom - h);   // inside the list area (not over the help strip)
      float top = std::clamp(fy + 31 * u - h * .5f, lo, hi);
      const float left = X(L.row_r) - w;
      dl->AddRectFilled(ImVec2(left + 6 * u, top + 8 * u), ImVec2(left + w + 6 * u, top + h + 8 * u), C(rgb(0, 0, 0, .35f), alpha), 10 * u);
      dl->AddRectFilled(ImVec2(left, top), ImVec2(left + w, top + h), C(ImVec4(T.panel.x, T.panel.y, T.panel.z, 1.f), alpha), 10 * u);
      dl->AddRect(ImVec2(left, top), ImVec2(left + w, top + h), C(kAccent, alpha), 10 * u, 0, 2 * u);
      for (int k = 0; k < vis; ++k) {
        const int o = first + k;
        const float y = top + 8 * u + k * rh;
        if (o == g_list_sel) dl->AddRectFilled(ImVec2(left + 8 * u, y), ImVec2(left + w - 8 * u, y + rh), C(T.focus, alpha * 1.5f), 6 * u);
        const bool cur = o == option_index(*it);
        text(dl, f_text, px, ImVec2(left + 20 * u, y + (rh - px) * .45f), C(o == g_list_sel ? T.text : T.dim, alpha), opt_label(*it, o));
        if (cur) dl->AddCircleFilled(ImVec2(left + w - 24 * u, y + rh * .5f), 5 * u, C(kAccent, alpha), 12);
        g_hits.push_back({ImVec2(left, y), ImVec2(left + w, y + rh), 6, o});
      }
    }
  }
}

void draw_hints(ImDrawList* dl, const Layout& L, float alpha) {
  const Tint& T = tint();
  const float u = L.u;
  const auto rows = rows_of(g_page, u);
  const ms::Item* it = focused_item(rows);
  const bool restart = g_focus >= 0 && g_focus < int(rows.size()) && rows[g_focus].kind == RowKind::Restart;
  const bool info = g_focus >= 0 && g_focus < int(rows.size()) && rows[g_focus].kind == RowKind::Info;
  const RowKind fk = g_focus >= 0 && g_focus < int(rows.size()) ? rows[g_focus].kind : RowKind::Section;
  const bool pact = fk >= RowKind::PresetNew;   // #144
  // help strip
  dl->AddLine(ImVec2(L.x0, L.help_top), ImVec2(L.x0 + L.panel_w, L.help_top), C(T.line, alpha), std::max(1.f, u));
  const float hp = fl(21 * u, 15), cw = L.panel_w - 96 * u;
  float y = L.help_top + 20 * u;
  if (fk == RowKind::Bind || fk == RowKind::BindReset) {   // #169
    const bool rs = fk == RowKind::BindReset;
    const char* name = rs ? "Reset all controls" : controls::action_label(rows[g_focus].idx % controls::kNumActions);
    const char* help = rs ? "Every button, stick and key back to how the game came (controller and keyboard / mouse)."
                          : "A: press the new key, mouse button, controller button or stick direction for it (Esc cancels). Y: back to its default.";
    const float nw = tsize(f_semi, hp, name).x, dash = tsize(f_text, hp, " — ").x;
    text(dl, f_semi, hp, ImVec2(L.content_l, y), C(T.text, alpha), name);
    text(dl, f_text, hp, ImVec2(L.content_l + nw, y), C(T.dim, alpha), " — ");
    const auto lines = wrap(f_text, hp, help, cw - nw - dash, cw);
    for (size_t k = 0; k < lines.size() && k < 2; ++k)
      text(dl, f_text, hp, ImVec2(k == 0 ? L.content_l + nw + dash : L.content_l, y + k * hp * 1.3f), C(T.dim, alpha), lines[k].c_str());
  } else if (pact) {
    const char* name = fk == RowKind::PresetNew ? "New preset" : fk == RowKind::PresetSave ? "Save changes" : fk == RowKind::PresetRename ? "Rename" : "Delete";
    const char* help = fk == RowKind::PresetNew ? "Makes a preset of the sliders as they are now (from Original: the game's own values) and asks for its name - pad: the on-screen keys, or type on the keyboard."
                     : fk == RowKind::PresetSave ? "Writes the sliders as they are now into this preset."
                     : fk == RowKind::PresetRename ? "A new name for this preset (the on-screen keys, or type)."
                     : "Removes this preset for good (A twice). The game goes back to Original.";
    const float nw = tsize(f_semi, hp, name).x, dash = tsize(f_text, hp, " \xE2\x80\x94 ").x;
    text(dl, f_semi, hp, ImVec2(L.content_l, y), C(T.text, alpha), name);
    text(dl, f_text, hp, ImVec2(L.content_l + nw, y), C(T.dim, alpha), " \xE2\x80\x94 ");
    const auto lines = wrap(f_text, hp, help, cw - nw - dash, cw);
    for (size_t k = 0; k < lines.size() && k < 2; ++k)
      text(dl, f_text, hp, ImVec2(k == 0 ? L.content_l + nw + dash : L.content_l, y + k * hp * 1.3f), C(T.dim, alpha), lines[k].c_str());
  } else if (it || restart || info || (g_focus >= 0 && g_focus < int(rows.size()) && (rows[g_focus].kind == RowKind::Extract || rows[g_focus].kind == RowKind::OpenTex))) {
    const bool ex = g_focus >= 0 && g_focus < int(rows.size()) && rows[g_focus].kind == RowKind::Extract;
    const bool op = g_focus >= 0 && g_focus < int(rows.size()) && rows[g_focus].kind == RowKind::OpenTex;
    const char* name = it ? it->label : restart ? "Restart the game" : ex ? "Extract the game's textures" : op ? "Open the textures folder" : "Version";
    std::string help = it ? it->help : restart ? "Closes the game and starts it again with the same settings (saved first)."
                     : ex ? "Saves every texture of the game as a picture in textures\\Original (a few minutes; the game keeps running). Edit a copy, put it in textures\\Custom."
                     : op ? "Opens the textures folder next to the game: Original (the game's own pictures) and Custom (your edits)."
                     : std::string("This build: ") + build_info::text() + ". Quote it in bug reports.";
    if (it && it->restart == 2)   // changed since the menu opened: say plainly that it waits for a restart (no pop-up, user)
      help += g_boot_set && raw_value(*it) != g_boot[it - ms::kItems] ? " Takes effect after you restart the game (System page > Restart the game)."
                                                                      : " Takes effect after a restart (System page).";
    if (g_toast_until > now_s()) help = g_toast;
    const float nw = tsize(f_semi, hp, name).x;
    text(dl, f_semi, hp, ImVec2(L.content_l, y), C(T.text, alpha), name);
    const float dash = tsize(f_text, hp, " \xE2\x80\x94 ").x;
    text(dl, f_text, hp, ImVec2(L.content_l + nw, y), C(T.dim, alpha), " \xE2\x80\x94 ");
    const auto lines = wrap(f_text, hp, help, cw - nw - dash, cw);
    for (size_t k = 0; k < lines.size() && k < 2; ++k)
      text(dl, f_text, hp, ImVec2(k == 0 ? L.content_l + nw + dash : L.content_l, y + k * hp * 1.3f), C(T.dim, alpha), lines[k].c_str());
  }
  // controller hints
  struct H { const char* g1; const char* g2; const char* label; };
  std::vector<H> hs;
  const bool exr = g_focus >= 0 && g_focus < int(rows.size()) && (rows[g_focus].kind == RowKind::Extract || rows[g_focus].kind == RowKind::OpenTex);
  const char* a = exr ? (rows[g_focus].kind == RowKind::Extract ? "Extract" : "Open") : restart ? "Restart" : it && is_switch(*it) ? "Switch" : it && is_list(*it) ? "Choose" : it && it->kind == ms::Kind::Path ? "Browse" : "Change";
  if (pact) a = fk == RowKind::PresetNew ? "New" : fk == RowKind::PresetSave ? "Save" : fk == RowKind::PresetRename ? "Rename" : "Delete";
  if (!info) hs.push_back({"A", nullptr, g_list ? "Select" : a});
  hs.push_back({"B", nullptr, "Back"});
  if (!g_list) {
    hs.push_back({"LB", "RB", "Page"});
    hs.push_back({"LT", "RT", "Section"});
    if ((it && has_default(*it)) || fk == RowKind::Bind) hs.push_back({"Y", nullptr, "Default"});
  }
  const float gy = L.H - 40 * u - 15 * u, gr = std::max(15 * u, 10.f), lp = fl(19 * u, 14), gp = fl(17 * u, 13), kp = fl(15 * u, 13);
  float x = L.content_l;
  auto glyph = [&](const char* g) {
    if (!strcmp(g, "A") || !strcmp(g, "B") || !strcmp(g, "Y")) {
      const ImVec4 c = !strcmp(g, "A") ? rgb(0x6C, 0xC0, 0x4A) : !strcmp(g, "B") ? rgb(0xE8, 0x48, 0x4A) : rgb(0xF2, 0xB1, 0x34);
      dl->AddCircleFilled(ImVec2(x + gr, gy), gr, C(c, alpha), 24);
      const ImVec2 ts = tsize(f_semi, gp, g);
      text(dl, f_semi, gp, ImVec2(x + gr - ts.x * .5f, gy - ts.y * .5f), !strcmp(g, "Y") ? C(rgb(0x20, 0x1A, 0x08), alpha) : C(rgb(255, 255, 255), alpha), g);
      x += 2 * gr;
    } else {
      const ImVec2 ts = tsize(f_semi, kp, g);
      const float w = std::max(44 * u, ts.x + 16 * u);
      dl->AddRectFilled(ImVec2(x, gy - gr), ImVec2(x + w, gy + gr), C(T.keycap, alpha), 8 * u);
      text(dl, f_semi, kp, ImVec2(x + (w - ts.x) * .5f, gy - ts.y * .5f), C(rgb(255, 255, 255), alpha), g);
      x += w;
    }
  };
  for (const H& h : hs) {
    glyph(h.g1);
    if (h.g2) { x += 6 * u; glyph(h.g2); }
    x += 8 * u;
    text(dl, f_text, lp, ImVec2(x, gy - tsize(f_text, lp, h.label).y * .5f), C(T.dim, alpha), h.label);
    x += tsize(f_text, lp, h.label).x + 18 * u;
  }
}

// #167 (user: "you can't drag the sliders, you have to click where you want it to go"): a press on a slider's track or
// knob holds that slider until the button is let go - the value follows the mouse, also outside the track
int g_slider_drag = -1;   // the row being dragged
ImVec2 g_slider_a, g_slider_b;   // its track's hit box (as drawn when the drag began)
void slider_to(const ms::Item& it, float mx, float u) {
  const float f = clamp01((mx - g_slider_a.x - 10 * u) / (g_slider_b.x - g_slider_a.x - 20 * u));
  const int v = std::clamp(it.lo + int(std::lround(f * (it.hi - it.lo))), it.lo, it.hi);
  if (v != raw_value(it)) set_raw(it, v);
}
void handle_mouse(const Layout& L) {
  if (osk::active()) return;   // #144 the on-screen keyboard takes the mouse (osk::draw)
  ImGuiIO& io = ImGui::GetIO();
  const ImVec2 m = io.MousePos;
  const bool moved = std::fabs(m.x - g_last_mouse.x) + std::fabs(m.y - g_last_mouse.y) > 0.5f;
  g_last_mouse = m;
  const auto rows = rows_of(g_page, L.u);
  if (g_slider_drag >= 0) {   // #167 a slider held by the mouse
    if (!io.MouseDown[0] || g_slider_drag >= int(rows.size()) || rows[g_slider_drag].kind != RowKind::Item) { g_slider_drag = -1; return; }
    const ms::Item& it = ms::kItems[rows[g_slider_drag].idx];
    if (it.kind == ms::Kind::Slider && !it.wip) slider_to(it, m.x, L.u);
    return;
  }
  if (io.MouseWheel != 0 && !g_list) {   // wheel: scroll the list
    float content = rows.empty() ? 0 : rows.back().y + rows.back().h;
    g_scroll_to = std::clamp(g_scroll_to - io.MouseWheel * 120 * L.u, 0.f, std::max(0.f, content - (L.list_bottom - L.list_top)));
    g_scroll_from = g_scroll; g_scroll_t = now_s(); g_scroll_dur = 0.12f;
  }
  for (auto hit = g_hits.rbegin(); hit != g_hits.rend(); ++hit) {
    const Hit& h = *hit;
    if (m.x < h.a.x || m.x > h.b.x || m.y < h.a.y || m.y > h.b.y) continue;
    if (h.kind == 6) {   // choice list option
      if (moved) g_list_sel = h.idx;
      if (io.MouseClicked[0]) { if (const ms::Item* it = focused_item(rows)) set_option(*it, h.idx); g_list = false; }
      return;
    }
    if (g_list) { if (io.MouseClicked[0]) g_list = false; return; }
    if (h.kind == 0 && moved && h.idx != g_focus && h.idx < int(rows.size())) set_focus(rows, h.idx, L, true);
    if (!io.MouseClicked[0]) continue;
    const ms::Item* it = h.idx < int(rows.size()) ? focused_item(rows) : nullptr;
    switch (h.kind) {
      case 0: if (h.idx != g_focus) set_focus(rows, h.idx, L); activate(L); return;
      case 1: { const int s = h.idx; if (focus_from(rows, s + 1) >= 0) { set_focus(rows, focus_from(rows, s + 1), L); g_scroll_from = g_scroll; g_scroll_to = rows[s].y; g_scroll_t = now_s(); g_scroll_dur = .16f; } return; }
      case 2: set_page(h.idx, L); return;
      case 3: if (it) change(*it, -1); return;
      case 4: if (it) change(*it, 1); return;
      case 5: if (it && it->kind == ms::Kind::Slider && !it->wip) {
          g_slider_drag = h.idx; g_slider_a = h.a; g_slider_b = h.b;   // #167 held until the button is let go
          slider_to(*it, m.x, L.u);
        }
        return;
    }
  }
}

}  // namespace

// ---------------------------------------------------------------- public
static const ImWchar kRanges[] = {0x0020, 0x00FF, 0x2014, 0x2014, 0};   // Latin-1 and the em dash
void init_fonts() {
  ImGuiIO& io = ImGui::GetIO();
  float scale = 1.f;   // built for the display's height: sharp up to there (smaller windows scale the text down)
  if (gxlle::g_window) {
    SDL_Rect r;
    if (SDL_GetDisplayBounds(SDL_GetDisplayForWindow(gxlle::g_window), &r)) scale = r.h * std::max(1.f, SDL_GetWindowPixelDensity(gxlle::g_window)) / 1080.f;
  }
  scale = std::clamp(scale, 1.f, 4.f);
  auto load = [&](std::initializer_list<const char*> files, float px) -> ImFont* {
    for (const char* f : files)
      if (FILE* t = fopen(f, "rb")) { fclose(t); if (ImFont* ft = io.Fonts->AddFontFromFileTTF(f, px * scale, nullptr, kRanges)) return ft; }
    return nullptr;
  };
  f_title = load({"C:/Windows/Fonts/bahnschrift.ttf", "C:/Windows/Fonts/segoeuib.ttf"}, 46);
  f_text = load({"C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf"}, 26);
  f_semi = load({"C:/Windows/Fonts/seguisb.ttf", "C:/Windows/Fonts/segoeuib.ttf", "C:/Windows/Fonts/arialbd.ttf"}, 22);
  logf("[menu] fonts built at %.2fx\n", scale);
  texture_extract::ensure_folders();   // #143 textures\Custom + textures\Original next to the game
  logf("[build] Shadow the Hedgehog recomp %s\n", build_info::text());   // #137
}

bool is_open() { return g_want || g_tl > 0; }

void command(const std::string& cmd) {
  std::string s = cmd;
  std::replace(s.begin(), s.end(), ':', ' ');   // INPUT_FILE arguments are one token: "page:1"
  const Layout L = full_layout();
  auto arg = [&](size_t n) { return s.size() > n ? s.substr(n) : std::string(); };
  if (s == "open") open_menu(true);
  else if (s == "close") open_menu(false);
  else if (s == "up") move_focus(-1, L);
  else if (s == "down") move_focus(1, L);
  else if (s == "left" || s == "right") { const auto rows = rows_of(g_page, L.u); if (const ms::Item* it = focused_item(rows)) change(*it, s == "left" ? -1 : 1); }
  else if (s == "a") activate(L);
  else if (s == "b") { if (g_list) g_list = false; else open_menu(false); }
  else if (s == "y") { const auto rows = rows_of(g_page, L.u); if (const ms::Item* it = focused_item(rows)) set_default(*it); }
  else if (s == "next") set_page(g_page + 1, L);
  else if (s == "prev") set_page(g_page - 1, L);
  else if (s.rfind("page ", 0) == 0) set_page(atoi(arg(5).c_str()), L);
  else if (s == "sweep") {   // tests: every item changed once through the menu, checked, then put back
    int ok = 0, bad = 0;
    std::string fails;
    for (const auto& it : ms::kItems) {
      const std::string k = it.key;
      if (k == "resolution" || k == "fullscreen" || k == "widescreen" || it.kind == ms::Kind::Path || it.wip) continue;   // window: own tests; wip: not changeable
      const int before = raw_value(it), opt = option_index(it);
      change(it, 1);
      if (raw_value(it) == before) change(it, -1);   // at the top already
      const bool changed = raw_value(it) != before;
      if (is_list(it) && opt >= 0) set_option(it, opt); else set_raw(it, before);   // back
      const bool restored = raw_value(it) == before;
      if (changed && restored) ++ok; else { ++bad; fails += " " + k + (changed ? "(not restored)" : "(no change)"); }
    }
    logf("[menu] sweep: %d items change and restore, %d failed:%s\n", ok, bad, fails.c_str());
  }
  else if (s.rfind("pad ", 0) == 0 || s.rfind("key ", 0) == 0) {   // the menu's own input path, as if pressed
    const std::string b = arg(4);
    In& i = g_inject;
    i = In{};
    if (s[0] == 'p') {
      if (b == "back") i.back = true; else if (b == "x") i.x = true; else if (b == "start") i.start = true; else if (b == "a") i.a = true; else if (b == "b") i.b = true; else if (b == "y") i.y = true;
      else if (b == "lb") i.lb = true; else if (b == "rb") i.rb = true; else if (b == "lt") i.lt = true; else if (b == "rt") i.rt = true;
      else if (b == "up") i.up = true; else if (b == "down") i.down = true; else if (b == "left") i.left = true; else if (b == "right") i.right = true;
    } else {
      if (b == "esc") i.esc = true; else if (b == "enter") i.enter = true; else if (b == "bksp") i.bksp = true; else if (b == "q") i.q = true;
      else if (b == "e") i.e = true; else if (b == "pgup") i.pgup = true; else if (b == "pgdn") i.pgdn = true; else if (b == "r") i.r = true;
      else if (b == "up") i.kup = true; else if (b == "down") i.kdown = true; else if (b == "left") i.kleft = true; else if (b == "right") i.kright = true;
    }
    g_inject_polls = 2;
  }
  else if (s == "press" || s == "release") ImGui::GetIO().AddMouseButtonEvent(0, s == "press");   // #167 tests: hold / let go the left button
  else if (s.rfind("track ", 0) == 0) {   // track <key>: where that item's slider is drawn (tests: drag from / to it)
    const ms::Item* it = find_item(arg(6).c_str()); const auto rows = rows_of(g_page, L.u);
    for (const Hit& h : g_hits) if (h.kind == 5 && h.idx < int(rows.size()) && rows[h.idx].kind == RowKind::Item && &ms::kItems[rows[h.idx].idx] == it)
      logf("[menu] track %s x %.0f..%.0f y %.0f\n", it->key, h.a.x, h.b.x, (h.a.y + h.b.y) * .5f);
  }
  else if (s.rfind("mouse ", 0) == 0 || s.rfind("click ", 0) == 0) {   // mouse to x y (window pixels); click = also a left click
    float x = 0, y = 0;
    if (sscanf(arg(6).c_str(), "%f %f", &x, &y) == 2) {
      ImGuiIO& io = ImGui::GetIO();
      io.AddMousePosEvent(x, y);
      if (s[0] == 'c') { io.AddMouseButtonEvent(0, true); g_click_up = true; }
    }
  }
  else if (s.rfind("window ", 0) == 0) {   // window W H: the window to that many pixels (tests: real 1080p / 480p frames)
    int w = 0, h = 0;
    if (sscanf(arg(7).c_str(), "%d %d", &w, &h) == 2 && gxlle::g_window) {   // exact pixels (points = pixels / pixel density)
      const float d = std::max(0.25f, SDL_GetWindowPixelDensity(gxlle::g_window));
      SDL_SetWindowSize(gxlle::g_window, int(w / d + .5f), int(h / d + .5f));
      SDL_SyncWindow(gxlle::g_window);
      int pw = 0, ph = 0;
      SDL_GetWindowSizeInPixels(gxlle::g_window, &pw, &ph);
      logf("[menu] window %d x %d pixels (asked %d x %d, density %.2f)\n", pw, ph, w, h, d);
    }
  }
  else if (s.rfind("section ", 0) == 0) { const int d = atoi(arg(8).c_str()); for (int k = 0; k < std::abs(d); ++k) jump_section(d > 0 ? 1 : -1, L); }
  else if (s.rfind("focus ", 0) == 0) {   // focus <key>: that item (switching page)
    const ms::Item* it = find_item(arg(6).c_str());
    if (it) {
      set_page(ms::kSections[it->section].page, L);
      const auto rows = rows_of(g_page, L.u);
      for (int i = 0; i < int(rows.size()); ++i) if (rows[i].kind == RowKind::Item && &ms::kItems[rows[i].idx] == it) set_focus(rows, i, L, false);
      g_scroll = g_scroll_to;
      g_focus_y = rows[g_focus].y;
    }
  }
  logf("[menu] command %s\n", s.c_str());
}

void input(const In& real) {
  In in = real;
  if (g_inject_polls > 0) {
    if (g_inject_polls == 2) {   // the press
      const bool* a = reinterpret_cast<const bool*>(&g_inject);
      bool* b = reinterpret_cast<bool*>(&in);
      for (size_t k = 0; k < sizeof(In) / sizeof(bool); ++k) b[k] = b[k] || a[k];
    }
    if (--g_inject_polls == 0) g_inject = In{};
  }
  static bool p_open = false, p_a = false, p_b = false, p_y = false, p_lb = false, p_rb = false, p_lt = false, p_rt = false;
  static Held h_up, h_down, h_left, h_right;
  if (controls::capturing()) {   // #169 press-to-bind owns every control; the menu's edges stay primed (no stray press after)
    p_open = in.back || in.esc; p_a = in.a || in.enter; p_b = in.b || in.bksp; p_y = in.y || in.r;
    p_lb = in.lb || in.q; p_rb = in.rb || in.e; p_lt = in.lt || in.pgup; p_rt = in.rt || in.pgdn;
    return;
  }
  if (osk::active()) {   // #144 the on-screen keyboard owns every control while open; the menu's edges stay primed
    osk::input(osk::Keys{in.up, in.down, in.left, in.right, in.a, in.b, in.x, in.y, in.start, in.back, in.lb, in.rb,
                         in.enter, in.esc, in.bksp, in.kleft, in.kright, in.kup, in.kdown});
    p_open = in.back || in.esc; p_a = in.a || in.enter; p_b = in.b || in.bksp; p_y = in.y || in.r;
    p_lb = in.lb || in.q; p_rb = in.rb || in.e; p_lt = in.lt || in.pgup; p_rt = in.rt || in.pgdn;
    return;
  }
  if (edge(p_open, in.back || in.esc) && !in.start) { open_menu(!g_want); return; }   // Back + Start = bug capture (#135)
  if (!g_want) { p_a = in.a || in.enter; p_b = in.b || in.bksp; return; }
  const Layout L = full_layout();
  const auto rows = rows_of(g_page, L.u);
  const ms::Item* it = focused_item(rows);
  const bool up = rep(h_up, in.up || in.kup), down = rep(h_down, in.down || in.kdown);
  const bool left = rep(h_left, in.left || in.kleft), right = rep(h_right, in.right || in.kright);
  const bool a = edge(p_a, in.a || in.enter), b = edge(p_b, in.b || in.bksp), y = edge(p_y, in.y || in.r);
  const bool lb = edge(p_lb, in.lb || in.q), rb = edge(p_rb, in.rb || in.e), lt = edge(p_lt, in.lt || in.pgup), rt = edge(p_rt, in.rt || in.pgdn);
  if (g_list && it) {   // the choice list owns the controls
    if (up) g_list_sel = std::max(0, g_list_sel - 1);
    if (down) g_list_sel = std::min(n_opts(*it) - 1, g_list_sel + 1);
    if (a) { set_option(*it, g_list_sel); g_list = false; }
    if (b) g_list = false;
    return;
  }
  if (up) move_focus(-1, L);
  if (down) move_focus(1, L);
  if (it && (left || right)) change(*it, left ? -1 : 1);
  if (a) activate(L);
  if (b) open_menu(false);
  if (y && it) set_default(*it);
  if (y && !it && g_focus >= 0 && g_focus < int(rows.size()) && rows[g_focus].kind == RowKind::Bind)   // #169
    controls::reset(rows[g_focus].idx / controls::kNumActions, rows[g_focus].idx % controls::kNumActions);
  if (lb) set_page(g_page - 1, L);
  if (rb) set_page(g_page + 1, L);
  if (lt) jump_section(-1, L);
  if (rt) jump_section(1, L);
}

void draw() {
  // timeline: 0..220 ms open; closing runs it back over 140 ms
  const double t = now_s();
  const float dt_ms = float((t - g_tl_at) * 1000.0);
  g_tl_at = t;
  g_tl = g_want ? std::min(220.f, g_tl + dt_ms) : std::max(0.f, g_tl - dt_ms * 220.f / 140.f);
  if (g_save_at && t > g_save_at) { settings::save(); g_save_at = 0; }
  {
    std::lock_guard lk(g_pick_mutex);
    if (g_pick_ready) { settings::g.custom_textures_dir = g_picked; g_pick_ready = false; settings::save(); }
  }
  if (g_tl <= 0) { if (g_click_up) { ImGui::GetIO().AddMouseButtonEvent(0, false); g_click_up = false; } return; }
  const Tint& T = tint();
  Layout L = full_layout();
  const float u = L.u;
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  g_hits.clear();
  // animation state
  {
    const float fe = quad_out(float((t - g_focus_t) / 0.09));
    const auto rows = rows_of(g_page, u);
    const float target = g_focus >= 0 && g_focus < int(rows.size()) ? rows[g_focus].y : 0;
    g_focus_y = g_focus_t > 0 && fe < 1 ? g_focus_from + (target - g_focus_from) * fe : target;
    const float se = cubic_out(float((t - g_scroll_t) / g_scroll_dur));
    g_scroll = g_scroll_t > 0 && se < 1 ? g_scroll_from + (g_scroll_to - g_scroll_from) * se : g_scroll_to;
  }
  const float tl = g_tl;
  auto part = [&](float start, float dur) { return cubic_out((tl - start) / dur); };
  // #143: the spines' stagger fits the 220 ms open whatever the page count (40 ms each with 3 pages; with 7 the last ones
  // never finished sliding in: overlapping titles and a gap before the panel - the user's fullscreen screenshot)
  const float stagger = n_pages() > 1 ? std::min(40.f, 100.f / float(n_pages() - 1)) : 0.f;
  const float panel_k = part(60, 160), scrim_k = clamp01(tl / 220.f);
  if (!L.fallback) {
    // scrim: over the menu, fading out 220 u right of the panel
    const float sr = L.x0 + L.panel_w;
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(sr, L.H), C(T.scrim, scrim_k));
    dl->AddRectFilledMultiColor(ImVec2(sr, 0), ImVec2(sr + 220 * u, L.H), C(T.scrim, scrim_k), C(T.scrim, 0), C(T.scrim, 0), C(T.scrim, scrim_k));
    // spines
    for (int p = 0; p < n_pages(); ++p) {
      const float k = part(stagger * p, 120);
      const float x = p * L.spine_w - (1 - k) * L.spine_w * (p + 1);
      dl->AddRectFilled(ImVec2(x, 0), ImVec2(x + L.spine_w, L.H), C(T.spine, k));
      dl->AddLine(ImVec2(x + L.spine_w - 1, 0), ImVec2(x + L.spine_w - 1, L.H), C(T.line, k), std::max(1.f, u));
      const bool on = p == g_page;
      const std::string title = upper(page_title(p));
      const float tw = text_spaced(nullptr, f_semi, 22 * u, ImVec2(0, 0), 0, title.c_str(), 1.5f * u);
      const float th = 22 * u * 1.15f;
      text_vertical(dl, f_semi, 22 * u, ImVec2(x + (L.spine_w - th) * .5f, 264 * u + tw * .5f), C(on ? T.text : T.dim, k), title.c_str(), 1.5f * u);
      g_hits.push_back({ImVec2(x, 0), ImVec2(x + L.spine_w, L.H), 2, p});
    }
    // the accent bar on the active spine (slides with page switches)
    {
      const float pe = cubic_out(float((t - g_page_t) / 0.14));
      const float from = g_prev_page * L.spine_w, to = g_page * L.spine_w;
      const float bx = from + (to - from) * pe;
      dl->AddRectFilled(ImVec2(bx, 0), ImVec2(bx + 5 * u, L.H), C(kAccent, part(stagger * g_page, 120)));
    }
  }
  // panel
  const float slide = L.fallback ? 0 : -(1 - panel_k) * 40 * u;
  const float rise = L.fallback ? (1 - panel_k) * 24 * u : 0;
  {
    const float x0 = L.x0 + slide;
    const ImVec4 pc = L.fallback ? ImVec4(T.panel.x, T.panel.y, T.panel.z, .98f) : T.panel;   // full screen: the game must not read through
    dl->AddRectFilled(ImVec2(x0, rise), ImVec2(x0 + L.panel_w, L.H + rise), C(pc, panel_k));
    if (!L.fallback)
      dl->AddRectFilledMultiColor(ImVec2(x0 + L.panel_w, 0), ImVec2(x0 + L.panel_w + 24 * u, L.H), C(rgb(0, 0, 0, .25f), panel_k), C(rgb(0, 0, 0, 0)),
                                  C(rgb(0, 0, 0, 0)), C(rgb(0, 0, 0, .25f), panel_k));
  }
  if (L.fallback) {   // tabs across the top
    const float k = part(0, 120), y = -(1 - k) * 46 * u, w = L.W / n_pages();
    dl->AddRectFilled(ImVec2(0, y), ImVec2(L.W, y + L.tabs_h), C(T.spine, k));
    for (int p = 0; p < n_pages(); ++p) {
      const std::string title = upper(page_title(p));
      const float tw = text_spaced(nullptr, f_semi, 20 * u, ImVec2(0, 0), 0, title.c_str(), 1.2f * u);
      text_spaced(dl, f_semi, 20 * u, ImVec2(p * w + (w - tw) * .5f, y + 11 * u), C(p == g_page ? T.text : T.dim, k), title.c_str(), 1.2f * u);
      g_hits.push_back({ImVec2(p * w, y), ImVec2((p + 1) * w, y + L.tabs_h), 2, p});
    }
    const float pe = cubic_out(float((t - g_page_t) / 0.14));
    const float bx = (g_prev_page + (g_page - g_prev_page) * pe) * w;
    dl->AddRectFilled(ImVec2(bx, y + L.tabs_h - 4 * u), ImVec2(bx + w, y + L.tabs_h), C(kAccent, k));
  }
  // page content: crossfade with a 24 u slide on page switches
  dl->PushClipRect(ImVec2(L.x0, L.tabs_h), ImVec2(L.x0 + L.panel_w, L.H), true);
  const float pe = cubic_out(float((t - g_page_t) / 0.14));
  if (pe < 1 && g_prev_page != g_page) draw_page(dl, L, g_prev_page, panel_k * (1 - pe), slide - g_page_dir * 24 * u * pe, false);
  draw_page(dl, L, g_page, panel_k * (pe < 1 ? pe : 1), slide + (pe < 1 ? g_page_dir * 24 * u * (1 - pe) : 0), true);
  dl->PopClipRect();
  if (!osk::active()) draw_hints(dl, L, panel_k);   // #144 the keyboard card has its own hint line
  if (osk::active()) {   // #144 over the panel, in the menu's colours
    osk::Style st{C(ImVec4(T.panel.x, T.panel.y, T.panel.z, 1.f)), C(T.text), C(T.dim), C(T.line), C(T.track), C(T.focus, 1.6f), C(kAccent), C(T.keycap), C(rgb(0, 0, 0, .45f)), f_text, f_semi};
    osk::draw(dl, ImVec2(L.x0, L.tabs_h), ImVec2(L.x0 + L.panel_w, L.H), u, st);
  }
  if (g_want && g_tl >= 220) handle_mouse(L);
  if (g_click_up && ImGui::GetIO().MouseDown[0]) { ImGui::GetIO().AddMouseButtonEvent(0, false); g_click_up = false; }
}

}  // namespace game_menu
