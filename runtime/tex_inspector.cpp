// Texture viewer (F3): every game texture drawn in the last moment, with its file name in textures\Original and
// whether textures\Custom replaces it - and "paint": a picked texture is shown in the game tinted with a loud colour
// and stripes, so you can see exactly where it is on screen. Nothing on disk changes (painting is an in-memory
// replacement with top priority; unpaint / closing the viewer removes it).
// Aurora collects the names (aurora::texture::set_usage_tracking / take_used_textures, lib/gx/texture.cpp).
#include "sys.h"
#include "settings_ui.h"
#include "texture_pack.h"
#include <dolphin/gx.h>
#include <aurora/texture.hpp>
#include <aurora/imgui.h>
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace tex_inspector {
namespace {
bool g_open = false;
struct Seen { uint64_t last_ns = 0; };
std::unordered_map<std::string, Seen> g_seen;          // key -> when it was last drawn
struct Where { std::string orig, custom, other; };     // relative to textures\ (empty = none)
std::unordered_map<std::string, Where> g_index;
bool g_indexed = false;
char g_filter[96] = "";
bool g_vanilla_only = false;

// paint: key -> registration + colour
struct Paint { aurora::texture::ReplacementRegistration reg; int colour; std::string vpath; };
std::map<std::string, Paint> g_paint;
std::mutex g_paint_mutex;
std::unordered_map<std::string, std::string> g_paint_src;   // virtual path -> key (read callback)
int g_next_colour = 0;
const unsigned char kColours[][3] = {{255, 0, 220}, {0, 230, 255}, {255, 230, 0}, {60, 255, 60}, {255, 120, 0}, {40, 90, 255}};
const int kNumColours = sizeof kColours / sizeof kColours[0];

// hover previews: pictures loaded once (shrunk to at most 256 px) and kept as ImGui textures
struct Prev { ImTextureID id = 0; int w = 0, h = 0; };
std::unordered_map<std::string, Prev> g_prev;   // relative path -> preview

const Prev& preview(const std::string& relp) {
  auto it = g_prev.find(relp);
  if (it != g_prev.end()) return it->second;
  Prev p;
  int w, h;
  std::vector<uint8_t> px;
  if (!relp.empty() && g_prev.size() < 400 &&
      texture_pack::read_png(texture_pack::textures_dir() / fs::u8path(relp), w, h, px)) {
    int s = 1;
    while (w / s > 256 || h / s > 256) s *= 2;
    if (s > 1) {   // box-shrink
      const int nw = std::max(1, w / s), nh = std::max(1, h / s);
      std::vector<uint8_t> m((size_t)nw * nh * 4);
      for (int y = 0; y < nh; ++y)
        for (int x = 0; x < nw; ++x)
          for (int i = 0; i < 4; ++i) {
            unsigned acc = 0;
            for (int yy = 0; yy < s; ++yy)
              for (int xx = 0; xx < s; ++xx) acc += px[((size_t)(y * s + yy) * w + x * s + xx) * 4 + i];
            m[((size_t)y * nw + x) * 4 + i] = (uint8_t)(acc / (s * s));
          }
      px.swap(m);
      w = nw; h = nh;
    }
    p.id = aurora_imgui_add_texture((uint32_t)w, (uint32_t)h, px.data());
    p.w = w; p.h = h;
  }
  return g_prev[relp] = p;
}

void preview_image(const char* label, const std::string& relp) {
  const Prev& p = preview(relp);
  ImGui::BeginGroup();
  ImGui::TextDisabled("%s", label);
  if (p.id) {
    const float k = 180.f / std::max(p.w, p.h);
    const ImVec2 pos = ImGui::GetCursorScreenPos(), sz(p.w * k, p.h * k);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float t = 10;   // checkerboard behind see-through parts
    for (float y = 0; y < sz.y; y += t)
      for (float x = 0; x < sz.x; x += t)
        dl->AddRectFilled(ImVec2(pos.x + x, pos.y + y), ImVec2(pos.x + std::min(x + t, sz.x), pos.y + std::min(y + t, sz.y)),
                          ((int)(x / t) + (int)(y / t)) % 2 ? IM_COL32(70, 70, 70, 255) : IM_COL32(100, 100, 100, 255));
    ImGui::Image(p.id, sz);
  } else {
    ImGui::Dummy(ImVec2(180, 20));
    ImGui::TextDisabled(relp.empty() ? "(none)" : "(could not load)");
  }
  ImGui::EndGroup();
}

std::string key_in(const std::string& name) {
  size_t a = name.find("[tex1_");
  if (a == std::string::npos) return {};
  size_t b = name.find(']', a);
  return b == std::string::npos ? std::string() : name.substr(a + 1, b - a - 1);
}

void build_index() {
  g_index.clear();
  const fs::path tex = texture_pack::textures_dir();
  struct Root { const char* dir; int slot; };
  const Root roots[] = {{"Original", 0}, {"Custom", 1}, {"Not needed", 2}};
  for (const Root& r : roots) {
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(tex / r.dir, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
      if (!it->is_regular_file(ec) || it->path().extension() != ".png") continue;
      std::string k = key_in(it->path().filename().string());
      if (k.empty()) continue;
      std::string relp = fs::relative(it->path(), tex, ec).string();
      Where& w = g_index[k];
      (r.slot == 0 ? w.orig : r.slot == 1 ? w.custom : w.other) = relp;
    }
  }
  g_indexed = true;
  logf("[texview] index: %zu keyed textures\n", g_index.size());
}

// the painted picture: what the game shows now (Custom result, else the original), tinted + diagonal stripes
bool paint_read(void*, const char* vpath_c, std::vector<uint8_t>& out) {
  // Aurora also asks for "<name>_mipN.png": the same picture at 1/2^N size
  std::string vpath = vpath_c;
  int mip = 0;
  {
    size_t m = vpath.rfind("_mip");
    if (m != std::string::npos && vpath.size() > 4 && vpath.compare(vpath.size() - 4, 4, ".png") == 0) {
      mip = atoi(vpath.c_str() + m + 4);
      vpath = vpath.substr(0, m) + ".png";
    }
  }
  std::string key;
  int colour = 0;
  {
    std::lock_guard lk(g_paint_mutex);
    auto it = g_paint_src.find(vpath);
    if (it == g_paint_src.end()) { logf("[texview] read for unknown %s\n", vpath.c_str()); return false; }
    key = it->second;
    auto p = g_paint.find(key);
    if (p != g_paint.end()) colour = p->second.colour;
  }
  const fs::path tex = texture_pack::textures_dir();
  int w = 0, h = 0;
  std::vector<uint8_t> px;
  auto wi = g_index.find(key);
  bool ok = false;
  if (wi != g_index.end()) {
    if (!wi->second.custom.empty()) ok = texture_pack::read_png(tex / fs::u8path(wi->second.custom), w, h, px);
    if (!ok && !wi->second.orig.empty()) ok = texture_pack::read_png(tex / fs::u8path(wi->second.orig), w, h, px);
    if (!ok && !wi->second.other.empty()) ok = texture_pack::read_png(tex / fs::u8path(wi->second.other), w, h, px);
  }
  if (!ok) {   // not exported: a plain painted square of the texture's size
    if (sscanf(key.c_str(), "tex1_%dx%d", &w, &h) != 2 || w <= 0 || h <= 0) return false;
    px.assign((size_t)w * h * 4, 255);
  }
  const unsigned char* c = kColours[colour % kNumColours];
  const int stripe = std::max(2, std::max(w, h) / 16);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      uint8_t* p = &px[((size_t)y * w + x) * 4];
      const bool dark = ((x + y) / stripe) % 2;
      const float k = dark ? 0.85f : 0.6f;
      for (int i = 0; i < 3; ++i) {
        const float base = dark ? c[i] * 0.45f : (float)c[i];
        p[i] = (uint8_t)std::lround(p[i] * (1 - k) + base * k);
      }
      if (p[3] < 40) p[3] = 40;   // see-through parts show faintly too
    }
  // Aurora asks for mip 1, 2, 3... until one is refused: stop after the 1x1 level (else it asks forever)
  if (mip > 0 && (w >> (mip - 1)) <= 1 && (h >> (mip - 1)) <= 1) return false;
  if (mip > 0) {   // box-shrink to the mip level's size
    const int mw = std::max(1, w >> mip), mh = std::max(1, h >> mip);
    std::vector<uint8_t> m((size_t)mw * mh * 4);
    for (int y = 0; y < mh; ++y)
      for (int x = 0; x < mw; ++x) {
        const int x0 = x * w / mw, x1 = std::max(x0 + 1, (x + 1) * w / mw), y0 = y * h / mh, y1 = std::max(y0 + 1, (y + 1) * h / mh);
        unsigned acc[4] = {0, 0, 0, 0}, n = 0;
        for (int yy = y0; yy < y1; ++yy)
          for (int xx = x0; xx < x1; ++xx, ++n)
            for (int i = 0; i < 4; ++i) acc[i] += px[((size_t)yy * w + xx) * 4 + i];
        for (int i = 0; i < 4; ++i) m[((size_t)y * mw + x) * 4 + i] = (uint8_t)(acc[i] / std::max(1u, n));
      }
    return texture_pack::write_png(m, mw, mh, out);
  }
  return texture_pack::write_png(px, w, h, out);
}

// (Aurora's register/unregister may wait for paint_read on a worker thread: never call them holding g_paint_mutex)
void set_paint(const std::string& key, bool on) {
  if (!on) {
    Paint p;
    {
      std::lock_guard lk(g_paint_mutex);
      auto it = g_paint.find(key);
      if (it == g_paint.end()) return;
      p = it->second;
      g_paint.erase(it);
    }
    aurora::texture::unregister_replacement(p.reg);
    std::lock_guard lk(g_paint_mutex);
    g_paint_src.erase(p.vpath);
    return;
  }
  static int serial = 0;
  Paint p;
  {
    std::lock_guard lk(g_paint_mutex);
    if (g_paint.count(key)) return;
    p.colour = g_next_colour++ % kNumColours;
    p.vpath = "texview/" + std::to_string(++serial) + "/" + key + ".png";
    g_paint_src[p.vpath] = key;
    g_paint[key] = p;   // reg filled in below
  }
  aurora::texture::VirtualFileSource src{paint_read, nullptr};
  auto reg = aurora::texture::register_virtual_replacement(p.vpath, src, {.priority = 1000});
  std::lock_guard lk(g_paint_mutex);
  if (!reg.id) {
    g_paint_src.erase(p.vpath);
    g_paint.erase(key);
    logf("[texview] could not paint %s\n", key.c_str());
    return;
  }
  g_paint[key].reg = reg;
  logf("[texview] painting %s (id %llu)\n", key.c_str(), (unsigned long long)reg.id);
}

void unpaint_all() {
  std::vector<std::string> keys;
  {
    std::lock_guard lk(g_paint_mutex);
    for (auto& [k, p] : g_paint) keys.push_back(k);
  }
  for (auto& k : keys) set_paint(k, false);
}

bool contains_ci(const std::string& hay, const char* needle) {
  if (!*needle) return true;
  std::string a = hay, b = needle;
  std::transform(a.begin(), a.end(), a.begin(), ::tolower);
  std::transform(b.begin(), b.end(), b.begin(), ::tolower);
  return a.find(b) != std::string::npos;
}
}  // namespace

// TEXLOG <label> (testing, INPUT_FILE): for the next 1.5 s Aurora tracks every texture drawn, then draw() appends
// "<label>, <key>" lines to work/texlog.txt (tab separated) - used to learn which screen really uses which texture.
std::mutex g_log_mutex;
std::string g_log_label;
uint64_t g_log_deadline = 0;

void log_used(const char* label) {
  std::lock_guard lk(g_log_mutex);
  aurora::texture::set_usage_tracking(true);
  aurora::texture::take_used_textures();   // forget what was drawn before
  g_log_label = (label && *label) ? label : "?";
  g_log_deadline = sys_time_ns() + 1500000000ull;
}

void toggle() {
  g_open = !g_open;
  aurora::texture::set_usage_tracking(g_open);
  if (g_open && !g_indexed) build_index();
  if (!g_open) {
    unpaint_all();
    g_seen.clear();
  }
  logf("[texview] %s\n", g_open ? "open" : "closed");
}

bool is_open() { return g_open; }

std::vector<std::pair<std::string, bool>> g_pending;   // paints asked from other threads, done in draw()
std::mutex g_pending_mutex;

void paint(const char* key, bool on) {
  std::lock_guard lk(g_pending_mutex);
  g_pending.emplace_back(key ? key : "", on);
}

void paint_now(const char* key, bool on) {
  if (!g_indexed) build_index();
  if (key && !strcmp(key, "ALL")) {   // testing: every texture drawn in the last 1.5 s
    const uint64_t now = sys_time_ns();
    for (std::string& k : aurora::texture::take_used_textures()) g_seen[k].last_ns = now;
    std::vector<std::string> keys;
    for (auto& [k, s] : g_seen)
      if (now - s.last_ns < 1500000000ull) keys.push_back(k);
    for (auto& k : keys) set_paint(k, on);
    return;
  }
  set_paint(key ? key : "", on);
}

void draw() {
  {
    std::vector<std::pair<std::string, bool>> todo;
    {
      std::lock_guard lk(g_pending_mutex);
      todo.swap(g_pending);
    }
    for (auto& [k, on] : todo) paint_now(k.c_str(), on);
  }
  {
    std::string label;
    {
      std::lock_guard lk(g_log_mutex);
      if (g_log_deadline && sys_time_ns() >= g_log_deadline) {
        label = g_log_label;
        g_log_deadline = 0;
      }
    }
    if (!label.empty()) {
      std::vector<std::string> keys = aurora::texture::take_used_textures();
      if (!g_open) aurora::texture::set_usage_tracking(false);
      const char* path = "work/texlog.txt";   // relative to the working folder (tools/session.ps1 starts the game in the project root)
      if (FILE* f = fopen(path, "ab")) {
        std::sort(keys.begin(), keys.end());
        for (auto& k : keys) fprintf(f, "%s\t%s\n", label.c_str(), k.c_str());
        fprintf(f, "%s\t-- end (%zu textures)\n", label.c_str(), keys.size());
        fclose(f);
      }
      logf("[texlog] %s: %zu textures\n", label.c_str(), keys.size());
    }
  }
  if (!g_open) return;
  const uint64_t now = sys_time_ns();
  for (std::string& k : aurora::texture::take_used_textures()) g_seen[k].last_ns = now;

  // drawn within the last 1.5 s (or painted) = "on screen now"
  struct Row { std::string key; const Where* w; bool recent; };
  std::vector<Row> rows;
  int n_vanilla = 0;
  for (auto& [k, s] : g_seen) {
    const bool recent = now - s.last_ns < 1500000000ull;
    bool painted;
    {
      std::lock_guard lk(g_paint_mutex);
      painted = g_paint.count(k) != 0;
    }
    if (!recent && !painted) continue;
    auto wi = g_index.find(k);
    const Where* w = wi == g_index.end() ? nullptr : &wi->second;
    const bool vanilla = !w || w->custom.empty();
    n_vanilla += vanilla;
    if (g_vanilla_only && !vanilla) continue;
    const std::string label = (w ? (w->custom.empty() ? (w->orig.empty() ? w->other : w->orig) : w->custom) : "") + " " + k;
    if (!contains_ci(label, g_filter)) continue;
    rows.push_back({k, w, recent});
  }
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
    const bool va = !a.w || a.w->custom.empty(), vb = !b.w || b.w->custom.empty();
    if (va != vb) return va;   // ones without a Custom result first
    return a.key < b.key;
  });

  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->Size.x - 20, 60), ImGuiCond_FirstUseEver, ImVec2(1, 0));
  ImGui::SetNextWindowSize(ImVec2(std::min(900.f, vp->Size.x * 0.55f), vp->Size.y * 0.7f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(0.9f);
  bool open = true;
  if (ImGui::Begin("Texture viewer  (F3 to close)", &open)) {
    ImGui::Text("%zu textures on screen now, %d without a Custom result.", rows.size(), n_vanilla);
    ImGui::TextDisabled("Tick Paint to colour that texture in the game. Click a name to copy it.");
    ImGui::SetNextItemWidth(260);
    ImGui::InputTextWithHint("##filter", "filter: name, folder or key", g_filter, sizeof g_filter);
    ImGui::SameLine();
    ImGui::Checkbox("Only without a Custom result", &g_vanilla_only);
    ImGui::SameLine();
    if (ImGui::Button("Unpaint all")) unpaint_all();
    ImGui::SameLine();
    if (ImGui::Button("Re-read folders")) build_index();
    ImGui::Separator();
    if (ImGui::BeginTable("tex", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                                        ImGuiTableFlags_BordersInnerV, ImVec2(0, 0))) {
      ImGui::TableSetupScrollFreeze(0, 1);
      ImGui::TableSetupColumn("Paint", ImGuiTableColumnFlags_WidthFixed, 58);
      ImGui::TableSetupColumn("In game", ImGuiTableColumnFlags_WidthFixed, 82);
      ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 1.0f);
      ImGui::TableSetupColumn("Folder", ImGuiTableColumnFlags_WidthStretch, 1.4f);
      ImGui::TableHeadersRow();
      ImGuiListClipper clip;
      clip.Begin((int)rows.size());
      while (clip.Step())
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
          const Row& r = rows[i];
          ImGui::PushID(i);
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          bool painted;
          int colour = 0;
          {
            std::lock_guard lk(g_paint_mutex);
            auto p = g_paint.find(r.key);
            painted = p != g_paint.end();
            if (painted) colour = p->second.colour;
          }
          if (ImGui::Checkbox("##p", &painted)) set_paint(r.key, painted);
          if (painted) {
            ImGui::SameLine();
            const unsigned char* c = kColours[colour % kNumColours];
            ImGui::ColorButton("##c", ImVec4(c[0] / 255.f, c[1] / 255.f, c[2] / 255.f, 1), ImGuiColorEditFlags_NoTooltip,
                               ImVec2(14, 14));
          }
          ImGui::TableNextColumn();
          if (!r.w) ImGui::TextColored(ImVec4(1, .55f, .3f, 1), "not exported");
          else if (!r.w->custom.empty()) ImGui::TextColored(ImVec4(.3f, .85f, .5f, 1), "Custom");
          else ImGui::TextColored(ImVec4(1, .8f, .3f, 1), r.w->orig.empty() ? "original*" : "original");
          ImGui::TableNextColumn();
          const std::string& path = r.w ? (r.w->custom.empty() ? (r.w->orig.empty() ? r.w->other : r.w->orig) : r.w->custom) : r.key;
          std::string name = fs::u8path(path).filename().string();
          if (name.size() > 4 && name.compare(name.size() - 4, 4, ".png") == 0) name.resize(name.size() - 4);
          if (ImGui::Selectable(name.c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) {
            ImGui::SetClipboardText(name.c_str());
          }
          if (ImGui::IsItemHovered()) {   // preview: the original and what the game uses now
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(path.c_str());
            ImGui::TextDisabled("key %s   (click: copy the file name)", r.key.c_str());
            if (r.w) {
              preview_image("Original", r.w->orig.empty() ? r.w->other : r.w->orig);
              ImGui::SameLine(0, 12);
              preview_image("Custom (in the game)", r.w->custom);
            } else {
              ImGui::TextDisabled("Not in the exported textures: no preview.");
            }
            ImGui::EndTooltip();
          }
          ImGui::TableNextColumn();
          ImGui::TextDisabled("%s", fs::u8path(path).parent_path().string().c_str());
          ImGui::PopID();
        }
      ImGui::EndTable();
    }
  }
  ImGui::End();
  if (!open) toggle();
}
}  // namespace tex_inspector
