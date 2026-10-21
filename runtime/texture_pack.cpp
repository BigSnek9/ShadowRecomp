// Custom textures: edited PNG/DDS files in <exe dir>\textures\Custom replace the game's textures (settings
// "Load custom textures"). tools/texture_dump.py exports every texture of the disc into textures\Original as
// "<name> [tex1_<w>x<h>_<hash>[_<palette hash>]_<format>].png"; copy any of them into Custom (any sub-folders, any
// size - higher resolution is fine), edit, and keep the bracket part of the name: that is the key Aurora computes for
// the original texture at run time (Dolphin texture-pack convention), so the edit replaces it wherever it is used.
//
// Files are registered with Aurora as "virtual" replacements so the readable name can stay in front of the key; Aurora
// reads each file lazily on its worker threads through read_file(). While the option is on, the folder is checked
// every 2 s and re-registered when anything changed, so saving an edited file shows up in the running game.
//
// UI masters (tools/ui_master.py): textures that are the same pieces arranged differently per stage - the font glyph
// sheets (each stage's sheet holds only the characters its text uses) and the pause screen's mission icon sheets -
// are described in <exe dir>\textures\ui_masters.txt as slots filled from one master image. When a master
// ("font master Latin.png", "mission icon master.png", ...) is anywhere in Custom, every texture built from it is
// registered (priority 1: wins over loose files with the same key) and composed on demand at the master's scale.
#define NOMINMAX
#include "sys.h"
#include "settings_ui.h"
#include <dolphin/gx.h>          // GXTexObj, used by aurora/texture.hpp
#include <aurora/texture.hpp>
#include <png.h>
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include <imstb_truetype.h>   // TrueType rasteriser (ships with Dear ImGui): card template text
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <thread>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace texture_pack {
namespace {
std::mutex g_mutex;                                       // guards g_files (read from Aurora worker threads)
std::unordered_map<std::string, fs::path> g_files;        // virtual path -> real file
std::vector<aurora::texture::ReplacementRegistration> g_regs;
int g_count = 0;
bool g_active = false;

// the custom textures folder: the launcher's choice (settings custom_textures_dir, UTF-8), else <exe dir>\textures\Custom
fs::path folder() {
  if (!settings::g.custom_textures_dir.empty()) {
    fs::path p = fs::u8path(settings::g.custom_textures_dir);
    std::error_code ec;
    if (fs::is_directory(p, ec)) return p;
  }
  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  return fs::path(exe).parent_path() / L"textures" / L"Custom";
}

// ---- UI masters
struct Piece { int x, y, w, h; };
struct Slot { int piece, x, y, w, h, ox, oy; };
struct MasterLayout {
  std::string name;
  int w = 0, h = 0;
  uint8_t bg[4] = {};
  std::vector<Piece> pieces;
};
struct SheetLayout {
  std::string key;
  int master = 0, w = 0, h = 0;
  std::vector<Slot> slots;
};
struct MasterImage {             // a master found in Custom, decoded
  const MasterLayout* layout = nullptr;
  int w = 0, h = 0;
  std::vector<uint8_t> rgba;
};
std::vector<MasterLayout> g_layout_masters;
std::vector<SheetLayout> g_layout_sheets;
std::unordered_map<std::string, std::pair<std::shared_ptr<MasterImage>, const SheetLayout*>> g_built;   // virtual path ->
int g_built_count = 0;

fs::path exe_dir() {
  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  return fs::path(exe).parent_path();
}

void read_layout() {   // once; ui_masters.txt is written by tools/ui_master.py
  static bool done = false;
  if (done) return;
  done = true;
  std::ifstream f(exe_dir() / L"textures" / L"ui_masters.txt");
  std::string line;
  while (std::getline(f, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream s(line);
    std::string tag;
    s >> tag;
    if (tag == "master") {   // master <name with spaces> <w> <h> <r g b a>
      std::vector<std::string> t;
      for (std::string w; s >> w;) t.push_back(w);
      if (t.size() < 7) continue;
      MasterLayout m;
      for (size_t i = 0; i + 6 < t.size(); ++i) m.name += (i ? " " : "") + t[i];
      size_t n = t.size();
      m.w = std::stoi(t[n - 6]);
      m.h = std::stoi(t[n - 5]);
      for (int i = 0; i < 4; ++i) m.bg[i] = (uint8_t)std::stoi(t[n - 4 + i]);
      g_layout_masters.push_back(std::move(m));
    } else if (tag == "p" && !g_layout_masters.empty()) {
      Piece p{};
      s >> p.x >> p.y >> p.w >> p.h;
      g_layout_masters.back().pieces.push_back(p);
    } else if (tag == "sheet" && !g_layout_masters.empty()) {
      SheetLayout sh;
      s >> sh.key >> sh.w >> sh.h;
      sh.master = (int)g_layout_masters.size() - 1;
      g_layout_sheets.push_back(std::move(sh));
    } else if (tag == "b" && !g_layout_sheets.empty()) {
      Slot b{};
      s >> b.piece >> b.x >> b.y >> b.w >> b.h >> b.ox >> b.oy;
      g_layout_sheets.back().slots.push_back(b);
    }
  }
  if (!g_layout_masters.empty())
    logf("[textures] ui_masters.txt: %zu masters, %zu textures built from them\n", g_layout_masters.size(),
         g_layout_sheets.size());
}

std::string lower(std::string s) {
  for (auto& ch : s) ch = (char)tolower((unsigned char)ch);
  return s;
}

bool decode_png(const fs::path& path, MasterImage& img) {
  png_image pi;
  memset(&pi, 0, sizeof pi);
  pi.version = PNG_IMAGE_VERSION;
  FILE* f = _wfopen(path.c_str(), L"rb");
  if (!f) return false;
  bool ok = png_image_begin_read_from_stdio(&pi, f);
  if (ok) {
    pi.format = PNG_FORMAT_RGBA;
    img.w = (int)pi.width;
    img.h = (int)pi.height;
    img.rgba.resize(PNG_IMAGE_SIZE(pi));
    ok = png_image_finish_read(&pi, nullptr, img.rgba.data(), 0, nullptr);
  }
  png_image_free(&pi);
  fclose(f);
  return ok;
}

// AI upscalers often return a master without transparency (RGB, or alpha all 255). For a master whose pieces sit on
// a transparent background, tools/ui_master.py saves the original transparency as textures\ui_masters\<name> alpha.png;
// it is scaled (bilinear) to the improved master's size and put back.
void restore_alpha(MasterImage& img) {
  if (img.layout->bg[3] != 0) return;
  for (size_t i = 3; i < img.rgba.size(); i += 4)
    if (img.rgba[i] != 255) return;   // the master has its own transparency
  MasterImage a;
  if (!decode_png(exe_dir() / L"textures" / L"ui_masters" / fs::path(img.layout->name + " alpha.png"), a)) {
    logf("[textures] %s has no transparency and no saved shape to restore it\n", img.layout->name.c_str());
    return;
  }
  for (int y = 0; y < img.h; ++y) {
    const double fy = std::clamp((y + 0.5) * a.h / img.h - 0.5, 0.0, a.h - 1.0);
    const int y0 = (int)fy, y1 = std::min(y0 + 1, a.h - 1);
    const double ty = fy - y0;
    for (int x = 0; x < img.w; ++x) {
      const double fx = std::clamp((x + 0.5) * a.w / img.w - 0.5, 0.0, a.w - 1.0);
      const int x0 = (int)fx, x1 = std::min(x0 + 1, a.w - 1);
      const double tx = fx - x0;
      auto at = [&](int xx, int yy) { return (double)a.rgba[((size_t)yy * a.w + xx) * 4]; };   // grey -> R
      const double v = (at(x0, y0) * (1 - tx) + at(x1, y0) * tx) * (1 - ty) + (at(x0, y1) * (1 - tx) + at(x1, y1) * tx) * ty;
      img.rgba[((size_t)y * img.w + x) * 4 + 3] = (uint8_t)std::lround(v);
    }
  }
  logf("[textures] %s: transparency restored from the original shape\n", img.layout->name.c_str());
}

void png_write_vec(png_structp p, png_bytep data, png_size_t n) {
  auto* v = (std::vector<uint8_t>*)png_get_io_ptr(p);
  v->insert(v->end(), data, data + n);
}

bool encode_png(const std::vector<uint8_t>& rgba, int w, int h, std::vector<uint8_t>& out) {
  png_structp p = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  if (!p) return false;
  png_infop info = png_create_info_struct(p);
  if (!info || setjmp(png_jmpbuf(p))) {
    png_destroy_write_struct(&p, &info);
    return false;
  }
  out.clear();
  png_set_write_fn(p, &out, png_write_vec, nullptr);
  png_set_compression_level(p, 1);   // read back right away: speed over size
  png_set_filter(p, 0, PNG_FILTER_NONE);
  png_set_IHDR(p, info, w, h, 8, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
               PNG_FILTER_TYPE_DEFAULT);
  png_write_info(p, info);
  for (int y = 0; y < h; ++y) png_write_row(p, (png_const_bytep)(rgba.data() + (size_t)y * w * 4));
  png_write_end(p, nullptr);
  png_destroy_write_struct(&p, &info);
  return true;
}

// One texture from its master, at the master's scale (an upscaled master gives an upscaled texture). Output pixel
// (X, Y) inside slot b is original position u = (X + .5) / scale - b.x within the slot; the piece is drawn with its
// point (ox, oy) at the slot origin, so it samples piece pixel u + ox, clipped to the slot and the piece; "over".
bool compose(const MasterImage& m, const SheetLayout& sh, std::vector<uint8_t>& out) {
  const MasterLayout& L = *m.layout;
  const double sx = (double)m.w / L.w, sy = (double)m.h / L.h;
  const int W = std::max(1, (int)std::lround(sh.w * sx)), H = std::max(1, (int)std::lround(sh.h * sy));
  std::vector<uint8_t> px((size_t)W * H * 4);
  for (size_t i = 0; i < px.size(); i += 4) memcpy(&px[i], L.bg, 4);
  for (const Slot& b : sh.slots) {
    if (b.piece < 0 || b.piece >= (int)L.pieces.size()) continue;
    const Piece& p = L.pieces[b.piece];
    const int u0 = std::max(0, -b.ox), u1 = std::min(b.w, p.w - b.ox);   // slot-relative range the piece covers
    const int v0 = std::max(0, -b.oy), v1 = std::min(b.h, p.h - b.oy);
    if (u0 >= u1 || v0 >= v1) continue;
    const int X0 = (int)std::lround((b.x + u0) * sx), X1 = (int)std::lround((b.x + u1) * sx);
    const int Y0 = (int)std::lround((b.y + v0) * sy), Y1 = (int)std::lround((b.y + v1) * sy);
    for (int Y = std::max(0, Y0); Y < std::min(H, Y1); ++Y) {
      const int my = std::clamp((int)((p.y + b.oy + ((Y + 0.5) / sy - b.y)) * sy), 0, m.h - 1);
      for (int X = std::max(0, X0); X < std::min(W, X1); ++X) {
        const int mx = std::clamp((int)((p.x + b.ox + ((X + 0.5) / sx - b.x)) * sx), 0, m.w - 1);
        const uint8_t* s = &m.rgba[((size_t)my * m.w + mx) * 4];
        uint8_t* d = &px[((size_t)Y * W + X) * 4];
        const int a = s[3];
        if (a == 255) { memcpy(d, s, 4); continue; }
        if (a == 0) continue;
        // PNG stores straight alpha. Preserve the colour of a translucent source
        // over transparent pixels; premultiplying here darkens its edge again
        // when the GPU later blends the composed texture.
        const int da = d[3];
        const int coverage = a * 255 + da * (255 - a);
        for (int c = 0; c < 3; ++c)
          d[c] = (uint8_t)((s[c] * a * 255 + d[c] * da * (255 - a) + coverage / 2) / coverage);
        d[3] = (uint8_t)((coverage + 127) / 255);
      }
    }
  }
  // Pad invisible texels beside the art with its edge colour. Linear filtering
  // must not mix the edge with black RGB just because its neighbour has alpha 0.
  // Alpha and the slot geometry remain unchanged.
  std::vector<uint8_t> known((size_t)W * H);
  for (size_t i = 0; i < known.size(); ++i) known[i] = px[i * 4 + 3] != 0;
  for (int pass = 0; pass < 2; ++pass) {
    auto next = known;
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
      const size_t i = (size_t)y * W + x;
      if (known[i]) continue;
      const size_t neighbours[] = {x ? i - 1 : i, x + 1 < W ? i + 1 : i,
                                   y ? i - W : i, y + 1 < H ? i + W : i};
      int sum[3] = {}, n = 0;
      for (size_t j : neighbours) if (known[j]) {
        for (int c = 0; c < 3; ++c) sum[c] += px[j * 4 + c];
        ++n;
      }
      if (n) {
        for (int c = 0; c < 3; ++c) px[i * 4 + c] = (uint8_t)((sum[c] + n / 2) / n);
        next[i] = 1;
      }
    }
    known.swap(next);
  }
  return encode_png(px, W, H, out);
}

// ---- Card templates (*.cards.txt anywhere in Custom): textures that are the same art with different text (stage
// title cards, "stage N / loading" labels...) are composed at run time from image layers (any texture by its key - an
// improved copy in Custom wins over textures\Original - or a plain PNG next to the template) and text layers drawn
// with a TrueType font, at `scale` x the original size. One template line per texture: its key and its strings.
// Registered with priority 1 (wins over loose files with the same key). Format: see tools/card_templates.py.
struct CardImage {               // an image layer
  std::string name;              // file stem, key ("tex1_...") or "[tex1_...]" name
  double angle = 0, scale = 1, x = 0, y = 0, w = 0, h = 0;   // original-pixel units; w/h > 0 = fit that box
  std::shared_ptr<MasterImage> img;
  int base_w = 0, base_h = 0;    // the layer's original size (from the key, or the file at 1x)
};
struct CardText {                // a text layer
  std::string font;              // a .ttf/.ttc path, or "@alias" from the nearest Fonts.txt
  double size = 16, x = 0, y = 0, spacing = 0, maxwidth = 0, outline = 0, bold = 0;
  uint8_t color[4] = {255, 255, 255, 255}, ocolor[4] = {0, 0, 0, 255};
  int align = 0;                 // 0 left, 1 centre, 2 right (x is the anchor)
  int caseconv = 0;              // 1 upper, 2 lower
  std::string str;               // `string "..."`: the text when the make line gives none for this layer
  bool boxed = false;            // `box x y w h`: the text's ink is fitted into that box (lines: "\n")
  double bx = 0, by = 0, bw = 0, bh = 0;
  std::vector<std::string> fontfiles;   // resolved at load: the font and its fallbacks
};
struct CardMake { std::string key; std::vector<std::string> texts; };
struct CardTemplate {
  fs::path file;
  int w = 0, h = 0;
  double scale = 4;
  std::vector<CardImage> images;
  std::vector<CardText> texts;
  std::vector<CardMake> makes;
};
std::vector<std::shared_ptr<CardTemplate>> g_cards;
std::unordered_map<std::string, std::pair<std::shared_ptr<CardTemplate>, size_t>> g_card_built;   // vpath -> make
std::unordered_map<std::string, std::shared_ptr<std::vector<uint8_t>>> g_fonts;                  // path -> TTF bytes

std::vector<std::string> card_tokens(const std::string& line) {
  std::vector<std::string> t;
  for (size_t i = 0; i < line.size();) {
    while (i < line.size() && isspace((unsigned char)line[i])) ++i;
    if (i >= line.size() || line[i] == '#') break;
    std::string tok;
    if (line[i] == '"') {
      size_t j = line.find('"', i + 1);
      if (j == std::string::npos) j = line.size();
      tok = line.substr(i + 1, j - i - 1);
      i = j + 1;
    } else {
      size_t j = i;
      while (j < line.size() && !isspace((unsigned char)line[j])) ++j;
      tok = line.substr(i, j - i);
      i = j;
    }
    t.push_back(tok);
  }
  return t;
}
void card_color(const std::string& s, uint8_t* c) {   // RRGGBB or RRGGBBAA
  unsigned v = (unsigned)strtoul(s.c_str(), nullptr, 16);
  if (s.size() >= 8) { c[0] = v >> 24; c[1] = v >> 16; c[2] = v >> 8; c[3] = v; }
  else { c[0] = v >> 16; c[1] = v >> 8; c[2] = v; c[3] = 255; }
}
// A file holds one template, or several: a `card` line after a `make` line starts the next one.
std::vector<std::shared_ptr<CardTemplate>> parse_cards(const fs::path& file) {
  std::ifstream f(file);
  std::vector<std::shared_ptr<CardTemplate>> all;
  auto t = std::make_shared<CardTemplate>();
  t->file = file;
  auto finish = [&]() {
    if (t->w > 0 && t->h > 0 && !t->makes.empty()) all.push_back(t);
    t = std::make_shared<CardTemplate>();
    t->file = file;
  };
  std::string line;
  while (std::getline(f, line)) {
    auto k = card_tokens(line);
    if (k.empty()) continue;
    auto num = [&](size_t i) { return i < k.size() ? atof(k[i].c_str()) : 0.0; };
    if (k[0] == "card" && !t->makes.empty()) finish();
    if (k[0] == "card" && k.size() >= 3) { t->w = (int)num(1); t->h = (int)num(2); }
    else if (k[0] == "scale") t->scale = std::clamp(num(1), 0.25, 16.0);
    else if (k[0] == "image" && k.size() >= 2) {
      CardImage im;
      im.name = k[1];
      for (size_t i = 2; i + 1 < k.size(); i += 2) {
        double v = num(i + 1);
        if (k[i] == "angle") im.angle = v; else if (k[i] == "scale") im.scale = v;
        else if (k[i] == "x") im.x = v; else if (k[i] == "y") im.y = v;
        else if (k[i] == "w") im.w = v; else if (k[i] == "h") im.h = v;
      }
      t->images.push_back(im);
    } else if (k[0] == "text") {
      CardText tx;
      for (size_t i = 1; i < k.size(); ++i) {
        const std::string& a = k[i];
        auto next = [&]() { return i + 1 < k.size() ? k[++i] : std::string(); };
        if (a == "font") tx.font = next();
        else if (a == "size") tx.size = atof(next().c_str());
        else if (a == "x") tx.x = atof(next().c_str());
        else if (a == "y") tx.y = atof(next().c_str());
        else if (a == "spacing") tx.spacing = atof(next().c_str());
        else if (a == "maxwidth") tx.maxwidth = atof(next().c_str());
        else if (a == "bold") tx.bold = atof(next().c_str());
        else if (a == "color") card_color(next(), tx.color);
        else if (a == "outline") { tx.outline = atof(next().c_str()); }
        else if (a == "outline_color") card_color(next(), tx.ocolor);
        else if (a == "align") { std::string v = next(); tx.align = v == "center" || v == "centre" ? 1 : v == "right" ? 2 : 0; }
        else if (a == "upper") tx.caseconv = 1;
        else if (a == "lower") tx.caseconv = 2;
        else if (a == "string") tx.str = next();
        else if (a == "box") {
          tx.boxed = true;
          tx.bx = atof(next().c_str()); tx.by = atof(next().c_str());
          tx.bw = atof(next().c_str()); tx.bh = atof(next().c_str());
        }
      }
      t->texts.push_back(tx);
    } else if (k[0] == "make" && k.size() >= 2) {
      CardMake m;
      m.key = k[1];
      for (size_t i = 2; i < k.size(); ++i) m.texts.push_back(k[i]);
      t->makes.push_back(m);
    }
  }
  finish();
  return all;
}

// Font aliases: `name = font file [, fallback ...]` lines in Fonts.txt - the nearest one walking up from the template's
// folder to Custom, else Custom\UI\Fonts.txt, Custom\UI\UI text\Fonts.txt or Custom\UI text\Fonts.txt (older place).
// Paths may be relative to the Fonts.txt folder.
std::vector<std::string> resolve_font(const std::string& spec, const fs::path& tmpl_dir) {
  if (spec.empty() || spec[0] != '@') return {spec};
  const std::string want = lower(spec.substr(1));
  std::vector<fs::path> tries;
  const fs::path root = folder();
  for (fs::path d = tmpl_dir; !d.empty(); d = d.parent_path()) {
    tries.push_back(d / L"Fonts.txt");
    if (lower(d.string()) == lower(root.string()) || d == d.parent_path()) break;
  }
  tries.push_back(root / L"UI" / L"Fonts.txt");
  tries.push_back(root / L"UI" / L"UI text" / L"Fonts.txt");
  tries.push_back(root / L"UI text" / L"Fonts.txt");
  for (const fs::path& p : tries) {
    std::ifstream f(p);
    if (!f) continue;
    std::string line;
    while (std::getline(f, line)) {
      if (line.empty() || line[0] == '#') continue;
      size_t eq = line.find('=');
      if (eq == std::string::npos) continue;
      auto trim = [](std::string s) {
        size_t a = s.find_first_not_of(" \t\r\""), b = s.find_last_not_of(" \t\r\"");
        return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
      };
      if (lower(trim(line.substr(0, eq))) != want) continue;
      std::vector<std::string> files;
      std::stringstream ss(line.substr(eq + 1));
      for (std::string item; std::getline(ss, item, ',');) {
        std::string s = trim(item);
        if (s.empty()) continue;
        fs::path fp = fs::u8path(s);
        if (fp.is_relative()) fp = p.parent_path() / fp;
        auto u8 = fp.u8string();   // C++20: std::u8string; card_font reopens it with fs::u8path
        files.push_back(std::string((const char*)u8.data(), u8.size()));
      }
      return files;
    }
  }
  logf("[textures] card template: font alias %s not found in any Fonts.txt\n", spec.c_str());
  return {};
}

// image layer source: a file in Custom (by stem, or containing the key), then textures\Original, then next to the
// template; the original size comes from the key ("tex1_64x128_...") or is the file's size
void resolve_card_image(CardImage& im, const fs::path& tmpl_dir, const std::vector<fs::path>& custom_files) {
  std::string want = lower(im.name);
  std::string key;
  {
    size_t a = want.find("tex1_");
    if (a != std::string::npos) {
      key = want.substr(a);
      size_t e = key.find_first_of("]. ");
      if (e != std::string::npos) key = key.substr(0, e);
      int w = 0, h = 0;
      if (sscanf(key.c_str(), "tex1_%dx%d", &w, &h) == 2) { im.base_w = w; im.base_h = h; }
    }
  }
  auto matches = [&](const fs::path& p) {
    std::string stem = lower(p.stem().string());
    return stem == want || (!key.empty() && stem.find(key) != std::string::npos);
  };
  // several matches in Custom (the generated art + an improved copy dropped in later): the biggest picture wins
  auto png_pixels = [](const fs::path& p) -> uint64_t {
    uint8_t h[24] = {};
    FILE* f = _wfopen(p.c_str(), L"rb");
    if (!f) return 0;
    size_t n = fread(h, 1, sizeof h, f);
    fclose(f);
    if (n < 24) return 0;
    uint32_t w = (h[16] << 24) | (h[17] << 16) | (h[18] << 8) | h[19], ht = (h[20] << 24) | (h[21] << 16) | (h[22] << 8) | h[23];
    return (uint64_t)w * ht;
  };
  fs::path found;
  uint64_t best = 0;
  for (const auto& p : custom_files)
    if (matches(p) && lower(p.extension().string()) == ".png") {
      uint64_t px = png_pixels(p);
      if (found.empty() || px > best) { found = p; best = px; }
    }
  if (found.empty()) {
    std::error_code ec;
    for (const fs::path& dir : {tmpl_dir, exe_dir() / L"textures" / L"Original"}) {
      for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
           !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec) && lower(it->path().extension().string()) == ".png" && matches(it->path())) {
          found = it->path();
          break;
        }
      if (!found.empty()) break;
    }
  }
  if (found.empty()) { logf("[textures] card template: image \"%s\" not found\n", im.name.c_str()); return; }
  auto img = std::make_shared<MasterImage>();
  if (!decode_png(found, *img)) return;
  if (!im.base_w) { im.base_w = img->w; im.base_h = img->h; }
  im.img = img;
}

std::shared_ptr<std::vector<uint8_t>> card_font(const std::string& path) {
  auto it = g_fonts.find(path);
  if (it != g_fonts.end()) return it->second;
  std::ifstream f(fs::u8path(path), std::ios::binary);
  if (!f) { logf("[textures] card template: font %s not found\n", path.c_str()); return g_fonts[path] = nullptr; }
  auto v = std::make_shared<std::vector<uint8_t>>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  return g_fonts[path] = v;
}

void blend_px(uint8_t* d, const uint8_t* c, double cov) {   // "over" with coverage
  double a = c[3] / 255.0 * std::clamp(cov, 0.0, 1.0);
  if (a <= 0) return;
  double da = d[3] / 255.0, oa = a + da * (1 - a);
  for (int i = 0; i < 3; ++i) d[i] = (uint8_t)std::lround(oa > 0 ? (c[i] * a + d[i] * da * (1 - a)) / oa : 0);
  d[3] = (uint8_t)std::lround(oa * 255);
}

void draw_card_image(std::vector<uint8_t>& px, int W, int H, double S, const CardImage& im) {
  if (!im.img) return;
  const MasterImage& src = *im.img;
  // like PIL rotate(angle, expand) then resize by `scale` (or to w x h), top-left at (x, y); original-pixel units
  const double rad = im.angle * 3.14159265358979 / 180.0, c = cos(rad), s = sin(rad);
  const double bw = im.base_w, bh = im.base_h;
  const double rw = fabs(bw * c) + fabs(bh * s), rh = fabs(bw * s) + fabs(bh * c);   // rotated box, original px
  const double sx = im.w > 0 ? im.w / rw : im.scale, sy = im.h > 0 ? im.h / rh : im.scale;
  const double ow = rw * sx, oh = rh * sy;
  const int X0 = std::max(0, (int)floor(im.x * S)), X1 = std::min(W, (int)ceil((im.x + ow) * S));
  const int Y0 = std::max(0, (int)floor(im.y * S)), Y1 = std::min(H, (int)ceil((im.y + oh) * S));
  for (int Y = Y0; Y < Y1; ++Y)
    for (int X = X0; X < X1; ++X) {
      // output pixel -> rotated-box coords (original px) -> unrotated image coords (PIL rotates counter-clockwise)
      double u = ((X + 0.5) / S - im.x) / sx - rw / 2, v = ((Y + 0.5) / S - im.y) / sy - rh / 2;
      double ix = c * u - s * v + bw / 2, iy = s * u + c * v + bh / 2;
      if (ix < 0 || iy < 0 || ix >= bw || iy >= bh) continue;
      double fx = ix / bw * src.w - 0.5, fy = iy / bh * src.h - 0.5;   // bilinear in the source
      int x0 = (int)floor(fx), y0 = (int)floor(fy);
      double tx = fx - x0, ty = fy - y0;
      double acc[4] = {0, 0, 0, 0};
      for (int j = 0; j < 2; ++j)
        for (int i = 0; i < 2; ++i) {
          int xx = std::clamp(x0 + i, 0, src.w - 1), yy = std::clamp(y0 + j, 0, src.h - 1);
          double wgt = (i ? tx : 1 - tx) * (j ? ty : 1 - ty);
          const uint8_t* p = &src.rgba[((size_t)yy * src.w + xx) * 4];
          double pa = p[3] / 255.0;
          for (int k = 0; k < 3; ++k) acc[k] += p[k] * pa * wgt;   // premultiplied
          acc[3] += pa * wgt;
        }
      if (acc[3] <= 0) continue;
      uint8_t col[4] = {(uint8_t)std::lround(acc[0] / acc[3]), (uint8_t)std::lround(acc[1] / acc[3]),
                        (uint8_t)std::lround(acc[2] / acc[3]), 255};
      blend_px(&px[((size_t)Y * W + X) * 4], col, acc[3]);
    }
}

std::vector<float> dilate_cov(const std::vector<float>& in, int CW, int CH, double r) {   // max filter, disc radius r
  if (r <= 0) return in;
  std::vector<float> out(in.size(), 0.f);
  int R = (int)ceil(r);
  for (int y = 0; y < CH; ++y)
    for (int x = 0; x < CW; ++x) {
      float m = 0;
      for (int dy = -R; dy <= R; ++dy)
        for (int dx = -R; dx <= R; ++dx) {
          double d = sqrt(double(dx * dx + dy * dy));
          if (d > r + 0.5) continue;
          int xx = x + dx, yy = y + dy;
          if (xx < 0 || yy < 0 || xx >= CW || yy >= CH) continue;
          m = std::max(m, in[(size_t)yy * CW + xx] * (float)std::clamp(r + 0.5 - d, 0.0, 1.0));
        }
      out[(size_t)y * CW + x] = m;
    }
  return out;
}

std::vector<uint32_t> utf8_codepoints(const std::string& s) {
  std::vector<uint32_t> out;
  for (size_t i = 0; i < s.size();) {
    unsigned char c = s[i];
    int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
    uint32_t cp = n == 1 ? c : c & (0x7F >> n);
    for (int k = 1; k < n && i + k < s.size(); ++k) cp = (cp << 6) | (s[i + k] & 0x3F);
    out.push_back(cp);
    i += n;
  }
  return out;
}

// Box text (`box x y w h`, tools/ui_text.py): the lines ("\n") are laid out in font units, and the whole block's ink
// is scaled to the box's height (minus outline / bold) and, if wider, squeezed to its width; centred in the box, lines
// centred on each other. Each character comes from the first font in tx.fontfiles that has it.
void draw_box_text(std::vector<uint8_t>& px, int W, int H, double S, const CardText& tx, const std::string& text) {
  struct F { std::shared_ptr<std::vector<uint8_t>> data; stbtt_fontinfo fi; float em; };
  std::vector<F> fonts;
  for (const std::string& file : tx.fontfiles) {
    F f;
    f.data = card_font(file);
    if (!f.data || !stbtt_InitFont(&f.fi, f.data->data(), stbtt_GetFontOffsetForIndex(f.data->data(), 0))) continue;
    f.em = stbtt_ScaleForMappingEmToPixels(&f.fi, 1.0f);   // em units per font unit
    fonts.push_back(f);
  }
  if (fonts.empty() || text.empty()) return;
  std::vector<std::string> lines;
  {
    std::string cur;
    for (size_t i = 0; i < text.size(); ++i) {
      if (text[i] == '\n' || (text[i] == '\\' && i + 1 < text.size() && text[i + 1] == 'n')) {
        lines.push_back(cur);
        cur.clear();
        if (text[i] == '\\') ++i;
      } else cur += text[i];
    }
    lines.push_back(cur);
  }
  struct G { int font, glyph; double x, base; };
  std::vector<G> glyphs;
  int a0, d0, g0;
  stbtt_GetFontVMetrics(&fonts[0].fi, &a0, &d0, &g0);
  const double line_adv = (a0 - d0 + g0) * fonts[0].em;
  double ix0 = 1e30, iy0 = 1e30, ix1 = -1e30, iy1 = -1e30;   // block ink box, em units, y down
  std::vector<std::pair<size_t, std::array<double, 2>>> line_spans;   // first glyph index, ink x range
  for (size_t li = 0; li < lines.size(); ++li) {
    double pen = 0, lx0 = 1e30, lx1 = -1e30;
    const double base = li * line_adv;
    size_t first = glyphs.size();
    int prev_font = -1, prev_glyph = 0;
    for (uint32_t cp : utf8_codepoints(lines[li])) {
      if (tx.caseconv == 1 && cp < 128) cp = toupper(cp);
      if (tx.caseconv == 2 && cp < 128) cp = tolower(cp);
      int fi = 0, gi = 0;
      for (; fi < (int)fonts.size(); ++fi)
        if ((gi = stbtt_FindGlyphIndex(&fonts[fi].fi, (int)cp)) != 0) break;
      if (fi == (int)fonts.size()) { fi = 0; gi = stbtt_FindGlyphIndex(&fonts[0].fi, (int)cp); }
      const F& f = fonts[fi];
      if (prev_font == fi) pen += stbtt_GetGlyphKernAdvance(&f.fi, prev_glyph, gi) * f.em;
      int bx0, by0, bx1, by1, adv, lsb;
      if (stbtt_GetGlyphBox(&f.fi, gi, &bx0, &by0, &bx1, &by1)) {
        lx0 = std::min(lx0, pen + bx0 * f.em);
        lx1 = std::max(lx1, pen + bx1 * f.em);
        iy0 = std::min(iy0, base - by1 * f.em);
        iy1 = std::max(iy1, base - by0 * f.em);
      }
      glyphs.push_back({fi, gi, pen, base});
      stbtt_GetGlyphHMetrics(&f.fi, gi, &adv, &lsb);
      pen += adv * f.em;
      prev_font = fi;
      prev_glyph = gi;
    }
    line_spans.push_back({first, {lx0, lx1}});
  }
  for (auto& [first, span] : line_spans)
    if (span[0] <= span[1]) { ix0 = std::min(ix0, span[0]); ix1 = std::max(ix1, span[1]); }
  if (ix0 > ix1 || iy0 > iy1) return;
  for (size_t li = 0; li < line_spans.size(); ++li) {   // centre each line on the widest
    auto [first, span] = line_spans[li];
    size_t last = li + 1 < line_spans.size() ? line_spans[li + 1].first : glyphs.size();
    if (span[0] > span[1]) continue;
    double shift = ((ix1 - ix0) - (span[1] - span[0])) / 2 - (span[0] - ix0);
    for (size_t g = first; g < last; ++g) glyphs[g].x += shift;
  }
  // output px the ink grows by on each side (outline + bold); never more than a fifth of the box, or small text vanishes
  const double grow = std::min((tx.outline + tx.bold) * S, 0.2 * std::min(tx.bw, tx.bh) * S);
  const double avail_w = std::max(1.0, tx.bw * S - 2 * grow), avail_h = std::max(1.0, tx.bh * S - 2 * grow);
  const double sy = avail_h / (iy1 - iy0), sx = std::min(sy, avail_w / (ix1 - ix0));
  const int pad = (int)ceil(grow) + 2;
  const int CX = (int)floor(tx.bx * S) - pad, CY = (int)floor(tx.by * S) - pad;
  const int CW = (int)ceil(tx.bw * S) + 2 * pad + 2, CH = (int)ceil(tx.bh * S) + 2 * pad + 2;
  const double ox = tx.bx * S + grow + (avail_w - (ix1 - ix0) * sx) / 2 - ix0 * sx - CX;   // em origin in canvas px
  const double oy = tx.by * S + grow - iy0 * sy - CY;
  std::vector<float> cov((size_t)CW * CH, 0.f);
  for (const G& g : glyphs) {
    const F& f = fonts[g.font];
    const double gx = ox + g.x * sx, gy = oy + g.base * sy;
    const int ix = (int)floor(gx), iy = (int)floor(gy);
    int bw, bh, xo, yo;
    unsigned char* bmp = stbtt_GetGlyphBitmapSubpixel(&f.fi, (float)(sx * f.em), (float)(sy * f.em),
                                                      (float)(gx - ix), (float)(gy - iy), g.glyph, &bw, &bh, &xo, &yo);
    if (!bmp) continue;
    for (int y = 0; y < bh; ++y)
      for (int x = 0; x < bw; ++x) {
        int cx = ix + xo + x, cy = iy + yo + y;
        if (cx >= 0 && cy >= 0 && cx < CW && cy < CH)
          cov[(size_t)cy * CW + cx] = std::max(cov[(size_t)cy * CW + cx], bmp[y * bw + x] / 255.f);
      }
    stbtt_FreeBitmap(bmp, nullptr);
  }
  std::vector<float> fill = dilate_cov(cov, CW, CH, tx.bold * S);
  std::vector<float> edge = dilate_cov(fill, CW, CH, tx.outline * S);
  for (int y = 0; y < CH; ++y)
    for (int x = 0; x < CW; ++x) {
      int X = CX + x, Y = CY + y;
      if (X < 0 || Y < 0 || X >= W || Y >= H) continue;
      uint8_t* d = &px[((size_t)Y * W + X) * 4];
      if (tx.outline > 0) blend_px(d, tx.ocolor, edge[(size_t)y * CW + x]);
      blend_px(d, tx.color, fill[(size_t)y * CW + x]);
    }
}

void draw_card_text(std::vector<uint8_t>& px, int W, int H, double S, const CardText& tx, std::string str) {
  if (tx.boxed) { draw_box_text(px, W, H, S, tx, str); return; }
  auto font = card_font(tx.fontfiles.empty() ? tx.font : tx.fontfiles[0]);
  if (!font || str.empty()) return;
  for (auto& ch : str) ch = (char)(tx.caseconv == 1 ? toupper((unsigned char)ch) : tx.caseconv == 2 ? tolower((unsigned char)ch) : ch);
  stbtt_fontinfo fi;
  if (!stbtt_InitFont(&fi, font->data(), stbtt_GetFontOffsetForIndex(font->data(), 0))) return;
  // size = cap height in original pixels
  int x0, y0, x1, y1;
  double em = stbtt_ScaleForPixelHeight(&fi, 100.0f);
  stbtt_GetCodepointBox(&fi, 'H', &x0, &y0, &x1, &y1);
  double cap100 = (y1 - y0) * em;
  double pxh = tx.size * S * 100.0 / std::max(1.0, cap100);   // pixel height giving that cap height
  float scale = stbtt_ScaleForPixelHeight(&fi, (float)pxh);
  // width -> horizontal squeeze for maxwidth
  std::vector<int> cps;
  for (unsigned char ch : str) cps.push_back(ch);
  double width = 0;
  for (size_t i = 0; i < cps.size(); ++i) {
    int adv, lsb;
    stbtt_GetCodepointHMetrics(&fi, cps[i], &adv, &lsb);
    width += adv * scale + tx.spacing * S;
    if (i + 1 < cps.size()) width += stbtt_GetCodepointKernAdvance(&fi, cps[i], cps[i + 1]) * scale;
  }
  double squeeze = tx.maxwidth > 0 && width > tx.maxwidth * S ? tx.maxwidth * S / width : 1.0;
  width *= squeeze;
  const int pad = (int)ceil((tx.outline + tx.bold) * S) + 2;
  const int CW = (int)ceil(width) + 2 * pad, CH = (int)ceil(tx.size * S * 1.6) + 2 * pad;
  std::vector<float> cov((size_t)CW * CH, 0.f);
  const int baseline = pad + (int)std::lround(tx.size * S);   // cap top at the canvas top + pad
  double pen = pad;
  for (size_t i = 0; i < cps.size(); ++i) {
    int gw, gh, gx, gy;
    unsigned char* bmp = stbtt_GetCodepointBitmap(&fi, (float)(scale * squeeze), scale, cps[i], &gw, &gh, &gx, &gy);
    for (int y = 0; y < gh; ++y)
      for (int x = 0; x < gw; ++x) {
        int cx = (int)std::lround(pen) + gx + x, cy = baseline + gy + y;
        if (cx >= 0 && cy >= 0 && cx < CW && cy < CH) cov[(size_t)cy * CW + cx] = std::max(cov[(size_t)cy * CW + cx], bmp[y * gw + x] / 255.f);
      }
    stbtt_FreeBitmap(bmp, nullptr);
    int adv, lsb;
    stbtt_GetCodepointHMetrics(&fi, cps[i], &adv, &lsb);
    pen += (adv * scale + tx.spacing * S) * squeeze;
    if (i + 1 < cps.size()) pen += stbtt_GetCodepointKernAdvance(&fi, cps[i], cps[i + 1]) * scale * squeeze;
  }
  auto dilate = [&](const std::vector<float>& in, double r) {   // max filter, disc of radius r (output px)
    if (r <= 0) return in;
    std::vector<float> out(in.size(), 0.f);
    int R = (int)ceil(r);
    for (int y = 0; y < CH; ++y)
      for (int x = 0; x < CW; ++x) {
        float m = 0;
        for (int dy = -R; dy <= R; ++dy)
          for (int dx = -R; dx <= R; ++dx) {
            double d = sqrt(double(dx * dx + dy * dy));
            if (d > r + 0.5) continue;
            int xx = x + dx, yy = y + dy;
            if (xx < 0 || yy < 0 || xx >= CW || yy >= CH) continue;
            float v = in[(size_t)yy * CW + xx] * (float)std::clamp(r + 0.5 - d, 0.0, 1.0);
            m = std::max(m, v);
          }
        out[(size_t)y * CW + x] = m;
      }
    return out;
  };
  std::vector<float> fill = dilate(cov, tx.bold * S);
  std::vector<float> edge = dilate(fill, tx.outline * S);
  // place: (x, y) = text top-left (cap top) in original px, x per alignment
  double ox = tx.x * S - pad - (tx.align == 1 ? width / 2 : tx.align == 2 ? width : 0);
  double oy = tx.y * S - pad;
  for (int y = 0; y < CH; ++y)
    for (int x = 0; x < CW; ++x) {
      int X = (int)std::lround(ox) + x, Y = (int)std::lround(oy) + y;
      if (X < 0 || Y < 0 || X >= W || Y >= H) continue;
      uint8_t* d = &px[((size_t)Y * W + X) * 4];
      if (tx.outline > 0) blend_px(d, tx.ocolor, edge[(size_t)y * CW + x]);
      blend_px(d, tx.color, fill[(size_t)y * CW + x]);
    }
}

bool compose_card(const CardTemplate& t, const CardMake& m, std::vector<uint8_t>& out) {
  const double S = t.scale;
  const int W = std::max(1, (int)std::lround(t.w * S)), H = std::max(1, (int)std::lround(t.h * S));
  std::vector<uint8_t> px((size_t)W * H * 4, 0);
  for (const CardImage& im : t.images) draw_card_image(px, W, H, S, im);
  for (size_t i = 0; i < t.texts.size(); ++i)
    draw_card_text(px, W, H, S, t.texts[i], i < m.texts.size() ? m.texts[i] : t.texts[i].str);
  return encode_png(px, W, H, out);
}

std::string key_of(const fs::path& p);

// ---- colour match (setting tex_match, live): a custom texture is pulled toward the colours of the ORIGINAL texture
// with the same key (textures\Original or Not needed; UI textures are left alone). Per channel, a smooth curve through
// the percentiles (0, 1, 10, 25, 50, 75, 90, 99, 100 %) of the replacement -> the same percentiles of the original, so
// the palette (darkest, typical, brightest values of each colour) matches but the new detail stays. 0 % = untouched.
// Done while Aurora reads the file; nothing is written to disk.
std::mutex g_orig_mutex;
std::unordered_map<std::string, fs::path> g_orig_index;
bool g_orig_indexed = false;

void index_originals() {   // under g_orig_mutex
  g_orig_indexed = true;
  for (const wchar_t* sub : {L"Original", L"Not needed"}) {
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(exe_dir() / L"textures" / sub, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
      if (!it->is_regular_file(ec)) continue;
      const fs::path& p = it->path();
      if (p.native().find(L"\\UI\\") != std::wstring::npos) continue;
      std::string k = key_of(p);
      if (!k.empty()) g_orig_index.emplace(k, p);
    }
  }
  logf("[textures] colour match: %zu original textures indexed\n", g_orig_index.size());
}

void channel_quantiles(const std::vector<uint8_t>& rgba, int ch, float* q) {
  static const double F[9] = {0, .01, .10, .25, .50, .75, .90, .99, 1};
  uint64_t hist[256] = {};
  uint64_t total = 0;
  for (size_t i = 0; i + 3 < rgba.size(); i += 4)
    if (rgba[i + 3] >= 128) { ++hist[rgba[i + ch]]; ++total; }
  uint64_t acc = 0;
  int v = 0;
  for (int k = 0; k < 9; ++k) {
    const uint64_t want = (uint64_t)(F[k] * (double)(total ? total - 1 : 0));
    while (v < 255 && acc + hist[v] <= want) acc += hist[v++];
    q[k] = (float)v;
  }
}

// false when there is nothing to match (no original, too few opaque pixels)
bool colour_match(const std::string& key, int percent, MasterImage& img) {
  fs::path orig;
  {
    std::lock_guard lk(g_orig_mutex);
    if (!g_orig_indexed) index_originals();
    auto it = g_orig_index.find(key);
    if (it == g_orig_index.end()) return false;
    orig = it->second;
  }
  MasterImage o;
  if (!decode_png(orig, o) || o.w <= 0 || o.h <= 0 || img.w <= 0) return false;
  const float s = std::clamp(percent, 0, 100) / 100.0f;
  for (int ch = 0; ch < 3; ++ch) {
    float qr[9], qo[9];
    channel_quantiles(img.rgba, ch, qr);
    channel_quantiles(o.rgba, ch, qo);
    // strictly increasing source points only
    float sx[9], dx[9];
    int n = 0;
    for (int k = 0; k < 9; ++k)
      if (n == 0 || qr[k] > sx[n - 1] + 0.5f) { sx[n] = qr[k]; dx[n] = qo[k]; ++n; }
    if (n < 2) continue;   // a flat colour: nothing to curve
    uint8_t lut[256];
    for (int v = 0; v < 256; ++v) {
      float m;
      if (v <= sx[0]) m = (float)v + (dx[0] - sx[0]);
      else if (v >= sx[n - 1]) m = (float)v + (dx[n - 1] - sx[n - 1]);
      else {
        int k = 1;
        while (k < n - 1 && v > sx[k]) ++k;
        m = dx[k - 1] + (dx[k] - dx[k - 1]) * ((float)v - sx[k - 1]) / (sx[k] - sx[k - 1]);
      }
      lut[v] = (uint8_t)std::clamp((int)std::lround((float)v + s * (m - (float)v)), 0, 255);
    }
    for (size_t i = (size_t)ch; i < img.rgba.size(); i += 4) img.rgba[i] = lut[img.rgba[i]];
  }
  return true;
}

bool read_file(void*, const char* path, std::vector<uint8_t>& out) {
  fs::path real;
  {
    std::unique_lock lk(g_mutex);
    auto ct = g_card_built.find(path);
    if (ct != g_card_built.end()) {
      auto [tmpl, idx] = ct->second;
      lk.unlock();
      return compose_card(*tmpl, tmpl->makes[idx], out);
    }
    auto bt = g_built.find(path);
    if (bt != g_built.end()) {
      auto [img, sheet] = bt->second;   // the image stays alive through the shared_ptr while composing
      lk.unlock();
      return compose(*img, *sheet, out);
    }
    auto it = g_files.find(path);
    if (it == g_files.end()) return false;   // includes Aurora's "_mipN" probes for files we do not have
    real = it->second;
  }
  if (const int pct = settings::g.tex_match; pct > 0) {
    std::string name = path;
    const size_t slash = name.rfind('/');
    if (slash != std::string::npos) name = name.substr(slash + 1);
    // (the virtual path ends in key_of() of the file: "tex1_<...>.png", the same form the original index uses)
    if (name.rfind("tex1_", 0) == 0 && lower(real.extension().string()) == ".png") {
      MasterImage img;
      if (decode_png(real, img) && colour_match(name, pct, img) && encode_png(img.rgba, img.w, img.h, out)) return true;
    }
  }
  FILE* f = _wfopen(real.c_str(), L"rb");
  if (!f) return false;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  out.resize(n > 0 ? (size_t)n : 0);
  bool ok = n > 0 && fread(out.data(), 1, out.size(), f) == out.size();
  fclose(f);
  return ok;
}

// "<anything> [tex1_...].png" or "tex1_....png" -> "tex1_....png"
std::string key_of(const fs::path& p) {
  // The app saves beside the destination then atomically renames; never register the temporary image.
  if (p.filename().string().ends_with(".tmp.png")) return {};
  std::string ext = p.extension().string();
  for (auto& ch : ext) ch = (char)tolower(ch);
  if (ext != ".png" && ext != ".dds") return {};
  std::string stem = p.stem().string();
  size_t a = stem.rfind("[tex1_");
  if (a != std::string::npos) {
    size_t b = stem.find(']', a);
    if (b == std::string::npos) return {};
    return stem.substr(a + 1, b - a - 1) + ext;
  }
  return stem.rfind("tex1_", 0) == 0 ? stem + ext : std::string{};
}

// cheap change detector: names, sizes and modification times of everything in the folder
uint64_t signature(const fs::path& root) {
  uint64_t h = 1469598103934665603ull;
  std::error_code ec;
  if (!fs::exists(root, ec)) return 0;
  for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
       !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    auto mix = [&](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
    for (wchar_t c : it->path().native()) mix(c);
    mix(it->file_size(ec));
    mix((uint64_t)it->last_write_time(ec).time_since_epoch().count());
  }
  return h;
}

struct LooseFile { fs::path path; std::string key; uint64_t stamp; };
struct PackScan {
  std::unordered_map<std::wstring, LooseFile> loose;
  uint64_t signature = 1469598103934665603ull, complex = 1469598103934665603ull;
};
struct LooseReg { std::string virtual_path; aurora::texture::ReplacementRegistration reg; uint64_t stamp; };
std::unordered_map<std::wstring, LooseReg> g_loose;
uint64_t g_complex_sig = 0, g_live_id = 0;
std::mutex g_scan_mutex;
std::optional<PackScan> g_pending_scan, g_reload_scan;

uint64_t file_stamp(const fs::path& p) {
  std::error_code ec;
  auto size = fs::file_size(p, ec);
  auto time = fs::last_write_time(p, ec).time_since_epoch().count();
  return ec ? 0 : size ^ ((uint64_t)time * 1099511628211ull);
}
PackScan scan_pack() {
  PackScan scan;
  std::error_code ec;
  std::vector<std::pair<fs::path, uint64_t>> other;
  bool cards = false;
  for (auto it = fs::recursive_directory_iterator(folder(), fs::directory_options::skip_permission_denied, ec);
       !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (!it->is_regular_file(ec) || it->path().filename().string().ends_with(".tmp.png")) continue;
    const fs::path p = it->path();
    const auto stamp = file_stamp(p);
    for (wchar_t c : p.native()) scan.signature = (scan.signature ^ c) * 1099511628211ull;
    scan.signature = (scan.signature ^ stamp) * 1099511628211ull;
    const std::string key = key_of(p);
    if (!key.empty()) scan.loose.emplace(p.native(), LooseFile{p, key, stamp});
    else other.push_back({p, stamp});
    cards |= lower(p.filename().string()).ends_with(".cards.txt");
  }
  for (auto& [p, stamp] : other) {
    bool dependency = cards;
    for (const MasterLayout& m : g_layout_masters)
      dependency |= lower(p.stem().string()) == lower(m.name);
    if (!dependency) continue;  // split-group sources and PC overlay assets are not texture registrations
    for (wchar_t c : p.native()) scan.complex = (scan.complex ^ c) * 1099511628211ull;
    scan.complex = (scan.complex ^ stamp) * 1099511628211ull;
  }
  return scan;
}

void unload() {
  aurora::texture::unregister_replacements(g_regs);   // waits for in-flight reads
  g_regs.clear();
  g_loose.clear();
  g_reload_scan.reset();
  std::lock_guard lk(g_mutex);
  g_files.clear();
  g_built.clear();
  g_card_built.clear();
  g_cards.clear();
  g_count = 0;
  g_built_count = 0;
}

void load() {
  unload();
  fs::path root = folder();
  std::error_code ec;
  fs::create_directories(root, ec);
  read_layout();
  std::vector<std::pair<std::string, fs::path>> found;
  std::unordered_map<std::string, fs::path> masters;   // lower-case master name -> file
  std::vector<fs::path> card_files, custom_pngs;
  for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
       !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    {
      std::string fname = lower(it->path().filename().string());
      if (fname.size() > 10 && fname.compare(fname.size() - 10, 10, ".cards.txt") == 0) card_files.push_back(it->path());
    }
    if (lower(it->path().extension().string()) == ".png") {
      custom_pngs.push_back(it->path());
      std::string stem = lower(it->path().stem().string());
      for (const MasterLayout& m : g_layout_masters)
        if (stem == lower(m.name)) masters[stem] = it->path();
    }
    std::string key = key_of(it->path());
    if (key.empty()) continue;
    // unique virtual path per file; its last component is the key Aurora parses
    found.push_back({"custom/" + std::to_string(found.size()) + "/" + key, it->path()});
  }
  {
    std::lock_guard lk(g_mutex);
    for (auto& [v, p] : found) g_files[v] = p;
  }
  aurora::texture::VirtualFileSource src{read_file, nullptr};
  for (auto& [v, p] : found) {
    auto r = aurora::texture::register_virtual_replacement(v, src);
    if (r.id) {
      g_regs.push_back(r);
      g_loose.emplace(p.native(), LooseReg{v, r, file_stamp(p)});
    }
  }
  // UI masters: every texture built from a master that is present
  for (size_t mi = 0; mi < g_layout_masters.size(); ++mi) {
    const MasterLayout& L = g_layout_masters[mi];
    auto mf = masters.find(lower(L.name));
    if (mf == masters.end()) continue;
    auto img = std::make_shared<MasterImage>();
    img->layout = &L;
    if (!decode_png(mf->second, *img) || img->w <= 0 || img->h <= 0) {
      logf("[textures] could not read %s\n", mf->second.string().c_str());
      continue;
    }
    restore_alpha(*img);
    int n = 0;
    for (const SheetLayout& sh : g_layout_sheets) {
      if (sh.master != (int)mi) continue;
      std::string v = "master/" + std::to_string(g_built_count + n) + "/" + sh.key + ".png";
      {
        std::lock_guard lk(g_mutex);
        g_built[v] = {img, &sh};
      }
      auto r = aurora::texture::register_virtual_replacement(v, src, {.priority = 1});
      if (r.id) { g_regs.push_back(r); ++n; }
    }
    g_built_count += n;
    logf("[textures] %s (%dx%d = %.2gx): %d textures built from it\n", L.name.c_str(), img->w, img->h,
         (double)img->w / L.w, n);
  }
  // card templates: every texture a template makes (priority 1, like masters)
  g_fonts.clear();
  for (const fs::path& cf : card_files) {
    auto all = parse_cards(cf);
    if (all.empty()) { logf("[textures] %s: no card / make lines\n", cf.string().c_str()); continue; }
    int file_n = 0;
    for (auto& t : all) {
    for (CardImage& im : t->images) resolve_card_image(im, cf.parent_path(), custom_pngs);
    for (CardText& tx : t->texts) tx.fontfiles = resolve_font(tx.font, cf.parent_path());
    int n = 0;
    for (size_t i = 0; i < t->makes.size(); ++i) {
      std::string v = "card/" + std::to_string(g_cards.size()) + "/" + std::to_string(i) + "/" + t->makes[i].key + ".png";
      {
        std::lock_guard lk(g_mutex);
        g_card_built[v] = {t, i};
      }
      auto r = aurora::texture::register_virtual_replacement(v, src, {.priority = 1});
      if (r.id) { g_regs.push_back(r); ++n; }
    }
    file_n += n;
    if (const char* dump = getenv("CARD_DUMP")) {   // testing / preview: every card as a PNG in that folder
      std::error_code ec2;
      fs::create_directories(fs::u8path(dump), ec2);
      for (const CardMake& m : t->makes) {
        std::vector<uint8_t> png;
        if (!compose_card(*t, m, png)) continue;
        std::ofstream o(fs::u8path(dump) / fs::u8path(m.key + ".png"), std::ios::binary);
        o.write((const char*)png.data(), (std::streamsize)png.size());
      }
    }
    g_cards.push_back(t);
    }
    logf("[textures] %s: %d textures made from it (%zu templates)\n", cf.filename().string().c_str(), file_n, all.size());
  }
  g_count = (int)g_regs.size();
  g_complex_sig = scan_pack().complex;
  logf("[textures] %d custom texture(s) from %s\n", g_count, root.string().c_str());
}

// Ordinary swaps keep every unchanged registration and GPU texture. At most two file changes are
// applied per frame; Aurora reads/decodes the new PNG on its streaming workers. Previously one road
// edit unregistered the whole pack, decoded the font masters again and queued hundreds of textures.
bool reload_loose(const PackScan& scan) {
  int changed = 0;
  for (auto it = g_loose.begin(); it != g_loose.end() && changed < 2;) {
    const auto next = scan.loose.find(it->first);
    if (next != scan.loose.end() && next->second.stamp == it->second.stamp) { ++it; continue; }
    const auto reg = it->second.reg;
    aurora::texture::unregister_replacement(reg);
    {
      std::lock_guard lk(g_mutex);
      g_files.erase(it->second.virtual_path);
    }
    std::erase_if(g_regs, [&](const auto& r) { return r.id == reg.id; });
    it = g_loose.erase(it);
    ++changed;
  }
  aurora::texture::VirtualFileSource src{read_file, nullptr};
  for (auto& [name, file] : scan.loose) {
    if (g_loose.contains(name)) continue;
    if (changed >= 2) break;
    std::string v = "custom/live/" + std::to_string(++g_live_id) + "/" + file.key;
    {
      std::lock_guard lk(g_mutex);
      g_files[v] = file.path;
    }
    auto reg = aurora::texture::register_virtual_replacement(v, src);
    if (reg.id) g_regs.push_back(reg);
    g_loose.emplace(name, LooseReg{v, reg, file.stamp});
    ++changed;
  }
  g_count = (int)g_regs.size();
  for (auto& [name, old] : g_loose) {
    auto next = scan.loose.find(name);
    if (next == scan.loose.end() || next->second.stamp != old.stamp) return false;
  }
  return g_loose.size() == scan.loose.size();
}
}  // namespace

// The folder is watched on a background thread (a big folder takes a moment to walk; the game thread only reloads).
std::atomic<bool> g_watch_on{false}, g_changed{false};
std::atomic<uint64_t> g_watch_sig{0};
void watcher() {
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(2));
    if (!g_watch_on) continue;
    auto scan = scan_pack();
    if (scan.signature != g_watch_sig.exchange(scan.signature)) {
      std::lock_guard lk(g_scan_mutex);
      g_pending_scan = std::move(scan);
      g_changed = true;
    }
  }
}

// textures\Custom textures OFF.txt (made / deleted by the AI Retexture app's "Game textures" switch): while it exists
// the custom textures are off, without touching the user's setting. Checked every second on its own thread.
std::atomic<bool> g_off_file{false};
void off_file_watcher() {
  for (;;) {
    std::error_code ec;
    g_off_file = fs::exists(folder().parent_path() / L"Custom textures OFF.txt", ec);
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}

// Once per frame (frame_rate.cpp): follows the setting and the folder contents.
void tick() {
  static bool thread_started = false, off_thread_started = false;
  static double reload_ms = 0;
  static uint32_t reload_frames = 0;
  if (!off_thread_started) {
    off_thread_started = true;
    std::error_code ec;
    g_off_file = fs::exists(folder().parent_path() / L"Custom textures OFF.txt", ec);
    std::thread(off_file_watcher).detach();
  }
  bool want = settings::g.custom_textures && !g_off_file;
  if (want != g_active) {
    g_active = want;
    if (want) {
      load();
      g_watch_sig = scan_pack().signature;
      g_changed = false;
      if (!thread_started) { thread_started = true; std::thread(watcher).detach(); }
    } else {
      unload();
    }
    g_watch_on = want;
    return;
  }
  {   // the colour-match setting changed (PC menu): every custom texture is read again with / without it
    static int last_match = settings::g.tex_match;
    if (settings::g.tex_match != last_match) {
      last_match = settings::g.tex_match;
      if (want) {
        {
          std::lock_guard lk(g_orig_mutex);
          g_orig_indexed = false;   // also picks up new originals
          g_orig_index.clear();
        }
        load();
        g_watch_sig = scan_pack().signature;
        g_changed = false;
        return;
      }
    }
  }
  if (want && g_changed.exchange(false)) {
    std::lock_guard lk(g_scan_mutex);
    if (g_pending_scan) {
      g_reload_scan = std::move(g_pending_scan);
      g_pending_scan.reset();
      reload_ms = 0;
      reload_frames = 0;
    }
  }
  if (want && g_reload_scan) {
    if (g_reload_scan->complex != g_complex_sig) load();  // actual master/template changes still rebuild the derived sheets
    else {
      const auto start = std::chrono::steady_clock::now();
      const bool done = reload_loose(*g_reload_scan);
      reload_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
      ++reload_frames;
      if (done) {
        if (getenv("TEXTURE_RELOAD_STATS"))
          logf("[textures] incremental reload: %.2f ms game-thread work across %u frame(s); unchanged textures retained\n", reload_ms, reload_frames);
        g_reload_scan.reset();
      }
    }
  }
}

int count() { return g_active ? g_count : 0; }
std::filesystem::path textures_dir() { return exe_dir() / L"textures"; }
std::filesystem::path ui_dir(const wchar_t* name) {
  // Custom\UI\<name>, Custom\<name>, else any Custom\<folder>\<name> (the user's own grouping, e.g. Custom\Shared)
  const fs::path custom = folder(), ui = custom / L"UI" / name;   // the chosen custom textures folder
  std::error_code ec;
  if (fs::is_directory(ui, ec)) return ui;
  if (fs::is_directory(custom / name, ec)) return custom / name;
  for (auto it = fs::directory_iterator(custom, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
    if (it->is_directory(ec) && fs::is_directory(it->path() / name, ec)) return it->path() / name;
  return ui;
}
bool read_png(const std::filesystem::path& file, int& w, int& h, std::vector<uint8_t>& rgba) {
  MasterImage m;
  if (!decode_png(file, m)) return false;
  w = m.w; h = m.h; rgba = std::move(m.rgba);
  return true;
}
bool write_png(const std::vector<uint8_t>& rgba, int w, int h, std::vector<uint8_t>& png) { return encode_png(rgba, w, h, png); }
std::string folder_path() { return folder().string(); }
}  // namespace texture_pack
