// Lighting module: GLOW LIGHTS ("Glowing objects cast light", settings light_emissive / _str / _range). The game fakes its
// lamps, fires, screens and light shafts with additive glow sprites; gx_lle reports each additive 3D sprite (centroid,
// size, vertex colour, in view space) through glow_add(). The module keeps the 16 strongest of a frame (merged when they
// overlap, stored in world space) and the lit shader adds them the NEXT frame as point lights on the map: into l_direct,
// so replace mode lights with them and the overlay brightens through its sun-shading term (subtle by default).
#include "lighting_core.h"
#include "settings_ui.h"
#include "sys.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace lighting {
namespace {
constexpr int kMax = 16;
struct Glow { V3 w, c; float r, score; };
std::vector<Glow> g_now, g_use;
Cam g_cam;                 // the frame's main camera (view -> world for the reports)
bool g_cam_ok = false;

V3 view_to_world(const Cam& k, V3 v) { return k.p + k.r * -v.x + k.u * v.y + k.a * -v.z; }
V3 world_to_view(const Cam& k, V3 w) { const V3 d = w - k.p; return {-dot(k.r, d), dot(k.u, d), -dot(k.a, d)}; }

float lumi(V3 c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }

// Steadiness (light_glow_flicker 0-100, default 0 = steadiest; GLOW_RAW=1 = the old per-frame lights): the reports are clustered (a lamp's sprites, or a lava / fire field's
// many sprites -> ONE wide light), clusters are matched to TRACKS with stable ids frame to frame, and each track fades in /
// out (~0.3 s), holds a short miss, low-passes its colour (~1 s window; a sudden bright flash still comes in at once) and
// its position / radius. The 16 slots go to the strongest tracks with hysteresis (a slot holder keeps it unless beaten
// clearly). GLOW_PROBE="x,y,z[;x,y,z]" logs the glow light at those world points each
// frame (wall-flicker measurement).
struct Track { V3 w, c, tw, tc; float r, tr, score, tscore, fade, miss; bool hit, slot; uint32_t id; };
std::vector<Track> g_tracks;
uint64_t g_last_ns = 0;
uint32_t g_next_id = 1;
std::vector<V3> g_probe;
bool g_probe_parsed = false;

void probe_log() {
  if (!g_probe_parsed) {
    g_probe_parsed = true;
    if (const char* e = getenv("GLOW_PROBE"))
      for (const char* q = e; *q;) {
        V3 v{};
        if (sscanf(q, "%f,%f,%f", &v.x, &v.y, &v.z) == 3) g_probe.push_back(v);
        const char* n = strchr(q, ';');
        if (!n) break;
        q = n + 1;
      }
  }
  if (g_probe.empty()) return;
  static uint64_t f = 0;
  ++f;
  const float range = std::clamp(settings::g.light_emissive_range, 25, 300) / 100.f;
  char line[256];
  int k = snprintf(line, sizeof line, "[glowprobe] f %llu n %zu t %zu", (unsigned long long)f, g_use.size(), g_tracks.size());
  for (const V3& pt : g_probe) {
    float v = 0;
    for (const Glow& g : g_use) {
      const V3 d = g.w - pt;
      const float l = std::sqrt(dot(d, d)), R = g.r * range;
      if (l < R) { const float a = 1.f - l / R; v += lumi(g.c) * a * a; }
    }
    k += snprintf(line + k, sizeof line - k, " %.4f", v);
    if (k > 200) break;
  }
  logf("%s\n", line);
}

void frame_start() {
  const uint64_t now = sys_time_ns();
  const float dt = g_last_ns ? std::clamp((now - g_last_ns) * 1e-9f, 0.f, 0.1f) : 1.f / 60.f;
  g_last_ns = now;
  static const bool raw = getenv("GLOW_RAW") != nullptr;   // test baseline: the old per-frame lights
  const float st = raw ? 0.f : 1.f - 0.6f * std::clamp(settings::g.light_glow_flicker, 0, 100) / 100.f;   // flicker 100 = a shimmer, never the raw per-frame popping
  // 1) cluster this frame's reports
  struct Cl { V3 w, first, csum, cmax; float r, ext, wsum, score; };
  std::vector<Cl> cl;
  const float join = 60.f + 240.f * st;   // sprites closer than this (or half a light's radius) are one light
  std::sort(g_now.begin(), g_now.end(), [](const Glow& a, const Glow& b) { return a.score > b.score; });
  for (const Glow& g : g_now) {
    const float wgt = std::max(g.score, 1e-4f);
    bool merged = false;
    for (Cl& o : cl) {
      const V3 d = g.w - o.w;
      const float lim = std::max(o.r * 0.5f, join);
      if (dot(d, d) < lim * lim) {
        o.wsum += wgt;
        o.w = o.w + d * (wgt / o.wsum);
        o.csum = o.csum + g.c * wgt;
        o.cmax = V3{std::max(o.cmax.x, g.c.x), std::max(o.cmax.y, g.c.y), std::max(o.cmax.z, g.c.z)};
        const V3 e = g.w - o.first;
        o.ext = std::max(o.ext, std::sqrt(dot(e, e)));
        o.r = std::max(o.r, g.r);
        o.score += g.score;
        merged = true;
        break;
      }
    }
    if (!merged && cl.size() < 64) cl.push_back({g.w, g.w, g.c * wgt, g.c, g.r, 0.f, wgt, g.score});
  }
  g_now.clear();
  // 2) match clusters to tracks (stable ids), strongest first
  for (Track& t : g_tracks) t.hit = false;
  for (const Cl& o : cl) {
    const V3 avg = o.csum * (1.f / o.wsum);
    const float r = std::min(std::max(o.r, o.ext + o.r * 0.5f), 800.f);   // a field of sprites: one wide light...
    const V3 c = (o.cmax + (avg - o.cmax) * (0.5f * st)) * (o.r / r);   // ...towards its average colour, no brighter overall
    Track* best = nullptr;
    float bd = 1e30f;
    for (Track& t : g_tracks) {
      if (t.hit) continue;
      const V3 d = o.w - t.w;
      const float dd = dot(d, d), lim = std::max(t.r * 0.6f, join * 1.5f);
      if (dd < lim * lim && dd < bd) { bd = dd; best = &t; }
    }
    if (best) {
      best->tw = o.w; best->tc = c; best->tr = r; best->tscore = o.score; best->hit = true; best->miss = 0;
    } else if (g_tracks.size() < 64) {
      const bool flash = lumi(c) > 0.9f || st <= 0.f;
      g_tracks.push_back({o.w, c, o.w, c, r, r, o.score, o.score, flash ? 1.f : 0.f, 0.f, true, false, g_next_id++});
    }
  }
  // 3) fade / low-pass
  const float fade_t = 0.02f + 0.3f * st, hold = 0.25f * st;
  const float ac = 1.f - std::exp(-dt / (0.02f + 0.5f * st));   // colour: ~1 s averaging window at 100
  for (Track& t : g_tracks) {
    if (!t.hit) { t.miss += dt; t.tscore *= 0.5f; }
    const float target = t.hit || t.miss < hold ? 1.f : 0.f;
    t.fade = target > t.fade ? std::min(1.f, t.fade + dt / fade_t) : std::max(0.f, t.fade - dt / fade_t);
    if (t.hit) {
      const bool flash = lumi(t.tc) > 0.9f && lumi(t.tc) > 2.5f * lumi(t.c) + 0.15f;
      const float a = flash ? 1.f - std::exp(-dt / 0.03f) : ac;
      t.c = t.c + (t.tc - t.c) * a;
      const V3 d = t.tw - t.w;
      const float ap = 1.f - std::exp(-dt / (dot(d, d) > 0.25f * t.r * t.r ? 0.03f : 0.02f + 0.3f * st));   // moving fast: follow
      t.w = t.w + d * ap;
      t.r = t.r + (t.tr - t.r) * ap;
    }
    t.score = t.score + (t.tscore - t.score) * ac;
  }
  g_tracks.erase(std::remove_if(g_tracks.begin(), g_tracks.end(), [&](const Track& t) { return !t.hit && t.miss >= hold && t.fade <= 0.f; }), g_tracks.end());
  // 4) the 16 slots, with hysteresis
  std::vector<Track*> rank;
  for (Track& t : g_tracks) rank.push_back(&t);
  auto key = [&](const Track* t) { return t->score * std::max(t->fade, 0.05f) * (t->slot ? 1.f + 0.5f * st : 1.f); };
  std::sort(rank.begin(), rank.end(), [&](const Track* a, const Track* b) { return key(a) > key(b); });
  g_use.clear();
  for (size_t i = 0; i < rank.size(); ++i) {
    Track& t = *rank[i];
    t.slot = (int)i < kMax;
    if (t.slot && t.fade > 0.f) g_use.push_back({t.w, t.c * t.fade, t.r, t.score});
  }
  probe_log();
  static const bool log = getenv("LIGHT_GLOWLOG") != nullptr;
  static uint64_t n = 0;
  if (log && ++n % 240 == 0 && !g_use.empty())
    logf("[glow] %zu lights (%zu tracks), strongest at (%.0f,%.0f,%.0f) radius %.0f colour %.2f,%.2f,%.2f\n", g_use.size(), g_tracks.size(),
         g_use[0].w.x, g_use[0].w.y, g_use[0].w.z, g_use[0].r, g_use[0].c.x, g_use[0].c.y, g_use[0].c.z);
}

void camera(CameraCtx& ctx) {
  if (ctx.index == 0) { g_cam = ctx.cam; g_cam_ok = true; }
  float* sl = ctx.slots;
  const auto& s = settings::g;
  if (!s.light_emissive || g_use.empty()) { sl[0] = 0; return; }
  const float str = std::clamp(s.light_emissive_str, 0, 300) / 100.f * 0.8f;
  const float range = std::clamp(s.light_emissive_range, 25, 300) / 100.f;
  int n = 0;
  for (const Glow& g : g_use) {
    const V3 v = world_to_view(ctx.cam, g.w);
    float* p = sl + (1 + n * 2) * 4;
    p[0] = v.x; p[1] = v.y; p[2] = v.z; p[3] = g.r * range;
    p[4] = g.c.x * str; p[5] = g.c.y * str; p[6] = g.c.z * str; p[7] = 0;
    if (++n == kMax) break;
  }
  sl[0] = (float)n;
}

const char* kPre = R"WGSL(
    {
      let g_n = u32(L({BASE}u).x);
      for (var g_i = 0u; g_i < g_n; g_i = g_i + 1u) {
        let g_p = L({BASE}u + 1u + g_i * 2u);
        let g_d = g_p.xyz - in.lview;
        let g_l = length(g_d);
        if (g_l < g_p.w) {
          let g_a = 1.0 - g_l / g_p.w;
          l_glow = l_glow + L({BASE}u + 2u + g_i * 2u).rgb * (g_a * g_a * max(dot(l_n, g_d / max(g_l, 0.001)), 0.0));
        }
      }
    })WGSL";

Module g_module{"glow lights", 1 + kMax * 2, "", kPre, "", frame_start, camera, nullptr, nullptr};
}  // namespace

Module* glow_module() { return &g_module; }

// gx_lle: an additive 3D sprite this frame, its centre / size in view space and its colour 0..1
void glow_add(const float view[3], const float colour[3], float radius) {
  if (!settings::g.light || !settings::g.light_emissive || !g_cam_ok || g_now.size() >= 512) return;
  if (!(radius > 1 && radius < 300)) return;   // big sheets (light shafts, haze, sky glow) are not lamps
  const float lum = 0.2126f * colour[0] + 0.7152f * colour[1] + 0.0722f * colour[2];
  if (lum < 0.05f) return;
  const V3 w = view_to_world(g_cam, V3{view[0], view[1], view[2]});
  const float dist = std::sqrt(view[0] * view[0] + view[1] * view[1] + view[2] * view[2]);
  // the light reaches ~4 sprite sizes (a lamp glow of 40 lights about 160 around it)
  g_now.push_back({w, V3{std::min(colour[0], 1.2f), std::min(colour[1], 1.2f), std::min(colour[2], 1.2f)}, std::clamp(radius * 6.f, 80.f, 800.f), lum * std::min(radius, 50.f) / (1.f + dist / 800.f)});   // near, bright lamps first
}
}  // namespace lighting
