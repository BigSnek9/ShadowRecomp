// GX / GP emulation: the guest's own GX SDK code writes real hardware FIFO commands to the
// write-gather pipe. We keep the PI FIFO ring semantics (so display lists built in memory work),
// and when the CP is linked we parse the command stream and replay it through Aurora's
// command processor, translating the few things that are host-pointer based in Aurora
// (vertex arrays, textures, TLUTs, EFB copies).
#include "sys.h"
#include "hw.h"
#include "sched.h"
#include "gx_lle.h"
#include "settings_ui.h"
#include "game.h"
#include "lighting_contact_ao.h"
#include "lighting.h"

#include <aurora/aurora.h>
#include <aurora/event.h>
namespace engine_limits { void frame(); }   // engine_limits.cpp (#154)
namespace first_boot_hint { void frame(); }   // first_boot_hint.cpp (#155)
#include <SDL3/SDL.h>
#define NOMINMAX
#include <windows.h>
#include <dwmapi.h>
#include <dolphin/gx.h>

#include "gx/gx.hpp"
#include "gx/command_processor.hpp"
#include "window.hpp"

#include <cstring>
#include <cstdlib>
#include <vector>
#include <algorithm>
#include <map>
#include <set>
#include <unordered_map>
#include <deque>
extern bool g_mash;   // input.cpp (--fast auto-input; cleared when a stage loads)

using aurora::gx::g_gxState;

namespace gxlle {
bool g_trace = false;
static bool g_aurora_up = false;
static bool g_frame_active = false;
static uint64_t g_xfb_copies = 0;
extern bool g_bp_status;
extern bool g_gp_halted;
extern std::vector<uint8_t> g_halt_q;
extern uint32_t g_rd_ptr;

// ----------------------------------------------------------------------------- register files
static uint16_t g_cp[0x40];
static uint16_t g_pe[0x40];
static bool g_pe_token_status = false, g_pe_finish_status = false;
static uint32_t g_token_value = 0;

// ----------------------------------------------------------------------------- stream state
static std::vector<uint8_t> g_pending;              // bytes of a not yet complete command
static uint32_t g_array_base[16];                   // guest physical addresses
static uint32_t g_array_size[16];                   // bytes made visible to Aurora so far
static uint32_t g_tex_mode0[8], g_tex_mode1[8], g_tex_image0[8], g_tex_image3[8], g_tex_settlut[8];
static uint32_t g_bp_cache[256];
struct TlutLoad { uint32_t mem = 0; uint32_t entries = 0; int slot = -1; };
static std::map<uint32_t, TlutLoad> g_tlut_by_offset;   // tmem offset -> load
static int g_next_tlut_slot = 0;
static uint32_t g_tlut_mem = 0;                     // BP 0x64
static uint64_t g_stat_cmds = 0, g_stat_draws = 0;
// The post-processing point (postfx.cpp): between the 3D scene and the HUD. The game draws everything through one
// render tree, and game_hooks.cpp hud_element_draw tags each of its objects (FIFO markers 0x02/0x03) - the world, the
// effects AND the HUD. Scene objects draw 3D (perspective); the HUD objects after them draw only 2D (orthographic).
// Westopolis, per frame: [1 2D] [3D x9] [1 2D] [3D x2] [2D] [3D] [2D screen filter] [2D x2 = the HUD]. Each frame
// notes the first top-level object of the trailing run of 2D-only objects (a 2D draw of an EFB copy counts as scene,
// see fx_order_draw); the next frame runs the post-processing when that object starts.
// FX_ORDER=1 logs one frame's top-level object sequence every 10 s.
// The scene camera for the ambient occlusion / GI (they rebuild positions from the depth buffer): the perspective
// projection used by the most full-screen 3D draws of the previous frame (the world, not a sky dome or a 3D HUD part).
struct FxBoundary {
  uint32_t obj = 0;         // current top-level object (0 = between objects)
  uint32_t d3 = 0, d2 = 0;  // its 3D / 2D draws
  uint32_t cand = 0;        // this frame: first object of the trailing 2D-only run (so far)
  uint32_t learned = 0;     // the previous frame's boundary object
  bool seq = false;         // FX_ORDER: logging this frame
  struct Tally { uint32_t raw[12]; uint32_t n; } tally[4];   // this frame's perspective projections + viewports
  int ntally = 0;
  // the previous frame's scene camera: GX projection (XF 0x1020..0x1025), then its viewport as a rectangle of the
  // screen in 0..1 units (left, top, width, height)
  float proj[10] = {};
  bool proj_ok = false;
};
static FxBoundary g_fxb;
static void fxb_flush() {
  if (g_fxb.seq && (g_fxb.d3 || g_fxb.d2))
    logf("[fx-seq]   %s %08X: %u 3D draws, %u 2D draws\n", g_fxb.obj ? "object" : "outside", g_fxb.obj, g_fxb.d3, g_fxb.d2);
  if (g_fxb.d3) g_fxb.cand = 0;
  else if (g_fxb.d2 && g_fxb.obj && !g_fxb.cand) g_fxb.cand = g_fxb.obj;
  g_fxb.d3 = g_fxb.d2 = 0;
}
static void fx_order_draw();


static inline const uint8_t* ram_ptr(uint32_t phys) { return g_ram + (phys & RT_RAM_MASK); }

// ----------------------------------------------------------------------------- stream emit helpers
static std::vector<uint8_t> g_out;
static void put8(uint8_t v) { g_out.push_back(v); }
static void put16(uint16_t v) { g_out.push_back(v >> 8); g_out.push_back(v & 0xFF); }
static void put32(uint32_t v) { for (int s = 24; s >= 0; s -= 8) g_out.push_back((v >> s) & 0xFF); }
static void put64(uint64_t v) { for (int s = 56; s >= 0; s -= 8) g_out.push_back((v >> s) & 0xFF); }
uint64_t g_prof_ns[4];   // FPS_LOG profiling: [0] GP parse+translate (gp_run), [1] Aurora command processing, [2] present
// #152: the clock is only read for these with FPS_LOG=2 (they were 4-6 clock reads per GX run, always)
static const bool g_prof_on = getenv("FPS_LOG") && atoi(getenv("FPS_LOG")) >= 2;   // FPS_LOG=2: with the split
static void run_out() {
  if (g_out.empty()) return;
  const uint64_t t0 = g_prof_on ? sys_time_ns() : 0;
  if (g_frame_active) aurora::gx::fifo::process(g_out.data(), (uint32_t)g_out.size());
  if (g_prof_on) g_prof_ns[1] += sys_time_ns() - t0;
  g_out.clear();
}

static void emit_arraybase(int idx, uint32_t size) {
  put8(GX_AURORA);
  put16((uint16_t)(GX_AURORA_LOAD_ARRAYBASE + idx));
  put64((uint64_t)(uintptr_t)ram_ptr(g_array_base[idx]));
  put32(size);
  put8(0);   // big endian
  g_array_size[idx] = size;
}

static void ensure_array(int idx, uint32_t need_bytes) {
  if (idx < 0 || idx > 15) return;
  if (need_bytes <= g_array_size[idx] && g_array_size[idx] != 0) return;
  uint32_t base = g_array_base[idx] & RT_RAM_MASK;
  uint32_t maxbytes = RT_RAM_SIZE > base ? RT_RAM_SIZE - base : 0;
  uint32_t sz = std::max<uint32_t>(need_bytes, 64);
  // grow geometrically so repeated small growth does not re-upload every draw
  sz = std::min<uint32_t>(std::max<uint32_t>(sz, g_array_size[idx] * 2), maxbytes);
  sz = (sz + 31) & ~31u;
  if (sz > maxbytes) sz = maxbytes;
  emit_arraybase(idx, sz);
}

// ----------------------------------------------------------------------------- textures
static uint32_t tex_fmt_from_copy(uint32_t reg52, bool zcopy) {
  uint32_t pf = (((reg52 >> 3) & 1) << 3) | ((reg52 >> 4) & 7);
  uint32_t inten = (reg52 >> 15) & 3;
  if (zcopy) {
    switch (pf) { case 0: return 0x10 | 0; case 1: return 0x11; case 3: return 0x13; case 6: return 0x16; default: return 0x16; }
  }
  switch (pf) {
    case 0: return inten == 3 ? 0 : 0x20;
    case 1: return 1;
    case 2: return inten == 3 ? 2 : 0x22;
    case 3: return inten == 3 ? 3 : 0x23;
    case 4: return 4;
    case 5: return 5;
    case 6: return inten == 3 ? 0x26 : 6;
    case 7: return 0x27;
    case 8: return 0x28;
    case 9: return 0x29;
    case 10: return 0x2A;
    case 11: return 0x2B;
    case 12: return 0x2C;
    default: return 6;
  }
}

static void emit_tlut(int slot, const TlutLoad& t, uint32_t fmt) {
  put8(GX_AURORA);
  put16(GX_AURORA_LOAD_TLUT);
  put8((uint8_t)slot);
  put64((uint64_t)(uintptr_t)ram_ptr(t.mem));
  put32(fmt);
  put16((uint16_t)t.entries);
  put32(0);
  put32(0);
}

static std::string g_last_proj, g_last_vp;   // TRACE_COPY: transform state when a copy texture is used
static int g_dump_next_draw = 0;
extern SDL_Window* g_window;   // defined with init_window()
static float g_vp_width = 640.0f;    // current XF viewport width (logical)
static uint32_t g_vp_raw[6] = {};    // current XF viewport 0x101A..0x101F as the game wrote it (float bits)
static float g_ws_factor = 1.0f;     // --widescreen: (4/3) / output aspect, refreshed every frame
extern bool g_ws_active;             // widescreen rendering right now (see present_frame)
static uint32_t g_map0_copy_tex = 0;   // EFB-copy texture currently on map 0 (0 = none)
static uint32_t g_proj_raw[7];       // last XF projection block as written by the game
static bool g_proj_ortho = false;
static int g_proj_mode = 0;          // what Aurora has for an ortho projection: 0 as written, else a HudSide

// --widescreen, 2D: the logical 640x480 screen is stretched over the 16:9 framebuffer (keeps every EFB copy and
// post-process pass exact). Perspective projections are corrected where they are written; orthographic ones are
// squeezed per draw so HUD/menus keep their 4:3 shape (centred; things placed past the 4:3 edges become visible).
// Not squeezed: full-screen quads (clears, post-process composites) and any quad that samples
// an EFB copy - those must keep covering exactly what the copies captured.
// Squeezed 2D is also scissored to the centred 4:3 area (settings "Hide UI outside 4:3"): menus and HUD elements
// the game parks just off its 640 px screen (slide-ins, scrolling banners) stay hidden as on a 4:3 TV, while the 3D
// world still fills the width. The game's own scissor (BP 0x20/0x21, +342 offset, inclusive) is intersected.
// Where a squeezed 2D draw goes: the centred 4:3 area, or (16:9 HUD, in a stage) the 4:3-shaped area at the left or
// right edge of the screen. In logical 640-wide units the screen is [0, 640]; the 4:3 area is 640 * g_ws_factor wide.
enum HudSide { kSideCenter = 1, kSideLeft = 2, kSideRight = 3, kSideAnchor = 4 };   // Anchor: #146 squeezed round its own centre
// #146 enemy HP bars: the game places them with the camera's widened view window (they land on their enemy in the
// widened 3D view), so the centred squeeze pulled them toward the middle a second time (21:9: a soldier at 83 % of the
// width had its bar at 67 %). They keep their squeezed shape but are squeezed round their own centre: g_anchor_ndc =
// the bar's centre (NDC x), one per bar - the quads of a bar overlap, a quad away from the last bar starts a new one.
static float g_anchor_ndc = 0.0f, g_proj_anchor = 1e9f;
static float g_anchor_box[2] = {1e9f, -1e9f};
static void side_span(int side, float& l, float& r) {
  float w = 640.0f * g_ws_factor;
  l = side == kSideLeft ? 0.0f : side == kSideRight ? 640.0f - w : 320.0f - w * 0.5f;
  r = l + w;
}
static int g_clip_mode = 0;          // UI clip scissor in Aurora: 0 none, else a HudSide
static void put_scissor(int side) {
  uint32_t s0 = g_bp_cache[0x20], s1 = g_bp_cache[0x21];
  if (side) {
    int left = (int)((s0 >> 12) & 0x7FF) - 342, right = (int)((s1 >> 12) & 0x7FF) - 342;
    float l, r;
    side_span(side, l, r);
    int cl = (int)std::ceil(l), cr = (int)std::floor(r) - 1;
    // #146: the game parks a HUD piece with its left edge just inside x 640 (its last column: the TV overscan on 4:3); in
    // widescreen that sliver stood as a 1-2 px line at the right edge of the centred 4:3 area (21:9 / 32:9 frames). The clip
    // ends one logical pixel earlier (Aurora also rounds a scissor's right edge up to the next render column).
    cr -= 1;
    left = std::max(left, cl);
    right = std::max(left, std::min(right, cr));
    s0 = (s0 & ~(0x7FFu << 12)) | ((uint32_t)(left + 342) << 12);
    s1 = (s1 & ~(0x7FFu << 12)) | ((uint32_t)(right + 342) << 12);
  }
  put8(0x61); put32(0x20000000u | s0);
  put8(0x61); put32(0x21000000u | s1);
}
static void set_ui_clip(int side) {
  if (side != g_clip_mode) { put_scissor(side); g_clip_mode = side; }
}
// mode 0: the game's projection as written; else squeezed to 4:3 width and placed at that side (NDC x offset)
static void put_proj(int mode) {
  put8(0x10);
  put32((6u << 16) | 0x1020u);
  for (int i = 0; i < 7; ++i) {
    uint32_t u = g_proj_raw[i];
    if (mode && i < 2) {
      float f;
      memcpy(&f, &u, 4);
      f *= g_ws_factor;
      if (i == 1) f += mode == kSideLeft ? -(1.0f - g_ws_factor) : mode == kSideRight ? (1.0f - g_ws_factor)
                     : mode == kSideAnchor ? (1.0f - g_ws_factor) * g_anchor_ndc : 0.0f;
      memcpy(&u, &f, 4);
    }
    put32(u);
  }
  g_proj_mode = mode;
  g_proj_anchor = mode == kSideAnchor ? g_anchor_ndc : 1e9f;
}

// ---- 16:9 HUD (setting hud_169): HUD widgets snap to the screen edges instead of the centred 4:3 area.
// The game draws every HUD object (gauges, ring counter, timer, dialogue box...) from its render-tree walker
// (fn_8004EC68, vtable +0x0C draw at 0x8004ED60). A call hook (callhooks.txt -> game_hooks.cpp hud_element_draw)
// writes private FIFO commands around each object draw: 0x02 <object> / 0x03. One object can draw several widgets
// (the top bar object draws both gauges and the timer), so within an object the quads of a frame are grouped into
// clusters of touching boxes = widgets. Each widget keeps its shape and goes to the side its centre is on
// (left / centre / right third). A widget overlapping one of the previous frame keeps that side, so nothing
// splits or jumps while it animates. Quads are placed by the previous frame's widgets (one frame late for new ones).
struct HudBox { float x0, x1, y0, y1; int side; };
struct HudElem {
  std::vector<HudBox> quads;     // this frame
  std::vector<HudBox> widgets;   // last completed frame
  uint64_t last_frame = 0;
  bool gauge = false;            // a Hero/Dark gauge (frame / emblem): every widget in a top-corner gauge zone
};
static std::unordered_map<uint32_t, HudElem> g_hud_elems;
static uint32_t g_elem_stack[16];
static int g_elem_depth = 0;
// a 2D draw of an EFB copy (the game's screen filters: glow, haze, blur - they blend an earlier copy of the scene back
// over it) still belongs to the scene: after the post-processing it would bring the ungraded picture back
static void fx_order_draw() {
  if (g_proj_ortho && !g_map0_copy_tex) { ++g_fxb.d2; return; }
  ++g_fxb.d3;
  if (g_proj_ortho || g_vp_width < 400.0f) return;
  uint32_t key[12];
  memcpy(key, g_proj_raw, 6 * 4);
  memcpy(key + 6, g_vp_raw, 6 * 4);
  for (int i = 0; i < g_fxb.ntally; ++i)
    if (!memcmp(g_fxb.tally[i].raw, key, sizeof key)) { ++g_fxb.tally[i].n; return; }
  if (g_fxb.ntally < 4) {
    auto& t = g_fxb.tally[g_fxb.ntally++];
    memcpy(t.raw, key, sizeof key);
    t.n = 1;
  }
}
static int side_of(float x0, float x1) {
  float cx = (x0 + x1) * 0.5f;
  if (x1 - x0 > 400.0f) return kSideCenter;
  return cx < 224.0f ? kSideLeft : cx > 416.0f ? kSideRight : kSideCenter;
}
static void hud_finish_element(uint32_t id, HudElem& h) {
  const float m = 10.0f;   // boxes closer than this belong to one widget
  std::vector<HudBox> cl;
  for (const HudBox& q : h.quads) {
    HudBox c = q;
    for (bool merged = true; merged;) {   // absorb every cluster this one touches
      merged = false;
      for (size_t i = 0; i < cl.size(); ++i) {
        const HudBox& o = cl[i];
        if (c.x0 - m <= o.x1 && o.x0 - m <= c.x1 && c.y0 - m <= o.y1 && o.y0 - m <= c.y1) {
          c = {std::min(c.x0, o.x0), std::max(c.x1, o.x1), std::min(c.y0, o.y0), std::max(c.y1, o.y1), 0};
          cl.erase(cl.begin() + i);
          merged = true;
          break;
        }
      }
    }
    cl.push_back(c);
  }
  static const bool log = getenv("HUD_LOG") != nullptr;
  const bool paused = game::pause_menu_active();
  for (HudBox& c : cl) {
    c.side = 0;
    // keep the side of the widget it was last frame - only if that is the SAME widget (similar size): a pause panel
    // sliding in over the objective counter ("4/45", right side) inherited "right" and stayed off-centre (user report)
    for (const HudBox& o : h.widgets) {
      if (!(c.x0 <= o.x1 && o.x0 <= c.x1 && c.y0 <= o.y1 && o.y0 <= c.y1)) continue;
      const float cw = c.x1 - c.x0, ow = o.x1 - o.x0, ch = c.y1 - c.y0, oh = o.y1 - o.y0;
      if (cw > 1.5f * ow + 8 || ow > 1.5f * cw + 8 || ch > 1.5f * oh + 8 || oh > 1.5f * ch + 8) continue;
      c.side = o.side;
      break;
    }
    if (!c.side) {
      // a widget that appears while the pause menu is up is part of the pause screen: it stays centred
      c.side = paused ? kSideCenter : side_of(c.x0, c.x1);
      if (log) logf("[hud] object %08X widget x[%.0f..%.0f] y[%.0f..%.0f] -> %s\n", id, c.x0, c.x1, c.y0, c.y1,
                    c.side == kSideLeft ? "left" : c.side == kSideRight ? "right" : "centre");
    }
  }
  h.widgets = std::move(cl);
  h.quads.clear();
  // the gauges: everything in the top-left (x < 245) or top-right (x > 395) band y 30..96; the score/timer object also
  // draws in the centre, so it never qualifies (the PC gauges, hud_gauges.cpp, replace these objects)
  h.gauge = !h.widgets.empty();
  for (const HudBox& w : h.widgets)
    if (!(w.y0 >= 30 && w.y1 <= 96 && (w.x1 <= 245 || w.x0 >= 395))) { h.gauge = false; break; }
}
// a 2D draw of the current HUD object: note its box (for the widget analysis) and say whether it is a gauge
static bool hud_gauge_quad(float x0, float y0, float x1, float y1) {
  if (g_elem_depth <= 0 || g_elem_depth > 16) return false;
  HudElem& h = g_hud_elems[g_elem_stack[g_elem_depth - 1]];
  if (h.last_frame + 120 < g_xfb_copies) h = HudElem{};
  h.last_frame = g_xfb_copies;
  if (h.quads.size() < 4096) h.quads.push_back({x0, x1, y0, y1, 0});
  return h.gauge;
}
static void hud_marker(bool begin, uint32_t id) {
  if (begin) {
    if (g_elem_depth == 0) {
      fxb_flush();
      g_fxb.obj = id;
      // the HUD starts: the 3D scene is complete -> post-processing (postfx.cpp; once per frame)
      // the lighting engine (lighting.cpp, docs/lighting/LIGHTING-ENGINE.md) runs here, once per frame
      if (id == g_fxb.learned && settings::g.light && game::in_stage()) {
        run_out();   // everything before the marker reaches Aurora first
        lighting::before_hud(g_xfb_copies, g_fxb.proj_ok ? g_fxb.proj : nullptr);
      }
      if (id == g_fxb.learned && settings::g.ao && g_fxb.proj_ok && game::in_stage()) {   // #148 contact AO on this frame's depth
        run_out();
        contact_ao::run(g_fxb.proj);
      }
    }
    if (g_elem_depth < 16) g_elem_stack[g_elem_depth] = id;
    ++g_elem_depth;
    g_anchor_box[0] = 1e9f; g_anchor_box[1] = -1e9f;   // #146 no HP bar of this object yet
    return;
  }
  if (g_elem_depth <= 0) return;
  --g_elem_depth;
  if (g_elem_depth == 0) { fxb_flush(); g_fxb.obj = 0; }
  if (g_elem_depth >= 16) return;
  uint32_t e = g_elem_stack[g_elem_depth];
  auto it = g_hud_elems.find(e);
  if (it != g_hud_elems.end() && !it->second.quads.empty()) hud_finish_element(e, it->second);
}
static int hud_side_for(float x0, float x1, float y0, float y1) {
  if (!settings::g.hud_169 || g_elem_depth <= 0 || g_elem_depth > 16 || !game::in_stage()) return kSideCenter;
  uint32_t e = g_elem_stack[g_elem_depth - 1];
  if (e & 1) return kSideCenter;   // #89: a world-anchored marker (lock-on mark, enemy HP bar): keep its projected place
  HudElem& h = g_hud_elems[e];
  if (h.last_frame + 120 < g_xfb_copies) h = HudElem{};   // not drawn for 2 s: a new object may live at that address
  h.last_frame = g_xfb_copies;
  if (h.quads.size() < 4096) h.quads.push_back({x0, x1, y0, y1, 0});
  // the widget this quad belongs to = one that contains the whole quad (a quad's centre alone can fall inside a
  // smaller neighbouring widget, e.g. a pause panel over the objective counter)
  for (const HudBox& w : h.widgets)
    if (x0 >= w.x0 - 1 && x1 <= w.x1 + 1 && y0 >= w.y0 - 1 && y1 <= w.y1 + 1) return w.side;
  float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
  if (!game::pause_menu_active())
    for (const HudBox& w : h.widgets)
      if (cx >= w.x0 && cx <= w.x1 && cy >= w.y0 && cy <= w.y1) return w.side;
  return game::pause_menu_active() ? kSideCenter : side_of(x0, x1);
}
// HUD_LOG=1: every distinct 2D (orthographic) draw once - vertex format, matrix, screen box - to design the 16:9 HUD
static void hud_log(const uint8_t* vtx, uint32_t count, uint32_t vs, int fmt) {
  static const bool on = getenv("HUD_LOG") != nullptr;
  if (!on || !g_proj_ortho || !game::in_stage()) return;
  const auto& af = g_gxState.vtxFmts[fmt].attrs[GX_VA_POS];
  const auto& m = g_gxState.pnMtx[g_gxState.currentPnMtx / 3].pos;
  char buf[256];
  int n = snprintf(buf, sizeof buf, "posdesc=%d type=%d cnt=%d frac=%d pnmtxidx=%d vs=%u mtx=%u t=(%.0f,%.0f) s=(%.2f,%.2f)",
                   (int)g_gxState.vtxDesc[GX_VA_POS], (int)af.type, (int)af.cnt, (int)af.frac,
                   (int)g_gxState.vtxDesc[GX_VA_PNMTXIDX], vs, g_gxState.currentPnMtx, m.m0.m[3], m.m1.m[3], m.m0.m[0], m.m1.m[1]);
  {
    static const bool sub2 = getenv("SUB_LOG") != nullptr;
    static int nfmt = 0;
    if (sub2 && !(g_gxState.vtxDesc[GX_VA_POS] == GX_DIRECT && af.type == GX_F32 && g_gxState.vtxDesc[GX_VA_PNMTXIDX] == GX_NONE) && nfmt++ < 80)
      logf("[sub] other 2D format: %s n %u tex %08X element %08X\n", buf, count, g_tex_image0[0],
           g_elem_depth > 0 && g_elem_depth <= 16 ? g_elem_stack[g_elem_depth - 1] : 0u);
  }
  if (g_gxState.vtxDesc[GX_VA_POS] == GX_DIRECT && af.type == GX_F32 && g_gxState.vtxDesc[GX_VA_PNMTXIDX] == GX_NONE) {
    auto f = [](const uint8_t* q) { uint32_t u = (q[0] << 24) | (q[1] << 16) | (q[2] << 8) | q[3]; float v; memcpy(&v, &u, 4); return v; };
    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
    for (uint32_t i = 0; i < count; ++i) {
      float x = f(vtx + i * vs), y = f(vtx + i * vs + 4);
      float sx = m.m0.m[0] * x + m.m0.m[1] * y + m.m0.m[3], sy = m.m1.m[0] * x + m.m1.m[1] * y + m.m1.m[3];
      x0 = std::min(x0, sx); x1 = std::max(x1, sx); y0 = std::min(y0, sy); y1 = std::max(y1, sy);
    }
    snprintf(buf + n, sizeof buf - n, " box x[%d..%d] y[%d..%d]", (int)x0 / 4 * 4, (int)x1 / 4 * 4, (int)y0 / 4 * 4, (int)y1 / 4 * 4);
    // SUB_LOG=1 (#87): every 2D draw in the subtitle band (y 330..440), with the HUD element it belongs to (0 = untagged)
    static const bool sub = getenv("SUB_LOG") != nullptr;
    static int nsub = 0, nmtx = 0;
    if (sub && (g_gxState.currentPnMtx != 0 || std::fabs(m.m0.m[3]) > 1 || std::fabs(m.m1.m[3]) > 1 || m.m0.m[0] != 1.0f) && nmtx++ < 120)
      logf("[sub] mtx %u t (%.1f,%.1f) s (%.2f,%.2f) local x[%.0f..%.0f] y[%.0f..%.0f] tex %08X element %08X\n", g_gxState.currentPnMtx,
           m.m0.m[3], m.m1.m[3], m.m0.m[0], m.m1.m[1], x0, x1, y0, y1, g_tex_image0[0],
           g_elem_depth > 0 && g_elem_depth <= 16 ? g_elem_stack[g_elem_depth - 1] : 0u);
    if (sub && y0 >= 330 && y1 <= 440 && x1 - x0 < 60 && x0 >= 100 && x1 <= 560 && nsub++ < 200)
      logf("[sub] quad x[%.0f..%.0f] y[%.0f..%.0f] vs %u n %u element %08X depth %d tex %08X\n", x0, x1, y0, y1, vs, count,
           g_elem_depth > 0 && g_elem_depth <= 16 ? g_elem_stack[g_elem_depth - 1] : 0u, g_elem_depth, g_tex_image0[0]);
  }
  static std::set<std::string> seen;
  if (seen.size() < 1500 && seen.insert(buf).second) logf("[hud] %s\n", buf);
}
// screen box of a 2D draw in the game's 640x480 units (direct f32 positions through the current matrix), false if unknown
static bool draw_box_2d(const uint8_t* vtx, uint32_t count, uint32_t vs, int fmt, float& x0, float& y0, float& x1, float& y1) {
  if (!count || g_gxState.vtxDesc[GX_VA_POS] != GX_DIRECT || g_gxState.vtxFmts[fmt].attrs[GX_VA_POS].type != GX_F32 ||
      g_gxState.vtxDesc[GX_VA_PNMTXIDX] != GX_NONE)
    return false;
  auto f = [](const uint8_t* q) { uint32_t u = (q[0] << 24) | (q[1] << 16) | (q[2] << 8) | q[3]; float v; memcpy(&v, &u, 4); return v; };
  const auto& m = g_gxState.pnMtx[g_gxState.currentPnMtx / 3].pos;
  x0 = y0 = 1e9f; x1 = y1 = -1e9f;
  for (uint32_t i = 0; i < count; ++i) {
    float x = f(vtx + i * vs), y = f(vtx + i * vs + 4);
    float sx = m.m0.m[0] * x + m.m0.m[1] * y + m.m0.m[3], sy = m.m1.m[0] * x + m.m1.m[1] * y + m.m1.m[3];
    x0 = std::min(x0, sx); x1 = std::max(x1, sx); y0 = std::min(y0, sy); y1 = std::max(y1, sy);
  }
  return true;
}
static void ws_prepare_draw(const uint8_t* vtx, uint32_t count, uint32_t vs, int fmt) {
  hud_log(vtx, count, vs, fmt);
  if (!g_ws_active || !g_proj_ortho || g_ws_factor == 1.0f) {
    if (g_proj_mode) { put_proj(0); run_out(); }
    if (g_clip_mode) { set_ui_clip(0); run_out(); }
    return;
  }
  int mode = g_vp_width >= 600.0f ? kSideCenter : 0;
  if (mode && g_gxState.vtxDesc[GX_VA_POS] == GX_DIRECT && g_gxState.vtxFmts[fmt].attrs[GX_VA_POS].type == GX_F32 &&
      g_gxState.vtxDesc[GX_VA_PNMTXIDX] == GX_NONE) {
    auto f = [](const uint8_t* q) { uint32_t u = (q[0] << 24) | (q[1] << 16) | (q[2] << 8) | q[3]; float v; memcpy(&v, &u, 4); return v; };
    const auto& m = g_gxState.pnMtx[g_gxState.currentPnMtx / 3].pos;   // the 2D draws' matrix (identity so far)
    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
    for (uint32_t i = 0; i < count; ++i) {
      const uint8_t* v = vtx + i * vs;
      float x = f(v), y = f(v + 4);
      float sx = m.m0.m[0] * x + m.m0.m[1] * y + m.m0.m[3], sy = m.m1.m[0] * x + m.m1.m[1] * y + m.m1.m[3];
      x0 = std::min(x0, sx); x1 = std::max(x1, sx); y0 = std::min(y0, sy); y1 = std::max(y1, sy);
    }
    bool full = x0 <= 1.0f && x1 >= 639.0f && y0 <= 1.0f && y1 >= 479.0f;
    static const bool strip_test = getenv("WS_STRIPS") != nullptr;   // experiment: full-height untextured strips
    if (strip_test && y0 <= 1.0f && y1 >= 479.0f && g_gxState.vtxDesc[GX_VA_TEX0] == GX_NONE) full = true;
    // Quads that sample an EFB copy (heat haze around flames, the speed-pad mirage around Shadow, blur scratch
    // passes) are screen effects placed by the game to line up with the 3D scene, which is widened rather than
    // squeezed - squeezing them shows a displaced copy of the scene (box around effects, double vision).
    // Only a draw with texture coordinates samples it: #89 the enemy HP bars are untextured quads drawn while map 0
    // still holds an earlier copy - they stayed unsqueezed (4:3 positions spread over 16:9, off their enemies)
    bool samples_copy = g_map0_copy_tex != 0 && g_gxState.vtxDesc[GX_VA_TEX0] != GX_NONE;
    if (full || samples_copy) mode = 0;
    else if (count) mode = hud_side_for(x0, x1, y0, y1);
    if (mode && count && g_elem_depth > 0 && g_elem_depth <= 16 && (g_elem_stack[g_elem_depth - 1] & 2u)) {   // #146 an HP bar
      if (x1 < g_anchor_box[0] - 2.0f || x0 > g_anchor_box[1] + 2.0f) {   // not touching the last bar: a new bar
        g_anchor_box[0] = x0; g_anchor_box[1] = x1; g_anchor_ndc = (x0 + x1) / 640.0f - 1.0f;
      }
      mode = kSideAnchor;
      static const bool alog = getenv("ANCHOR_LOG") != nullptr;   // #146 test: the HP bar quads and their anchor
      if (alog) { static int n = 0; if (n++ < 400) logf("[anchor] box x[%.1f..%.1f] y[%.1f..%.1f] ndc %.3f f %.3f\n", x0, x1, y0, y1, g_anchor_ndc, g_ws_factor); }
    }
  }
  if (mode != g_proj_mode || (mode == kSideAnchor && g_anchor_ndc != g_proj_anchor)) { put_proj(mode); run_out(); }
  // #146: a world-anchored marker (enemy HP bar, lock-on mark; game_hooks.cpp tags it) is squeezed like the HUD, which puts
  // it on its target in the widened view - also when the target stands outside the 4:3 area, so the 4:3 clip spares it
  const bool anchored = g_elem_depth > 0 && g_elem_depth <= 16 && (g_elem_stack[g_elem_depth - 1] & 1u);
  int clip = mode && settings::g.clip_ui_43 && !anchored ? (mode == kSideAnchor ? kSideCenter : mode) : 0;
  if (clip != g_clip_mode) { set_ui_clip(clip); run_out(); }
}
static bool g_frame_dump = false;      // TRACE_FRAME: dumping the draws of one frame

// TRACE_COPY: log every distinct screen box / texcoord range drawn with an EFB-copy texture (pos f32 xyz + clr + tex f32)
static void trace_copy_quad(const uint8_t* d, uint32_t count, uint32_t vs) {
  if (vs != 24) return;
  auto f = [](const uint8_t* q) { uint32_t u = (q[0] << 24) | (q[1] << 16) | (q[2] << 8) | q[3]; float v; memcpy(&v, &u, 4); return v; };
  float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f, s0 = 1e9f, s1 = -1e9f, t0 = 1e9f, t1 = -1e9f;
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* v = d + i * vs;
    float x = f(v), y = f(v + 4), s = f(v + 16), t = f(v + 20);
    x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y);
    s0 = std::min(s0, s); s1 = std::max(s1, s); t0 = std::min(t0, t); t1 = std::max(t1, t);
  }
  static std::map<std::string, int> seen;
  char buf[200];
  snprintf(buf, sizeof buf, "tex %08X box x[%.0f..%.0f] y[%.0f..%.0f] st s[%.3f..%.3f] t[%.3f..%.3f] alpha=%02X", g_map0_copy_tex, x0, x1, y0, y1,
           s0, s1, t0, t1, d[15]);
  if (seen.size() < 200 && seen[buf]++ == 0) logf("[copy-quad] %s\n", buf);
}
static std::map<uint32_t, uint32_t> g_copy_dests;   // EFB-copy destination -> (w << 16 | h)  (TRACE_COPY)
// FX_SEQ=1 (testing): for 24 frames in a stage, every EFB copy ('C' dest wxh) and the first draw per frame that samples
// each copy ('U' tex, 2D/3D) with the frame number - shows per-frame / alternating screen-effect sequences.
static void fx_seq_event(char kind, uint32_t addr, uint32_t a, uint32_t b) {
  static const bool on = getenv("FX_SEQ") != nullptr;
  if (!on || !game::in_stage()) return;
  static uint64_t first = 0, last_frame = ~0ull;
  static std::set<uint32_t> used;
  if (!first) first = g_xfb_copies + 120;   // skip the first 2 s in the stage
  if (g_xfb_copies < first || g_xfb_copies > first + 24) return;
  if (g_xfb_copies != last_frame) { used.clear(); last_frame = g_xfb_copies; }
  if (kind == 'U' && !used.insert(addr).second) return;
  if (kind == 'C') logf("[fx] frame %llu copy -> %08X %ux%u\n", (unsigned long long)g_xfb_copies, addr, a, b);
  else logf("[fx] frame %llu   use %08X (%s)\n", (unsigned long long)g_xfb_copies, addr, a ? "2D" : "3D");
}
// COPY_PROBE: after the stage loads, every 4 s a different EFB-copy texture is replaced by a non-copy RAM texture
// when sampled (slot 0 = none), to identify which screen-effect pass a visual bug comes from.
static bool g_copy_probe = getenv("COPY_PROBE") != nullptr;
static std::vector<uint32_t> g_probe_order;
static uint64_t g_probe_boot = sys_time_ns();
static uint32_t copy_probe_redirect(uint32_t addr) {
  static uint64_t t0 = 0;
  if (g_mash) return addr;
  if (!t0) t0 = sys_time_ns();
  int slot = (int)(((sys_time_ns() - t0) / 4000000000ull) % 7);
  static int last = -1;
  if (slot != last) {
    last = slot;
    logf("[copy-probe] up=%.1fs slot %d corrupts %08X\n", (sys_time_ns() - g_probe_boot) / 1e9, slot,
         slot && slot <= (int)g_probe_order.size() ? g_probe_order[slot - 1] : 0);
  }
  if (slot && slot <= (int)g_probe_order.size() && g_probe_order[slot - 1] == addr) return 0x00100000;   // arbitrary RAM
  return addr;
}
// EFB-copy destinations vs later reuse of that RAM. Our copies live only on the GPU (Aurora maps the destination
// address to the copied texture forever), but on hardware the copy writes into RAM and a later texture load into the
// same memory replaces it. A sparse fingerprint of the RAM region is taken at copy time; when the address is bound
// as a texture and the RAM no longer matches, the game has reused the memory -> forget the copy mapping so the real
// texture data is used (fixed: menu text garbled after stages that used screen effects at those addresses).
static std::map<uint32_t, uint64_t> g_copy_fp;   // copy dest (physical) -> RAM fingerprint at copy time
static uint64_t ram_fingerprint(uint32_t addr, uint32_t bytes) {
  uint64_t h = 1469598103934665603ull;
  if (!bytes) return h;
  for (uint32_t i = 0; i < 64; ++i) {
    uint32_t off = (uint32_t)(((uint64_t)bytes * i) / 64) & ~7u;
    uint32_t a = (addr + off) & RT_RAM_MASK;
    if (a + 8 > RT_RAM_SIZE) break;
    uint64_t v;
    memcpy(&v, g_ram + a, 8);
    h = (h ^ v) * 1099511628211ull;
  }
  return h;
}
static uint32_t g_texobj_version = 0;   // bumped when a copy mapping is dropped -> Aurora re-resolves binds

static void emit_texobj(int map) {
  uint32_t img0 = g_tex_image0[map];
  uint32_t width = (img0 & 0x3FF) + 1, height = ((img0 >> 10) & 0x3FF) + 1, fmt = (img0 >> 20) & 0xF;
  uint32_t addr = ((g_tex_image3[map] & 0x00FFFFFFu) << 5) & RT_RAM_MASK;
  uint32_t mode0 = g_tex_mode0[map], mode1 = g_tex_mode1[map];
  uint32_t minf = (mode0 >> 5) & 7;
  bool mips = (minf == 1 || minf == 2 || minf == 5 || minf == 6) && ((mode1 >> 8) & 0xFF) > 0;
  uint32_t tlut_slot = 0;
  bool is_ci = fmt == 8 || fmt == 9 || fmt == 10;
  if (is_ci) {
    uint32_t settlut = g_tex_settlut[map];
    uint32_t off = settlut & 0x3FF;
    uint32_t tfmt = (settlut >> 10) & 3;
    auto it = g_tlut_by_offset.find(off);
    if (it != g_tlut_by_offset.end()) {
      if (it->second.slot < 0) it->second.slot = (g_next_tlut_slot++) % 16;
      emit_tlut(it->second.slot, it->second, tfmt);
      tlut_slot = it->second.slot;
    }
  }
  static const bool trace_copy_env = getenv("TRACE_COPY") != nullptr;
  if (!g_copy_dests.empty() && trace_copy_env) {
    auto it = g_copy_dests.find(addr);
    static std::map<uint64_t, int> seen;
    if (it != g_copy_dests.end() &&
        seen[((uint64_t)addr << 24) ^ (width << 12) ^ height ^ ((uint64_t)map << 56) ^ std::hash<std::string>()(g_last_proj + g_last_vp)]++ == 0)
      logf("[copy-use] map%d tex %08X declared %ux%u fmt=%u mode0=%06X (copied as %ux%u) proj %s | vp %s\n", map, addr, width, height, fmt,
           mode0, it->second >> 16, it->second & 0xFFFF, g_last_proj.c_str(), g_last_vp.c_str());
    static const bool trace_tcg_env = getenv("TRACE_COPY_TCG") != nullptr;
    if (it != g_copy_dests.end() && trace_tcg_env) {
      static std::map<uint32_t, int> once;
      if (once[addr]++ == 0) {
        g_dump_next_draw = 2;
        run_out();
        auto& st = aurora::gx::g_gxState;
        logf("    numTexGens=%u\n", (unsigned)st.numTexGens);
        for (unsigned t = 0; t < st.numTexGens && t < 8; ++t) {
          const auto& g = st.tcgs[t];
          logf("    tcg%u type=%d src=%d mtx=%d post=%d norm=%d\n", t, (int)g.type, (int)g.src, (int)g.mtx, (int)g.postMtx, (int)g.normalize);
          if (g.mtx != GX_IDENTITY) {
            const float* m = reinterpret_cast<const float*>(&st.texMtxs[(g.mtx - GX_TEXMTX0) / 3]);
            logf("      texmtx: %g %g %g %g | %g %g %g %g | %g %g %g %g\n", m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11]);
          }
        }
      }
    }
  }
  if (auto it = g_copy_dests.find(addr); it != g_copy_dests.end()) {
    auto fp = g_copy_fp.find(addr);
    uint32_t bytes = (it->second >> 16) * (it->second & 0xFFFF) * 4;
    if (fp != g_copy_fp.end() && fp->second != ram_fingerprint(addr, bytes)) {
      aurora::gx::g_gxState.copyTextures.erase(ram_ptr(addr));
      g_copy_dests.erase(it);
      g_copy_fp.erase(fp);
      ++g_texobj_version;
      static int n = 0;
      if (n++ < 20) logf("[gx] RAM at old EFB-copy destination %08X was reused: using the real texture\n", addr);
    }
  }
  if (map == 0) g_map0_copy_tex = g_copy_dests.count(addr) ? addr : 0;
  static const bool tex_addrs = getenv("TEX_ADDRS") != nullptr;   // testing: where each distinct texture lives in RAM
  if (tex_addrs) {
    static std::set<uint64_t> seen;
    if (seen.insert(((uint64_t)addr << 32) ^ (width << 20) ^ (height << 8) ^ fmt).second && seen.size() < 5000)
      logf("[tex-addr] %08X %ux%u fmt=%u tlut_mem=%08X\n", addr | 0x80000000u, width, height, fmt,
           is_ci && g_tlut_by_offset.count(g_tex_settlut[map] & 0x3FF) ? g_tlut_by_offset[g_tex_settlut[map] & 0x3FF].mem | 0x80000000u : 0);
  }
  if (g_copy_probe) addr = copy_probe_redirect(addr);
  put8(GX_AURORA);
  put16(GX_AURORA_LOAD_TEXOBJ);
  put8((uint8_t)map);
  put64((uint64_t)(uintptr_t)ram_ptr(addr));
  put32(width);
  put32(height);
  put32(fmt);
  put32(tlut_slot);
  put8(mips ? 1 : 0);
  put32(0);                       // texObjId 0: Aurora caches by content hash
  put32(g_texobj_version);        // bumped when a copy mapping is dropped, so that bind is re-resolved
}

// ----------------------------------------------------------------------------- vertex format helpers
static uint32_t vtx_size(int fmt) {
  uint32_t sz = 0;
  const auto& vf = g_gxState.vtxFmts[fmt];
  for (int i = GX_VA_PNMTXIDX; i <= GX_VA_TEX7; ++i) {
    const auto& af = vf.attrs[i];
    switch (g_gxState.vtxDesc[i]) {
      case GX_NONE: break;
      case GX_DIRECT: {
        auto attr = (GXAttr)i;
        sz += aurora::gx::comp_type_size(attr, af.type) * aurora::gx::comp_cnt_count(attr, af.cnt);
        break;
      }
      case GX_INDEX8: sz += (i == GX_VA_NRM && af.cnt == GX_NRM_NBT3) ? 3 : 1; break;
      case GX_INDEX16: sz += (i == GX_VA_NRM && af.cnt == GX_NRM_NBT3) ? 6 : 2; break;
    }
  }
  return sz;
}

// GAUGE_LOG=1 (testing): each distinct gauge draw once - screen box, texture (address, size) and its texcoord range,
// vertex colour - the layout of the game's gauges, to rebuild them from one picture (hud_gauges.cpp)
static void gauge_log(const uint8_t* vtx, uint32_t count, int fmt, float x0, float y0, float x1, float y1) {
  static const bool on = getenv("GAUGE_LOG") != nullptr;
  if (!on || !count || x1 - x0 > 300 || y1 - y0 > 200) return;
  const auto& vf = g_gxState.vtxFmts[fmt];
  int off = 0, tex_off = -1, clr_off = -1;
  for (int i = GX_VA_PNMTXIDX; i <= GX_VA_TEX7; ++i) {
    const auto& af = vf.attrs[i];
    switch (g_gxState.vtxDesc[i]) {
      case GX_NONE: break;
      case GX_DIRECT:
        if (i == GX_VA_TEX0) tex_off = off;
        if (i == GX_VA_CLR0 && af.type == GX_RGBA8) clr_off = off;
        off += aurora::gx::comp_type_size((GXAttr)i, af.type) * aurora::gx::comp_cnt_count((GXAttr)i, af.cnt);
        break;
      case GX_INDEX8: off += 1; break;
      case GX_INDEX16: off += 2; break;
    }
  }
  const uint32_t vs = (uint32_t)off;
  const auto& ta = vf.attrs[GX_VA_TEX0];
  auto comp = [&](const uint8_t* q) -> float {   // one direct texcoord component
    switch (ta.type) {
      case GX_F32: { uint32_t u = (q[0] << 24) | (q[1] << 16) | (q[2] << 8) | q[3]; float v; memcpy(&v, &u, 4); return v; }
      case GX_S16: return (int16_t)((q[0] << 8) | q[1]) / float(1 << ta.frac);
      case GX_U16: return (uint16_t)((q[0] << 8) | q[1]) / float(1 << ta.frac);
      case GX_S8: return (int8_t)q[0] / float(1 << ta.frac);
      default: return q[0] / float(1 << ta.frac);
    }
  };
  const int csz = ta.type == GX_F32 ? 4 : (ta.type == GX_S16 || ta.type == GX_U16) ? 2 : 1;
  float s0 = 1e9f, s1 = -1e9f, t0 = 1e9f, t1 = -1e9f;
  if (tex_off >= 0)
    for (uint32_t i = 0; i < count; ++i) {
      float s = comp(vtx + i * vs + tex_off), t = comp(vtx + i * vs + tex_off + csz);
      s0 = std::min(s0, s); s1 = std::max(s1, s); t0 = std::min(t0, t); t1 = std::max(t1, t);
    }
  uint32_t clr = clr_off >= 0 ? (vtx[clr_off] << 24 | vtx[clr_off + 1] << 16 | vtx[clr_off + 2] << 8 | vtx[clr_off + 3]) : 0;
  char k[300];
  snprintf(k, sizeof k, "%08X x[%.1f..%.1f] y[%.1f..%.1f] tex %08X %ux%u texdesc %d type %d st s[%.4f..%.4f] t[%.4f..%.4f] clr %08X n=%u",
           g_elem_depth > 0 ? g_elem_stack[g_elem_depth - 1] : 0, x0, x1, y0, y1,
           ((g_tex_image3[0] & 0x00FFFFFFu) << 5) & RT_RAM_MASK, (g_tex_image0[0] & 0x3FF) + 1, ((g_tex_image0[0] >> 10) & 0x3FF) + 1,
           (int)g_gxState.vtxDesc[GX_VA_TEX0], (int)ta.type, s0, s1, t0, t1, clr, count);
  static std::set<std::string> seen;
  if (seen.size() < 600 && seen.insert(k).second) logf("[gauge-quad] %s\n", k);
}

static void scan_draw_indices(const uint8_t* p, uint32_t count, int fmt) {
  const auto& vf = g_gxState.vtxFmts[fmt];
  uint32_t maxidx[16];
  bool used[16] = {};
  memset(maxidx, 0, sizeof maxidx);
  // #152: the vertex layout once per draw (it was worked out again for every vertex and attribute: ~5 % of the game
  // thread); then the vertices are walked with it - same bytes, same indices
  struct Step { uint16_t skip; uint8_t width, n; int8_t a; };   // skip: direct bytes; width 1/2: index bytes, n of them, slot a
  Step plan[GX_VA_TEX7 + 1];
  int np = 0;
  for (int i = GX_VA_PNMTXIDX; i <= GX_VA_TEX7; ++i) {
    const auto& af = vf.attrs[i];
    auto t = g_gxState.vtxDesc[i];
    if (t == GX_NONE) continue;
    if (t == GX_DIRECT) {
      auto attr = (GXAttr)i;
      const uint32_t sz = aurora::gx::comp_type_size(attr, af.type) * aurora::gx::comp_cnt_count(attr, af.cnt);
      if (np && plan[np - 1].width == 0) plan[np - 1].skip += (uint16_t)sz;   // consecutive direct attributes: one skip
      else plan[np++] = {(uint16_t)sz, 0, 0, -1};
      continue;
    }
    const int a = i - GX_VA_POS;
    plan[np++] = {0, (uint8_t)(t == GX_INDEX8 ? 1 : 2), (uint8_t)((i == GX_VA_NRM && af.cnt == GX_NRM_NBT3) ? 3 : 1), (int8_t)(a >= 0 && a < 16 ? a : -1)};
    if (a >= 0 && a < 16) used[a] = true;
  }
  for (uint32_t v = 0; v < count; ++v) {
    for (int j = 0; j < np; ++j) {
      const Step& st = plan[j];
      if (!st.width) { p += st.skip; continue; }
      for (int k = 0; k < st.n; ++k) {
        const uint32_t idx = st.width == 1 ? (uint32_t)*p : (uint32_t)((p[0] << 8) | p[1]);
        p += st.width;
        if (st.a >= 0 && idx != 0xFFFF && idx > maxidx[st.a]) maxidx[st.a] = idx;
      }
    }
  }
  for (int a = 0; a < 16; ++a)
    if (used[a]) {
      uint32_t stride = g_gxState.arrays[GX_VA_POS + a].stride;
      if (!stride) stride = 1;
      ensure_array(a, (maxidx[a] + 1) * stride + stride);
    }
}

// ----------------------------------------------------------------------------- BP handling
// Clear the whole EFB (colour black, depth from BP 0x51) at the start of every frame.
// The game never clears with copies; it draws full-screen "clear" quads whose edges sit at x/y = 0.5 (its own
// pixel-centre offset). At 1x that still covers pixel 0, but at an Nx internal resolution render column/row 0 is
// left out. The additive glow passes then keep accumulating into that column every frame, and the 128x128/64x64
// glow textures stretch it into bright stripes along the left and top screen edges. Aurora's display copy ignores
// the clear bit, so do it here with a full-EFB texture copy (to a dummy destination) that clears.
static void clear_efb_black() {
  static uint8_t dummy[64];
  put8(GX_AURORA); put16(GX_AURORA_LOAD_COPY_SRC);
  put32(0); put32(0); put32(640); put32(480);
  put8(GX_AURORA); put16(GX_AURORA_LOAD_COPY_DST);
  put32(16); put32(16); put32(GX_TF_RGBA8); put8(0);
  put8(GX_AURORA); put16(GX_AURORA_LOAD_COPY_DEST);
  put64((uint64_t)(uintptr_t)dummy);
  put8(0x61); put32(0x4F000000u);                         // clear colour A/R = 0
  put8(0x61); put32(0x50000000u);                         // clear colour G/B = 0
  put8(0x61); put32(0x52000800u);                         // texture copy with clear
  put8(0x61); put32(0x4F000000u | g_bp_cache[0x4F]);       // restore the game's clear colour
  put8(0x61); put32(0x50000000u | g_bp_cache[0x50]);
  run_out();
}

static void efb_copy(uint32_t value) {
  bool to_xfb = (value >> 14) & 1;
  bool clear = (value >> 11) & 1;
  uint32_t src_tl = g_bp_cache[0x49], src_wh = g_bp_cache[0x4A];
  uint32_t left = src_tl & 0x3FF, top = (src_tl >> 10) & 0x3FF;
  uint32_t w = (src_wh & 0x3FF) + 1, h = ((src_wh >> 10) & 0x3FF) + 1;
  if (to_xfb) {
    {
      static int target = getenv("TRACE_FRAME") ? atoi(getenv("TRACE_FRAME")) : -1;
      static int n = 0;
      if (target >= 0 && !g_mash) {
        if (g_frame_dump) { g_frame_dump = false; logf("[frame] ---- end ----\n"); }
        if (n++ == target) { g_frame_dump = true; logf("[frame] ---- begin (frame %d after stage load) ----\n", target); }
      }
    }
    {
      static uint32_t last = ~0u;
      uint32_t k = src_tl ^ (src_wh << 1) ^ (g_bp_cache[0x4E] << 7) ^ (value & 0x7FFF);
      if (k != last) {
        last = k;
        logf("[xfb-copy] src=(%u,%u %ux%u) clear=%d yscale(BP4E)=%06X disp_stride(BP4D)=%06X value=%06X\n", left, top, w, h, (int)clear,
             g_bp_cache[0x4E], g_bp_cache[0x4D], value & 0xFFFFFF);
      }
    }
    // display copy: Aurora presents the EFB itself; just honour the clear and end the frame
    put8(0x61); put32(0x52000000u | (value & 0x00FFFFFFu));
    run_out();
    {   // the scene / HUD boundary for the next frame (see FxBoundary)
      fxb_flush();
      if (g_fxb.seq) logf("[fx-seq] boundary object %08X\n", g_fxb.cand);
      g_fxb.learned = g_fxb.cand;
      g_fxb.cand = 0;
      int best = -1;
      for (int i = 0; i < g_fxb.ntally; ++i)
        if (best < 0 || g_fxb.tally[i].n > g_fxb.tally[best].n) best = i;
      g_fxb.proj_ok = best >= 0;
      if (best >= 0) {
        float f[12];
        memcpy(f, g_fxb.tally[best].raw, sizeof f);   // the words are float bits
        memcpy(g_fxb.proj, f, 6 * 4);
        // viewport: XF scale (w/2, -h/2), origin (x + w/2 + 342, y + h/2 + 342) on the logical 640x480 screen
        const float sx = f[6], sy = f[7], ox = f[9] - 342.0f, oy = f[10] - 342.0f;
        g_fxb.proj[6] = (ox - sx) / 640.0f;
        g_fxb.proj[7] = (oy + sy) / 480.0f;
        g_fxb.proj[8] = 2.0f * sx / 640.0f;
        g_fxb.proj[9] = -2.0f * sy / 480.0f;
        if (g_fxb.proj[8] <= 0.0f || g_fxb.proj[9] <= 0.0f) g_fxb.proj_ok = false;
      }
      if (g_fxb.seq && best >= 0)
        logf("[fx-seq] scene projection %g %g %g %g %g %g, viewport %.3f %.3f %.3f %.3f (%u draws)\n", g_fxb.proj[0],
             g_fxb.proj[1], g_fxb.proj[2], g_fxb.proj[3], g_fxb.proj[4], g_fxb.proj[5], g_fxb.proj[6], g_fxb.proj[7],
             g_fxb.proj[8], g_fxb.proj[9], g_fxb.tally[best].n);
      g_fxb.ntally = 0;
      static const bool order = getenv("FX_ORDER") != nullptr;
      g_fxb.seq = order && (g_xfb_copies % 600) == 599;
      if (g_fxb.seq) logf("[fx-seq] frame %llu:\n", (unsigned long long)g_xfb_copies + 1);
    }
    ++g_xfb_copies;
    present_frame();
    clear_efb_black();
    return;
  }
  bool half = (value >> 9) & 1;
  uint32_t dw = half ? (w >> 1) : w, dh = half ? (h >> 1) : h;
  uint32_t peCtrl = g_bp_cache[0x43];
  bool zcopy = (peCtrl & 7) == 3;
  uint32_t fmt = tex_fmt_from_copy(value, zcopy);
  uint32_t dest = (g_bp_cache[0x4B] & 0x1FFFFF) << 5;
  {
    static bool trace = getenv("TRACE_COPY") != nullptr;
    static std::map<uint64_t, int> seen;
    uint64_t key = ((uint64_t)left << 52) ^ ((uint64_t)top << 42) ^ ((uint64_t)w << 31) ^ ((uint64_t)h << 20) ^ ((uint64_t)fmt << 12) ^ (dest >> 5) ^ ((uint64_t)half << 63);
    g_copy_dests[dest & RT_RAM_MASK] = (dw << 16) | dh;   // always: widescreen + reuse detection need it
    fx_seq_event('C', dest & RT_RAM_MASK, w, h);
    g_copy_fp[dest & RT_RAM_MASK] = ram_fingerprint(dest & RT_RAM_MASK, dw * dh * 4);
    if (trace && !g_mash) {   // the per-frame sequence, once the stage is running
      static int n = 0;
      if (n++ < 30) logf("[copy-seq] dest=%08X src=(%u,%u %ux%u) clear=%d clearcolor(BP4F/50)=%06X/%06X\n", dest, left, top, w, h, (int)clear,
                         g_bp_cache[0x4F], g_bp_cache[0x50]);
    }
    if (trace && seen[key]++ == 0)
      logf("[copy] src=(%u,%u %ux%u) dst=%ux%u fmt=%u half=%d clear=%d z=%d dest=%08X stride(BP4D)=%u\n", left, top, w, h, dw, dh, fmt,
           (int)half, (int)clear, (int)zcopy, dest, g_bp_cache[0x4D] & 0x3FF);
  }
  if (g_copy_probe) {
    if (std::find(g_probe_order.begin(), g_probe_order.end(), dest) == g_probe_order.end()) g_probe_order.push_back(dest);
  }
  put8(GX_AURORA); put16(GX_AURORA_LOAD_COPY_SRC);
  put32(left); put32(top); put32(w); put32(h);
  put8(GX_AURORA); put16(GX_AURORA_LOAD_COPY_DST);
  put32(dw); put32(dh); put32(fmt); put8(0);
  put8(GX_AURORA); put16(GX_AURORA_LOAD_COPY_DEST);
  put64((uint64_t)(uintptr_t)ram_ptr(dest));
  put8(0x61); put32(0x52000000u | (value & 0x00FFFFFFu));
  run_out();
}

static void handle_bp(uint32_t value) {
  uint32_t reg = value >> 24;
  g_bp_cache[reg] = value & 0x00FFFFFF;
  if (g_trace && reg != 0xFE) logf("[gx] BP %02X = %06X\n", reg, value & 0xFFFFFF);
  // BLEND_LOG=1: every distinct blend setup (PE_CMODE0) once. Destination-alpha factors (6/7) matter for the FP16
  // (HDR) render path: there alpha can exceed 1.0 after additive blending, unlike the GameCube's clamped EFB.
  if (reg == 0x41) {
    static const bool blend_log = getenv("BLEND_LOG") != nullptr;
    static std::set<uint32_t> seen;
    uint32_t v = value & 0xFFFF;
    if (blend_log && seen.insert(v).second) {
      static const char* fac[8] = {"zero", "one", "src/dstclr", "inv clr", "srcalpha", "invsrcalpha", "DSTALPHA", "INVDSTALPHA"};
      logf("[blend] cmode0 %04X: blend=%u logic=%u(op %u) sub=%u src=%s dst=%s color=%u alpha=%u\n", v, v & 1, (v >> 1) & 1,
           (v >> 12) & 15, (v >> 11) & 1, fac[(v >> 8) & 7], fac[(v >> 5) & 7], (v >> 3) & 1, (v >> 4) & 1);
    }
  }
  {
    static bool trace = getenv("TRACE_COPY") != nullptr;
    static uint32_t last[3] = {~0u, ~0u, ~0u};
    int k = reg == 0x59 ? 0 : reg == 0x20 ? 1 : reg == 0x21 ? 2 : -1;
    if (trace && k >= 0 && last[k] != (value & 0xFFFFFF)) {
      last[k] = value & 0xFFFFFF;
      static int n = 0;
      if (n++ < 200) logf("[bp%02X] %06X\n", reg, value & 0xFFFFFF);
    }
  }
  // textures
  if ((reg >= 0x80 && reg <= 0x9F) || (reg >= 0xA0 && reg <= 0xBF)) {
    uint32_t base = reg >= 0xA0 ? 0xA0 : 0x80;
    uint32_t idx = reg - base;
    uint32_t kind = idx / 4, map = (idx & 3) + (base == 0xA0 ? 4 : 0);
    uint32_t v = value & 0xFFFFFF;
    switch (kind) {
      case 0: g_tex_mode0[map] = v; break;
      case 1: g_tex_mode1[map] = v; break;
      case 2: g_tex_image0[map] = v; break;
      case 5: g_tex_image3[map] = v; break;
      case 6: g_tex_settlut[map] = v; break;
      default: break;
    }
    put8(0x61); put32(value);
    if (kind == 5 || kind == 6) emit_texobj((int)map);
    return;
  }
  switch (reg) {
    case 0x45:   // PE_DONE
      put8(0x61); put32(value);
      run_out();
      pe_finish();
      return;
    case 0x47:
      g_token_value = value & 0xFFFF;
      return;
    case 0x48:
      g_token_value = value & 0xFFFF;
      pe_token();
      return;
    case 0x64: g_tlut_mem = (value & 0x1FFFFF) << 5; break;
    case 0x65: {
      TlutLoad t;
      t.mem = g_tlut_mem;
      t.entries = ((value >> 10) & 0x3FF) * 16;
      g_tlut_by_offset[value & 0x3FF] = t;
      break;
    }
    case 0x52:
      efb_copy(value);
      return;
    default: break;
  }
  put8(0x61); put32(value);
  if ((reg == 0x20 || reg == 0x21) && g_clip_mode) put_scissor(g_clip_mode);   // keep the 4:3 UI clip
}

// ----------------------------------------------------------------------------- CP / XF / draws
static void handle_cp(uint8_t addr, uint32_t value) {
  if (addr >= 0xA0 && addr <= 0xAF) {
    int idx = addr - 0xA0;
    g_array_base[idx] = value & 0x03FFFFFFu;
    g_array_size[idx] = 0;
    emit_arraybase(idx, 64);
    return;
  }
  put8(0x08); put8(addr); put32(value);
}

// Returns bytes consumed, 0 if more data is required.
static size_t parse_command(const uint8_t* p, size_t avail, bool in_dl);

static void run_display_list(uint32_t addr, uint32_t size) {
  const uint8_t* dl = ram_ptr(addr);
  size_t off = 0;
  while (off < size) {
    size_t n = parse_command(dl + off, size - off, true);
    if (n == 0) break;   // truncated display list
    off += n;
  }
}

static size_t parse_command(const uint8_t* p, size_t avail, bool in_dl) {
  uint8_t cmd = p[0];
  uint8_t op = cmd & 0xF8;
  ++g_stat_cmds;
  if (cmd == 0x02 || cmd == 0x03) {   // private: HUD element begin <id> / end (game_hooks.cpp hud_element_draw)
    if (avail < 5) return 0;
    hud_marker(cmd == 0x02, ((uint32_t)p[1] << 24) | (p[2] << 16) | (p[3] << 8) | p[4]);
    return 5;
  }
  if (cmd == 0x04) {   // private: a screen camera begins its update (widescreen.cpp hle_RwCameraBeginUpdate)
    if (avail < 49) return 0;
    float ltm[12];
    for (int i = 0; i < 12; ++i) {
      const uint8_t* q = p + 1 + i * 4;
      const uint32_t u = ((uint32_t)q[0] << 24) | (q[1] << 16) | (q[2] << 8) | q[3];
      memcpy(&ltm[i], &u, 4);
    }
    run_out();   // the draws before it reach Aurora first
    lighting::camera_begin(ltm);
    return 49;
  }
  if (cmd & 0x80) {
    if (avail < 3) return 0;
    uint32_t count = (p[1] << 8) | p[2];
    uint32_t vs = vtx_size(cmd & 7);
    size_t total = 3 + (size_t)count * vs;
    if (avail < total) return 0;
    if (count && g_map0_copy_tex) trace_copy_quad(p + 3, count, vs);
    if (count && g_frame_dump) {
      // TRACE_FRAME=<n>: describe every draw of the n-th displayed frame after the stage loads
      auto& st = aurora::gx::g_gxState;
      const auto& vf = st.vtxFmts[cmd & 7];
      const float* m = reinterpret_cast<const float*>(&st.pnMtx[st.currentPnMtx / 3]);
      char pos[96] = "";
      if (st.vtxDesc[GX_VA_POS] == GX_DIRECT && vf.attrs[GX_VA_POS].type == GX_F32 && st.vtxDesc[GX_VA_PNMTXIDX] == GX_NONE) {
        auto f = [](const uint8_t* q) { uint32_t u = (q[0] << 24) | (q[1] << 16) | (q[2] << 8) | q[3]; float v; memcpy(&v, &u, 4); return v; };
        float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f, z0 = 1e9f, z1 = -1e9f;
        for (uint32_t i = 0; i < count; ++i) {
          const uint8_t* v = p + 3 + i * vs;
          x0 = std::min(x0, f(v)); x1 = std::max(x1, f(v)); y0 = std::min(y0, f(v + 4)); y1 = std::max(y1, f(v + 4));
          z0 = std::min(z0, f(v + 8)); z1 = std::max(z1, f(v + 8));
        }
        snprintf(pos, sizeof pos, "x[%g..%g] y[%g..%g] z[%g..%g]", x0, x1, y0, y1, z0, z1);
      }
      logf("[frame] draw %02X n=%u vs=%u tex0=%08X proj=%d(%g,%g) posmtx t=(%g,%g,%g) s=(%g,%g,%g) %s\n", cmd, count, vs, g_map0_copy_tex,
           (int)st.projType, reinterpret_cast<const float*>(&st.proj)[0], reinterpret_cast<const float*>(&st.proj)[5], m[3], m[7], m[11],
           m[0], m[5], m[10], pos);
      logf("[frame]   blend(41)=%06X zmode(40)=%06X genmode(00)=%06X tex0 img=%08X tev0 c(C0)=%06X a(C1)=%06X alphacmp(F3)=%06X\n",
           g_bp_cache[0x41], g_bp_cache[0x40], g_bp_cache[0x00], (g_bp_cache[0x94] & 0xFFFFFF) << 5, g_bp_cache[0xC0],
           g_bp_cache[0xC1], g_bp_cache[0xF3]);
    }
    {
      // experiment: SKIP_FEEDBACK=<hex texture addr> drops the draws that sample that EFB-copy texture
      // (comma-separated list; only full-screen draws are skipped, so the blur chain itself still runs)
      static std::vector<uint32_t> skip = [] {
        std::vector<uint32_t> v;
        if (const char* s = getenv("SKIP_FEEDBACK"))
          for (const char* q = s; *q;) { v.push_back((uint32_t)strtoul(q, nullptr, 16)); q = strchr(q, ','); if (!q) break; ++q; }
        return v;
      }();
      if (!skip.empty() && g_map0_copy_tex && std::find(skip.begin(), skip.end(), g_map0_copy_tex) != skip.end() && vs == 24) {
        uint32_t u = (p[3] << 24) | (p[4] << 16) | (p[5] << 8) | p[6];
        bool fullscreen = false;
        for (uint32_t i = 0; i < count; ++i) {
          const uint8_t* q = p + 3 + i * vs;
          u = (q[0] << 24) | (q[1] << 16) | (q[2] << 8) | q[3];
          float x; memcpy(&x, &u, 4);
          if (x >= 600.0f) fullscreen = true;
        }
        if (fullscreen) return total;
      }
      // Settings: "Game bloom" off. The game's glow is a full-screen draw of a small (<=256x256) downsampled screen
      // copy; skipping those draws removes the bloom without touching the rest of the effect chain.
      // Settings: bloom strength (game_bloom_strength %, launcher Picture tab): the bloom quad's vertex colour (pos f32 x3,
      // colour, st) x strength; 0 % = no bloom, 100 % = untouched. Immediate draws only (a display list lives in guest memory
      // and would be scaled again every frame). BLOOM_LOG=1 logs the first quads (colour, blend) to check the effect.
      // Bloom quality Smooth (game_bloom_quality 1, default): the small copy (~1/4 - 1/8 of the screen) is upscaled with the
      // game's own filter, which shows as square blocks around lamps. Smooth draws the quad as a 3x3 tent of copies shifted
      // by one copy texel, weights 1-2-1 (sum 1: no new brightness), with the texture filter forced to linear for these
      // draws (BP 0x80 written before and restored after). The strength slider multiplies the weights.
      if (g_game_bloom && settings::g.game_bloom_quality == 1 && !in_dl && g_map0_copy_tex && vs == 24) {
        auto it = g_copy_dests.find(g_map0_copy_tex);
        const uint32_t cw = it != g_copy_dests.end() ? (it->second >> 16) : 0, ch = it != g_copy_dests.end() ? (it->second & 0xFFFF) : 0;
        if (cw && ch && cw <= 256 && ch <= 256) {
          bool fullscreen = false;
          for (uint32_t i = 0; i < count && !fullscreen; ++i) {
            const uint8_t* q = p + 3 + i * vs;
            uint32_t u = (q[0] << 24) | (q[1] << 16) | (q[2] << 8) | q[3];
            float x; memcpy(&x, &u, 4);
            if (x >= 600.0f) fullscreen = true;
          }
          if (fullscreen) {
            const int pct = std::clamp(settings::g.game_bloom_strength, 0, 200);
            if (pct == 0) return total;
            auto rdf = [](const uint8_t* q) { uint32_t u = (q[0] << 24) | (q[1] << 16) | (q[2] << 8) | q[3]; float v; memcpy(&v, &u, 4); return v; };
            auto wrf = [](uint8_t* q, float v) { uint32_t u; memcpy(&u, &v, 4); q[0] = u >> 24; q[1] = u >> 16; q[2] = u >> 8; q[3] = u; };
            auto bp = [&](uint32_t value) { uint8_t b[5] = {0x61, uint8_t(value >> 24), uint8_t(value >> 16), uint8_t(value >> 8), uint8_t(value)}; parse_command(b, 5, true); };
            const uint32_t mode0 = g_tex_mode0[0];
            bp((0x80u << 24) | ((mode0 & ~0xF0u) | 0x10u | (1u << 5)));   // mag linear, min linear (map 0)
            static const float wt[3] = {0.25f, 0.5f, 0.25f};
            static const float sp = getenv("BLOOM_SMOOTH_SPACING") ? (float)atof(getenv("BLOOM_SMOOTH_SPACING")) : 1.5f;   // taps 1.5 copy texels apart
            for (int ty = -1; ty <= 1; ++ty)
              for (int tx = -1; tx <= 1; ++tx) {
                std::vector<uint8_t> tap(p, p + total);
                const float k = wt[tx + 1] * wt[ty + 1] * pct / 100.f;
                for (uint32_t i = 0; i < count; ++i) {
                  uint8_t* v = tap.data() + 3 + i * vs;
                  wrf(v + 16, rdf(v + 16) + sp * float(tx) / float(cw));
                  wrf(v + 20, rdf(v + 20) + sp * float(ty) / float(ch));
                  for (int c = 0; c < 4; ++c) v[12 + c] = (uint8_t)std::clamp((int)std::lround(v[12 + c] * k), 0, 255);
                }
                parse_command(tap.data(), tap.size(), true);
              }
            bp((0x80u << 24) | mode0);   // the game's own filter again
            static const bool blog = getenv("BLOOM_LOG") != nullptr;
            static int logged = 0;
            if (blog && logged++ < 4) logf("[bloom] smooth: copy %ux%u, filter mode0 %06X, 9 taps x%.2f\n", cw, ch, mode0, pct / 100.0);
            return total;
          }
        }
      }
      if (g_game_bloom && settings::g.game_bloom_strength != 100 && !in_dl && g_map0_copy_tex && vs == 24) {
        auto it = g_copy_dests.find(g_map0_copy_tex);
        if (it != g_copy_dests.end() && (it->second >> 16) <= 256 && (it->second & 0xFFFF) <= 256) {
          bool fullscreen = false;
          for (uint32_t i = 0; i < count && !fullscreen; ++i) {
            const uint8_t* q = p + 3 + i * vs;
            uint32_t u = (q[0] << 24) | (q[1] << 16) | (q[2] << 8) | q[3];
            float x; memcpy(&x, &u, 4);
            if (x >= 600.0f) fullscreen = true;
          }
          if (fullscreen) {
            const int pct = std::clamp(settings::g.game_bloom_strength, 0, 200);
            if (pct == 0) return total;
            static const bool blog = getenv("BLOOM_LOG") != nullptr;
            static int logged = 0;
            if (blog && logged++ < 6)
              logf("[bloom] quad tex %08X n=%u rgba %02X%02X%02X%02X blend %06X -> x%.2f\n", g_map0_copy_tex, count, p[3 + 12], p[3 + 13], p[3 + 14],
                   p[3 + 15], g_bp_cache[0x41], pct / 100.0);
            // the quad is additive (ONE / ONE) with white vertex colour: below 100 % its colour is scaled; above, it is drawn
            // once more with the extra share (a copy through this parser, in_dl = true so it is not scaled again)
            auto scale = [&](uint8_t* w, double k) {
              for (uint32_t i = 0; i < count; ++i)
                for (int c = 0; c < 4; ++c) {
                  uint8_t& v = w[3 + i * vs + 12 + c];
                  v = (uint8_t)std::clamp((int)std::lround(v * k), 0, 255);
                }
            };
            if (pct < 100) scale(const_cast<uint8_t*>(p), pct / 100.0);
            if (pct > 100) {
              std::vector<uint8_t> extra(p, p + total);
              scale(extra.data(), (pct - 100) / 100.0);
              parse_command(extra.data(), extra.size(), true);
            }
          }
        }
      }
      if (!g_game_bloom && g_map0_copy_tex && vs == 24) {
        auto it = g_copy_dests.find(g_map0_copy_tex);
        if (it != g_copy_dests.end() && (it->second >> 16) <= 256 && (it->second & 0xFFFF) <= 256) {
          bool fullscreen = false;
          for (uint32_t i = 0; i < count && !fullscreen; ++i) {
            const uint8_t* q = p + 3 + i * vs;
            uint32_t u = (q[0] << 24) | (q[1] << 16) | (q[2] << 8) | q[3];
            float x; memcpy(&x, &u, 4);
            if (x >= 600.0f) fullscreen = true;
          }
          if (fullscreen) return total;
        }
      }
    }
    if (count && g_dump_next_draw) {
      // TRACE_COPY_TCG: describe the draw that follows binding an EFB-copy texture
      --g_dump_next_draw;
      auto& st = aurora::gx::g_gxState;
      const auto& vf = st.vtxFmts[cmd & 7];
      logf("    draw cmd=%02X count=%u vsize=%u pnmtx=%u | POS desc=%d type=%d cnt=%d frac=%d | TEX0 desc=%d type=%d cnt=%d frac=%d | PNMTXIDX desc=%d\n",
           cmd, count, vs, st.currentPnMtx, (int)st.vtxDesc[GX_VA_POS], (int)vf.attrs[GX_VA_POS].type, (int)vf.attrs[GX_VA_POS].cnt,
           (int)vf.attrs[GX_VA_POS].frac, (int)st.vtxDesc[GX_VA_TEX0], (int)vf.attrs[GX_VA_TEX0].type, (int)vf.attrs[GX_VA_TEX0].cnt,
           (int)vf.attrs[GX_VA_TEX0].frac, (int)st.vtxDesc[GX_VA_PNMTXIDX]);
      char hex[3 * 96 + 1] = {0};
      size_t nb = std::min<size_t>((size_t)count * vs, 96);
      for (size_t i = 0; i < nb; ++i) snprintf(hex + i * 3, 4, "%02X ", p[3 + i]);
      logf("      data: %s\n", hex);
      const float* m = reinterpret_cast<const float*>(&st.pnMtx[st.currentPnMtx / 3]);
      logf("      posmtx: %g %g %g %g | %g %g %g %g | %g %g %g %g\n", m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11]);
      const float* pj = reinterpret_cast<const float*>(&st.proj);
      logf("      proj(type %d): %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g\n", (int)st.projType, pj[0], pj[1], pj[2], pj[3], pj[4],
           pj[5], pj[6], pj[7], pj[8], pj[9], pj[10], pj[11], pj[12], pj[13], pj[14], pj[15]);
      const auto& lv = st.logicalViewport;
      const auto& rv = st.renderViewport;
      logf("      aurora viewport: logical %g %g %g %g  render %g %g %g %g  policy %d\n", lv.left, lv.top, lv.width, lv.height, rv.left, rv.top,
           rv.width, rv.height, (int)st.viewportPolicy);
    }
    if (count) {
      if (g_map0_copy_tex) {
        fx_seq_event('U', g_map0_copy_tex, g_proj_ortho ? 1 : 0, 0);
        static const bool fxc = getenv("FX_SEQ") != nullptr;
        if (fxc && vs == 24 && game::in_stage()) {   // colour of every effect quad (pos f32 x3, colour, st)
          static int n = 0;
          static uint64_t first = 0;
          if (!first) first = g_xfb_copies + 120;
          if (g_xfb_copies >= first && g_xfb_copies <= first + 12 && n++ < 400)
            logf("[fx] frame %llu     quad tex %08X n=%u rgba=%02X%02X%02X%02X blend=%06X\n", (unsigned long long)g_xfb_copies,
                 g_map0_copy_tex, count, p[3 + 12], p[3 + 13], p[3 + 14], p[3 + 15], g_bp_cache[0x41]);
        }
      }
      // lighting 'Glowing objects cast light' (lighting_glow.cpp): every additive 3D sprite (blend on, dst factor ONE) with
      // direct float positions reports its centre, size and vertex colour (pos f32 x3, colour at +12 when present)
      if (settings::g.light && settings::g.light_emissive && !g_proj_ortho && !g_map0_copy_tex && (g_bp_cache[0x41] & 1) &&
          ((g_bp_cache[0x41] >> 5) & 7) == 1 && game::in_stage()) {
        auto& st = aurora::gx::g_gxState;
        const auto& vf = st.vtxFmts[cmd & 7];
        // positions: F32 xyz, direct or indexed (8 / 16 bit) into the position array; colour: RGBA8 direct or indexed (else white)
        auto asz = [&](int attr, int comps) -> uint32_t {   // bytes this attribute takes in a vertex
          const int d = (int)st.vtxDesc[attr];
          if (d == GX_INDEX8) return 1;
          if (d == GX_INDEX16) return 2;
          if (d != GX_DIRECT) return 0;
          if (attr == GX_VA_CLR0 || attr == GX_VA_CLR1) {
            const int t = (int)vf.attrs[attr].type;
            return t == GX_RGBA8 || t == GX_RGBX8 ? 4 : t == GX_RGB8 || t == GX_RGBA6 ? 3 : 2;
          }
          const int t = (int)vf.attrs[attr].type;
          return (uint32_t)comps * (t == GX_F32 ? 4 : t == GX_U16 || t == GX_S16 ? 2 : 1);
        };
        const int pd = (int)st.vtxDesc[GX_VA_POS];
        const uint32_t pos_sz = asz(GX_VA_POS, 3), nrm_sz = asz(GX_VA_NRM, 3);
        const auto& pa = st.arrays[GX_VA_POS];
        const bool pos_ok = vf.attrs[GX_VA_POS].type == GX_F32 && vf.attrs[GX_VA_POS].cnt == GX_POS_XYZ &&
                            (pd == GX_DIRECT || ((pd == GX_INDEX8 || pd == GX_INDEX16) && pa.data && pa.stride >= 12));
        if (pos_ok && st.vtxDesc[GX_VA_PNMTXIDX] == GX_NONE && st.vtxDesc[GX_VA_TEX0MTXIDX] == GX_NONE && pos_sz && vs >= pos_sz &&
            st.currentPnMtx < st.pnMtx.size()) {
          auto f = [](const uint8_t* q) { uint32_t u = (q[0] << 24) | (q[1] << 16) | (q[2] << 8) | q[3]; float v; memcpy(&v, &u, 4); return v; };
          auto idx = [](const uint8_t* q, int d) { return d == GX_INDEX8 ? (uint32_t)q[0] : (uint32_t)((q[0] << 8) | q[1]); };
          const int cd = (int)st.vtxDesc[GX_VA_CLR0];
          const auto& ca = st.arrays[GX_VA_CLR0];
          const bool clr = vf.attrs[GX_VA_CLR0].type == GX_RGBA8 &&
                           (cd == GX_DIRECT || ((cd == GX_INDEX8 || cd == GX_INDEX16) && ca.data && ca.stride >= 4));
          const uint32_t clr_off = pos_sz + nrm_sz;
          float c[3] = {0, 0, 0}, lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f}, ce[3] = {0, 0, 0};
          const uint32_t step = std::max<uint32_t>(1, count / 256), parr = pa.size;
          uint32_t n = 0;
          for (uint32_t i = 0; i < count; i += step) {
            const uint8_t* v = p + 3 + i * vs;
            const uint8_t* q = v;
            if (pd != GX_DIRECT) {
              const uint32_t o = idx(v, pd) * pa.stride;
              if (o + 12 > parr) continue;
              q = static_cast<const uint8_t*>(pa.data) + o;
            }
            for (int k = 0; k < 3; ++k) { const float x = f(q + 4 * k); lo[k] = std::min(lo[k], x); hi[k] = std::max(hi[k], x); ce[k] += x; }
            const uint8_t* cc = nullptr;
            if (clr && cd == GX_DIRECT && clr_off + 4 <= vs) cc = v + clr_off;
            else if (clr && cd != GX_DIRECT && clr_off + (cd == GX_INDEX8 ? 1 : 2) <= vs) {
              const uint32_t o = idx(v + clr_off, cd) * ca.stride;
              if (o + 4 <= ca.size) cc = static_cast<const uint8_t*>(ca.data) + o;
            }
            for (int k = 0; k < 3; ++k) c[k] += cc ? cc[k] / 255.f : 1.f;
            ++n;
          }
          if (n) {
            for (int k = 0; k < 3; ++k) { ce[k] /= (float)n; c[k] /= (float)n; }
            float m[12];
            memcpy(m, &st.pnMtx[st.currentPnMtx].pos, sizeof m);
            const float vp[3] = {m[0] * ce[0] + m[1] * ce[1] + m[2] * ce[2] + m[3], m[4] * ce[0] + m[5] * ce[1] + m[6] * ce[2] + m[7],
                                 m[8] * ce[0] + m[9] * ce[1] + m[10] * ce[2] + m[11]};
            const float sx = std::sqrt(m[0] * m[0] + m[4] * m[4] + m[8] * m[8]);
            const float r = 0.5f * sx * std::sqrt((hi[0] - lo[0]) * (hi[0] - lo[0]) + (hi[1] - lo[1]) * (hi[1] - lo[1]) + (hi[2] - lo[2]) * (hi[2] - lo[2]));
            lighting::glow_add(vp, c, r);
          }
        } else {
          static const bool glog = getenv("LIGHT_GLOWLOG") != nullptr;
          static int nlog = 0;
          if (glog && nlog++ < 30)
            logf("[glow] skipped additive draw: pos desc %d type %d cnt %d pnmtxidx %d nrm %d clr0 %d vs %u n %u\n", (int)st.vtxDesc[GX_VA_POS],
                 (int)vf.attrs[GX_VA_POS].type, (int)vf.attrs[GX_VA_POS].cnt, (int)st.vtxDesc[GX_VA_PNMTXIDX], (int)st.vtxDesc[GX_VA_NRM],
                 (int)st.vtxDesc[GX_VA_CLR0], vs, count);
        }
      }
      static const bool no_haze = getenv("WS_NO_HAZE") != nullptr;   // experiment: hide 3D draws sampling an EFB copy
      if (no_haze && g_map0_copy_tex && !g_proj_ortho) return total;
      static const bool no_restore = getenv("WS_NO_RESTORE") != nullptr;   // experiment: skip 2D draws of 640x480 copies
      if (no_restore && g_map0_copy_tex && g_proj_ortho) {
        auto it = g_copy_dests.find(g_map0_copy_tex);
        if (it != g_copy_dests.end() && (it->second >> 16) >= 600) return total;
      }
      static const bool plog = getenv("PAUSE_LOG") != nullptr;
      if (plog && pause_menu::culling()) {   // PAUSE_LOG: every draw while the PC pause menu is up (to find leaks)
        float bx0, by0, bx1, by1;
        bool ok = draw_box_2d(p + 3, count, vs, cmd & 7, bx0, by0, bx1, by1);
        static std::set<std::string> seen;
        char key[200];
        const auto& af = g_gxState.vtxFmts[cmd & 7].attrs[GX_VA_POS];
        const uint32_t i0 = g_tex_image0[0];
        snprintf(key, sizeof key, "ortho=%d box=%s %.0f..%.0f x %.0f..%.0f fmt pos=%d type=%d cnt=%d pnmtx=%d n=%u tex %ux%u f%u @%08X",
                 (int)g_proj_ortho, ok ? "yes" : "no", ok ? bx0 : 0, ok ? bx1 : 0, ok ? by0 : 0, ok ? by1 : 0,
                 (int)g_gxState.vtxDesc[GX_VA_POS], (int)af.type, (int)af.cnt, (int)g_gxState.vtxDesc[GX_VA_PNMTXIDX], count,
                 (i0 & 0x3FF) + 1, ((i0 >> 10) & 0x3FF) + 1, (i0 >> 20) & 0xF, ((g_tex_image3[0] & 0x00FFFFFFu) << 5) & RT_RAM_MASK);
        if (seen.size() < 6000 && seen.insert(key).second) logf("[pause] draw %s\n", key);
      }
      // the PC pause menu replaces the game's: its 2D draws inside the menu's area are dropped (HUD around it stays)
      // ... and its text: the game's font draws each glyph as a 6-vertex quad (direct f32 positions) from a 256x256 C4
      // glyph sheet through the 3D camera's projection (found with PAUSE_LOG) - the 2D filter below never sees it
      if (!g_proj_ortho && pause_menu::culling() && count <= 6 && g_gxState.vtxDesc[GX_VA_POS] == GX_DIRECT &&
          g_gxState.vtxFmts[cmd & 7].attrs[GX_VA_POS].type == GX_F32 && g_gxState.vtxDesc[GX_VA_PNMTXIDX] == GX_NONE &&
          (g_tex_image0[0] & 0x3FF) == 255 && ((g_tex_image0[0] >> 10) & 0x3FF) == 255 && ((g_tex_image0[0] >> 20) & 0xF) == 8)
        return total;
      if (g_proj_ortho && pause_menu::culling()) {
        float bx0, by0, bx1, by1;
        if (draw_box_2d(p + 3, count, vs, cmd & 7, bx0, by0, bx1, by1)) {
          if (pause_menu::cull_2d(bx0, by0, bx1, by1)) return total;
        } else {   // a 2D draw we cannot measure (logged once per vertex format)
          static std::set<std::string> seen;
          const int fmt = cmd & 7;
          const auto& af = g_gxState.vtxFmts[fmt].attrs[GX_VA_POS];
          char key[160];
          snprintf(key, sizeof key, "posdesc=%d type=%d cnt=%d frac=%d pnmtxidx=%d vs=%u tex=%08X", (int)g_gxState.vtxDesc[GX_VA_POS],
                   (int)af.type, (int)af.cnt, (int)af.frac, (int)g_gxState.vtxDesc[GX_VA_PNMTXIDX], vs, ((g_tex_image3[0] & 0x00FFFFFFu) << 5) & RT_RAM_MASK);
          if (seen.size() < 50 && seen.insert(key).second) logf("[pause] unmeasured 2D draw: %s\n", key);
        }
      }
      ws_prepare_draw(p + 3, count, vs, cmd & 7);
      scan_draw_indices(p + 3, count, cmd & 7);
      g_out.insert(g_out.end(), p, p + total);
      run_out();
      ++g_stat_draws;
      fx_order_draw();
    }
    return total;
  }
  switch (op) {
    case 0x00: return 1;
    case 0x08: {
      if (avail < 6) return 0;
      handle_cp(p[1], (p[2] << 24) | (p[3] << 16) | (p[4] << 8) | p[5]);
      run_out();
      return 6;
    }
    case 0x10: {
      if (avail < 5) return 0;
      uint32_t hdr = (p[1] << 24) | (p[2] << 16) | (p[3] << 8) | p[4];
      size_t total = 5 + (size_t)(((hdr >> 16) & 0xFFFF) + 1) * 4;
      if (avail < total) return 0;
      {
        static bool trace = getenv("TRACE_COPY") != nullptr;
        uint32_t addr = hdr & 0xFFFF, cnt = ((hdr >> 16) & 0xFFFF) + 1;
        if (trace && addr <= 0x101A && addr + cnt >= 0x1020) {
          const uint8_t* d = p + 5 + (0x101A - addr) * 4;
          float v[6];
          for (int i = 0; i < 6; ++i) { uint32_t u = (d[i*4] << 24) | (d[i*4+1] << 16) | (d[i*4+2] << 8) | d[i*4+3]; memcpy(&v[i], &u, 4); }
          static std::map<std::string, int> seen;
          char buf[160];
          snprintf(buf, sizeof buf, "sx=%.1f sy=%.1f ox=%.1f oy=%.1f -> left=%.1f top=%.1f w=%.1f h=%.1f", v[0], v[1], v[3], v[4],
                   v[3] - 342 - v[0], v[4] - 342 + v[1], v[0] * 2, -v[1] * 2);
          g_last_vp = buf;
          if (seen[buf]++ == 0) logf("[viewport] %s (xf write at %04X x%u)\n", buf, addr, cnt);
        }
        if (trace && addr <= 0x1020 && addr + cnt >= 0x1027) {
          const uint8_t* d = p + 5 + (0x1020 - addr) * 4;
          float v[7];
          for (int i = 0; i < 7; ++i) { uint32_t u = (d[i*4] << 24) | (d[i*4+1] << 16) | (d[i*4+2] << 8) | d[i*4+3]; memcpy(&v[i], &u, 4); }
          uint32_t type; memcpy(&type, &v[6], 4); type = (d[24] << 24) | (d[25] << 16) | (d[26] << 8) | d[27];
          static std::map<std::string, int> seenp;
          char buf[200];
          snprintf(buf, sizeof buf, "%s p=[%g %g %g %g %g %g]", type ? "ORTHO" : "PERSP", v[0], v[1], v[2], v[3], v[4], v[5]);
          g_last_proj = buf;
          if (seenp.size() < 60 && seenp[buf]++ == 0) logf("[proj] %s\n", buf);
        } else if (trace && addr >= 0x1020 && addr <= 0x1026) {
          static int n2 = 0;
          if (n2++ < 50) logf("[proj] PARTIAL xf write at %04X x%u\n", addr, cnt);
        }
      }
      size_t at = g_out.size();
      g_out.insert(g_out.end(), p, p + total);
      {
        // The game's GX library encodes the viewport origin as xOrig + w/2 + 342 (hardware), Aurora's XF decoder
        // subtracts 340 (matching its own GXSetViewport). Rebase XF 0x101D/0x101E so everything isn't shifted 2 px.
        uint32_t addr = hdr & 0xFFFF, cnt = ((hdr >> 16) & 0xFFFF) + 1;
        // remember the viewport width: --widescreen only corrects projections used for full-screen rendering
        if (addr <= 0x101A && addr + cnt > 0x101A) {
          const uint8_t* d = &g_out[at + 5 + (0x101A - addr) * 4];
          uint32_t u = (d[0] << 24) | (d[1] << 16) | (d[2] << 8) | d[3];
          float sx;
          memcpy(&sx, &u, 4);
          g_vp_width = 2.0f * sx;
        }
        if (addr <= 0x101A && addr + cnt >= 0x1020) {   // the whole viewport (as the game wrote it, before the rebase)
          const uint8_t* d = &g_out[at + 5 + (0x101A - addr) * 4];
          for (int i = 0; i < 6; ++i) g_vp_raw[i] = (d[i * 4] << 24) | (d[i * 4 + 1] << 16) | (d[i * 4 + 2] << 8) | d[i * 4 + 3];
        }
        if (addr <= 0x1020 && addr + cnt >= 0x1027) {
          // remember the game's projection; orthographic (2D) ones are squeezed per draw in ws_prepare_draw()
          const uint8_t* d = &g_out[at + 5 + (0x1020 - addr) * 4];
          for (int i = 0; i < 7; ++i) g_proj_raw[i] = (d[i * 4] << 24) | (d[i * 4 + 1] << 16) | (d[i * 4 + 2] << 8) | d[i * 4 + 3];
          g_proj_ortho = g_proj_raw[6] != 0;
          g_proj_mode = 0;
        }
        // Perspective (3D) widescreen is done at the camera instead (widescreen.cpp: RwCameraSetViewWindow), so that
        // the CPU-side frustum culling widens too. This GPU-only fallback stays available for experiments.
        static const bool ws_xf_persp = getenv("WS_XF_PERSPECTIVE") != nullptr;
        if (ws_xf_persp && g_ws_active && g_ws_factor != 1.0f && g_vp_width >= 600.0f && addr <= 0x1020 && addr + cnt >= 0x1027) {
          uint8_t* d = &g_out[at + 5 + (0x1020 - addr) * 4];
          uint32_t type = (d[24] << 24) | (d[25] << 16) | (d[26] << 8) | d[27];
          if (type == 0) {   // perspective: x_clip = p0*x + p1*z -> scale both
            for (int i = 0; i < 2; ++i) {
              uint8_t* q = d + i * 4;
              uint32_t u = (q[0] << 24) | (q[1] << 16) | (q[2] << 8) | q[3];
              float f;
              memcpy(&f, &u, 4);
              f *= g_ws_factor;
              memcpy(&u, &f, 4);
              q[0] = (uint8_t)(u >> 24); q[1] = (uint8_t)(u >> 16); q[2] = (uint8_t)(u >> 8); q[3] = (uint8_t)u;
            }
          }
        }
        for (uint32_t r = 0x101D; r <= 0x101E; ++r) {
          if (r < addr || r >= addr + cnt) continue;
          uint8_t* d = &g_out[at + 5 + (r - addr) * 4];
          uint32_t u = (d[0] << 24) | (d[1] << 16) | (d[2] << 8) | d[3];
          float f;
          memcpy(&f, &u, 4);
          f -= 2.0f;
          memcpy(&u, &f, 4);
          d[0] = (uint8_t)(u >> 24); d[1] = (uint8_t)(u >> 16); d[2] = (uint8_t)(u >> 8); d[3] = (uint8_t)u;
        }
      }
      run_out();
      return total;
    }
    case 0x20: case 0x28: case 0x30: case 0x38: {
      if (avail < 5) return 0;
      uint32_t srcidx = (p[1] << 8) | p[2];
      uint32_t addrlen = (p[3] << 8) | p[4];
      uint32_t len = (addrlen >> 12) + 1;
      int arr = 12 + (op - 0x20) / 8;
      uint32_t stride = g_gxState.arrays[GX_VA_POS + arr].stride;
      ensure_array(arr, srcidx * (stride ? stride : 1) + len * 4);
      g_out.insert(g_out.end(), p, p + 5);
      run_out();
      return 5;
    }
    case 0x40: {
      if (avail < 9) return 0;
      uint32_t a = ((p[1] << 24) | (p[2] << 16) | (p[3] << 8) | p[4]) & 0x03FFFFFF;
      uint32_t s = (p[5] << 24) | (p[6] << 16) | (p[7] << 8) | p[8];
      if (!in_dl) run_display_list(a, s);
      return 9;
    }
    case 0x48:
      g_out.push_back(0x48);
      run_out();
      return 1;
    case 0x60:
      if (cmd != 0x61) { return 1; }
      if (avail < 5) return 0;
      handle_bp(((uint32_t)p[1] << 24) | (p[2] << 16) | (p[3] << 8) | p[4]);
      run_out();
      return 5;
    default:
      logf("[gx] unknown FIFO opcode %02X (skipping)\n", cmd);
      return 1;
  }
}

static void pump() {
  size_t off = 0;
  while (off < g_pending.size()) {
    size_t n = parse_command(g_pending.data() + off, g_pending.size() - off, false);
    if (n == 0) break;
    off += n;
  }
  if (off) g_pending.erase(g_pending.begin(), g_pending.begin() + off);
}

static uint64_t g_finish_count = 0, g_token_count = 0;
static void dump_pending();
static size_t gp_queue_bytes();   // below (PE / CP register interface)
static uint32_t gp_queue_front();
void print_stats() {
  dump_pending();
  logf("     gx: cmds=%llu draws=%llu xfb_copies=%llu finish=%llu token=%llu pending=%zu pe5=%04X cp1=%04X halted=%d bp_status=%d gpq=%zu bytes (front %08X) bp=%04X%04X wptr=%08X\n",
       (unsigned long long)g_stat_cmds, (unsigned long long)g_stat_draws, (unsigned long long)g_xfb_copies,
       (unsigned long long)g_finish_count, (unsigned long long)g_token_count, g_pending.size(), g_pe[5], g_cp[1],
       (int)g_gp_halted, (int)g_bp_status, gp_queue_bytes(), gp_queue_front(), g_cp[0x1F], g_cp[0x1E], hw::S.pi_fifo_wptr);
}

void dump_pending() {
  if (g_pending.empty()) return;
  char hex[256] = {0};
  size_t n = std::min<size_t>(g_pending.size(), 40);
  for (size_t i = 0; i < n; ++i) snprintf(hex + i * 3, 4, "%02X ", g_pending[i]);
  logf("     gx pending bytes: %s\n", hex);
  uint8_t cmd = g_pending[0];
  if (cmd & 0x80) {
    uint32_t count = g_pending.size() >= 3 ? (g_pending[1] << 8) | g_pending[2] : 0;
    logf("     gx pending draw: cmd=%02X fmt=%d count=%u vtx_size=%u (need %zu bytes, have %zu)\n", cmd, cmd & 7, count, vtx_size(cmd & 7), (size_t)3 + count * vtx_size(cmd & 7), g_pending.size());
    for (int i = GX_VA_PNMTXIDX; i <= GX_VA_TEX7; ++i) {
      auto t = g_gxState.vtxDesc[i];
      if (t != GX_NONE) logf("       attr %d desc=%d type=%d cnt=%d\n", i, (int)t, (int)g_gxState.vtxFmts[cmd & 7].attrs[i].type, (int)g_gxState.vtxFmts[cmd & 7].attrs[i].cnt);
    }
  }
}

// ----------------------------------------------------------------------------- frame presentation
static void apply_pending_fullscreen();
void present_frame() {
  if (!g_aurora_up) return;
  static const char* fs_test_env = getenv("FS_TEST");
  if (const char* t = fs_test_env) {   // test hook: toggle fullscreen at T s and back at T+8 s after launch
    static uint64_t t0 = sys_time_ns();
    static int done = 0;
    double s = (sys_time_ns() - t0) / 1e9, at = atof(t);
    if ((done == 0 && s > at) || (done == 1 && s > at + 8)) { ++done; toggle_fullscreen(); }
  }
  // frame limiter: a skipped frame keeps recording into the current Aurora frame (the game keeps running at 60)
  if (g_frame_active && settings::skip_present()) return;
  uint64_t prof_t0 = g_prof_on ? sys_time_ns() : 0;
  struct PT { uint64_t t; ~PT() { if (t) g_prof_ns[2] += sys_time_ns() - t; } } prof_present{prof_t0};
  if (g_frame_active) {
    settings::draw();
    aurora_end_frame();
    engine_limits::frame();   // #154 BUF_LOG: peak per-frame GPU buffer use (engine_limits.cpp)
    first_boot_hint::frame();   // #155 'Press Esc or Back for settings' until the menu was opened once
  }
  g_frame_active = false;
  apply_pending_fullscreen();
  if (g_render_height == 0 && g_window) {   // Auto: render at the window's own pixel height, follow resizes
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(g_window, &pw, &ph);
    static int last_h = -1;
    if (ph > 0 && ph != last_h) { last_h = ph; apply_display_settings(); }
  }
  // Cutscene bars: in widescreen, in-engine cutscenes are shown as the original 4:3 picture (pillarboxed), since
  // their sets end beyond the 4:3 frame (skybox edges, missing scenery). Switch the render mode around them.
  if (g_widescreen) {
    bool want = !(settings::g.cutscene_bars && game::cutscene_active());
    if (want != g_ws_active) { logf("[gx] %s\n", want ? "cutscene ended: widescreen" : "cutscene: 4:3 with bars"); apply_display_settings(); }
  }
  if (g_ws_active && g_window) {
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(g_window, &pw, &ph);
    if (pw > 0 && ph > 0) g_ws_factor = std::min(1.0f, (4.0f / 3.0f) / ((float)pw / (float)ph));
  } else {
    g_ws_factor = 1.0f;
  }
  const AuroraEvent* ev = aurora_update();
  while (ev && ev->type != AURORA_NONE) {
    if (ev->type == AURORA_EXIT) { logf("[gx] window close requested\n"); sys_dump_state("window closed"); sys_exit_guarded(0); }
    ++ev;
  }
  g_frame_active = aurora_begin_frame();
  if (g_frame_active) lighting::frame_start();
}

// The launcher (launcher.cpp) draws whole frames of its own before the game boots: events, one ImGui frame, present.
// Returns false when the window was asked to close.
bool ui_frame(void (*draw)()) {
  if (!g_aurora_up) return false;
  const AuroraEvent* ev = aurora_update();
  while (ev && ev->type != AURORA_NONE) {
    if (ev->type == AURORA_EXIT) return false;
    ++ev;
  }
  apply_pending_fullscreen();
  if (!g_frame_active) g_frame_active = aurora_begin_frame();
  if (!g_frame_active) return true;   // minimised: nothing to draw
  draw();
  aurora_end_frame();
  g_frame_active = false;
  return true;
}
// The launcher is done: the game's frames start (present_frame expects a frame in progress).
static void size_game_window();
void ui_resume_game() {
  if (g_launcher_mode && g_window) {   // the launcher's compact window becomes the game window
    g_launcher_mode = false;
    SDL_SetWindowMinimumSize(g_window, 0, 0);
    size_game_window();
    if (settings::g.width > 0 && settings::g.height > 0) resize_window_pixels(settings::g.width, settings::g.height);
    SDL_SyncWindow(g_window);
    apply_display_settings();
    if (settings::g.fullscreen && !is_fullscreen()) toggle_fullscreen();
  }
  if (g_aurora_up && !g_frame_active) g_frame_active = aurora_begin_frame();
  if (g_frame_active) lighting::frame_start();
}

void present_frame_events() {
  if (!g_aurora_up) return;
  const AuroraEvent* ev = aurora_update();
  while (ev && ev->type != AURORA_NONE) {
    if (ev->type == AURORA_EXIT) { logf("[gx] window close requested\n"); sys_dump_state("window closed"); sys_exit_guarded(0); }
    ++ev;
  }
}

static void log_cb(AuroraLogLevel level, const char* module, const char* message, unsigned int len) {
  if (level == LOG_DEBUG && !g_trace) return;
  if (len >= 11 && strstr(std::string(message, len).c_str(), "display copy")) return;   // logged every frame (handled by us)
  static const char* names[] = {"DEBUG", "INFO", "WARN", "ERROR", "FATAL"};
  logf("[aurora %s %s] %.*s\n", names[level], module, (int)len, message);
  if (level == LOG_FATAL) { fflush(stderr); _exit(4); }
}

SDL_Window* g_window = nullptr;
int g_render_height = 0;   // render resolution height in pixels (0 = Auto: the window's pixel height)
bool g_widescreen = false;   // --widescreen: 16:9 (perspective corrected, 2D stretched) - the user's setting
bool g_ws_active = false;    // widescreen rendering right now (off during cutscenes with bars)
bool g_game_bloom = true;    // settings: the game's own glow/bloom pass
// Fullscreen is only requested here (from the input poll, i.e. in the middle of a frame); it is applied in
// present_frame() between aurora_end_frame() and aurora_update(), when no frame is being recorded. Switching it
// mid-frame hung the renderer (the swapchain resize raced the frame in flight).
static bool g_fs_toggle_pending = false;
void toggle_fullscreen() { g_fs_toggle_pending = true; }
static void apply_pending_fullscreen() {
  if (!g_fs_toggle_pending || !g_window) return;
  g_fs_toggle_pending = false;
  bool fs = (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN) != 0;
  logf("[gx] %s fullscreen\n", fs ? "leaving" : "entering");
  SDL_SetWindowFullscreen(g_window, !fs);
  SDL_SyncWindow(g_window);
}

// Render at an exact integer multiple of 640x480 (scaled to the window at present). The game feeds the previous
// frame back through EFB copies (motion-blur style); at a fractional scale every pass resamples slightly and the
// error accumulates into a smear along the screen edges. Callable at any time (settings UI).
// Render height in pixels: the chosen resolution, or the window's own pixel height (Auto). Any value works: the
// render-grid bias (render_pixel_bias in Aurora's gx.cpp) is derived per axis from whatever scale results.
int effective_render_height() {
  if (g_render_height > 0) return g_render_height;
  int pw = 0, ph = 0;
  if (g_window) SDL_GetWindowSizeInPixels(g_window, &pw, &ph);
  return ph > 0 ? ph : 480;
}
void apply_display_settings() {
  int h = effective_render_height();
  aurora::window::set_frame_buffer_scale((float)h / 480.0f);
  g_ws_active = g_widescreen && !(settings::g.cutscene_bars && game::cutscene_active());
  if (g_ws_active) {
    // widescreen: the framebuffer follows the window aspect and the logical 640x480 screen is stretched onto it;
    // perspective projections are corrected where written (XF 0x1020), 2D is squeezed per draw (ws_prepare_draw).
    aurora::window::set_frame_buffer_aspect_fit(false);
    AuroraSetViewportPolicy(AURORA_VIEWPORT_STRETCH);
    logf("[gx] widescreen: render height %d, width follows the window aspect\n", h);
  } else {
    aurora::window::set_frame_buffer_aspect_fit(true);
    AuroraSetViewportPolicy(AURORA_VIEWPORT_FIT);
    logf("[gx] render resolution %dx%d (4:3)\n", h * 4 / 3, h);
  }
}
float widescreen_camera_factor() { return (g_ws_active && g_ws_factor > 0.0f && g_ws_factor < 0.999f) ? 1.0f / g_ws_factor : 1.0f; }
bool is_fullscreen() { return g_window && (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN) != 0; }
void resize_window_pixels(int w, int h) {
  if (!g_window || is_fullscreen() || g_launcher_mode) return;
  float ds = SDL_GetWindowDisplayScale(g_window);   // SDL window sizes are in points; convert from pixels
  if (ds <= 0) ds = 1;
  int pw = (int)(w / ds + 0.5f), ph = (int)(h / ds + 0.5f);
  SDL_Rect r;
  if (SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(g_window), &r)) {   // keep it on screen, same aspect
    float k = std::min(1.0f, std::min((float)r.w / pw, (float)(r.h - 40) / ph));
    pw = (int)(pw * k);
    ph = (int)(ph * k);
  }
  SDL_SetWindowSize(g_window, pw, ph);
  SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
}
// windowed: resize the window to the current aspect (keeps the height)
void fit_window_to_aspect() {
  if (!g_window || is_fullscreen() || g_launcher_mode) return;
  int w = 0, h = 0;
  SDL_GetWindowSize(g_window, &w, &h);
  SDL_SetWindowSize(g_window, g_widescreen ? h * 16 / 9 : h * 4 / 3, h);
}

// 4:3 (or 16:9 with --widescreen) window at ~85% of the usable desktop height, centred
static void size_game_window() {
  if (!g_window || is_fullscreen()) return;
  SDL_Rect r;
  if (SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(g_window), &r)) {
    int h = r.h * 85 / 100, w = g_widescreen ? h * 16 / 9 : h * 4 / 3;
    if (w > r.w * 95 / 100) { w = r.w * 95 / 100; h = g_widescreen ? w * 9 / 16 : w * 3 / 4; }
    SDL_SetWindowSize(g_window, w, h);
    SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
  }
}

bool g_launcher_mode = false;
void init_window() {
  if (g_aurora_up) return;   // the launcher (launcher.cpp) opens the window before the game boots
  AuroraConfig cfg{};
  cfg.appName = "Shadow the Hedgehog (recomp)";
  cfg.logCallback = log_cb;
  cfg.logLevel = LOG_INFO;
  cfg.windowWidth = 1280;
  cfg.windowHeight = 960;
  cfg.mem1Size = 0;
  cfg.mem2Size = 0;
  cfg.vsync = false;
  cfg.hdr = settings::g.hdr;                  // HDR output groundwork (Aurora gpu.cpp: FP16 scRGB + output transform)
  cfg.hdrPaperWhiteNits = (float)settings::g.hdr_paper_white;
  cfg.hdrPeakNits = (float)settings::g.hdr_peak;
  // TEX_DUMP=1 (testing): Aurora logs the replacement key of every texture used ("missing runtime key") and dumps it
  // as DDS into <exe dir>\cache\texture_dumps - used to check tools/texture_dump.py names against the real keys.
  static std::string cache_dir;
  // Private diagnostics can use a separate cache without touching the player's pipeline/GPU databases.
  const char* cache_override = getenv("SHADOW_CACHE_DIR");
  if (cache_override && *cache_override) {
    cache_dir = cache_override;
    cfg.cachePath = cache_dir.c_str();
  } else if (getenv("TEX_DUMP")) {
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    cache_dir = exe;
    cache_dir = cache_dir.substr(0, cache_dir.find_last_of("\\/")) + "\\cache";
    cfg.cachePath = cache_dir.c_str();
    cfg.allowTextureDumps = true;
  }
  if (g_quiet) SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");   // --quiet: do not steal focus
  static char arg0[] = "shadow";
  static char* argv[] = {arg0, nullptr};
  lighting::register_builtin_modules();   // cached lit pipelines must see all modules before warmup starts
  AuroraInfo info = aurora_initialize(1, argv, &cfg);
  g_window = info.window;
  if (g_window) {
    // Windows 11 draws a light 1-px outline around windows (looked like a white border on the right/bottom edge of
    // the picture). DWMWA_BORDER_COLOR = DWMWA_COLOR_NONE removes it.
    if (HWND hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(g_window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr)) {
      COLORREF none = 0xFFFFFFFE;   // DWMWA_COLOR_NONE
      DwmSetWindowAttribute(hwnd, 34 /* DWMWA_BORDER_COLOR */, &none, sizeof none);
    }
  }
  if (g_window && g_launcher_mode) {
    // the launcher: a compact window (points), centred; the game size is applied when Play is pressed
    int w = 0, h = 0;
    settings::launcher_window_size(&w, &h);
    SDL_SetWindowSize(g_window, w, h);
    SDL_SetWindowMinimumSize(g_window, w * 3 / 4, h * 3 / 4);
    SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
  } else size_game_window();
  if (g_window) SDL_SyncWindow(g_window);
  settings::init_fonts();   // before the first ImGui frame; after sizing (the launcher's fonts follow its window)
  apply_display_settings();
  // --quiet (tests): keep the window behind everything and never take focus, so the user can keep working
  if (g_quiet && g_window) {
    if (HWND hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(g_window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr)) {
      SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
      if (HWND fg = GetForegroundWindow(); fg && fg != hwnd) SetWindowPos(fg, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    }
  }
  g_aurora_up = true;
  g_frame_active = aurora_begin_frame();
  logf("[gx] aurora initialised, frame active=%d\n", (int)g_frame_active);
  settings::apply_startup();
}

// ----------------------------------------------------------------------------- PE / CP register interface
bool g_bp_status = false;
bool g_gp_halted = false;
bool g_gp_no_halt = false;           // custom frame rate: a long-parked GP passes its breakpoint (gp_run)
std::vector<uint8_t> g_halt_q;       // (unused; kept for the stats line)
struct Block { uint32_t addr; uint8_t d[32]; };
std::deque<Block> g_gp_queue;       // committed 32-byte FIFO blocks the GP has not read yet
uint32_t g_rd_ptr = 0;              // CP read pointer (physical)
static size_t gp_queue_bytes() { return g_gp_queue.size() * 32; }
static uint32_t gp_queue_front() { return g_gp_queue.empty() ? 0u : g_gp_queue.front().addr; }

void resume_gp() { g_gp_halted = false; }   // the GP continues at its next run (it is asynchronous)

// The GP consumes the FIFO asynchronously: it lags the CPU, which is what the game's breakpoint
// based resource tracking relies on.
//
// Custom frame rate (frame_rate.cpp): RenderWare parks the GP at a breakpoint at the start of the next frame and moves
// it from the retrace callback once the frame being shown has drawn (draw-sync token). This GP only runs when polled,
// so at high rates it can be parked before a token the callback is waiting for while the FIFO overflow interrupt has
// suspended the CPU: nothing can move. Real hardware avoids it by timing. In that mode a GP parked for longer than a
// few frames' worth of time passes the breakpoint (as a GP that got there earlier would have) - g_gp_no_halt.
void gp_run() {
  struct T { uint64_t t = g_prof_on ? sys_time_ns() : 0; ~T() { if (t) g_prof_ns[0] += sys_time_ns() - t; } } prof_timer;
  bool bp_on = (g_cp[1] & 2) != 0;
  uint32_t bp = (((uint32_t)g_cp[0x1F] << 16) | g_cp[0x1E]) & 0x03FFFFE0u;
  static uint64_t halted_since = 0;
  static uint32_t pass_bp = ~0u;   // breakpoint address the GP was let through
  // #114 fail-soft: at the original rate too, a GP parked for 2 s (a frame never takes that long) is let through instead
  // of the game waiting on its draw-sync token forever
  if (g_gp_halted) {
    uint64_t now = sys_time_ns();
    if (!halted_since) halted_since = now;
    else if (now - halted_since > (g_gp_no_halt ? 100000000ull : 2000000000ull)) {
      static int n = 0;
      if (n++ < 5) logf("[gx] GP parked at breakpoint %08X for %.0f ms (%s): passing it\n", bp, (now - halted_since) / 1e6,
                        g_gp_no_halt ? "custom frame rate" : "stall guard");
      g_gp_halted = false;
      pass_bp = bp;
    }
  }
  if (!g_gp_halted) halted_since = 0;
  while (!g_gp_halted && !g_gp_queue.empty()) {
    Block& b = g_gp_queue.front();
    if (bp_on && b.addr == bp) {
      if (pass_bp == bp) {
        pass_bp = ~0u;   // once
      } else {
        g_gp_halted = true;
        g_bp_status = true;
        if (g_trace) logf("[gx] GP halted at breakpoint %08X\n", bp);
        break;
      }
    }
    g_pending.insert(g_pending.end(), b.d, b.d + 32);
    g_gp_queue.pop_front();
    pump();
  }
  g_rd_ptr = g_gp_queue.empty() ? (hw::S.pi_fifo_wptr & 0x03FFFFFFu) : g_gp_queue.front().addr;
}
void pe_finish() { g_pe_finish_status = true; ++g_finish_count; }
void pe_token() { g_pe_token_status = true; ++g_token_count; g_pe[7] = (uint16_t)g_token_value; }
uint64_t xfb_count() { return g_xfb_copies; }
}  // namespace gxlle

// ======================================================================================= hw glue
namespace hw {
// FIFO watermarks (CP_SR bit0 = over the high watermark, bit1 = under the low watermark). The GX library takes the
// overflow interrupt to suspend the writing thread and the underflow one to resume it; without them the CPU can lap
// the GP in the FIFO ring and a breakpoint then matches an older lap's block at the same address (deadlocks the
// game's frame ring in-stage).
static uint16_t cp_watermark_bits() {
  uint32_t dist = (uint32_t)gxlle::g_gp_queue.size() * 32;
  uint32_t hi = (((uint32_t)gxlle::g_cp[0x15] << 16) | gxlle::g_cp[0x14]) & 0x03FFFFE0u;
  uint32_t lo = (((uint32_t)gxlle::g_cp[0x17] << 16) | gxlle::g_cp[0x16]) & 0x03FFFFE0u;
  uint16_t v = 0;
  if (hi && dist > hi) v |= 1;
  if (dist < lo) v |= 2;
  return v;
}
uint16_t cp_read16(uint32_t off) {
  uint32_t i = (off >> 1) & 0x3F;
  if (i == 0) return (uint16_t)(0x000C | (gxlle::g_bp_status ? 0x10 : 0) | cp_watermark_bits());   // CP_SR
  if (i == 28) return (uint16_t)gxlle::g_rd_ptr;                             // CP_READ_PTR
  if (i == 29) return (uint16_t)(gxlle::g_rd_ptr >> 16);
  if (i == 24 || i == 25) {                                                  // CP read/write distance
    uint32_t cnt = (uint32_t)gxlle::g_gp_queue.size() * 32;
    return (uint16_t)(i == 24 ? cnt : cnt >> 16);
  }
  return gxlle::g_cp[i];
}
void cp_write16(uint32_t off, uint16_t v) {
  uint32_t i = (off >> 1) & 0x3F;
  gxlle::g_cp[i] = v;
  static const bool trace_bp = getenv("TRACE_BP") != nullptr;
  if (trace_bp && (i == 0x1E || i == 1)) {
    static int n = 0;
    if (n++ < 12) {
      logf("[bp-trace] CP reg %02X <- %04X (bp=%08X)\n", i, v, (((uint32_t)gxlle::g_cp[0x1F] << 16) | gxlle::g_cp[0x1E]));
      if (Cpu* cc = sched_current_cpu()) sys_backtrace(*cc);
    }
  }
  if (i == 1) {                              // CP_CR
    if (gxlle::g_trace) logf("[gx] CP_CR <- %04X  bp=%08X wptr=%08X halted=%d\n", v, ((uint32_t)gxlle::g_cp[0x1F] << 16) | gxlle::g_cp[0x1E], hw::S.pi_fifo_wptr, (int)gxlle::g_gp_halted);
    if (!(v & 2)) {                          // breakpoint disabled: clear status and let the GP run on
      gxlle::g_bp_status = false;
      gxlle::resume_gp();
    }
  }
}

uint16_t pe_read16(uint32_t off) {
  uint32_t i = (off >> 1) & 0x3F;
  if (i == 5) {
    uint16_t v = gxlle::g_pe[5] & 0x3;
    if (gxlle::g_pe_token_status) v |= 4;
    if (gxlle::g_pe_finish_status) v |= 8;
    return v;
  }
  if (i == 7) return (uint16_t)gxlle::g_token_value;
  return gxlle::g_pe[i];
}
void pe_write16(uint32_t off, uint16_t v) {
  uint32_t i = (off >> 1) & 0x3F;
  if (i == 5) {
    gxlle::g_pe[5] = v & 3;
    if (v & 4) gxlle::g_pe_token_status = false;
    if (v & 8) gxlle::g_pe_finish_status = false;
    return;
  }
  gxlle::g_pe[i] = v;
}
uint32_t cp_pe_intsr() {
  uint32_t v = 0;
  if (gxlle::g_pe_token_status && (gxlle::g_pe[5] & 1)) v |= (1u << 9);
  if (gxlle::g_pe_finish_status && (gxlle::g_pe[5] & 2)) v |= (1u << 10);
  if (gxlle::g_bp_status && (gxlle::g_cp[1] & 0x20)) v |= (1u << 11);     // CP breakpoint interrupt
  uint16_t wm = cp_watermark_bits();
  if (((wm & 1) && (gxlle::g_cp[1] & 0x04)) || ((wm & 2) && (gxlle::g_cp[1] & 0x08))) v |= (1u << 11);   // FIFO over/underflow
  return v;
}
uint32_t efb_peek(uint32_t ea, int size) {
  static int n = 0;
  if (n++ < 30) {
    logf("[efb] peek %08X size %d (#%d)\n", ea, size, n);
    if (Cpu* cc = sched_current_cpu()) sys_backtrace(*cc);
  }
  return 0;
}
void efb_poke(uint32_t, uint32_t, int) {}
void gx_init() { gxlle::init_window(); }
void vsync() {
  // SDL events (keyboard / pad state / window close); at most every 4 ms when retraces are fast (high frame rates)
  static uint64_t last_events = 0;
  uint64_t now = sys_time_ns();
  if (now - last_events >= 4000000ull) { last_events = now; gxlle::present_frame_events(); }
  si_vsync();                        // controller polling
}
void vi_xfb_changed() {}
}  // namespace hw

static uint32_t g_wgp_total = 0;
static uint8_t g_wgbuf[32];
static uint32_t g_wglen = 0;
void hw_wgp_write(uint32_t v, int size) {
  // The write-gather pipe collects 32-byte blocks; only complete blocks reach the PI FIFO.
  uint8_t* const wgbuf = g_wgbuf;
  uint32_t& wglen = g_wglen;
  int n = size;
  if (wglen + (uint32_t)n < 32 && (n == 1 || n == 2 || n == 4)) {   // #152: no block completes - the bytes straight in
    if (n == 4) { const uint32_t be = _byteswap_ulong(v); memcpy(wgbuf + wglen, &be, 4); }
    else if (n == 2) { wgbuf[wglen] = (uint8_t)(v >> 8); wgbuf[wglen + 1] = (uint8_t)v; }
    else wgbuf[wglen] = (uint8_t)v;
    wglen += n; g_wgp_total += n;
    return;
  }
  uint8_t b[4];
  for (int i = 0; i < n; ++i) b[i] = (uint8_t)(v >> (8 * (n - 1 - i)));
  for (int i = 0; i < n; ++i) {
    wgbuf[wglen++] = b[i];
    if (wglen < 32) continue;
    wglen = 0;
    // ---- commit one block into the PI FIFO ring
    uint32_t base = hw::S.pi_fifo_base & 0x03FFFFE0u, end = hw::S.pi_fifo_end & 0x03FFFFFFu;
    uint32_t wp = hw::S.pi_fifo_wptr & 0x03FFFFFFu;
    uint32_t wrap = hw::S.pi_fifo_wptr & 0x04000000u;
    uint32_t addr = wp & ~31u;
    if ((addr & RT_RAM_MASK) + 32 <= RT_RAM_SIZE) memcpy(g_ram + (addr & RT_RAM_MASK), wgbuf, 32);
    wp += 32;
    if (end && wp >= end + 4) { wp = base; wrap ^= 0x04000000u; }
    hw::S.pi_fifo_wptr = wp | wrap;
    if (!(gxlle::g_cp[1] & 0x10)) continue;              // not linked: data just sits in memory
    gxlle::Block blk;
    blk.addr = addr;
    memcpy(blk.d, wgbuf, 32);
    gxlle::g_gp_queue.push_back(blk);                    // the GP reads it asynchronously (gp_run)
    // Interrupts are only taken at polls (every 4000 backward branches): a copy loop could write a whole FIFO lap
    // before the high-watermark interrupt suspends it (seen at boot, and an intermittent in-stage deadlock after it).
    // Real hardware raises it immediately, so end the poll budget as soon as a GX interrupt condition is up.
    if (hw::cp_pe_intsr() & hw::S.pi_intmsk)
      if (Cpu* cc = sched_current_cpu()) cc->budget = 0;
    {
      uint32_t fifo_size = end > base ? end - base : 0;
      // The real GP drains concurrently. Here it runs when polled, and a CPU writing a lot between polls could lap
      // it in the ring; a breakpoint then matched the older lap's block at that address (GP parked a frame early ->
      // the frame ring deadlocked, seen at high frame rates). So drain as soon as the FIFO is half full.
      if (fifo_size && !gxlle::g_gp_halted && gxlle::g_gp_queue.size() * 32 > fifo_size / 2) gxlle::gp_run();
      static int laps = 0;
      if (fifo_size && gxlle::g_gp_queue.size() * 32 > fifo_size && laps++ < 5)
        logf("[gx] CPU lapped the GP in the FIFO (%zu bytes queued, FIFO %u bytes, halted=%d cp_cr=%04X msr=%08X)\n",
             gxlle::g_gp_queue.size() * 32, fifo_size, (int)gxlle::g_gp_halted, gxlle::g_cp[1],
             sched_current_cpu() ? sched_current_cpu()->msr : 0u);
    }
  }
  g_wgp_total += n;
}
void hw_wgp_flush() {}
