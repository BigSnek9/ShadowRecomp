// #143 "Extract the game's textures" (in-game menu, System page): every texture on the disc -> PNG under
// <game>\textures\Original, in tidy folders, named so an edited copy dropped into textures\Custom replaces it. A C++ port
// of tools/texture_dump.py (same folders, names and keys): RenderWare texture dictionaries (.txd, GameCube native,
// platform 6) loose and inside "One" archives (.one, PRS-compressed), plus textures embedded in UI layouts (.gncp).
// Name: "<texture name> [tex1_<w>x<h>_<texhash>[_<tluthash>]_<fmt>].png" - the bracket part is the key Aurora computes
// at run time (Dolphin convention: XXH64 of the base level; palette textures also XXH64 of the palette entries used).
// Runs on its own thread; the menu shows its progress. ensure_folders() makes textures\Custom and textures\Original
// on start (with a short README) so players find where edits go.
#include "texture_extract.h"
#include "sys.h"
#include "texture_pack.h"
#include <xxhash.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

namespace fs = std::filesystem;
namespace texture_extract {
namespace {
std::atomic<int> g_state{0};          // 0 idle, 1 scanning, 2 writing, 3 done, 4 failed
std::atomic<int> g_found{0}, g_written{0};
std::mutex g_msg_mutex;
std::string g_msg;

fs::path exe_dir() {
  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  return fs::path(exe).parent_path();
}
uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }
uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
uint16_t be16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }

// ---- PRS (Sega LZ)
std::vector<uint8_t> prs_decompress(const std::vector<uint8_t>& src, size_t start) {
  std::vector<uint8_t> out;
  size_t i = start;
  const size_t n = src.size();
  uint32_t ctrl = 0;
  int bits = 0;
  auto bit = [&]() -> int {
    if (bits == 0) { if (i >= n) throw 0; ctrl = src[i++]; bits = 8; }
    const int b = ctrl & 1;
    ctrl >>= 1;
    --bits;
    return b;
  };
  while (i < n) {
    if (bit()) { out.push_back(src[i++]); continue; }
    long off, size;
    if (bit()) {
      if (i + 1 >= n) break;
      const uint32_t v = src[i] | (src[i + 1] << 8);
      i += 2;
      if (v == 0) break;
      off = long(v >> 3) - 0x2000;
      size = v & 7;
      if (size == 0) { if (i >= n) break; size = src[i++] + 1; }
      else size += 2;
    } else {
      size = ((bit() << 1) | bit()) + 2;
      if (i >= n) break;
      off = long(src[i++]) - 0x100;
    }
    const long p = long(out.size()) + off;
    if (p < 0) throw 0;
    for (long k = 0; k < size; ++k) out.push_back(out[size_t(p + k)]);
  }
  return out;
}

// ---- RW texture natives
struct Block { int bw, bh, bpp; };
bool block_of(int fmt, Block& b) {
  switch (fmt) {
    case 0: case 8: case 14: b = {8, 8, 4}; return true;
    case 1: case 2: case 9: b = {8, 4, 8}; return true;
    case 3: case 4: case 5: case 10: b = {4, 4, 16}; return true;
    case 6: b = {4, 4, 32}; return true;
    default: return false;
  }
}
size_t level_size(int fmt, int w, int h) {
  Block b{};
  block_of(fmt, b);
  return size_t((w + b.bw - 1) / b.bw) * ((h + b.bh - 1) / b.bh) * b.bw * b.bh * b.bpp / 8;
}
struct Tex { std::string name; int fmt, w, h, tlfmt; std::vector<uint8_t> data, pal; };
bool native_texture(const uint8_t* s, size_t len, Tex& t) {
  if (len < 0x6C || be32(s) != 6) return false;
  t.name.assign(reinterpret_cast<const char*>(s + 0x18), strnlen(reinterpret_cast<const char*>(s + 0x18), 0x20));
  t.w = be16(s + 0x5C); t.h = be16(s + 0x5E);
  t.fmt = s[0x62]; t.tlfmt = s[0x63];
  Block b{};
  if (!block_of(t.fmt, b) || t.w == 0 || t.h == 0 || t.w > 1024 || t.h > 1024) return false;
  size_t p = 0x68;
  t.pal.clear();
  if (t.fmt == 8 || t.fmt == 9) {
    const size_t n = t.fmt == 8 ? 16 : 256;
    if (p + n * 2 > len) return false;
    t.pal.assign(s + p, s + p + n * 2);
    p += n * 2;
  }
  p += 4;
  const size_t base = level_size(t.fmt, t.w, t.h);
  if (p + base > len) return false;
  t.data.assign(s + p, s + p + base);
  return true;
}
void natives_in(const std::vector<uint8_t>& d, std::vector<Tex>& out) {
  static const uint8_t sig[4] = {0x15, 0, 0, 0};
  auto find = [&](size_t from) -> size_t {
    for (size_t k = from; k + 4 <= d.size(); ++k) if (!memcmp(&d[k], sig, 4)) return k;
    return std::string::npos;
  };
  size_t i = find(0);
  while (i != std::string::npos) {
    if (i + 24 <= d.size() && !memcmp(&d[i + 8], &d[i + 20], 4) && le32(&d[i + 12]) == 1) {
      const uint32_t sz = le32(&d[i + 4]), ssz = le32(&d[i + 16]);
      if (ssz <= sz && i + 24 + ssz <= d.size()) {
        Tex t;
        if (native_texture(&d[i + 24], ssz, t)) { out.push_back(std::move(t)); i = find(i + 24 + ssz); continue; }
      }
    }
    i = find(i + 1);
  }
}

// ---- GX decoding to RGBA8
void c565(uint32_t v, uint8_t* o) { o[0] = uint8_t(((v >> 11) & 31) * 255 / 31); o[1] = uint8_t(((v >> 5) & 63) * 255 / 63); o[2] = uint8_t((v & 31) * 255 / 31); o[3] = 255; }
void c5a3(uint32_t v, uint8_t* o) {
  if (v & 0x8000) { o[0] = uint8_t(((v >> 10) & 31) * 255 / 31); o[1] = uint8_t(((v >> 5) & 31) * 255 / 31); o[2] = uint8_t((v & 31) * 255 / 31); o[3] = 255; }
  else { o[0] = uint8_t(((v >> 8) & 15) * 17); o[1] = uint8_t(((v >> 4) & 15) * 17); o[2] = uint8_t((v & 15) * 17); o[3] = uint8_t(((v >> 12) & 7) * 255 / 7); }
}
void cia8(uint32_t v, uint8_t* o) { const uint8_t i = uint8_t(v & 0xFF); o[0] = o[1] = o[2] = i; o[3] = uint8_t((v >> 8) & 0xFF); }
void pal_rgba(const Tex& t, int idx, uint8_t* o) {
  const uint32_t v = size_t(idx) * 2 + 1 < t.pal.size() ? be16(&t.pal[size_t(idx) * 2]) : 0;
  if (t.tlfmt == 0) cia8(v, o); else if (t.tlfmt == 1) c565(v, o); else c5a3(v, o);
}
bool decode(const Tex& t, std::vector<uint8_t>& rgba) {
  const int w = t.w, h = t.h;
  Block b{};
  if (!block_of(t.fmt, b) || t.fmt == 10) return false;
  rgba.assign(size_t(w) * h * 4, 0);
  const int bx = (w + b.bw - 1) / b.bw, by = (h + b.bh - 1) / b.bh;
  const uint8_t* d = t.data.data();
  auto put = [&](int x, int y, const uint8_t* px) { if (x < w && y < h) memcpy(&rgba[(size_t(y) * w + x) * 4], px, 4); };
  if (t.fmt == 14) {   // CMPR: 8x8 blocks of four DXT1 4x4 sub-blocks (TL, TR, BL, BR)
    size_t o = 0;
    for (int yb = 0; yb < by; ++yb)
      for (int xb = 0; xb < bx; ++xb)
        for (int s = 0; s < 4; ++s, o += 8) {
          const uint32_t c0 = be16(d + o), c1 = be16(d + o + 2);
          uint8_t p[4][4];
          c565(c0, p[0]); c565(c1, p[1]);
          for (int k = 0; k < 4; ++k) {
            if (c0 > c1) { p[2][k] = uint8_t((2 * p[0][k] + p[1][k]) / 3); p[3][k] = uint8_t((p[0][k] + 2 * p[1][k]) / 3); }
            else { p[2][k] = uint8_t((p[0][k] + p[1][k]) / 2); p[3][k] = 0; }
          }
          p[2][3] = 255;
          p[3][3] = c0 > c1 ? 255 : 0;
          for (int y = 0; y < 4; ++y) {
            const uint8_t row = d[o + 4 + y];
            for (int x = 0; x < 4; ++x) put(xb * 8 + (s & 1) * 4 + x, yb * 8 + (s >> 1) * 4 + y, p[(row >> (6 - 2 * x)) & 3]);
          }
        }
    return true;
  }
  if (t.fmt == 6) {   // RGBA8: per 4x4 block 16 AR pairs then 16 GB pairs
    size_t o = 0;
    for (int yb = 0; yb < by; ++yb)
      for (int xb = 0; xb < bx; ++xb, o += 64)
        for (int k = 0; k < 16; ++k) {
          const uint8_t px[4] = {d[o + k * 2 + 1], d[o + 32 + k * 2], d[o + 32 + k * 2 + 1], d[o + k * 2]};
          put(xb * 4 + (k & 3), yb * 4 + (k >> 2), px);
        }
    return true;
  }
  size_t texel = 0;   // the other formats: texels in block order
  for (int yb = 0; yb < by; ++yb)
    for (int xb = 0; xb < bx; ++xb)
      for (int y = 0; y < b.bh; ++y)
        for (int x = 0; x < b.bw; ++x, ++texel) {
          uint8_t px[4];
          switch (t.fmt) {
            case 0: case 8: {
              const uint8_t byte = d[texel / 2];
              const int v = (texel & 1) ? (byte & 15) : (byte >> 4);
              if (t.fmt == 8) pal_rgba(t, v, px); else { px[0] = px[1] = px[2] = px[3] = uint8_t(v * 17); }
              break;
            }
            case 1: case 9: {
              const int v = d[texel];
              if (t.fmt == 9) pal_rgba(t, v, px); else { px[0] = px[1] = px[2] = px[3] = uint8_t(v); }
              break;
            }
            case 2: { const uint8_t v = d[texel]; px[0] = px[1] = px[2] = uint8_t((v & 15) * 17); px[3] = uint8_t((v >> 4) * 17); break; }
            case 3: cia8(be16(d + texel * 2), px); break;
            case 4: c565(be16(d + texel * 2), px); break;
            case 5: c5a3(be16(d + texel * 2), px); break;
            default: return false;
          }
          put(xb * b.bw + x, yb * b.bh + y, px);
        }
  return true;
}
std::string key_name(const Tex& t) {
  char buf[96];
  const unsigned long long th = XXH64(t.data.data(), t.data.size(), 0);
  if (t.fmt == 8 || t.fmt == 9) {
    int lo = 255, hi = 0;
    for (uint8_t byte : t.data) {
      if (t.fmt == 8) { lo = std::min({lo, byte >> 4, byte & 15}); hi = std::max({hi, byte >> 4, byte & 15}); }
      else { lo = std::min(lo, int(byte)); hi = std::max(hi, int(byte)); }
    }
    const size_t a = size_t(2 * lo), e = std::min(t.pal.size(), size_t(2 * (hi + 1)));
    const unsigned long long tl = XXH64(t.pal.data() + a, e > a ? e - a : 0, 0);
    snprintf(buf, sizeof buf, "tex1_%dx%d_%016llx_%016llx_%d", t.w, t.h, th, tl, t.fmt);
  } else {
    snprintf(buf, sizeof buf, "tex1_%dx%d_%016llx_%d", t.w, t.h, th, t.fmt);
  }
  return buf;
}

// ---- folders (as tools/texture_dump.py)
const std::map<std::string, std::string> kStages = {
    {"0100", "01 Westopolis"}, {"0200", "02 Digital Circuit"}, {"0201", "02 Glyphic Canyon"}, {"0202", "02 Lethal Highway"},
    {"0300", "03 Cryptic Castle"}, {"0301", "03 Prison Island"}, {"0302", "03 Circus Park"}, {"0400", "04 Central City"},
    {"0401", "04 The Doom"}, {"0402", "04 Sky Troops"}, {"0403", "04 Mad Matrix"}, {"0404", "04 Death Ruins"},
    {"0500", "05 The ARK"}, {"0501", "05 Air Fleet"}, {"0502", "05 Iron Jungle"}, {"0503", "05 Space Gadget"},
    {"0504", "05 Lost Impact"}, {"0600", "06 GUN Fortress"}, {"0601", "06 Black Comet"}, {"0602", "06 Lava Shelter"},
    {"0603", "06 Cosmic Fall"}, {"0604", "06 Final Haunt"}, {"0700", "07 The Last Way"}};
const std::map<std::string, std::string> kLangs = {{"EN", "English"}, {"FR", "French"}, {"GE", "German"}, {"IT", "Italian"}, {"JP", "Japanese"}, {"SP", "Spanish"}};
const std::map<std::string, std::string> kUiNames = {
    {"honban", "Title screen and main menu"}, {"2P_Result", "2P result screen"}, {"2P_Start", "2P start screen"},
    {"BattleMode", "Battle mode menu"}, {"GameOver", "Game over"}, {"InfoWindow", "Info windows and pop-ups"},
    {"Options", "Options menu"}, {"RouteDisplay", "Story route map"}, {"SelectMode", "Select mode (stage select)"},
    {"Story_Library", "Library (story, movies)"}, {"Synopsis", "Synopsis screens"}, {"PalSel", "PAL language select"},
    {"StafRoll", "Staff roll"}, {"number", "Numbers"}, {"FadeLogo", "Logos"}, {"advertise", "Advertisement"}, {"Logo", "Logos"},
    {"E3RomTitle", "Logos"}, {"Loading", "Loading screens"}, {"Result", "Stage results"}, {"MissionFailed", "Mission failed"},
    {"MediaMenu", "Library (story, movies)"}, {"Warning", "Warnings"}};
const std::set<std::string> kHud = {"CommandWindow", "2P_CommandWindow", "DarkGauge", "HeroGauge", "Hissatu_Dark", "Hissatu_Hero", "HPBar",
                                    "Score", "TWindow", "BbuttonWindow", "VHArrow", "BossGauge", "EggBoss", "2P_Ring", "2P_Star"};
const std::map<std::string, std::string> kBossPortraits = {{"BBull", "Black Bull"}, {"BFalcon", "Blue Falcon"}, {"Bdoom", "Black Doom"},
    {"DevDoom", "Devil Doom"}, {"EBreaker", "Egg Breaker"}, {"EDealer", "Egg Dealer"}, {"HDog", "Heavy Dog"}, {"SandD", "Sonic and Diablon"}};
const std::map<std::string, std::string> kSub = {{"texcm", "Level surfaces"}, {"gdt", "Objects"}, {"fx", "Effects"}};

std::string stage_name(const std::string& sid) { auto it = kStages.find(sid); return it != kStages.end() ? it->second + " (stg" + sid + ")" : "stg" + sid; }
std::string stage_folder(const std::string& sid) {
  auto it = kStages.find(sid);
  if (it != kStages.end()) return "Stages/" + it->second + " (stg" + sid + ")";
  const std::string g = sid.substr(0, 2);
  if ((g == "02" || g == "03" || g == "04" || g == "05" || g == "06" || g == "07") && sid.substr(2) >= "10") return "Stages/Boss and special stages/stg" + sid;
  if (g == "09") return "Stages/2P battle stages/stg" + sid;
  return "Stages/Other (menus, tests, extra)/stg" + sid;
}
std::pair<std::string, std::string> lang_split(const std::string& base) {
  static const std::regex re("(.*?)_?(EN|FR|GE|IT|JP|SP)(\\d\\d)?$");
  std::smatch m;
  if (std::regex_match(base, m, re) && m[1].length()) return {m[1], kLangs.at(m[2])};
  return {base, ""};
}
std::string ui_folder(const std::string& stem) {
  auto [base, lang] = lang_split(stem);
  static const std::regex re("(StageTitle|pause)(\\d{4}|Boss)$");
  std::smatch m;
  std::string nice;
  if (std::regex_match(base, m, re)) {
    const std::string what = m[1] == "StageTitle" ? "Stage title cards" : "HUD and pause menu/Per stage";
    nice = what + "/" + (m[2] != "Boss" ? stage_name(m[2]) : std::string("Boss stages"));
  } else if (kHud.count(base)) {
    nice = "HUD (in stage)/" + base;
  } else if (base.rfind("BChara_", 0) == 0) {
    auto it = kBossPortraits.find(base.substr(7));
    nice = "Character portraits (dialogue)/Bosses/" + (it != kBossPortraits.end() ? it->second : base.substr(7));
  } else {
    std::string low = base;
    for (char& ch : low) ch = char(tolower((unsigned char)ch));
    if (low.rfind("mchara", 0) == 0) {
      std::string who = base.find('_') != std::string::npos ? base.substr(base.find('_') + 1) : "Shadow";
      for (size_t k = 0; k < who.size(); ++k) who[k] = char(k ? tolower((unsigned char)who[k]) : toupper((unsigned char)who[k]));
      nice = "Character portraits (dialogue)/" + who;
    } else {
      auto it = kUiNames.find(base);
      nice = it != kUiNames.end() ? it->second : base;
    }
  }
  if (!lang.empty() && lang != "English") return "UI/Menus and HUD/" + nice + "/Other languages/" + lang;
  return "UI/Menus and HUD/" + nice;
}
std::string place(const std::vector<std::string>& parts) {
  const std::string& top = parts[0];
  std::string stem = parts.back();
  if (stem.find('.') != std::string::npos) stem = stem.substr(0, stem.rfind('.'));
  if (top == "csdFiles" || (parts.size() == 1 && (stem == "InfoWindow" || stem == "number" || stem == "FadeLogo" || stem == "advertise"))) return ui_folder(stem);
  if (top == "palsel") {
    std::string code = stem.size() >= 2 ? stem.substr(stem.size() - 2) : stem;
    for (char& ch : code) ch = char(toupper((unsigned char)ch));
    auto it = kLangs.find(code);
    const std::string lang = it != kLangs.end() ? it->second : code;
    return "UI/Menus and HUD/PAL language select" + (lang == "English" ? std::string() : "/Other languages/" + lang);
  }
  if (top == "stafroll") return "UI/Menus and HUD/Staff roll";
  if (top == "fonts") {
    const std::string sub = parts.size() > 2 ? parts[1] : "Common";
    auto [base, lang] = lang_split(stem);
    (void)base;
    std::string low = sub;
    for (char& ch : low) ch = char(tolower((unsigned char)ch));
    const std::string where = low.rfind("stg", 0) == 0 ? stage_name(sub.substr(3)) : sub;
    if (lang.empty()) lang = "Common";
    if (lang != "English") return "UI/Fonts (subtitle and text glyphs)/Other languages/" + lang + "/" + where;
    return "UI/Fonts (subtitle and text glyphs)/" + where;
  }
  if (top.rfind("stg", 0) == 0 && top.size() == 7) return stage_folder(top.substr(3));
  if (top == "stg_cmn_gdt.one") return "Objects (shared by all stages)";
  if (top == "character") return "Characters/" + stem;
  if (top == "enemy") return parts.size() > 2 && parts[1] == "boss" ? "Enemies/Bosses/" + stem : "Enemies/" + stem;
  if (top == "event") return "Cutscenes (in-engine)/" + stem;
  if (top == "common") return "Effects and common/" + stem;
  return "Other/" + stem;
}
std::string sub_name(const std::string& folder, std::string entry) {
  for (char& ch : entry) ch = char(tolower((unsigned char)ch));
  entry = std::regex_replace(entry, std::regex("^stg\\d{4}_"), "");
  auto it = kSub.find(entry);
  if (it != kSub.end()) return it->second;
  std::string last = folder.substr(folder.rfind('/') + 1);
  for (char& ch : last) ch = char(tolower((unsigned char)ch));
  if (entry == "texture" || entry == "stg_gdt" || last.rfind(entry, 0) == 0 || folder.rfind("UI/", 0) == 0 || folder.rfind("Objects", 0) == 0) return "";
  return entry;
}
bool slurp(const fs::path& p, std::vector<uint8_t>& out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  out.assign(std::istreambuf_iterator<char>(f), {});
  return true;
}
void set_msg(const std::string& m) { std::lock_guard lk(g_msg_mutex); g_msg = m; }

void run_extract(fs::path src, fs::path out) {
  struct Rec { std::string folder; Tex t; std::string key; };
  std::vector<Rec> recs;
  auto collect = [&](const std::string& folder, std::vector<Tex>& texs) {
    for (auto& t : texs) { recs.push_back({folder, std::move(t), ""}); recs.back().key = key_name(recs.back().t); }
    g_found = int(recs.size());
  };
  std::vector<fs::path> files;
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(src, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
    if (it->is_regular_file(ec)) files.push_back(it->path());
  std::sort(files.begin(), files.end());
  for (const fs::path& p : files) {
    std::string ext = p.extension().string();
    for (char& ch : ext) ch = char(tolower((unsigned char)ch));
    std::vector<std::string> parts;
    for (const auto& part : fs::relative(p, src, ec)) parts.push_back(part.string());
    if (parts.empty()) continue;
    if (ext == ".txd" || ext == ".gncp") {
      std::vector<uint8_t> d;
      if (!slurp(p, d)) continue;
      std::vector<Tex> texs;
      natives_in(d, texs);
      collect(place(parts), texs);
    } else if (ext == ".one") {
      std::vector<uint8_t> d;
      if (!slurp(p, d) || d.size() < 0xB0) continue;
      const std::string ver(reinterpret_cast<const char*>(&d[0xC]), 11);
      if (ver != "One Ver 0.6" && ver != "One Ver 0.5") continue;
      const uint32_t count = le32(&d[0x1C]);
      std::set<std::pair<uint32_t, uint32_t>> seen;
      for (uint32_t k = 0; k < count; ++k) {
        const size_t e = 0xB0 + size_t(k) * 0x38;
        if (e + 0x38 > d.size()) break;
        std::string name(reinterpret_cast<const char*>(&d[e]), strnlen(reinterpret_cast<const char*>(&d[e]), 0x2C));
        const uint32_t size = le32(&d[e + 0x2C]), off = le32(&d[e + 0x30]) + 0xC;
        if (!seen.insert({off, size}).second || off >= d.size()) continue;
        std::vector<uint8_t> raw;
        try { raw = prs_decompress(d, off); } catch (...) { raw.clear(); }
        static const uint8_t sig[4] = {0x15, 0, 0, 0};
        auto has = [&](const std::vector<uint8_t>& v) { return std::search(v.begin(), v.end(), sig, sig + 4) != v.end(); };
        if (!has(raw)) {
          raw.assign(d.begin() + off, d.begin() + std::min(d.size(), size_t(off) + size));   // stored uncompressed?
          if (!has(raw)) continue;
        }
        const std::string folder = place(parts);
        std::string stem = name.find('.') != std::string::npos ? name.substr(0, name.rfind('.')) : name;
        const std::string sub = sub_name(folder, stem);
        std::vector<Tex> texs;
        natives_in(raw, texs);
        collect(sub.empty() ? folder : folder + "/" + sub, texs);
      }
    }
  }
  // HUD / pause menu: textures several stages share go into one folder
  std::map<std::string, std::set<std::string>> per_stage;
  for (const Rec& r : recs) if (r.folder.find("/HUD and pause menu/Per stage/") != std::string::npos) per_stage[r.key].insert(r.folder);
  for (Rec& r : recs) {
    const size_t at = r.folder.find("/HUD and pause menu/Per stage/");
    if (at == std::string::npos || per_stage[r.key].size() <= 1) continue;
    const size_t ol = r.folder.find("/Other languages/");
    const std::string lang = ol != std::string::npos ? r.folder.substr(ol) : "";
    r.folder = r.folder.substr(0, at) + "/HUD and pause menu" + lang;
  }
  g_state = 2;
  fs::create_directories(out, ec);
  std::ofstream index(out / "index.txt", std::ios::binary);
  std::set<std::pair<std::string, std::string>> written;
  int bad = 0;
  for (const Rec& r : recs) {
    if (!written.insert({r.folder, r.key}).second) continue;
    std::vector<uint8_t> rgba, png;
    if (!decode(r.t, rgba) || !texture_pack::write_png(rgba, r.t.w, r.t.h, png)) { ++bad; continue; }
    std::string safe = std::regex_replace(r.t.name, std::regex("[<>:\"/\\\\|?*]"), "_");
    if (safe.empty()) safe = "unnamed";
    const fs::path dir = out / fs::u8path(r.folder);
    fs::create_directories(dir, ec);
    std::ofstream f(dir / fs::u8path(safe + " [" + r.key + "].png"), std::ios::binary);
    f.write(reinterpret_cast<const char*>(png.data()), std::streamsize(png.size()));
    index << r.key << '\t' << r.folder << '/' << safe << '\n';
    ++g_written;
  }
  logf("[textures] extract: %d textures found, %d PNG written, %d could not be decoded -> %s\n", int(recs.size()), int(g_written), bad,
       out.string().c_str());
  set_msg("Done: " + std::to_string(int(g_written)) + " textures in textures\\Original");
  g_state = 3;
}
}  // namespace

void ensure_folders() {
  std::error_code ec;
  const fs::path t = exe_dir() / L"textures";
  fs::create_directories(t / L"Custom", ec);
  fs::create_directories(t / L"Original", ec);
  const fs::path readme = t / L"README.txt";
  if (!fs::exists(readme, ec))
    if (FILE* f = _wfopen(readme.c_str(), L"wb")) {
      fputs("Textures\r\n\r\n"
            "Original - the game's own textures as PNG. Fill it from the in-game menu: System > Extract the game's textures.\r\n"
            "           Look at them, edit a copy.\r\n"
            "Custom   - put your edited PNGs here (any sub-folders). Keep the [tex1_...] part of the file name: that is how\r\n"
            "           the game finds the texture it replaces. Switch on Graphics > Load custom textures in the menu.\r\n", f);
      fclose(f);
    }
}

void start() {
  int idle = 0, done = 3, failed = 4;
  if (!(g_state.compare_exchange_strong(idle, 1) || g_state.compare_exchange_strong(done, 1) || g_state.compare_exchange_strong(failed, 1))) return;
  g_found = 0;
  g_written = 0;
  const fs::path src = fs::u8path(g_disc_root) / L"files";
  const fs::path out = exe_dir() / L"textures" / L"Original";
  std::error_code ec;
  if (g_disc_root.empty() || !fs::is_directory(src, ec)) { set_msg("The game's files were not found"); g_state = 4; return; }
  set_msg("");
  logf("[textures] extract: %s -> %s\n", src.string().c_str(), out.string().c_str());
  std::thread([src, out] {
    try { run_extract(src, out); }
    catch (...) { set_msg("Stopped by an error - see the log"); logf("[textures] extract failed\n"); g_state = 4; }
  }).detach();
}
int state() { return g_state; }
std::string status() {
  switch (g_state.load()) {
    case 1: return "Reading the disc: " + std::to_string(int(g_found)) + " textures found";
    case 2: return "Writing: " + std::to_string(int(g_written)) + " of " + std::to_string(int(g_found));
    case 3: case 4: { std::lock_guard lk(g_msg_mutex); return g_msg; }
    default: return "";
  }
}
void open_folder() {
  ensure_folders();
  const std::wstring p = (exe_dir() / L"textures").wstring();
  ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
}  // namespace texture_extract
