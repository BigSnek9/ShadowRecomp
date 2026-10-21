// #144 on-screen keyboard (osk.h): one card, five rows of keys (digits, three letter rows, Shift / Space / Delete / Done).
// Pad and keyboard input arrive through input() (raw held states; edges and repeats here), typed characters and mouse
// clicks through draw() (ImGui's text queue and mouse of that frame). The look follows the menu's blades
// (docs/launcher/IN-GAME-MENU-STYLE.md): flat keys on the track colour, the focused one in the focus colour with an
// accent edge, sizes in u = height / 1080 like the menu.
#include "osk.h"
#include "sys.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace osk {
namespace {
const char* const kRows[4] = {"1234567890", "qwertyuiop", "asdfghjkl'", "zxcvbnm-._"};
struct Wide { const char* label; int col, span; };
const Wide kWide[4] = {{"Shift", 0, 2}, {"Space", 2, 4}, {"Delete", 6, 2}, {"Done", 8, 2}};
constexpr int kCols = 10, kRowsN = 5;

std::recursive_mutex g_mx;
bool g_on = false;
std::string g_title, g_text;
size_t g_max = 24, g_caret = 0;
Check g_check;
Done g_done;
int g_row = 1, g_col = 0;           // focus: rows 0-3 = the character keys, row 4 = kWide[g_col]
bool g_shift = true;                 // one-shot: the next letter is a capital
std::string g_flash; double g_flash_t = -10;
SDL_Window* g_ime = nullptr;
double g_last_del = -10;            // B on an empty name cancels only when it was not just emptied by B (tapping B to clear)
struct Held { bool on = false; double t0 = 0, last = 0; };
Held h_up, h_down, h_left, h_right, h_bksp, h_lb, h_rb, h_kl, h_kr, h_ku, h_kd;
bool p_a, p_b, p_x, p_y, p_start, p_back, p_enter, p_esc;

double now_s() { return double(sys_time_ns()) * 1e-9; }
bool edge(bool& prev, bool now) { const bool e = now && !prev; prev = now; return e; }
bool rep(Held& h, bool now) {   // first press, then every 80 ms after 350 ms held
  const double t = now_s();
  if (!now) { h.on = false; return false; }
  if (!h.on) { h.on = true; h.t0 = h.last = t; return true; }
  if (t - h.t0 > 0.35 && t - h.last > 0.08) { h.last = t; return true; }
  return false;
}
void flash(const std::string& s) { g_flash = s; g_flash_t = now_s(); }
void changed() { logf("[osk] text '%s'\n", g_text.c_str()); }

void type_char(char ch, bool from_key) {   // from_key: an on-screen key (Shift applies); else typed (as it came)
  if (strchr("[];=", ch)) { flash(std::string("A name cannot hold ") + ch); return; }   // movement_presets.ini section / key syntax
  if (g_text.size() >= g_max) { flash("At most " + std::to_string(g_max) + " characters"); return; }
  if (from_key && g_shift && ch >= 'a' && ch <= 'z') { ch = char(ch - 32); g_shift = false; }
  g_text.insert(g_text.begin() + std::min(g_caret, g_text.size()), ch); ++g_caret;
  changed();
}
void backspace() {
  if (!g_caret || g_text.empty()) return;
  g_text.erase(g_caret - 1, 1); --g_caret; g_last_del = now_s();
  if (g_text.empty()) g_shift = true;
  changed();
}
void stop_ime() { if (g_ime) { SDL_StopTextInput(g_ime); g_ime = nullptr; } }
void finish(bool ok) {
  if (ok && g_check) if (const char* why = g_check(g_text)) { flash(why); logf("[osk] done refused: %s\n", why); return; }
  Done d = std::move(g_done); const std::string t = g_text;
  g_on = false; g_done = nullptr; g_check = nullptr; stop_ime();
  logf("[osk] %s '%s'\n", ok ? "done" : "cancelled", t.c_str());
  if (d) d(ok, t);
}
void press_focused() {
  if (g_row < 4) { type_char(kRows[g_row][g_col], true); return; }
  switch (g_col) {
    case 0: g_shift = !g_shift; break;
    case 1: type_char(' ', false); break;
    case 2: backspace(); break;
    default: finish(true); break;
  }
}
int wide_at(int col) { for (int i = 0; i < 4; ++i) if (col >= kWide[i].col && col < kWide[i].col + kWide[i].span) return i; return 3; }
void move(int dx, int dy) {
  if (dy) {
    const int col = g_row == 4 ? kWide[g_col].col : g_col;
    g_row = (g_row + dy + kRowsN) % kRowsN;
    g_col = g_row == 4 ? wide_at(col) : col;
  }
  if (dx) g_col = g_row == 4 ? (g_col + dx + 4) % 4 : (g_col + dx + kCols) % kCols;
}
}  // namespace

void open(const std::string& title, const std::string& initial, size_t max_len, Check check, Done done) {
  std::lock_guard lk(g_mx);
  if (g_on) finish(false);   // one at a time: a keyboard still open is cancelled first
  g_on = true; g_title = title; g_text = initial.substr(0, max_len); g_max = max_len; g_caret = g_text.size();
  g_check = std::move(check); g_done = std::move(done); g_row = 1; g_col = 0; g_shift = g_text.empty(); g_flash.clear();
  p_a = p_b = p_x = p_y = p_start = p_back = p_enter = p_esc = true;   // the press that opened it does not count
  if (SDL_Window* w = SDL_GetKeyboardFocus()) { SDL_StartTextInput(w); g_ime = w; }
  logf("[osk] open '%s' (%s)\n", g_title.c_str(), g_text.c_str());
}
bool active() { return g_on; }
void close() { std::lock_guard lk(g_mx); if (g_on) finish(false); }

bool input(const Keys& k) {
  std::lock_guard lk(g_mx);
  if (!g_on) return false;
  if (rep(h_up, k.up) || rep(h_ku, k.kup)) move(0, -1);
  if (rep(h_down, k.down) || rep(h_kd, k.kdown)) move(0, 1);
  if (rep(h_left, k.left)) move(-1, 0);
  if (rep(h_right, k.right)) move(1, 0);
  if (rep(h_lb, k.lb) || rep(h_kl, k.kleft)) g_caret = g_caret ? g_caret - 1 : 0;
  if (rep(h_rb, k.rb) || rep(h_kr, k.kright)) g_caret = std::min(g_caret + 1, g_text.size());
  if (edge(p_a, k.a)) press_focused();
  if (!g_on) return true;
  if (edge(p_b, k.b)) { if (g_text.empty()) { if (now_s() - g_last_del > 0.8) { finish(false); return true; } } else backspace(); }
  if (rep(h_bksp, k.bksp)) backspace();
  if (edge(p_x, k.x)) type_char(' ', false);
  if (edge(p_y, k.y)) g_shift = !g_shift;
  const bool ok = edge(p_start, k.start) | edge(p_enter, k.enter), no = edge(p_back, k.back) | edge(p_esc, k.esc);
  if (ok) finish(true);
  else if (no && g_on) finish(false);
  return true;
}

void draw(ImDrawList* dl, ImVec2 a, ImVec2 b, float u, const Style& st) {
  std::lock_guard lk(g_mx);
  if (!g_on) return;
  ImGuiIO& io = ImGui::GetIO();
  for (ImWchar c : io.InputQueueCharacters) if (c >= 32 && c < 127 && g_on) type_char(char(c), false);   // the keyboard (ASCII names)
  io.InputQueueCharacters.resize(0);
  ImFont* body = st.font ? st.font : io.Fonts->Fonts[0];
  ImFont* bold = st.bold ? st.bold : body;
  auto px = [&](float v, float floor_px) { return std::max(v * u, floor_px); };
  const float pad = 28 * u, gap = 8 * u, kh = px(58, 30), title_px = px(30, 17), field_px = px(30, 17), key_px = px(24, 14),
              msg_px = px(20, 13), hint_px = px(18, 12), field_h = field_px + 30 * u;
  const float W = std::min(b.x - a.x - 40 * u, 860 * u);
  const float H = pad + title_px + 14 * u + field_h + 8 * u + msg_px + 14 * u + kRowsN * kh + (kRowsN - 1) * gap + 16 * u + hint_px + pad;
  const ImVec2 o((a.x + b.x - W) / 2, std::max(a.y + 10 * u, (a.y + b.y - H) / 2));
  dl->AddRectFilled(a, b, st.scrim);
  dl->AddRectFilled(o, ImVec2(o.x + W, o.y + H), st.panel, 4 * u);
  dl->AddRectFilled(o, ImVec2(o.x + 6 * u, o.y + H), st.accent);   // the accent spine
  float y = o.y + pad; const float x0 = o.x + pad + 6 * u, x1 = o.x + W - pad;
  dl->AddText(bold, title_px, ImVec2(x0, y), st.text, g_title.c_str()); y += title_px + 14 * u;
  // the name field (click: the caret goes there)
  const ImVec2 fa(x0, y), fb(x1, y + field_h);
  dl->AddRectFilled(fa, fb, st.track, 3 * u); dl->AddRect(fa, fb, st.line, 3 * u, 0, std::max(1.f, 1.5f * u));
  const float tx = fa.x + 14 * u, ty = fa.y + (field_h - field_px) / 2;
  if (g_text.empty()) dl->AddText(body, field_px, ImVec2(tx, ty), st.dim, "Type a name");
  else dl->AddText(body, field_px, ImVec2(tx, ty), st.text, g_text.c_str());
  const float cx = tx + body->CalcTextSizeA(field_px, FLT_MAX, 0, g_text.c_str(), g_text.c_str() + std::min(g_caret, g_text.size())).x;
  if (std::fmod(ImGui::GetTime(), 1.0) < 0.6) dl->AddRectFilled(ImVec2(cx, ty), ImVec2(cx + std::max(2.f, 2 * u), ty + field_px), st.accent);
  char count[32]; snprintf(count, sizeof count, "%zu / %zu", g_text.size(), g_max);
  dl->AddText(body, msg_px, ImVec2(fb.x - 14 * u - body->CalcTextSizeA(msg_px, FLT_MAX, 0, count).x, fa.y + (field_h - msg_px) / 2), st.dim, count);
  const ImVec2 m = io.MousePos; const bool click = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
  if (click && m.x >= fa.x && m.x < fb.x && m.y >= fa.y && m.y < fb.y) {
    size_t best = g_text.size();
    for (size_t i = 0; i <= g_text.size(); ++i)
      if (tx + body->CalcTextSizeA(field_px, FLT_MAX, 0, g_text.c_str(), g_text.c_str() + i).x >= m.x - field_px * 0.25f) { best = i; break; }
    g_caret = best;
  }
  y += field_h + 8 * u;
  // the reason a name is refused (live), else a short flash (too long ...)
  const char* why = g_check ? g_check(g_text) : nullptr;
  const bool fl = !g_flash.empty() && now_s() - g_flash_t < 2.5;
  if (fl || (why && !g_text.empty())) dl->AddText(body, msg_px, ImVec2(x0, y), st.accent, fl ? g_flash.c_str() : why);
  y += msg_px + 14 * u;
  // the keys
  const float kw = (x1 - x0 - (kCols - 1) * gap) / kCols;
  auto key = [&](float kx, float ky, float w, const char* label, bool focus, bool lit, int row, int col) {
    const ImVec2 ka(kx, ky), kb(kx + w, ky + kh);
    const bool hover = m.x >= ka.x && m.x < kb.x && m.y >= ka.y && m.y < kb.y;
    dl->AddRectFilled(ka, kb, focus ? st.focus : lit ? st.line : st.track, 3 * u);
    if (focus) dl->AddRect(ka, kb, st.accent, 3 * u, 0, std::max(1.5f, 2.5f * u));
    else if (hover) dl->AddRect(ka, kb, st.line, 3 * u, 0, std::max(1.f, 1.5f * u));
    const ImVec2 ts = bold->CalcTextSizeA(key_px, FLT_MAX, 0, label);
    dl->AddText(bold, key_px, ImVec2(kx + (w - ts.x) / 2, ky + (kh - ts.y) / 2), st.text, label);
    if (hover && click) { g_row = row; g_col = col; press_focused(); }
  };
  for (int r = 0; r < 4 && g_on; ++r)
    for (int c = 0; c < kCols && g_on; ++c) {
      char lab[2] = {kRows[r][c], 0};
      if (g_shift && lab[0] >= 'a' && lab[0] <= 'z') lab[0] = char(lab[0] - 32);
      key(x0 + c * (kw + gap), y + r * (kh + gap), kw, lab, g_row == r && g_col == c, false, r, c);
    }
  for (int i = 0; i < 4 && g_on; ++i)
    key(x0 + kWide[i].col * (kw + gap), y + 4 * (kh + gap), kWide[i].span * kw + (kWide[i].span - 1) * gap, kWide[i].label,
        g_row == 4 && g_col == i, i == 0 && g_shift, 4, i);
  y += kRowsN * kh + (kRowsN - 1) * gap + 16 * u;
  dl->AddText(body, hint_px, ImVec2(x0, y), st.dim, "A type   B delete   X space   Y shift   START done   BACK cancel   |   or type on the keyboard: Enter done, Esc cancel");
}

void command(const std::string& s) {   // tests
  std::lock_guard lk(g_mx);
  if (!g_on) { logf("[osk] (not open) %s\n", s.c_str()); return; }
  const size_t c = s.find(' ');
  const std::string op = s.substr(0, c), arg = c == std::string::npos ? "" : s.substr(c + 1);
  if (op == "type") for (char ch : arg) type_char(ch == '_' ? ' ' : ch, false);
  else if (op == "chars") ImGui::GetIO().AddInputCharactersUTF8(arg.c_str());   // like typing on the keyboard (read in the next draw)
  else if (op == "key") {   // an on-screen key by its label: key q / key Shift / key Done
    for (int r = 0; r < 4; ++r) for (int k = 0; k < kCols; ++k) if (arg.size() == 1 && kRows[r][k] == arg[0]) { g_row = r; g_col = k; }
    for (int i = 0; i < 4; ++i) if (arg == kWide[i].label) { g_row = 4; g_col = i; }
    press_focused();
  }
  else if (op == "move") move(arg == "left" ? -1 : arg == "right" ? 1 : 0, arg == "up" ? -1 : arg == "down" ? 1 : 0);
  else if (op == "a") press_focused();
  else if (op == "b") { if (g_text.empty()) { if (now_s() - g_last_del > 0.8) finish(false); } else backspace(); }
  else if (op == "x") type_char(' ', false);
  else if (op == "y") g_shift = !g_shift;
  else if (op == "done") finish(true);
  else if (op == "cancel") finish(false);
  if (g_on) logf("[osk] focus %d:%d shift %d caret %zu\n", g_row, g_col, int(g_shift), g_caret);
}
}  // namespace osk
