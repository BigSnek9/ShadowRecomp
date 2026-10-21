// The launcher: shadow.exe opens on this screen (a compact window of its own) and the game only boots when the player
// presses Play; the game window size and fullscreen are applied then (gxlle::ui_resume_game). Every PC setting lives
// here (there is no in-game settings menu). Pages: Home, Display, Lighting, Audio, Controls, Gameplay, Textures,
// Models, About. Mouse first; keyboard / controller navigation through ImGui's nav.
// Skipped by --stage / --fast / --quiet / --no-launcher (tests and tools) and by env SHADOW_NO_LAUNCHER.
//
// Look: dark, Shadow-red accents, soft glows behind glass panels. Everything is drawn with ImGui's draw list (no extra
// UI library); the art is the game's emblem and its title-menu Shadow (resources 201 / 202).
// Performance: the launcher draws at most ~120 frames per second and sleeps between frames; animations run on real
// time (ImGui's DeltaTime), never per frame. The game itself is not running yet.
#include "settings_ui.h"
#include "game_menu.h"
#include "movement.h"
#include "cheats.h"
#include "gx_lle.h"
#include "sys.h"
#include <imgui.h>
#include <SDL3/SDL.h>
#include <aurora/gfx.h>
#include <aurora/imgui.h>
#include <png.h>
#include <cstring>
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
extern std::string g_disc_root;   // disc.cpp

namespace settings {
namespace {
// ---------------------------------------------------------------------------------------------------- look
constexpr float kDesignH = 820.0f;   // layout units: 1 unit = 1 pixel at a window this tall
ImFont* f_body = nullptr;    // body text
ImFont* f_small = nullptr;   // help / captions
ImFont* f_label = nullptr;   // semibold labels, navigation
ImFont* f_h2 = nullptr;      // section titles, values
ImFont* f_h1 = nullptr;      // page titles
ImFont* f_title = nullptr;   // the game title
float g_ui = 1.0f;           // layout scale this frame (window height / kDesignH)
float g_font_ui = 1.0f;      // scale the fonts were built at (FontGlobalScale = g_ui / g_font_ui)
float fsz(const ImFont* f) { return f->FontSize * ImGui::GetIO().FontGlobalScale; }
float U(float v) { return v * g_ui; }

ImU32 rgba(int r, int g, int b, float a) { return IM_COL32(r, g, b, (int)(std::clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f)); }
ImU32 white(float a) { return rgba(255, 255, 255, a); }
ImU32 mix(ImU32 a, ImU32 b, float t) {
  ImVec4 x = ImGui::ColorConvertU32ToFloat4(a), y = ImGui::ColorConvertU32ToFloat4(b);
  return ImGui::ColorConvertFloat4ToU32(ImVec4(x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t, x.z + (y.z - x.z) * t, x.w + (y.w - x.w) * t));
}
const ImU32 kBg = IM_COL32(10, 10, 13, 255), kText = IM_COL32(238, 238, 242, 255), kSoft = IM_COL32(196, 198, 208, 255),
            kMuted = IM_COL32(138, 141, 154, 255), kFaint = IM_COL32(98, 100, 112, 255), kRed = IM_COL32(210, 24, 56, 255),
            kRedHi = IM_COL32(240, 60, 90, 255), kRedDeep = IM_COL32(150, 14, 40, 255), kGold = IM_COL32(232, 186, 74, 255);
ImVec4 col(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

void apply_style() {
  ImGuiStyle& s = ImGui::GetStyle();
  s = ImGuiStyle();
  ImGui::StyleColorsDark(&s);
  const float u = g_ui;
  s.WindowRounding = 0; s.ChildRounding = 14 * u; s.FrameRounding = 8 * u; s.PopupRounding = 10 * u;
  s.GrabRounding = 6 * u; s.ScrollbarRounding = 6 * u; s.TabRounding = 6 * u;
  s.WindowPadding = ImVec2(0, 0); s.FramePadding = ImVec2(12 * u, 7 * u); s.ItemSpacing = ImVec2(10 * u, 8 * u);
  s.ItemInnerSpacing = ImVec2(8 * u, 6 * u); s.ScrollbarSize = 8 * u; s.GrabMinSize = 12 * u; s.WindowBorderSize = 0;
  s.ChildBorderSize = 0; s.PopupBorderSize = 1; s.FrameBorderSize = 1;
  ImVec4* c = s.Colors;
  c[ImGuiCol_WindowBg] = col(kBg); c[ImGuiCol_ChildBg] = col(IM_COL32(0, 0, 0, 0)); c[ImGuiCol_PopupBg] = col(IM_COL32(22, 21, 27, 250));
  c[ImGuiCol_Text] = col(kText); c[ImGuiCol_TextDisabled] = col(kFaint); c[ImGuiCol_Border] = col(white(0.08f));
  c[ImGuiCol_FrameBg] = col(white(0.05f)); c[ImGuiCol_FrameBgHovered] = col(white(0.08f)); c[ImGuiCol_FrameBgActive] = col(white(0.11f));
  c[ImGuiCol_SliderGrab] = col(kRed); c[ImGuiCol_SliderGrabActive] = col(kRedHi); c[ImGuiCol_CheckMark] = col(kRed);
  c[ImGuiCol_Button] = col(white(0.06f)); c[ImGuiCol_ButtonHovered] = col(white(0.10f)); c[ImGuiCol_ButtonActive] = col(white(0.14f));
  c[ImGuiCol_Header] = col(rgba(210, 24, 56, 0.22f)); c[ImGuiCol_HeaderHovered] = col(rgba(210, 24, 56, 0.34f));
  c[ImGuiCol_HeaderActive] = col(rgba(210, 24, 56, 0.48f)); c[ImGuiCol_ScrollbarBg] = col(IM_COL32(0, 0, 0, 0));
  c[ImGuiCol_ScrollbarGrab] = col(white(0.10f)); c[ImGuiCol_ScrollbarGrabHovered] = col(white(0.16f));
  c[ImGuiCol_ScrollbarGrabActive] = col(white(0.20f)); c[ImGuiCol_NavHighlight] = col(kRedHi); c[ImGuiCol_Separator] = col(white(0.07f));
}

// ---------------------------------------------------------------------------------------------------- drawing helpers
// Smooth 0..1 that follows `on` over real time (not frames): hover / selection fades.
float anim(ImGuiID id, bool on, float speed = 14.0f) {
  float* v = ImGui::GetStateStorage()->GetFloatRef(id, 0.0f);
  *v += ((on ? 1.0f : 0.0f) - *v) * (1.0f - std::exp(-ImGui::GetIO().DeltaTime * speed));
  return *v;
}
// Recolour the vertices added since v0 with a linear gradient p0 -> p1 (alpha included; anti-aliased edges kept).
void shade(ImDrawList* dl, int v0, ImVec2 p0, ImVec2 p1, ImU32 c0, ImU32 c1) {
  const ImVec2 d(p1.x - p0.x, p1.y - p0.y);
  const float len2 = std::max(d.x * d.x + d.y * d.y, 1e-6f);
  const ImVec4 a = col(c0), b = col(c1);
  for (int i = v0; i < dl->VtxBuffer.Size; ++i) {
    ImDrawVert& v = dl->VtxBuffer[i];
    float t = std::clamp(((v.pos.x - p0.x) * d.x + (v.pos.y - p0.y) * d.y) / len2, 0.0f, 1.0f);
    float va = ((v.col >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f;
    v.col = ImGui::ColorConvertFloat4ToU32(ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t,
                                                  (a.w + (b.w - a.w) * t) * va));
  }
}
void grad_rect(ImDrawList* dl, ImVec2 a, ImVec2 b, ImVec2 p0, ImVec2 p1, ImU32 c0, ImU32 c1, float r = 0, ImDrawFlags fl = 0) {
  int v0 = dl->VtxBuffer.Size;
  dl->AddRectFilled(a, b, IM_COL32_WHITE, r, fl);
  shade(dl, v0, p0, p1, c0, c1);
}
// A soft round glow: colour at the centre fading smoothly to nothing at radius r.
void radial(ImDrawList* dl, ImVec2 c, float r, ImU32 colr) {
  const int seg = 72, rings = 7;
  const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
  const ImVec4 cf = col(colr);
  dl->PrimReserve(seg * 3 + (rings - 1) * seg * 6, 1 + seg * rings);
  const ImDrawIdx base = (ImDrawIdx)dl->_VtxCurrentIdx;
  dl->PrimWriteVtx(c, uv, colr);
  for (int k = 1; k <= rings; ++k) {
    float t = (float)k / rings, a = 1.0f - t * t * (3.0f - 2.0f * t);
    ImU32 cc = ImGui::ColorConvertFloat4ToU32(ImVec4(cf.x, cf.y, cf.z, cf.w * a));
    for (int i = 0; i < seg; ++i) {
      float ang = i * 6.2831853f / seg;
      dl->PrimWriteVtx(ImVec2(c.x + std::cos(ang) * r * t, c.y + std::sin(ang) * r * t), uv, cc);
    }
  }
  for (int i = 0; i < seg; ++i) {
    dl->PrimWriteIdx(base); dl->PrimWriteIdx((ImDrawIdx)(base + 1 + i)); dl->PrimWriteIdx((ImDrawIdx)(base + 1 + (i + 1) % seg));
  }
  for (int k = 0; k < rings - 1; ++k)
    for (int i = 0; i < seg; ++i) {
      ImDrawIdx a0 = (ImDrawIdx)(base + 1 + k * seg + i), a1 = (ImDrawIdx)(base + 1 + k * seg + (i + 1) % seg);
      ImDrawIdx b0 = (ImDrawIdx)(a0 + seg), b1 = (ImDrawIdx)(a1 + seg);
      dl->PrimWriteIdx(a0); dl->PrimWriteIdx(b0); dl->PrimWriteIdx(b1);
      dl->PrimWriteIdx(a0); dl->PrimWriteIdx(b1); dl->PrimWriteIdx(a1);
    }
}
void soft_shadow(ImDrawList* dl, ImVec2 a, ImVec2 b, float r, float strength = 1.0f) {
  for (int i = 4; i >= 1; --i) {
    float g = U(3.0f * i);
    dl->AddRectFilled(ImVec2(a.x - g * 0.6f, a.y - g * 0.2f + U(4)), ImVec2(b.x + g * 0.6f, b.y + g + U(4)),
                      rgba(0, 0, 0, 0.07f * strength), r + g);
  }
}
// The panel look: dark, nearly opaque, a little lighter at the top, hairline border and a top highlight.
void panel(ImDrawList* dl, ImVec2 a, ImVec2 b, float r, float hover = 0.0f, bool shadow = true) {
  if (shadow) soft_shadow(dl, a, b, r);
  grad_rect(dl, a, b, a, ImVec2(a.x, b.y), mix(rgba(25, 24, 30, 0.94f), rgba(32, 30, 38, 0.96f), hover),
            mix(rgba(17, 17, 21, 0.94f), rgba(22, 21, 27, 0.96f), hover), r);
  dl->AddRect(a, b, white(0.07f + 0.06f * hover), r, 0, 1.0f);
  dl->AddLine(ImVec2(a.x + r, a.y + 1), ImVec2(b.x - r, a.y + 1), white(0.06f + 0.04f * hover), 1.0f);
}
void text_at(ImDrawList* dl, ImFont* f, ImVec2 p, ImU32 c, const char* s) { dl->AddText(f, fsz(f), p, c, s); }
ImVec2 text_size(ImFont* f, const char* s) {
  ImVec2 v = f->CalcTextSizeA(fsz(f), FLT_MAX, 0.0f, s);
  return v;
}
void hand_cursor() { if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand); }

// Line icons (drawn, no icon font). c = centre, s = size.
enum Icon { kIcHome, kIcDisplay, kIcSun, kIcSpeaker, kIcPad, kIcFlag, kIcImage, kIcCube, kIcInfo, kIcPower, kIcPlay,
            kIcFolder, kIcSparkle, kIcExpand, kIcGauge, kIcHdr };
void icon(ImDrawList* dl, int id, ImVec2 c, float s, ImU32 k) {
  const float t = std::max(1.0f, U(1.5f));
  auto P = [&](float x, float y) { return ImVec2(c.x + x * s, c.y + y * s); };
  switch (id) {
    case kIcHome: {
      ImVec2 roof[] = {P(-0.5f, -0.02f), P(0, -0.46f), P(0.5f, -0.02f)};
      dl->AddPolyline(roof, 3, k, 0, t);
      ImVec2 body[] = {P(-0.34f, -0.15f), P(-0.34f, 0.44f), P(0.34f, 0.44f), P(0.34f, -0.15f)};
      dl->AddPolyline(body, 4, k, 0, t);
      dl->AddLine(P(-0.08f, 0.44f), P(-0.08f, 0.16f), k, t); dl->AddLine(P(-0.08f, 0.16f), P(0.08f, 0.16f), k, t);
      dl->AddLine(P(0.08f, 0.16f), P(0.08f, 0.44f), k, t);
      break;
    }
    case kIcDisplay:
      dl->AddRect(P(-0.5f, -0.4f), P(0.5f, 0.24f), k, s * 0.08f, 0, t);
      dl->AddLine(P(0, 0.24f), P(0, 0.42f), k, t); dl->AddLine(P(-0.24f, 0.44f), P(0.24f, 0.44f), k, t);
      break;
    case kIcSun:
      dl->AddCircle(c, s * 0.19f, k, 0, t);
      for (int i = 0; i < 8; ++i) {
        float a = i * 0.785398f;
        dl->AddLine(P(std::cos(a) * 0.33f, std::sin(a) * 0.33f), P(std::cos(a) * 0.48f, std::sin(a) * 0.48f), k, t);
      }
      break;
    case kIcSpeaker: {
      ImVec2 sp[] = {P(-0.46f, -0.14f), P(-0.22f, -0.14f), P(0.02f, -0.38f), P(0.02f, 0.38f), P(-0.22f, 0.14f), P(-0.46f, 0.14f)};
      dl->AddPolyline(sp, 6, k, ImDrawFlags_Closed, t);
      dl->PathArcTo(P(0.06f, 0), s * 0.2f, -0.8f, 0.8f, 10); dl->PathStroke(k, 0, t);
      dl->PathArcTo(P(0.06f, 0), s * 0.38f, -0.85f, 0.85f, 14); dl->PathStroke(k, 0, t);
      break;
    }
    case kIcPad:
      dl->AddRect(P(-0.5f, -0.26f), P(0.5f, 0.3f), k, s * 0.26f, 0, t);
      dl->AddLine(P(-0.3f, 0.02f), P(-0.1f, 0.02f), k, t); dl->AddLine(P(-0.2f, -0.08f), P(-0.2f, 0.12f), k, t);
      dl->AddCircleFilled(P(0.16f, 0.06f), s * 0.05f, k); dl->AddCircleFilled(P(0.3f, -0.05f), s * 0.05f, k);
      break;
    case kIcFlag: {
      dl->AddLine(P(-0.34f, -0.48f), P(-0.34f, 0.48f), k, t);
      ImVec2 fl[] = {P(-0.34f, -0.42f), P(0.42f, -0.24f), P(-0.34f, -0.04f)};
      dl->AddPolyline(fl, 3, k, 0, t);
      break;
    }
    case kIcImage: {
      dl->AddRect(P(-0.5f, -0.4f), P(0.5f, 0.4f), k, s * 0.1f, 0, t);
      ImVec2 m[] = {P(-0.42f, 0.32f), P(-0.1f, -0.02f), P(0.14f, 0.2f), P(0.28f, 0.06f), P(0.44f, 0.24f)};
      dl->AddPolyline(m, 5, k, 0, t);
      dl->AddCircle(P(0.22f, -0.18f), s * 0.08f, k, 0, t);
      break;
    }
    case kIcCube: {
      ImVec2 h[] = {P(0, -0.5f), P(0.44f, -0.25f), P(0.44f, 0.25f), P(0, 0.5f), P(-0.44f, 0.25f), P(-0.44f, -0.25f)};
      dl->AddPolyline(h, 6, k, ImDrawFlags_Closed, t);
      dl->AddLine(P(-0.44f, -0.25f), P(0, 0), k, t); dl->AddLine(P(0.44f, -0.25f), P(0, 0), k, t); dl->AddLine(P(0, 0), P(0, 0.5f), k, t);
      break;
    }
    case kIcInfo:
      dl->AddCircle(c, s * 0.46f, k, 0, t);
      dl->AddLine(P(0, -0.02f), P(0, 0.24f), k, t); dl->AddCircleFilled(P(0, -0.2f), s * 0.05f, k);
      break;
    case kIcPower:
      dl->PathArcTo(c, s * 0.4f, -1.5708f + 0.6f, 4.7124f - 0.6f, 24);   // open at the top
      dl->PathStroke(k, 0, t);
      dl->AddLine(P(0, -0.5f), P(0, -0.06f), k, t);
      break;
    case kIcPlay: {
      dl->AddTriangleFilled(P(-0.3f, -0.42f), P(0.45f, 0), P(-0.3f, 0.42f), k);
      break;
    }
    case kIcFolder: {
      ImVec2 f[] = {P(-0.5f, -0.36f), P(-0.12f, -0.36f), P(0, -0.22f), P(0.5f, -0.22f), P(0.5f, 0.4f), P(-0.5f, 0.4f)};
      dl->AddPolyline(f, 6, k, ImDrawFlags_Closed, t);
      break;
    }
    case kIcSparkle: {
      ImVec2 st[] = {P(0, -0.5f), P(0.12f, -0.12f), P(0.5f, 0), P(0.12f, 0.12f), P(0, 0.5f), P(-0.12f, 0.12f), P(-0.5f, 0), P(-0.12f, -0.12f)};
      dl->AddPolyline(st, 8, k, ImDrawFlags_Closed, t);
      break;
    }
    case kIcExpand:
      dl->AddLine(P(-0.45f, -0.15f), P(-0.45f, -0.45f), k, t); dl->AddLine(P(-0.45f, -0.45f), P(-0.15f, -0.45f), k, t);
      dl->AddLine(P(0.45f, 0.15f), P(0.45f, 0.45f), k, t); dl->AddLine(P(0.45f, 0.45f), P(0.15f, 0.45f), k, t);
      dl->AddLine(P(0.15f, -0.45f), P(0.45f, -0.45f), k, t); dl->AddLine(P(0.45f, -0.45f), P(0.45f, -0.15f), k, t);
      dl->AddLine(P(-0.45f, 0.15f), P(-0.45f, 0.45f), k, t); dl->AddLine(P(-0.45f, 0.45f), P(-0.15f, 0.45f), k, t);
      break;
    case kIcGauge:
      dl->PathArcTo(P(0, 0.12f), s * 0.46f, 3.14159f, 6.28318f, 20); dl->PathStroke(k, 0, t);
      dl->AddLine(P(0, 0.12f), P(0.22f, -0.16f), k, t); dl->AddCircleFilled(P(0, 0.12f), s * 0.06f, k);
      break;
    case kIcHdr:
      dl->AddRect(P(-0.5f, -0.34f), P(0.5f, 0.34f), k, s * 0.1f, 0, t);
      dl->AddTriangleFilled(P(-0.36f, 0.22f), P(0.36f, -0.22f), P(0.36f, 0.22f), mix(k, IM_COL32(0, 0, 0, 0), 0.4f));
      break;
  }
}

// ---------------------------------------------------------------------------------------------------- data helpers
struct Res { int w, h; };
std::vector<Res> resolutions() {
  static std::vector<Res> list;
  if (!list.empty()) return list;
  const Res common[] = {{3840, 2160}, {3440, 1440}, {2560, 1600}, {2560, 1440}, {2560, 1080}, {2048, 1536}, {1920, 1440},
                        {1920, 1200}, {1920, 1080}, {1680, 1050}, {1600, 1200}, {1600, 900}, {1440, 1080}, {1440, 900},
                        {1366, 768},  {1280, 1024}, {1280, 960},  {1280, 800},  {1280, 720},  {1024, 768}, {800, 600}, {640, 480}};
  for (const Res& r : common) list.push_back(r);
  int n = 0;
  if (SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(SDL_GetPrimaryDisplay(), &n)) {
    for (int i = 0; i < n; ++i) list.push_back({modes[i]->w, modes[i]->h});
    SDL_free(modes);
  }
  std::sort(list.begin(), list.end(), [](const Res& a, const Res& b) { return a.h != b.h ? a.h > b.h : a.w > b.w; });
  list.erase(std::unique(list.begin(), list.end(), [](const Res& a, const Res& b) { return a.w == b.w && a.h == b.h; }), list.end());
  return list;
}
std::vector<int> fps_values() {
  std::vector<int> v = {30, 60, 120, 144, 165, 240, 360};
  int hz = display_refresh_hz();
  if (std::find(v.begin(), v.end(), hz) == v.end()) v.push_back(hz);
  std::sort(v.begin(), v.end());
  v.push_back(0);   // unlimited
  return v;
}
std::string fps_name(int v) {
  if (v == 0) return "Unlimited";
  char b[40];
  if (v == 60) snprintf(b, sizeof b, "60 (original)");
  else if (v == display_refresh_hz()) snprintf(b, sizeof b, "%d (display)", v);
  else snprintf(b, sizeof b, "%d", v);
  return b;
}
std::string u8(const fs::path& p) {   // UTF-8 path text (C++20 u8string() is std::u8string)
  auto s = p.u8string();
  return std::string(s.begin(), s.end());
}
std::string u8g(const fs::path& p) {
  auto s = p.generic_u8string();
  return std::string(s.begin(), s.end());
}
fs::path exe_dir() {
  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  return fs::path(exe).parent_path();
}
fs::path textures_folder() {
  if (!g.custom_textures_dir.empty()) {
    std::error_code ec;
    fs::path p = fs::u8path(g.custom_textures_dir);
    if (fs::is_directory(p, ec)) return p;
  }
  return exe_dir() / L"textures" / L"Custom";
}
fs::path mods_folder() { return exe_dir() / L"mods" / L"files"; }
void open_in_explorer(const fs::path& p) {
  std::error_code ec;
  fs::create_directories(p, ec);
  ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// custom texture count, counted on a worker thread (a big folder takes a moment)
std::atomic<int> g_tex_count{-1};
std::atomic<bool> g_tex_counting{false};
std::string g_tex_counted_for;
void count_textures() {
  std::string want = u8(textures_folder());
  if (g_tex_counting || want == g_tex_counted_for) return;
  g_tex_counted_for = want;
  g_tex_counting = true;
  g_tex_count = -1;
  std::thread([want] {
    int n = 0;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(fs::u8path(want), fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
      std::string s = u8(it->path().filename());
      if (s.find("[tex1_") != std::string::npos || s.rfind("tex1_", 0) == 0) ++n;
    }
    g_tex_count = n;
    g_tex_counting = false;
  }).detach();
}

// folder picker (SDL's dialog calls back on another thread)
std::mutex g_pick_mutex;
std::string g_picked;
bool g_pick_ready = false;
void SDLCALL on_folder_picked(void*, const char* const* list, int) {
  std::lock_guard lk(g_pick_mutex);
  if (list && list[0]) { g_picked = list[0]; g_pick_ready = true; }
}

// model mods: <exe dir>\mods\files\<disc path> (runtime/disc.cpp); a disabled one is renamed "<file>.disabled"
struct ModFile { fs::path path; std::string rel; bool enabled; uintmax_t size; };
std::vector<ModFile> g_mods;
uint64_t g_mods_at = 0;
const std::vector<ModFile>& list_mods(bool refresh = false) {   // re-read at most twice a second
  uint64_t now = sys_time_ns();
  if (!refresh && g_mods_at && now - g_mods_at < 500000000ull) return g_mods;
  g_mods_at = now;
  g_mods.clear();
  std::error_code ec;
  const fs::path root = mods_folder();
  for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
       !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    fs::path p = it->path();
    bool on = p.extension() != L".disabled";
    fs::path shown = on ? p : p.parent_path() / p.stem();
    g_mods.push_back({p, u8g(fs::relative(shown, root, ec)), on, it->file_size(ec)});
  }
  std::sort(g_mods.begin(), g_mods.end(), [](const ModFile& a, const ModFile& b) { return a.rel < b.rel; });
  return g_mods;
}

// ---------------------------------------------------------------------------------------------------- art
// The game's red Shadow emblem (resource 201) and the title-menu Shadow (resource 202), uploaded once as ImGui textures.
struct Art { ImTextureID tex = 0; float aspect = 1.0f; };
Art g_emblem, g_hero;
Art load_art(int id) {
  Art a;
  HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
  HGLOBAL h = r ? LoadResource(nullptr, r) : nullptr;
  const void* data = h ? LockResource(h) : nullptr;
  if (!data) return a;
  png_image pi;
  memset(&pi, 0, sizeof pi);
  pi.version = PNG_IMAGE_VERSION;
  if (!png_image_begin_read_from_memory(&pi, data, SizeofResource(nullptr, r))) return a;
  pi.format = PNG_FORMAT_RGBA;
  std::vector<uint8_t> px(PNG_IMAGE_SIZE(pi));
  if (png_image_finish_read(&pi, nullptr, px.data(), 0, nullptr)) {
    a.tex = aurora_imgui_add_texture(pi.width, pi.height, px.data());
    a.aspect = (float)pi.width / (float)pi.height;
  }
  png_image_free(&pi);
  return a;
}
// the emblem in a box of height h with its top-left at p
void draw_emblem(ImDrawList* dl, ImVec2 p, float h, ImU32 tint = IM_COL32_WHITE) {
  if (!g_emblem.tex) return;
  dl->AddImage(g_emblem.tex, p, ImVec2(p.x + h * g_emblem.aspect, p.y + h), ImVec2(0, 0), ImVec2(1, 1), tint);
}

// ---------------------------------------------------------------------------------------------------- widgets
bool g_play = false;

// The red call-to-action. Returns true when clicked.
bool play_button(const char* id, ImVec2 size, ImFont* font, const char* label) {
  ImVec2 p = ImGui::GetCursorScreenPos();
  bool click = ImGui::InvisibleButton(id, size);
  bool hov = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
  hand_cursor();
  float h = anim(ImGui::GetItemID(), hov);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 q(p.x + size.x, p.y + size.y);
  const float r = std::min(U(12), size.y * 0.5f);
  for (int i = 3; i >= 1; --i) {   // red glow under it, stronger on hover
    float g = U(4.0f * i);
    dl->AddRectFilled(ImVec2(p.x - g * 0.5f, p.y - g * 0.2f + U(3)), ImVec2(q.x + g * 0.5f, q.y + g * 0.7f + U(3)),
                      rgba(210, 24, 56, 0.05f + 0.06f * h), r + g);
  }
  ImU32 top = mix(IM_COL32(232, 46, 76, 255), IM_COL32(250, 74, 102, 255), h), bot = mix(IM_COL32(176, 16, 44, 255), IM_COL32(200, 24, 54, 255), h);
  if (held) { top = IM_COL32(200, 28, 58, 255); bot = IM_COL32(150, 12, 36, 255); }
  grad_rect(dl, p, q, p, ImVec2(p.x, q.y), top, bot, r);
  dl->AddRect(p, q, white(0.14f + 0.08f * h), r, 0, 1.0f);
  dl->AddLine(ImVec2(p.x + r, p.y + 1), ImVec2(q.x - r, p.y + 1), white(0.25f), 1.0f);
  ImVec2 ts = text_size(font, label);
  float is = fsz(font) * 0.62f, gap = U(10);
  float x = p.x + (size.x - (is + gap + ts.x)) * 0.5f, cy = p.y + size.y * 0.5f;
  icon(dl, kIcPlay, ImVec2(x + is * 0.5f, cy), is, kText);
  text_at(dl, font, ImVec2(x + is + gap, cy - ts.y * 0.5f), kText, label);
  return click;
}

// iOS-style switch, animated on real time
bool toggle_switch(const char* id, bool* v) {
  float h = U(22), w = U(40);
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + (ImGui::GetFrameHeight() - h) * 0.5f));
  p = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton(id, ImVec2(w, h));
  bool clicked = ImGui::IsItemClicked() || (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Space));
  if (clicked) *v = !*v;
  hand_cursor();
  bool hov = ImGui::IsItemHovered();
  float t = anim(ImGui::GetItemID(), *v, 18.0f), hv = anim(ImGui::GetItemID() + 1, hov);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImU32 off = white(0.10f + 0.05f * hv), on = mix(kRed, kRedHi, hv);
  dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), mix(off, on, t), h * 0.5f);
  dl->AddRect(p, ImVec2(p.x + w, p.y + h), white(0.08f * (1 - t)), h * 0.5f);
  float r = h * 0.5f - U(3);
  ImVec2 kc(p.x + h * 0.5f + (w - h) * t, p.y + h * 0.5f);
  dl->AddCircleFilled(ImVec2(kc.x, kc.y + U(1)), r, rgba(0, 0, 0, 0.25f));
  dl->AddCircleFilled(kc, r, IM_COL32(250, 250, 252, 255));
  return clicked;
}

// A settings row: label (+ help underneath, muted) on the left, the control on the right.
float g_ctrl_x = 0, g_ctrl_w = 0;
float g_row_y = 0;
void row_begin(const char* label, const char* help) {
  g_row_y = ImGui::GetCursorPosY();
  ImGui::BeginGroup();
  ImGui::PushFont(f_label);
  ImGui::TextUnformatted(label);
  ImGui::PopFont();
  if (help && *help) {
    ImGui::PushFont(f_small);
    ImGui::PushStyleColor(ImGuiCol_Text, col(kMuted));
    ImGui::PushTextWrapPos(g_ctrl_x - U(28));
    ImGui::TextWrapped("%s", help);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::PopFont();
  }
  ImGui::EndGroup();
  float after = ImGui::GetCursorPosY();
  ImGui::SetCursorPos(ImVec2(g_ctrl_x, g_row_y));
  g_row_y = after;
}
void row_end() {
  float y = std::max(ImGui::GetCursorPosY(), g_row_y);
  ImGui::SetCursorPosY(y + U(7));
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y - U(4)), ImVec2(p.x + ImGui::GetContentRegionAvail().x, p.y - U(4)), white(0.05f));
  ImGui::Dummy(ImVec2(1, U(6)));
}
// the control of a row that is right-aligned (switches)
void ctrl_right(float w) { ImGui::SetCursorPosX(g_ctrl_x + g_ctrl_w - w); }

bool setting_toggle(const char* label, const char* help, bool* v) {
  row_begin(label, help);
  ImGui::PushID(label);
  ctrl_right(U(40));
  bool c = toggle_switch("##t", v);
  ImGui::PopID();
  row_end();
  if (c) save();
  return c;
}
bool setting_slider(const char* label, const char* help, int* v, int lo, int hi, const char* fmt) {
  row_begin(label, help);
  ImGui::PushID(label);
  ImGui::SetNextItemWidth(g_ctrl_w);
  bool c = ImGui::SliderInt("##s", v, lo, hi, fmt, ImGuiSliderFlags_AlwaysClamp);
  if (ImGui::IsItemDeactivatedAfterEdit()) save();
  ImGui::PopID();
  row_end();
  return c;
}
bool setting_choice(const char* label, const char* help, const std::vector<std::string>& opts, int cur, int* out) {
  row_begin(label, help);
  ImGui::PushID(label);
  ImGui::SetNextItemWidth(g_ctrl_w);
  bool c = false;
  const char* shown = (cur >= 0 && cur < (int)opts.size()) ? opts[cur].c_str() : "?";
  if (ImGui::BeginCombo("##c", shown, ImGuiComboFlags_HeightLarge)) {
    for (int i = 0; i < (int)opts.size(); ++i)
      if (ImGui::Selectable(opts[i].c_str(), i == cur)) { *out = i; c = true; }
    ImGui::EndCombo();
  }
  ImGui::PopID();
  row_end();
  return c;
}
void setting_info(const char* label, const char* help, const std::string& value) {
  row_begin(label, help);
  ImGui::PushStyleColor(ImGuiCol_Text, col(kSoft));
  ImGui::PushTextWrapPos(g_ctrl_x + g_ctrl_w);
  ImGui::TextWrapped("%s", value.c_str());
  ImGui::PopTextWrapPos();
  ImGui::PopStyleColor();
  row_end();
}

// A card: a panel with a title; the content goes inside. The panel is drawn behind the child after it is measured.
void card_begin(const char* title, const char* subtitle = nullptr) {
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(U(24), U(20)));
  ImGui::BeginChild(title, ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
  ImGui::PushFont(f_h2);
  ImGui::TextUnformatted(title);
  ImGui::PopFont();
  if (subtitle) {
    ImGui::PushFont(f_small);
    ImGui::PushStyleColor(ImGuiCol_Text, col(kMuted));
    ImGui::TextWrapped("%s", subtitle);
    ImGui::PopStyleColor();
    ImGui::PopFont();
  }
  ImGui::Dummy(ImVec2(1, U(6)));
  g_ctrl_w = std::min(ImGui::GetContentRegionAvail().x * 0.42f, U(380));
  g_ctrl_x = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - g_ctrl_w;
}
void card_end() {
  ImGui::EndChild();
  ImGui::PopStyleVar();
  panel(ImGui::GetWindowDrawList(), ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), U(14));
  ImGui::Dummy(ImVec2(1, U(10)));
}
void banner(const char* text, ImU32 accent) {
  ImVec2 p = ImGui::GetCursorScreenPos();
  float w = ImGui::GetContentRegionAvail().x;
  ImGui::PushFont(f_small);
  ImVec2 ts = ImGui::CalcTextSize(text, nullptr, false, w - U(52));
  float h = ts.y + U(26);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  grad_rect(dl, p, ImVec2(p.x + w, p.y + h), p, ImVec2(p.x + w, p.y), mix(accent, IM_COL32(0, 0, 0, 0), 0.82f),
            rgba(255, 255, 255, 0.02f), U(12));
  dl->AddRect(p, ImVec2(p.x + w, p.y + h), mix(accent, IM_COL32(0, 0, 0, 0), 0.7f), U(12));
  dl->AddCircleFilled(ImVec2(p.x + U(20), p.y + h * 0.5f), U(4), accent);
  ImGui::SetCursorScreenPos(ImVec2(p.x + U(36), p.y + U(13)));
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w - U(52));
  ImGui::PushStyleColor(ImGuiCol_Text, col(kSoft));
  ImGui::TextWrapped("%s", text);
  ImGui::PopStyleColor();
  ImGui::PopTextWrapPos();
  ImGui::PopFont();
  ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h + U(14)));
  ImGui::Dummy(ImVec2(1, 1));
}

// ---------------------------------------------------------------------------------------------------- pages
enum Page { kHome, kDisplay, kLighting, kAudio, kControls, kGameplay, kTextures, kModels, kAbout, kPages };
const char* kPageNames[kPages] = {"Home", "Display", "Lighting", "Audio", "Controls", "Gameplay", "Textures", "Models", "About"};
const int kPageIcons[kPages] = {kIcHome, kIcDisplay, kIcSun, kIcSpeaker, kIcPad, kIcFlag, kIcImage, kIcCube, kIcInfo};
const char* kPageSubs[kPages] = {
    "Ready when you are.",
    "Resolution, aspect ratio, frame rate, HUD layout and HDR.",
    "The new lighting engine: real shadows, sky light, ambient occlusion, bounced light and lit light sources.",
    "Volume of each part of the sound.",
    "Camera and controls.",
    "Cutscenes, stage select and the classic cheat codes.",
    "Custom and AI-remastered textures: where they are and how they are used.",
    "Replacement 3D models (mods).",
    "Version, files and folders."};
int g_page = kHome;

std::string display_summary() {
  char res[64];
  if (g.height == 0) snprintf(res, sizeof res, "Auto");
  else snprintf(res, sizeof res, "%d x %d", g.width > 0 ? g.width : g.height * 4 / 3, g.height);
  return res;
}

void page_home() {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  // ---- hero: the game's title art on the right, fading into the panel; the title and Play on the left
  ImVec2 a = ImGui::GetCursorScreenPos();
  const float w = ImGui::GetContentRegionAvail().x, h = U(318), r = U(18);
  ImVec2 b(a.x + w, a.y + h);
  soft_shadow(dl, a, b, r, 1.6f);
  grad_rect(dl, a, b, a, ImVec2(b.x, a.y), IM_COL32(15, 11, 14, 255), IM_COL32(30, 12, 18, 255), r);
  if (g_hero.tex) {
    const float x0 = a.x + w * 0.38f, rw = b.x - x0;
    const float ra = rw / h, ia = g_hero.aspect;
    ImVec2 uv0(0, 0), uv1(1, 1);
    if (ra > ia) { float vh = ia / ra; uv0.y = 0.06f; uv1.y = std::min(1.0f, uv0.y + vh); uv0.y = uv1.y - vh; }
    else { float uw = ra / ia; uv0.x = (1 - uw) * 0.5f; uv1.x = uv0.x + uw; }
    dl->AddImageRounded(g_hero.tex, ImVec2(x0, a.y), b, uv0, uv1, IM_COL32(255, 255, 255, 235), r, ImDrawFlags_RoundCornersRight);
    // fade the art's left edge into the panel, and darken its bottom for the text
    grad_rect(dl, ImVec2(x0 - 1, a.y), ImVec2(x0 + rw * 0.55f, b.y), ImVec2(x0, a.y), ImVec2(x0 + rw * 0.55f, a.y),
              IM_COL32(20, 11, 15, 255), IM_COL32(20, 11, 15, 0));
    grad_rect(dl, ImVec2(a.x, a.y + h * 0.40f), b, ImVec2(a.x, a.y + h * 0.40f), ImVec2(a.x, b.y), rgba(8, 6, 8, 0),
              rgba(8, 6, 8, 0.80f), r, ImDrawFlags_RoundCornersBottom);
  }
  dl->PushClipRect(ImVec2(a.x, a.y), ImVec2(b.x, b.y - r * 0.5f), true);   // a low red glow behind the title
  radial(dl, ImVec2(a.x + w * 0.18f, b.y + h * 0.1f), w * 0.32f, rgba(210, 24, 56, 0.16f));
  dl->PopClipRect();
  dl->AddRect(a, b, white(0.08f), r, 0, 1.0f);
  dl->AddLine(ImVec2(a.x + r, a.y + 1), ImVec2(b.x - r, a.y + 1), white(0.08f), 1.0f);
  {   // emblem chip, top-left
    ImVec2 c0(a.x + U(26), a.y + U(24)), c1(c0.x + U(42), c0.y + U(42));
    dl->AddRectFilled(c0, c1, rgba(0, 0, 0, 0.35f), U(11));
    dl->AddRect(c0, c1, white(0.12f), U(11));
    float eh = U(30);
    draw_emblem(dl, ImVec2(c0.x + (U(42) - eh * g_emblem.aspect) * 0.5f, c0.y + U(6)), eh);
  }
  float y = a.y + h * 0.25f;
  {   // tag chip
    const char* tag = "PC RECOMPILATION";
    ImVec2 ts = text_size(f_small, tag);
    ImVec2 c0(a.x + U(28), y), c1(c0.x + ts.x + U(18), c0.y + ts.y + U(8));
    dl->AddRectFilled(c0, c1, rgba(210, 24, 56, 0.20f), U(6));
    dl->AddRect(c0, c1, rgba(240, 60, 90, 0.35f), U(6));
    text_at(dl, f_small, ImVec2(c0.x + U(9), c0.y + U(4)), kRedHi, tag);
    y = c1.y + U(10);
  }
  text_at(dl, f_title, ImVec2(a.x + U(25), y), kText, "SHADOW");
  y += fsz(f_title) - U(2);
  text_at(dl, f_h2, ImVec2(a.x + U(28), y), kSoft, "T H E   H E D G E H O G");
  y += fsz(f_h2) + U(14);
  {
    char line[160];
    snprintf(line, sizeof line, "%s  \xC2\xB7  %s fps  \xC2\xB7  %s  \xC2\xB7  %s lighting", g.widescreen ? "Widescreen" : "4:3",
             g.fps_target ? std::to_string(g.fps_target).c_str() : "Unlimited", g.custom_textures ? "Custom textures" : "Original textures",
             g.light ? "Modern" : "Original");
    text_at(dl, f_body, ImVec2(a.x + U(28), y), kSoft, line);
  }
  ImGui::SetCursorScreenPos(ImVec2(a.x + U(28), b.y - U(28) - U(50)));
  if (play_button("##heroplay", ImVec2(U(184), U(50)), f_h2, "PLAY")) g_play = true;
  text_at(dl, f_small, ImVec2(a.x + U(28) + U(184) + U(18), b.y - U(28) - U(25) - fsz(f_small) * 0.5f), kMuted,
          "or press START / Ctrl+Enter");
  ImGui::SetCursorScreenPos(ImVec2(a.x, b.y + U(30)));

  // ---- your setup: six tiles, each opens its page
  ImGui::PushFont(f_h2);
  ImGui::TextUnformatted("Your setup");
  ImGui::PopFont();
  ImGui::Dummy(ImVec2(1, U(2)));
  const float gap = U(14), tw = (w - gap * 2) / 3.0f, th = U(92);
  auto tile = [&](int idx, int ic, const char* title, const std::string& value, const std::string& sub, int page) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(idx);
    if (ImGui::InvisibleButton("##tile", ImVec2(tw, th))) g_page = page;
    hand_cursor();
    float hv = anim(ImGui::GetItemID(), ImGui::IsItemHovered());
    ImGui::PopID();
    ImVec2 q(p.x + tw, p.y + th);
    panel(dl, p, q, U(14), hv);
    ImVec2 ic0(p.x + U(16), p.y + U(16)), ic1(ic0.x + U(30), ic0.y + U(30));
    dl->AddRectFilled(ic0, ic1, mix(white(0.05f), rgba(210, 24, 56, 0.22f), hv), U(8));
    icon(dl, ic, ImVec2((ic0.x + ic1.x) * 0.5f, (ic0.y + ic1.y) * 0.5f), U(15), mix(kSoft, kRedHi, hv));
    text_at(dl, f_small, ImVec2(ic1.x + U(12), ic0.y + (U(30) - fsz(f_small)) * 0.5f), kMuted, title);
    text_at(dl, f_h2, ImVec2(p.x + U(16), ic1.y + U(10)), kText, value.c_str());
    ImVec2 vs = text_size(f_h2, value.c_str());
    text_at(dl, f_small, ImVec2(p.x + U(16) + vs.x + U(10), ic1.y + U(10) + (vs.y - fsz(f_small)) * 0.5f + U(1)), kMuted, sub.c_str());
  };
  tile(0, kIcDisplay, "Display", display_summary(),
       g.widescreen ? (g.fullscreen ? "widescreen, fullscreen" : "widescreen, window") : (g.fullscreen ? "4:3, fullscreen" : "4:3, window"), kDisplay);
  ImGui::SameLine(0, gap);
  tile(1, kIcGauge, "Frame rate", fps_name(g.fps_target), g.show_fps ? "counter on" : "", kDisplay);
  ImGui::SameLine(0, gap);
  tile(2, kIcSun, "Lighting", g.light ? "Modern" : "Original", g.light ? "new engine" : "the game's own", kLighting);
  ImGui::Dummy(ImVec2(1, gap - ImGui::GetStyle().ItemSpacing.y));
  count_textures();
  int n = g_tex_count;
  tile(3, kIcImage, "Custom textures", g.custom_textures ? "On" : "Off", n < 0 ? std::string("counting...") : std::to_string(n) + " files", kTextures);
  ImGui::SameLine(0, gap);
  size_t mods = 0;
  for (auto& m : list_mods()) mods += m.enabled ? 1 : 0;
  tile(4, kIcCube, "Models", std::to_string(mods), mods == 1 ? "replacement in use" : "replacements in use", kModels);
  ImGui::SameLine(0, gap);
  tile(5, kIcSpeaker, "Audio", std::to_string(g.vol_master) + "%", "master volume", kAudio);
}

void page_display() {
  card_begin("Screen");
  {
    std::vector<std::string> o = {"Auto (match the window)"};
    int cur = 0;
    auto rl = resolutions();
    for (size_t i = 0; i < rl.size(); ++i) {
      char b[48];
      snprintf(b, sizeof b, "%d x %d%s", rl[i].w, rl[i].h, std::fabs((double)rl[i].w / rl[i].h - 4.0 / 3.0) < 0.02 ? "  (4:3)" : "");
      o.push_back(b);
      if (g.height != 0 && rl[i].w == g.width && rl[i].h == g.height) cur = (int)i + 1;
    }
    int sel;
    if (setting_choice("Resolution", "Render resolution. Any size works; Auto follows the window.", o, cur, &sel)) {
      if (sel == 0) g.height = g.width = 0;
      else { g.width = rl[sel - 1].w; g.height = rl[sel - 1].h; g.widescreen = std::fabs((double)g.width / g.height - 4.0 / 3.0) >= 0.02; }
      apply_display();
    }
  }
  {
    int sel;
    if (setting_choice("Aspect ratio", "Widescreen widens the 3D view (no stretching); the HUD keeps its shape.",
                       {"4:3 (original)", "Widescreen"}, g.widescreen ? 1 : 0, &sel)) { g.widescreen = sel == 1; apply_display(); }
  }
  setting_toggle("Fullscreen", "Applied when you press Play. In the game: F11 or Alt+Enter at any time.", &g.fullscreen);
  card_end();

  card_begin("Frame rate", "Real frames, never interpolation. Game speed does not depend on the frame rate.");
  {
    auto v = fps_values();
    std::vector<std::string> o;
    for (int x : v) o.push_back(fps_name(x));
    int cur = (int)(std::find(v.begin(), v.end(), g.fps_target) - v.begin());
    if (cur == (int)v.size()) { o.push_back(std::to_string(g.fps_target) + " (custom)"); }
    int sel;
    if (setting_choice("Frame rate", "The rate the game runs and draws at.", o, cur, &sel) && sel < (int)v.size()) { g.fps_target = v[sel]; save(); }
    int cv = g.fps_target > 0 ? g.fps_target : 500;
    if (setting_slider("Custom frame rate", "Any target from 30 to 500.", &cv, 30, 500, "%d fps")) g.fps_target = cv;
    setting_toggle("Show FPS counter", nullptr, &g.show_fps);
  }
  card_end();

  card_begin("HUD and menus");
  {
    int sel;
    if (g.widescreen && setting_choice("HUD layout", "Widescreen, in a stage: HUD elements move to the edges of the 16:9 screen.",
                                       {"4:3 (centred)", "16:9 (screen edges)"}, g.hud_169 ? 1 : 0, &sel)) { g.hud_169 = sel == 1; save(); }
    if (setting_choice("HUD gauges", "New: the Hero/Dark gauges drawn by the PC version from one picture.", {"Original", "New"},
                       g.gauges_new ? 1 : 0, &sel)) { g.gauges_new = sel == 1; save(); }
    setting_toggle("Hide UI outside 4:3", "Widescreen: menu pieces the game parks just off its 4:3 screen stay hidden.", &g.clip_ui_43);
    setting_toggle("Cutscenes in 4:3 with bars", "Widescreen: in-engine cutscenes keep the original framing.", &g.cutscene_bars);
  }
  card_end();

  card_begin("HDR", "For HDR displays (turn HDR on in Windows first).");
  {
    if (setting_toggle("HDR output", aurora_hdr_available() ? "16-bit float rendering, scRGB output." : "Not available on this display.", &g.hdr)) apply_hdr();
    if (g.hdr) {
      if (setting_slider("Paper white", "Brightness of the game's normal white.", &g.hdr_paper_white, 80, 600, "%d nits")) apply_hdr();
      if (setting_slider("Peak brightness", "Your display's maximum brightness.", &g.hdr_peak, 400, 4000, "%d nits")) apply_hdr();
      if (setting_slider("Highlight boost", "Stretches the brightest normal parts toward the peak.", &g.hdr_highlight, 0, 100, "%d%%")) apply_hdr();
      if (setting_slider("Saturation", nullptr, &g.hdr_saturation, 50, 150, "%d%%")) apply_hdr();
      if (setting_slider("Vibrance", nullptr, &g.hdr_vibrance, 0, 100, "%d%%")) apply_hdr();
      if (setting_slider("Contrast", nullptr, &g.hdr_contrast, 70, 140, "%d%%")) apply_hdr();
      if (setting_slider("Black level", nullptr, &g.hdr_black, -20, 20, "%d")) apply_hdr();
    }
  }
  card_end();
}

void page_lighting() {
  // #147 (user): the lighting layer is the real-time sun shadows on the game's own lighting, plus a colour pass. The other
  // parts are archived (docs/lighting/ARCHIVED.md): their cards below only show with LIGHT_ARCHIVE=0 (settings::archive_lighting).
  static const bool archived = [] { const char* e = getenv("LIGHT_ARCHIVE"); return !(e && *e == '0'); }();
  if (archived) {
    card_begin("Real-time shadows", "Shadows from the sun, cast by the real 3D scene, on top of the game's own lighting.");
    setting_toggle("Real-time shadows", "Off = the original look.", &g.light);
    {
      int sel;
      if (setting_choice("Quality", "Shadow filtering and update rates.", {"Low", "Medium", "High", "Ultra"}, g.light_quality, &sel)) {
        g.light_quality = sel;
        save();
      }
      if (setting_choice("Shadow resolution", nullptr, {"1024", "2048", "4096", "8192"}, g.light_shadow_res, &sel)) { g.light_shadow_res = sel; save(); }
    }
    setting_slider("Shadow distance", "How far from the camera shadows reach.", &g.light_shadow_dist, 25, 300, "%d%%");
    setting_toggle("Characters cast shadows", "A soft real-time shadow from characters and enemies onto the world.", &g.light_char_cast);
    {
      bool ao = false;
      ImGui::BeginDisabled();
      setting_toggle("Ambient occlusion (work in progress)", "A new ambient occlusion is being made. Not available yet.", &ao);
      ImGui::EndDisabled();
    }
    card_end();
    card_begin("Colour", "For colour fine-tuning, SDR and HDR. All at default = the original picture.");
    bool c = false;
    c |= setting_slider("Black level", "Below 0 crushes the darkest tones, above 0 lifts them.", &g.col_black, -50, 50, "%d");
    c |= setting_slider("White level", nullptr, &g.col_white, 50, 150, "%d%%");
    c |= setting_slider("Exposure", "In tenths of a stop.", &g.col_exposure, -20, 20, "%d");
    c |= setting_slider("Gamma", "Above 100% brightens the mid tones.", &g.col_gamma, 50, 200, "%d%%");
    c |= setting_slider("Contrast", nullptr, &g.col_contrast, 50, 150, "%d%%");
    c |= setting_slider("Saturation", nullptr, &g.col_saturation, 0, 200, "%d%%");
    c |= setting_slider("Vibrance", "Dull colours change more than already strong ones.", &g.col_vibrance, -100, 100, "%d%%");
    c |= setting_slider("Red", nullptr, &g.col_r, 50, 150, "%d%%");
    c |= setting_slider("Green", nullptr, &g.col_g, 50, 150, "%d%%");
    c |= setting_slider("Blue", nullptr, &g.col_b, 50, 150, "%d%%");
    if (c) apply_hdr();
    if (ImGui::Button("Reset colour")) {
      g.col_black = g.col_exposure = g.col_vibrance = 0;
      g.col_white = g.col_gamma = g.col_contrast = g.col_saturation = g.col_r = g.col_g = g.col_b = 100;
      apply_hdr();
      save();
    }
    card_end();
    card_begin("Game bloom");
    setting_toggle("Game bloom / glow", "The game's own glow pass (its halos around lamps and lights).", &g.game_bloom);
    {
      int sel = g.game_bloom_quality;
      if (setting_choice("Bloom quality", "Smooth: the glow without square blocks (same brightness). Original: the game's upscale.",
                         {"Original", "Smooth"}, g.game_bloom_quality, &sel)) { g.game_bloom_quality = sel; save(); }
    }
    setting_slider("Bloom strength", "100% = the original glow. 0% = none.", &g.game_bloom_strength, 0, 200, "%d%%");
    gxlle::g_game_bloom = g.game_bloom;
    card_end();
    return;
  }
  banner("In development. The new lighting engine is being built part by part; these settings are saved now and each part "
         "uses them as soon as it lands. Everything is computed in world space and runs on time, never on the frame rate.",
         kGold);
  card_begin("Lighting engine");
  setting_toggle("Modern lighting", "Replaces the game's flat lighting with the new engine. Off = the original look.", &g.light);
  {
    int sel;
    if (setting_choice("Quality", "Shadow filtering, AO/GI resolution and update rates.", {"Low", "Medium", "High", "Ultra"},
                       g.light_quality, &sel)) { g.light_quality = sel; save(); }
  }
  setting_toggle("Replace the game's lighting and bloom", "Off (default): the game's own lighting and bloom with the real-time layer on top. On: the game's own lighting and bloom are removed and this engine lights everything.", &g.light_replace);
  setting_toggle("Game bloom / glow", "The game's own glow pass (its halos around lamps and lights).", &g.game_bloom);
  {
    int sel = g.game_bloom_quality;
    if (setting_choice("Bloom quality", "Smooth: the glow without square blocks (same brightness). Original: the game's upscale.",
                       {"Original", "Smooth"}, g.game_bloom_quality, &sel)) { g.game_bloom_quality = sel; save(); }
  }
  setting_slider("Bloom strength", "100% = the original glow. 0% = none.", &g.game_bloom_strength, 0, 200, "%d%%");
  setting_slider("Keep the game's light", "Only with Replace on: how much of the game's own baked light (street lamps, neon, lava, interiors) stays. 0% = full replace (night stages and interiors go dark).", &g.light_baked, 0, 100, "%d%%");
  setting_toggle("Replace also removes the game's bloom", "Only with Replace on. Off: the game's glow halos stay.", &g.light_replace_bloom);
  gxlle::g_game_bloom = g.game_bloom && !(g.light && g.light_replace && g.light_replace_bloom);
  card_end();
  card_begin("Look", "The real-time layer on top of the game's own lighting (replace off).");
  {
    int sel;
    if (setting_choice("Look", "Stylized: soft, tinted shadows. Original match: neutral. Dramatic: stronger sun and shadows.",
                       {"Stylized", "Original match", "Dramatic"}, g.light_look, &sel)) { g.light_look = sel; save(); }
  }
  setting_slider("Sun shading", "How much the sun's direction brightens / darkens surfaces.", &g.light_exposure, 0, 300, "%d%%");
  setting_slider("Shadow strength", nullptr, &g.light_shadow_strength, 0, 300, "%d%%");
  setting_slider("Shadow colour", "0% = neutral grey shadows, 100% = the stage's tint.", &g.light_shadow_tint, 0, 200, "%d%%");
  setting_slider("Ambient fill", "Above 100% lifts shadows and corners, below darkens them.", &g.light_ambient, 0, 200, "%d%%");
  setting_slider("Saturation", nullptr, &g.light_saturation, 0, 200, "%d%%");
  setting_toggle("World shading only", "Characters and enemies keep the game's own shading.", &g.light_world_only);
  setting_toggle("Characters cast shadows", "A soft real-time shadow from characters and enemies onto the world.", &g.light_char_cast);
  setting_toggle("Vanilla blob (circle) shadows", "The game's circles under characters (always on with lighting off).", &g.light_blob);
  card_end();
  card_begin("Sun and sky");
  setting_slider("Sun", "Strength of the direct sunlight.", &g.light_sun, 0, 300, "%d%%");

  setting_toggle("Real-time shadows", "Shadows from the sun and sky on everything, cast by the real 3D scene.", &g.light_shadows);
  {
    int sel;
    if (setting_choice("Shadow resolution", nullptr, {"1024", "2048", "4096", "8192"}, g.light_shadow_res, &sel)) { g.light_shadow_res = sel; save(); }
  }
  setting_slider("Shadow distance", "How far from the camera shadows reach.", &g.light_shadow_dist, 25, 300, "%d%%");
  setting_slider("Off-screen shadow coverage", "Keep nearby objects outside the screen as shadow casters. Higher costs more draw calls.", &g.light_caster_margin, 0, 2000, "%d");
  setting_slider("Camera draw range", "100% = original camera range; object-specific LOD is separate.", &g.draw_distance, 100, 400, "%d%%");
  setting_slider("Object draw distance", "100% = original. Objects appear and stay active further away (decor / rings / breakables up to 3x; triggers, checkpoints, goal and spawners keep their range).", &g.draw_objects, 100, 300, "%d%%");
  setting_toggle("Stage atmosphere profiles", "Use each stage and area's authored lighting and haze.", &g.light_profiles);
  setting_slider("Sky light", "Soft light from the whole sky, blocked by what is overhead.", &g.light_sky, 0, 200, "%d%%");
  card_end();
  card_begin("Ambient occlusion", "Contact shading where surfaces meet: under objects, along curbs and walls, in corners.");
  setting_choice("Type", "Screen-space (default): fine contact shading from what is on screen. World-space: the older soft, "
                 "baked darkening (also from what is off screen).", {"World-space", "Screen-space"}, g.light_ao_mode, &g.light_ao_mode);
  setting_choice("Quality", "Screen-space only. Higher = sharper and steadier, costs more GPU.", {"Low", "Medium", "High"}, g.light_ao_quality, &g.light_ao_quality);
  setting_slider("Strength", nullptr, &g.light_ao, 0, 200, "%d%%");
  setting_slider("Reach", nullptr, &g.light_ao_radius, 25, 300, "%d%%");
  card_end();
  card_begin("Global illumination", "Light bouncing between surfaces: colour from lit walls, fire and lamps spills onto what is near.");
  setting_slider("Strength", nullptr, &g.light_gi, 0, 200, "%d%%");
  setting_slider("Bounces", nullptr, &g.light_gi_bounces, 1, 3, "%d");
  card_end();
  card_begin("Light sources", "The game fakes lamps, screens and fires with a glow. Here they become real lights.");
  setting_toggle("Glowing objects cast light", "Found from the game's own bloom.", &g.light_emissive);
  setting_slider("Brightness", nullptr, &g.light_emissive_str, 0, 300, "%d%%");
  setting_slider("Range", nullptr, &g.light_emissive_range, 25, 300, "%d%%");
  setting_slider("Flicker", "0 = lava and fire give one steady glow on the walls; higher = a subtle living shimmer. Flashes stay quick.", &g.light_glow_flicker, 0, 100, "%d%%");
  card_end();
  card_begin("Surface detail");
  setting_toggle("Parallax", "Real depth on stone, brick and ground textures that have a height map.", &g.parallax);
  setting_slider("Depth", nullptr, &g.parallax_depth, 0, 200, "%d%%");
  card_end();
}

void page_textures() {
  card_begin("Custom textures", "Replacement pictures for the game's textures (the AI Retexture tool writes them here).");
  setting_toggle("Load custom textures", "Changes in the folder show up in the game within a couple of seconds.", &g.custom_textures);
  {
    row_begin("Folder", nullptr);
    std::string shown = u8(textures_folder());
    ImGui::PushStyleColor(ImGuiCol_Text, col(kSoft));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + g_ctrl_w);
    ImGui::TextWrapped("%s", shown.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::SetCursorPosX(g_ctrl_x);
    if (ImGui::Button("Browse...")) {
      std::string def = u8(textures_folder());
      SDL_ShowOpenFolderDialog(on_folder_picked, nullptr, gxlle::g_window, def.c_str(), false);
    }
    ImGui::SameLine();
    if (ImGui::Button("Open")) open_in_explorer(textures_folder());
    ImGui::SameLine();
    ImGui::BeginDisabled(g.custom_textures_dir.empty());
    if (ImGui::Button("Default")) { g.custom_textures_dir.clear(); save(); }
    ImGui::EndDisabled();
    row_end();
    {
      std::lock_guard lk(g_pick_mutex);
      if (g_pick_ready) { g.custom_textures_dir = g_picked; g_pick_ready = false; save(); }
    }
  }
  count_textures();
  {
    int n = g_tex_count;
    setting_info("Textures in the folder", nullptr, n < 0 ? std::string("counting...") : std::to_string(n));
  }
  setting_slider("Match colours to the originals", "Pulls each custom texture's palette back toward the original's, "
                 "keeping the new detail (in memory only). 0% = as they are.", &g.tex_match, 0, 100, "%d%%");
  card_end();
}

void page_models() {
  card_begin("Replacement models", "A file in the mods folder replaces the game's file with the same path (for example "
             "character\\shadow.one). Made with tools\\model_swap.py.");
  auto mods = list_mods();   // a copy: toggling renames files and refreshes the list
  if (mods.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, col(kMuted));
    ImGui::TextUnformatted("No replacement models yet.");
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(1, U(6)));
  }
  for (auto& m : mods) {
    bool on = m.enabled;
    char help[64];
    snprintf(help, sizeof help, "%.1f MB", m.size / 1048576.0);
    if (setting_toggle(m.rel.c_str(), help, &on)) {
      std::error_code ec;
      fs::path target = on ? m.path.parent_path() / m.path.stem() : fs::path(m.path.native() + L".disabled");
      fs::rename(m.path, target, ec);
      list_mods(true);
    }
  }
  if (ImGui::Button("Open mods folder")) open_in_explorer(mods_folder());
  card_end();
  card_begin("Import a model", "Coming: import an OBJ / glTF, fit it to a character's skeleton and preview it here before playing.");
  ImGui::BeginDisabled();
  ImGui::Button("Import model...");
  ImGui::EndDisabled();
  card_end();
}

void page_audio() {
  card_begin("Volume", "100% = the original level.");
  setting_slider("Master", nullptr, &g.vol_master, 0, 200, "%d%%");
  setting_slider("Sound effects", nullptr, &g.vol_sfx, 0, 200, "%d%%");
  setting_slider("Music", nullptr, &g.vol_music, 0, 200, "%d%%");
  setting_slider("Dialogue", nullptr, &g.vol_dialogue, 0, 200, "%d%%");
  card_end();
}

void page_controls() {
  card_begin("Camera");
  setting_slider("Field of view", "100% = original. Culling follows, so nothing pops in at the edges.", &g.fov_percent, 70, 150, "%d%%");
  setting_slider("Camera sensitivity", "Right stick, arrow keys and mouse look.", &g.cam_sens, 25, 300, "%d%%");
  setting_toggle("Invert camera X", nullptr, &g.invert_cam_x);
  setting_toggle("Invert camera Y", nullptr, &g.invert_cam_y);
  card_end();
  card_begin("Keyboard and controller");
  setting_info("Move / camera", nullptr, "Left stick or WASD  /  right stick or arrows (hold right mouse: mouse look)");
  setting_info("Jump / action", nullptr, "A or Space/Enter  -  B or Shift/X  -  X: C  -  Y: V");
  setting_info("L / R / Z", nullptr, "Triggers  /  LB or Q, E, R or Ctrl");
  setting_info("Pause", nullptr, "START or Esc/Tab");
  setting_info("Fullscreen", nullptr, "F11 or Alt+Enter");
  setting_info("Texture viewer", nullptr, "F3");
  card_end();
}

void page_gameplay() {
  card_begin("Gameplay");
  setting_toggle("Skip any cutscene", "START skips in-engine cutscenes even the first time.", &g.skip_any_cutscene);
  setting_toggle("Enemy counts on the checkpoint warp screen", "A red number on each checkpoint: the mission enemies still alive nearest it.", &g.warp_enemy_counts);
  {
    int sel = g.skin;
    if (setting_choice("Shadow looks like", "The game's own 2P / Metal / Super / Evil / Sonic models on Shadow's moves (next start; needs the skins folder).",
                       {"Shadow (original)", "2P Yellow", "2P Green", "2P Orange", "Metal blue", "Metal purple", "Super Shadow",
                        "Evil Shadow", "Sonic"}, g.skin, &sel))
      g.skin = sel;
  }
  setting_toggle("L recentres the camera", "Press L to swing the camera behind Shadow. Fixed camera spots are left alone.", &g.cam_recentre);
  {
    bool v = g.unlock_all_stages;
    row_begin("Unlock every stage (Select Mode)", "This session only: the unlock is never written to the memory card (saving still works).");
    ImGui::PushID("unlock");
    ctrl_right(U(40));
    if (toggle_switch("##t", &v)) g.unlock_all_stages = v;
    ImGui::PopID();
    row_end();
  }
  card_end();
  card_begin("Cheats", "The classic Action Replay codes, for this session only.");
  for (int i = 0; i < cheats::COUNT; ++i) {
    row_begin(cheats::info(i).name, cheats::info(i).help);
    ImGui::PushID(i);
    ctrl_right(U(40));
    toggle_switch("##c", &cheats::enabled(i));
    ImGui::PopID();
    row_end();
  }
  card_end();
  card_begin("Movement tweak", "Shadow's physics. Original is the game untouched. In the game: F1 next preset, F2 this menu.");
  movement::draw_tweak();   // the same Tweak menu as F2 in the game (presets file, grouped sliders, readouts)
  card_end();
}

void page_about() {
  card_begin("Shadow the Hedgehog - PC recompilation");
  setting_info("Build", nullptr, __DATE__ "  " __TIME__);
  setting_info("Game disc", nullptr, g_disc_root);
  row_begin("Folders", nullptr);
  if (ImGui::Button("Game folder")) open_in_explorer(exe_dir());
  ImGui::SameLine();
  if (ImGui::Button("Logs")) open_in_explorer(exe_dir() / L"logs");
  ImGui::SameLine();
  if (ImGui::Button("Saves")) open_in_explorer(exe_dir() / L"saves");
  row_end();
  card_end();
}

// ---------------------------------------------------------------------------------------------------- sidebar / quick panel
bool nav_item(int id, const char* label, int ic, bool sel, float side) {
  ImGui::SetCursorPosX(U(12));
  ImVec2 a = ImGui::GetCursorScreenPos();
  ImVec2 sz(side - U(24), U(38));
  ImGui::PushID(id);
  bool click = ImGui::InvisibleButton("##nav", sz);
  hand_cursor();
  bool hov = ImGui::IsItemHovered();
  float h = anim(ImGui::GetItemID(), hov), s = anim(ImGui::GetItemID() + 7, sel);
  ImGui::PopID();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 b(a.x + sz.x, a.y + sz.y);
  float fill = std::max(0.075f * s, 0.04f * h);
  if (fill > 0.002f) {
    grad_rect(dl, a, b, a, ImVec2(a.x, b.y), white(fill * 1.25f), white(fill * 0.8f), U(9));
    dl->AddRect(a, b, white(0.09f * s), U(9));
  }
  ImU32 tc = mix(mix(kMuted, kSoft, h), kText, s);
  icon(dl, ic, ImVec2(a.x + U(22), a.y + sz.y * 0.5f), U(16), mix(tc, kRedHi, s));
  text_at(dl, f_label, ImVec2(a.x + U(44), a.y + (sz.y - fsz(f_label)) * 0.5f), tc, label);
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y + U(4));
  return click;
}
void side_header(const char* text) {
  ImGui::SetCursorPosX(U(26));
  ImGui::PushFont(f_small);
  ImGui::PushStyleColor(ImGuiCol_Text, col(kFaint));
  ImGui::TextUnformatted(text);
  ImGui::PopStyleColor();
  ImGui::PopFont();
  ImGui::Dummy(ImVec2(1, U(2)));
}
void side_rule(float side) {
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::GetWindowDrawList()->AddLine(ImVec2(ImGui::GetWindowPos().x, p.y + U(6)), ImVec2(ImGui::GetWindowPos().x + side, p.y + U(6)), white(0.06f));
  ImGui::Dummy(ImVec2(1, U(16)));
}

// one quick toggle row (Home's right column)
bool quick_row(int ic, const char* name, const char* sub, bool* v) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p = ImGui::GetCursorScreenPos();
  const float w = ImGui::GetContentRegionAvail().x, h = U(52);
  float cr = U(17);
  ImVec2 c(p.x + cr, p.y + h * 0.5f);
  dl->AddCircleFilled(c, cr, *v ? rgba(210, 24, 56, 0.20f) : white(0.05f));
  dl->AddCircle(c, cr, *v ? rgba(240, 60, 90, 0.40f) : white(0.09f), 0, 1.0f);
  icon(dl, ic, c, U(15), *v ? kRedHi : kSoft);
  float tx = p.x + cr * 2 + U(12);
  text_at(dl, f_label, ImVec2(tx, p.y + h * 0.5f - fsz(f_label) + U(1)), kText, name);
  text_at(dl, f_small, ImVec2(tx, p.y + h * 0.5f + U(2)), kMuted, sub);
  ImGui::SetCursorScreenPos(ImVec2(p.x + w - U(40), p.y + (h - ImGui::GetFrameHeight()) * 0.5f));
  ImGui::PushID(name);
  bool ch = toggle_switch("##q", v);
  ImGui::PopID();
  ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h + U(4)));
  ImGui::Dummy(ImVec2(1, 1));
  if (ch) save();
  return ch;
}
void quick_panel() {
  ImGui::PushFont(f_h2);
  ImGui::TextUnformatted("Quick settings");
  ImGui::PopFont();
  ImGui::PushFont(f_small);
  ImGui::PushStyleColor(ImGuiCol_Text, col(kMuted));
  ImGui::TextUnformatted("Everything else is in the pages on the left.");
  ImGui::PopStyleColor();
  ImGui::PopFont();
  ImGui::Dummy(ImVec2(1, U(10)));
  auto group = [](const char* t) {
    ImGui::PushFont(f_small);
    ImGui::PushStyleColor(ImGuiCol_Text, col(kFaint));
    ImGui::TextUnformatted(t);
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(1, U(2)));
  };
  group("Look");
  quick_row(kIcSun, "Modern lighting", g.light ? "New engine" : "Original lighting", &g.light);
  quick_row(kIcImage, "Custom textures", g.custom_textures ? "Loaded" : "Original textures", &g.custom_textures);
  quick_row(kIcSparkle, "Bloom / glow", g.game_bloom ? "The game's glow" : "Off", &g.game_bloom);
  ImGui::Dummy(ImVec2(1, U(8)));
  group("Screen");
  quick_row(kIcExpand, "Fullscreen", g.fullscreen ? "When you play" : "Window", &g.fullscreen);
  {
    bool ws = g.widescreen;
    if (quick_row(kIcDisplay, "Widescreen", ws ? "Wider 3D view" : "4:3 original", &ws)) { g.widescreen = ws; apply_display(); }
  }
  quick_row(kIcGauge, "FPS counter", g.show_fps ? "Shown in game" : "Hidden", &g.show_fps);
  if (aurora_hdr_available())
    if (quick_row(kIcHdr, "HDR", g.hdr ? "HDR output" : "Standard range", &g.hdr)) apply_hdr();
}

// ---------------------------------------------------------------------------------------------------- frame
void draw_launcher() {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  {   // everything scales with the window (fonts were built for the launcher's starting size)
    float want = std::clamp(vp->Size.y / kDesignH, 0.5f, 4.0f);
    if (std::fabs(want - g_ui) > 0.005f) { g_ui = want; apply_style(); }
    ImGui::GetIO().FontGlobalScale = g_ui / g_font_ui;
  }
  const float W = vp->Size.x, H = vp->Size.y;
  ImGui::SetNextWindowPos(vp->Pos);
  ImGui::SetNextWindowSize(vp->Size);
  ImGui::Begin("##launcher", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
                                          ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar |
                                          ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PushFont(f_body);
  ImDrawList* bg = ImGui::GetWindowDrawList();
  const ImVec2 o = vp->Pos;
  // background: near black with soft Shadow-red light
  radial(bg, ImVec2(o.x + W * 0.10f, o.y - H * 0.18f), W * 0.62f, rgba(150, 12, 36, 0.30f));
  radial(bg, ImVec2(o.x + W * 1.02f, o.y + H * 1.12f), W * 0.55f, rgba(110, 10, 44, 0.26f));
  radial(bg, ImVec2(o.x + W * 0.62f, o.y + H * 0.30f), W * 0.38f, rgba(70, 14, 24, 0.12f));

  const float side = U(250);
  const float right = g_page == kHome ? U(300) : 0.0f;
  bg->AddRectFilled(o, ImVec2(o.x + side, o.y + H), rgba(0, 0, 0, 0.42f));
  bg->AddLine(ImVec2(o.x + side, o.y), ImVec2(o.x + side, o.y + H), white(0.06f));
  if (right > 0) {
    bg->AddRectFilled(ImVec2(o.x + W - right, o.y), ImVec2(o.x + W, o.y + H), rgba(0, 0, 0, 0.32f));
    bg->AddLine(ImVec2(o.x + W - right, o.y), ImVec2(o.x + W - right, o.y + H), white(0.06f));
  }

  // ---- sidebar: the emblem and the name
  {
    ImVec2 c0(o.x + U(20), o.y + U(22)), c1(c0.x + U(40), c0.y + U(40));
    grad_rect(bg, c0, c1, c0, ImVec2(c0.x, c1.y), white(0.09f), white(0.04f), U(11));
    bg->AddRect(c0, c1, white(0.10f), U(11));
    float eh = U(28);
    draw_emblem(bg, ImVec2(c0.x + (U(40) - eh * g_emblem.aspect) * 0.5f, c0.y + U(6)), eh);
    text_at(bg, f_label, ImVec2(c1.x + U(12), c0.y + U(2)), kText, "Shadow the Hedgehog");
    text_at(bg, f_small, ImVec2(c1.x + U(12), c0.y + U(2) + fsz(f_label) + U(1)), kMuted, "PC recompilation");
  }
  ImGui::SetCursorPos(ImVec2(0, U(82)));
  side_rule(side);
  if (nav_item(kHome, kPageNames[kHome], kPageIcons[kHome], g_page == kHome, side)) g_page = kHome;
  ImGui::Dummy(ImVec2(1, U(2)));
  side_rule(side);
  side_header("Settings");
  for (int i = kDisplay; i <= kGameplay; ++i)
    if (nav_item(i, kPageNames[i], kPageIcons[i], g_page == i, side)) g_page = i;
  ImGui::Dummy(ImVec2(1, U(2)));
  side_rule(side);
  side_header("Mods");
  for (int i = kTextures; i <= kModels; ++i)
    if (nav_item(i, kPageNames[i], kPageIcons[i], g_page == i, side)) g_page = i;
  // bottom: about, quit
  ImGui::SetCursorPos(ImVec2(0, H - U(118)));
  side_rule(side);
  if (nav_item(kAbout, kPageNames[kAbout], kPageIcons[kAbout], g_page == kAbout, side)) g_page = kAbout;
  if (nav_item(100, "Quit", kIcPower, false, side)) { save(); sys_exit_guarded(0); }

  // ---- content
  const float pad = U(34);
  ImGui::SetCursorPos(ImVec2(side + pad, 0));
  ImGui::BeginChild("##content", ImVec2(W - side - right - pad * 2 + U(10), H), ImGuiChildFlags_None);
  ImGui::Dummy(ImVec2(1, U(26)));
  if (g_page != kHome) {   // page title, its line, and Play on the right
    ImVec2 p = ImGui::GetCursorScreenPos();
    float cw = ImGui::GetContentRegionAvail().x - U(10);
    ImGui::PushFont(f_h1);
    ImGui::TextUnformatted(kPageNames[g_page]);
    ImGui::PopFont();
    ImGui::PushFont(f_small);
    ImGui::PushStyleColor(ImGuiCol_Text, col(kMuted));
    ImGui::TextUnformatted(kPageSubs[g_page]);
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImVec2 after = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(ImVec2(p.x + cw - U(132), p.y + U(4)));
    if (play_button("##topplay", ImVec2(U(132), U(42)), f_label, "PLAY")) g_play = true;
    ImGui::SetCursorScreenPos(after);
    ImGui::Dummy(ImVec2(1, U(14)));
  }
  ImGui::BeginGroup();
  ImGui::PushItemWidth(-U(10));
  switch (g_page) {
    case kHome: page_home(); break;
    case kDisplay: page_display(); break;
    case kLighting: page_lighting(); break;
    case kTextures: page_textures(); break;
    case kModels: page_models(); break;
    case kAudio: page_audio(); break;
    case kControls: page_controls(); break;
    case kGameplay: page_gameplay(); break;
    case kAbout: page_about(); break;
  }
  ImGui::PopItemWidth();
  ImGui::EndGroup();
  ImGui::Dummy(ImVec2(1, U(30)));
  ImGui::EndChild();

  if (right > 0) {
    ImGui::SetCursorPos(ImVec2(W - right, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(U(22), U(28)));
    ImGui::BeginChild("##quick", ImVec2(right, H), ImGuiChildFlags_AlwaysUseWindowPadding);
    quick_panel();
    ImGui::EndChild();
    ImGui::PopStyleVar();
  }
  ImGui::PopFont();
  ImGui::End();
  if (ImGui::IsKeyPressed(ImGuiKey_GamepadStart) || (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Enter))) g_play = true;
}
}  // namespace

// The launcher window, in SDL points: 1360 x 820, smaller on small desktops (same shape), centred by the caller.
void launcher_window_size(int* w, int* h) {
  int ww = 1360, hh = 820;
  if (gxlle::g_window) {   // follow Windows' display scale (SDL sizes here are pixels unless the window is high-density)
    float ds = SDL_GetWindowDisplayScale(gxlle::g_window), pd = SDL_GetWindowPixelDensity(gxlle::g_window);
    float k = (ds > 0 && pd > 0) ? ds / pd : 1.0f;
    if (k > 1.01f) { ww = (int)(ww * k); hh = (int)(hh * k); }
  }
  SDL_Rect r;
  if (gxlle::g_window && SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(gxlle::g_window), &r)) {
    float k = std::min(1.0f, std::min(r.w * 0.92f / ww, r.h * 0.90f / hh));
    ww = (int)(ww * k);
    hh = (int)(hh * k);
  }
  *w = ww;
  *h = hh;
}

// Fonts for the launcher (and the defaults for the in-game overlays). Launcher: built for its window's pixel height
// (crisp at the starting size; a resized window scales them with FontGlobalScale).
void init_fonts() {
  ImGuiIO& io = ImGui::GetIO();
  float ui = 1.0f;
  if (gxlle::g_launcher_mode && gxlle::g_window) {
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(gxlle::g_window, &pw, &ph);
    ui = ph > 0 ? ph / kDesignH : 1.0f;
  } else {
    int wh = 900;
    SDL_Rect r;
    if (gxlle::g_window && SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(gxlle::g_window), &r)) wh = r.h;
    float dpi = gxlle::g_window ? SDL_GetWindowPixelDensity(gxlle::g_window) : 1.0f;
    ui = wh * std::max(1.0f, dpi) / 900.0f;
  }
  g_font_ui = g_ui = std::clamp(ui, 0.6f, 4.0f);
  if (io.Fonts->Fonts.empty()) io.Fonts->AddFontDefault();   // stays first: the in-game overlays use it
  auto load = [&](std::initializer_list<const char*> files, float px) -> ImFont* {
    for (const char* f : files)
      if (FILE* t = fopen(f, "rb")) {
        fclose(t);
        if (ImFont* ft = io.Fonts->AddFontFromFileTTF(f, px * g_font_ui)) return ft;
      }
    return io.Fonts->Fonts[0];
  };
  f_body = load({"C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf"}, 15);
  f_small = load({"C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf"}, 13);
  f_label = load({"C:/Windows/Fonts/seguisb.ttf", "C:/Windows/Fonts/segoeuib.ttf", "C:/Windows/Fonts/arialbd.ttf"}, 15);
  f_h2 = load({"C:/Windows/Fonts/seguisb.ttf", "C:/Windows/Fonts/segoeuib.ttf", "C:/Windows/Fonts/arialbd.ttf"}, 18);
  f_h1 = load({"C:/Windows/Fonts/bahnschrift.ttf", "C:/Windows/Fonts/segoeuib.ttf"}, 28);
  f_title = load({"C:/Windows/Fonts/bahnschrift.ttf", "C:/Windows/Fonts/segoeuib.ttf"}, 58);
  game_menu::init_fonts();   // #132 in-game settings menu
}
}  // namespace settings

namespace launcher {
void run() {
  using namespace settings;
  gxlle::g_launcher_mode = true;   // compact window; the game size / fullscreen are applied on Play
  gxlle::init_window();            // init_fonts runs inside
  apply_style();
  g_emblem = load_art(201);
  g_hero = load_art(202);
  ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
  logf("[launcher] open\n");
  // tests: SHADOW_LAUNCHER_PAGE=<0-8> opens on that page, SHADOW_LAUNCHER_AUTOPLAY=<seconds> presses Play
  if (const char* pg = getenv("SHADOW_LAUNCHER_PAGE")) g_page = std::clamp(atoi(pg), 0, kPages - 1);
  const char* ap = getenv("SHADOW_LAUNCHER_AUTOPLAY");
  const uint64_t autoplay_at = ap ? sys_time_ns() + (uint64_t)(atof(ap) * 1e9) : 0;
  uint64_t last = 0;
  while (!g_play) {
    if (autoplay_at && sys_time_ns() >= autoplay_at) g_play = true;
    if (!gxlle::ui_frame(draw_launcher)) { save(); logf("[launcher] window closed\n"); sys_exit_guarded(0); }
    // ~120 frames per second at most: the launcher must not burn the GPU / CPU while you read it
    uint64_t now = sys_time_ns();
    if (last && now - last < 8000000ull) SDL_DelayNS(8000000ull - (now - last));
    last = sys_time_ns();
  }
  save();
  ImGui::GetIO().ConfigFlags &= ~(ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad);
  ImGui::GetIO().FontGlobalScale = 1.0f;
  ImGui::GetStyle() = ImGuiStyle();   // settings::draw() restyles the in-game overlays
  ImGui::StyleColorsDark();
  gxlle::ui_resume_game();
  logf("[launcher] play\n");
}
}  // namespace launcher
