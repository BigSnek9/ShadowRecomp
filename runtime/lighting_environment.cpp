// Live stage/area atmosphere profiles. No screen-space buffers or frame-count timers.
#include "lighting_environment.h"
#include "settings_ui.h"
#include "sys.h"
#include "stage_workshop.h"
#define NOMINMAX
#include <windows.h>
#include <png.h>
#include <filesystem>
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace lighting {
namespace {
enum Field { SunTint, Sky, Ground, FogColour, SunStrength, SkyStrength, FogDensity, FogStart, SunAz, SunEl, Exposure, UnlitDist, Baked,
             ShadowStrength, ShadowTint, Saturation, AoStrength, Count };
struct Rule {
  std::string name, trigger;
  std::string sky_texture;
  std::array<float,4> sky_rect{0,0,1,1}; // normalized x,y,width,height; exclude scenery in an atlas
  float sky_gain=1;
  uint64_t sky_stamp=0;
  int stage = -1, priority = 0;
  bool bounded = false;
  V3 lo{}, hi{};
  // L1 player regions: player_bounds = box the PLAYER must be in (the camera is ignored); blend_margin = units over which
  // the rule fades in from the box face inward (0 = hard edge); hysteresis = once active, the box counts as grown by this
  // many units until the player has left it by more (no flicker on an edge).
  bool pbounded = false;
  V3 plo{}, phi{};
  float margin = 0, hysteresis = 32;
  bool latched = false;   // runtime: inside (hysteresis included) last frame
  Atmosphere value;
  std::array<bool, Count> fields{};
  float transition = 1.5f;
};
std::vector<Rule> g_rules, g_profile_rules, g_stage_rules;   // g_rules = profiles + the stage file, by priority
std::unordered_map<std::string, int> g_triggers;   // name -> the stage it was set in (a new stage drops older ones)
uint64_t g_stage_stamp = ~0ull;
int g_stage_file = -1;
Atmosphere g_current;
int g_stage = -1;
uint64_t g_last = 0, g_poll = 0, g_stamp = ~0ull;

std::string trim(std::string s) {
  const auto a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return {};
  return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}
std::string exe_dir() {
  char exe[MAX_PATH]; GetModuleFileNameA(nullptr, exe, MAX_PATH);
  const std::string p = exe;
  return p.substr(0, p.find_last_of("\\/"));
}
std::string profile_path() { return exe_dir() + "\\lighting_profiles.ini"; }
// L1 / L4: a Stage Workshop build's rules for one stage (same format), used after lighting_profiles.ini
std::string stage_rules_path(int stage) {
  char b[64]; snprintf(b, sizeof b, "\\mods\\stages\\stg%04d.light.ini", stage);
  return exe_dir() + b;
}
bool numbers(std::string s, float* out, int n) {
  std::replace(s.begin(), s.end(), ',', ' ');
  std::istringstream in(s);
  for (int i = 0; i < n; ++i) if (!(in >> out[i]) || !std::isfinite(out[i])) return false;
  std::string extra; return !(in >> extra);
}
std::filesystem::path sky_path(const Rule& r) {
  auto p=std::filesystem::u8path(r.sky_texture);
  return p.is_absolute() ? p : std::filesystem::path(profile_path()).parent_path()/p;
}
uint64_t sky_stamp(const Rule& r) {
  std::error_code ec;
  auto t=std::filesystem::last_write_time(sky_path(r),ec);
  return ec ? 0 : uint64_t(t.time_since_epoch().count());
}
bool sample_sky(Rule& r) {
  const auto path=sky_path(r);
  FILE* file=_wfopen(path.c_str(),L"rb");
  if(!file) return false;
  png_image im{}; im.version=PNG_IMAGE_VERSION;
  bool ok=png_image_begin_read_from_stdio(&im,file)!=0;
  std::vector<unsigned char> pixels;
  if(ok && im.width && im.height && im.width<=4096 && im.height<=4096) {
    im.format=PNG_FORMAT_RGBA; pixels.resize(PNG_IMAGE_SIZE(im));
    ok=png_image_finish_read(&im,nullptr,pixels.data(),0,nullptr)!=0;
  } else ok=false;
  if(ok) {
    const auto& a=r.sky_rect;
    const int x0=int(a[0]*im.width),y0=int(a[1]*im.height);
    const int x1=std::min(int(std::ceil((a[0]+a[2])*im.width)),int(im.width));
    const int y1=std::min(int(std::ceil((a[1]+a[3])*im.height)),int(im.height));
    double sum[3]{},weight=0;
    // Area mean, alpha weighted. No per-frame decode or GPU readback. This renderer's
    // light multipliers operate in the authored display colour domain, as do profile RGBs.
    for(int y=y0;y<y1;++y) for(int x=x0;x<x1;++x) {
      const auto* c=&pixels[(size_t(y)*im.width+x)*4];
      const double w=c[3]/255.; weight+=w;
      for(int k=0;k<3;++k) sum[k]+=c[k]/255.*w;
    }
    ok=weight>0;
    if(ok) {
      r.value.sky={float(sum[0]/weight)*r.sky_gain,float(sum[1]/weight)*r.sky_gain,float(sum[2]/weight)*r.sky_gain};
      r.fields[Sky]=true; r.sky_stamp=sky_stamp(r);
      logf("[environment] sky texture %s RGB (%g,%g,%g)\n",r.sky_texture.c_str(),r.value.sky.x,r.value.sky.y,r.value.sky.z);
    }
  }
  png_image_free(&im); fclose(file); return ok;
}
// One rules file (lighting_profiles.ini format). false = invalid: the caller keeps its previous rules.
bool parse_rules(const std::string& path, std::vector<Rule>& rules) {
  std::ifstream file(path);
  if (!file) return false;
  std::string line;
  bool ok = true;
  while (std::getline(file, line)) {
    line = trim(line.substr(0, line.find_first_of(";#")));
    if (line.empty()) continue;
    if (line.front() == '[' && line.back() == ']') {
      if (rules.size() == 64) { ok = false; break; }
      rules.emplace_back(); rules.back().name = line.substr(1, line.size() - 2); continue;
    }
    const auto eq = line.find('=');
    if (eq == std::string::npos || rules.empty()) { ok = false; break; }
    auto& r = rules.back();
    const auto key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
    if (key == "trigger") { r.trigger = value; continue; }
    if (key == "blend_margin" || key == "hysteresis") {
      float v = 0;
      if (!numbers(value, &v, 1) || v < 0 || v > 20000) { ok = false; break; }
      (key == "blend_margin" ? r.margin : r.hysteresis) = v;
      continue;
    }
    if (key == "sky_texture") { r.sky_texture=value; if(value.empty()) {ok=false;break;} continue; }
    if (key == "sky_texture_rect") {
      if(!numbers(value,r.sky_rect.data(),4)) {ok=false;break;}
      const auto& a=r.sky_rect;
      if(a[0]<0 || a[1]<0 || a[2]<=0 || a[3]<=0 || a[0]+a[2]>1 || a[1]+a[3]>1) {ok=false;break;}
      continue;
    }
    if (key == "sky_texture_gain") {
      if(!numbers(value,&r.sky_gain,1) || r.sky_gain<0 || r.sky_gain>3) {ok=false;break;}
      continue;
    }
    float a[6]{};
    const int n = key == "bounds" || key == "player_bounds" ? 6 : (key == "sun_tint" || key == "sky" || key == "ground" || key == "fog_colour" || key == "shadow_tint") ? 3 : 1;
    if (!numbers(value, a, n)) { ok = false; break; }
    // the generator's character keys are the user's tickboxes (light_world_only / light_char_cast): accepted, not used
    if (key == "world_shading_only" || key == "characters_cast_shadows") continue;
    if (key == "stage") { if (a[0] < 0 || a[0] > 9999 || std::floor(a[0]) != a[0]) { ok = false; break; } r.stage = int(a[0]); }
    else if (key == "priority") r.priority = int(std::clamp(a[0], -1000.f, 1000.f));
    else if (key == "transition") r.transition = std::clamp(a[0], 0.f, 30.f);
    else if (key == "bounds") {
      if (a[0] > a[3] || a[1] > a[4] || a[2] > a[5]) { ok = false; break; }
      r.bounded = true; r.lo = {a[0], a[1], a[2]}; r.hi = {a[3], a[4], a[5]};
    } else if (key == "player_bounds") {
      if (a[0] > a[3] || a[1] > a[4] || a[2] > a[5]) { ok = false; break; }
      r.pbounded = true; r.plo = {a[0], a[1], a[2]}; r.phi = {a[3], a[4], a[5]};
    } else {
      Field f;
      if (key == "sun_tint") { f = SunTint; r.value.sun_tint = {a[0], a[1], a[2]}; }
      else if (key == "sky") { f = Sky; r.value.sky = {a[0], a[1], a[2]}; }
      else if (key == "ground") { f = Ground; r.value.ground = {a[0], a[1], a[2]}; }
      else if (key == "fog_colour") { f = FogColour; r.value.fog_colour = {a[0], a[1], a[2]}; }
      else if (key == "sun_strength") { f = SunStrength; r.value.sun_strength = std::clamp(a[0], 0.f, 3.f); }
      else if (key == "sky_strength") { f = SkyStrength; r.value.sky_strength = std::clamp(a[0], 0.f, 3.f); }
      else if (key == "fog_density") { f = FogDensity; r.value.fog_density = std::clamp(a[0], 0.f, 0.02f); }
      else if (key == "fog_start") { f = FogStart; r.value.fog_start = std::clamp(a[0], 0.f, 60000.f); }
      else if (key == "sun_azimuth") { f = SunAz; r.value.sun_azimuth = std::clamp(a[0], 0.f, 360.f); }
      else if (key == "sun_elevation") { f = SunEl; r.value.sun_elevation = std::clamp(a[0], 2.f, 90.f); }
      else if (key == "exposure") { f = Exposure; r.value.exposure = std::clamp(a[0], 0.05f, 4.f); }
      else if (key == "baked") { f = Baked; r.value.baked = std::clamp(a[0], 0.f, 1.f); }
      else if (key == "shadow_strength") { f = ShadowStrength; r.value.shadow_strength = std::clamp(a[0], 0.f, 1.f); }
      else if (key == "shadow_tint") { f = ShadowTint; r.value.shadow_tint = {std::clamp(a[0], 0.f, 3.f), std::clamp(a[1], 0.f, 3.f), std::clamp(a[2], 0.f, 3.f)}; }
      else if (key == "saturation") { f = Saturation; r.value.saturation = std::clamp(a[0], 0.f, 2.f); }
      else if (key == "ao_strength") { f = AoStrength; r.value.ao_strength = std::clamp(a[0], 0.f, 1.f); }
      else if (key == "unlit_distance") { f = UnlitDist; r.value.unlit_distance = std::clamp(a[0], 0.f, 200000.f); }
      else { ok = false; break; }
      if (n == 3 && (a[0] < 0 || a[1] < 0 || a[2] < 0 || a[0] > 3 || a[1] > 3 || a[2] > 3)) { ok = false; break; }
      r.fields[f] = true;
    }
  }
  for (const auto& r : rules) if (r.stage < 0 || r.name.empty()) ok = false;
  if(ok) for(auto& r:rules) if(!r.sky_texture.empty() && !sample_sky(r)) {ok=false;break;}
  return ok;
}
uint64_t file_stamp(const std::string& path, bool& exists, uint64_t& size) {
  WIN32_FILE_ATTRIBUTE_DATA stat{};
  exists = GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &stat);
  size = exists ? (uint64_t(stat.nFileSizeHigh) << 32) | stat.nFileSizeLow : 0;
  return exists ? (uint64_t(stat.ftLastWriteTime.dwHighDateTime) << 32) | stat.ftLastWriteTime.dwLowDateTime : 0;
}
// Both sources, rechecked every 0.5 s by file time: lighting_profiles.ini (exe folder, the launcher / user) and the
// current stage's mods\stages\stgNNNN.light.ini (a Stage Workshop build). A bad file keeps that source's previous rules.
void poll(uint64_t now, int stage) {
  if (g_poll && now - g_poll < 500000000ull && stage == g_stage_file) return;
  g_poll = now;
  bool changed = false, exists; uint64_t size;
  const std::string path = profile_path();
  uint64_t stamp = file_stamp(path, exists, size);
  if (stamp != g_stamp) {
    g_stamp = stamp; changed = true;
    std::vector<Rule> rules;
    if (!exists) g_profile_rules.clear();
    else if (size > 65536) logf("[environment] profiles too large; keeping previous rules\n");
    else if (parse_rules(path, rules)) { g_profile_rules = std::move(rules); logf("[environment] lighting_profiles.ini: %zu rules\n", g_profile_rules.size()); }
    else logf("[environment] invalid profiles; keeping previous rules\n");
  } else {
    for(auto* list : {&g_profile_rules, &g_stage_rules}) for(auto& r:*list) if(!r.sky_texture.empty() && sky_stamp(r)!=r.sky_stamp) {
      // Keep the last good colour while the editor is halfway through saving an image.
      Rule candidate=r; if(sample_sky(candidate)) { r=std::move(candidate); changed = true; }
    }
  }
  const std::string spath = stage_rules_path(stage);
  stamp = stage >= 0 ? file_stamp(spath, exists, size) : 0;
  if (stage != g_stage_file || stamp != g_stage_stamp) {
    g_stage_file = stage; g_stage_stamp = stamp; changed = true;
    std::vector<Rule> rules;
    if (!stamp) g_stage_rules.clear();
    else if (size > 65536) logf("[environment] %s too large; keeping previous rules\n", spath.c_str());
    else if (parse_rules(spath, rules)) {
      for (auto& r : rules) if (r.stage != stage && r.stage != 0) logf("[environment] stage file rule %s is for stage %d\n", r.name.c_str(), r.stage);
      g_stage_rules = std::move(rules);
      logf("[environment] stg%04d.light.ini: %zu rules\n", stage, g_stage_rules.size());
    } else logf("[environment] invalid %s; keeping previous rules\n", spath.c_str());
  }
  if (!changed) return;
  g_rules = g_profile_rules;
  g_rules.insert(g_rules.end(), g_stage_rules.begin(), g_stage_rules.end());
  std::stable_sort(g_rules.begin(), g_rules.end(), [](const Rule& a, const Rule& b) { return a.priority < b.priority; });
}
Atmosphere defaults() {
  Atmosphere p;
  const V3 sc = env().sun_colour;
  const float lum = 0.2126f * sc.x + 0.7152f * sc.y + 0.0722f * sc.z;
  const V3 mid{lum * 0.5f + 0.25f, lum * 0.5f + 0.32f, lum * 0.5f + 0.5f};
  p.sky = {mid.x * 0.62f, mid.y * 0.66f, mid.z * 0.78f};
  p.ground = {mid.x * 0.34f, mid.y * 0.32f, mid.z * 0.3f};
  return p;
}
float blend(float a, float b, float k) { return a + (b - a) * k; }
V3 blend(V3 a, V3 b, float k) { return a + (b - a) * k; }
// w = how much of the rule applies (1 = fully; < 1 inside a player region's blend margin)
void apply(Atmosphere& p, const Rule& r, float w = 1) {
  const auto& v = r.value;
  if (r.fields[SunTint]) p.sun_tint = blend(p.sun_tint, v.sun_tint, w);
  if (r.fields[Sky]) p.sky = blend(p.sky, v.sky, w);
  if (r.fields[Ground]) p.ground = blend(p.ground, v.ground, w);
  if (r.fields[FogColour]) p.fog_colour = blend(p.fog_colour, v.fog_colour, w);
  if (r.fields[SunStrength]) p.sun_strength = blend(p.sun_strength, v.sun_strength, w);
  if (r.fields[SkyStrength]) p.sky_strength = blend(p.sky_strength, v.sky_strength, w);
  if (r.fields[FogDensity]) { p.fog_density = blend(p.fog_density, v.fog_density, w); p.fog_override = blend(p.fog_override, v.fog_density > 0 ? 1.f : 0.f, w); }
  if (r.fields[FogStart]) p.fog_start = blend(p.fog_start, v.fog_start, w);
  if (r.fields[SunAz]) { p.sun_azimuth += std::remainder(v.sun_azimuth - p.sun_azimuth, 360.f) * w; p.sun_direction_override = blend(p.sun_direction_override, 1.f, w); }
  if (r.fields[SunEl]) { p.sun_elevation = blend(p.sun_elevation, v.sun_elevation, w); p.sun_direction_override = blend(p.sun_direction_override, 1.f, w); }
  if (r.fields[Exposure]) p.exposure = blend(p.exposure, v.exposure, w);
  if (r.fields[UnlitDist]) p.unlit_distance = blend(p.unlit_distance, v.unlit_distance, w);
  if (r.fields[Baked]) p.baked = blend(p.baked, v.baked, w);
  if (r.fields[ShadowStrength]) p.shadow_strength = blend(p.shadow_strength, v.shadow_strength, w);
  if (r.fields[ShadowTint]) p.shadow_tint = blend(p.shadow_tint, v.shadow_tint, w);
  if (r.fields[Saturation]) p.saturation = blend(p.saturation, v.saturation, w);
  if (r.fields[AoStrength]) p.ao_strength = blend(p.ao_strength, v.ao_strength, w);
}
// signed distance INTO a box (>= 0 inside: to the nearest face; < 0 outside: minus the distance to the box)
float inside_by(V3 p, V3 lo, V3 hi) {
  const float dx = std::min(p.x - lo.x, hi.x - p.x), dy = std::min(p.y - lo.y, hi.y - p.y), dz = std::min(p.z - lo.z, hi.z - p.z);
  const float in = std::min(dx, std::min(dy, dz));
  if (in >= 0) return in;
  const float ox = std::max(0.f, -dx), oy = std::max(0.f, -dy), oz = std::max(0.f, -dz);
  return -std::sqrt(ox * ox + oy * oy + oz * oz);
}
void camera(CameraCtx& ctx) {
  const auto& p = atmosphere();
  ctx.slots[0] = p.fog_colour.x; ctx.slots[1] = p.fog_colour.y; ctx.slots[2] = p.fog_colour.z;
  ctx.slots[3] = p.fog_density; ctx.slots[4] = p.fog_start; ctx.slots[5] = p.fog_override;
}
const char* kApply = R"WGSL(
    let e_fog = L({BASE}u);
    if (l_has_override) {
        l_fog_weight = 1.0;
        l_fogged = prev.rgb;
    } else if (l_replace()) {
        let e_params = L({BASE}u + 1u);
        let e_depth = max(-in.lview.z - e_params.x, 0.0);
        let e_amount = min(1.0 - exp(-e_depth * e_fog.w), 0.95);
        l_fog_weight = clamp(e_params.y, 0.0, 1.0) * (1.0 - l_fv);   // unlit far surfaces (sky, backdrops) keep their own look
        l_fogged = mix(prev.rgb, e_fog.rgb, e_amount);
    })WGSL";
Module g_module{"environment", 2, "", "", "", nullptr, camera, nullptr, nullptr};
}
Module* environment_module() { g_module.wgsl_post = kApply; return &g_module; }
// Looks relative to the profile (generated = Stylized; make_profiles.py overlay_looks: stylized sun 0.5 / exposure 0.6 /
// shadow 0.45 / saturation 1.1, original 0.6 / 0.8 / 0.6 / 1.0 with a white tint, dramatic 0.8 / 0.75 / 0.75 / 1.2, darker tint)
Look look() {
  const auto& p = g_current;
  const auto& s = settings::g;
  Look l;
  if (s.light_replace) { l.exposure = p.exposure; return l; }
  struct F { float exp, sun, shadow, sat, ao; } const kF[3] = {{1, 1, 1, 1, 0.45f}, {1.33f, 1.2f, 1.33f, 1 / 1.1f, 1}, {1.25f, 1.6f, 1.67f, 1.2f / 1.1f, 1}};
  const F& f = kF[std::clamp(s.light_look, 0, 2)];
  auto pc = [](int v, int lo, int hi) { return std::clamp(v, lo, hi) / 100.f; };
  l.exposure = p.exposure * f.exp * pc(s.light_exposure, 0, 300);
  l.sun = f.sun;
  l.saturation = std::clamp(p.saturation * f.sat * pc(s.light_saturation, 0, 200), 0.f, 2.f);
  l.ao = s.light_ao > 0 ? std::clamp(p.ao_strength * f.ao, 0.f, 1.f) : 0.f;   // #143: no AO unless the player switched it on (profiles cannot)
  l.fill = std::clamp(2.f - pc(s.light_ambient, 0, 200), 0.f, 2.f);   // ambient 150 % = shadows / corners half as dark
  V3 tint = s.light_look == 1 ? V3{1, 1, 1} : s.light_look == 2 ? p.shadow_tint * 0.82f : p.shadow_tint;
  const float t = pc(s.light_shadow_tint, 0, 200);
  tint = V3{1, 1, 1} + (tint - V3{1, 1, 1}) * t;
  const float st = std::clamp(p.shadow_strength * f.shadow * pc(s.light_shadow_strength, 0, 300), 0.f, 1.f);
  auto ch = [&](float c) { return std::clamp(1 - st * (1 - 0.5f * std::clamp(c, 0.f, 2.f)), 0.f, 1.f); };
  l.shadow = {ch(tint.x), ch(tint.y), ch(tint.z)};
  {   // GI: the ground colour's hue at the same luminance, light_gi % x (1 / 1.4 / 1.6 for 1 / 2 / 3 bounces), at most 35 %
    const V3 g = p.ground;
    const float lum = std::max(0.2126f * g.x + 0.7152f * g.y + 0.0722f * g.z, 1e-3f);
    static const float kB[3] = {1.f, 1.4f, 1.6f};
    const float k = std::clamp(s.light_gi / 100.f * 0.35f * kB[std::clamp(s.light_gi_bounces, 1, 3) - 1], 0.f, 0.7f);
    const V3 hue{g.x / lum, g.y / lum, g.z / lum};
    l.gi = V3{1, 1, 1} + (hue - V3{1, 1, 1}) * k;
  }
  return l;
}
const Atmosphere& atmosphere() { return g_current; }
int environment_stage() { return g_stage; }
void set_environment_trigger(std::string_view name, bool active) {
  if (name.empty() || name.size() > 64) return;
  if (active && (g_triggers.size() < 64 || g_triggers.count(std::string(name)))) g_triggers[std::string(name)] = lighting_stage();
  else if (!active) g_triggers.erase(std::string(name));
}
void update_environment(const Cam& cam) {
  const uint64_t now = sys_time_ns();
  const int stage = lighting_stage();
  poll(now, stage);
  const bool changed = stage != g_stage;
  if (changed) {   // a new stage: triggers set during another stage go (ones this stage's start logic set already stay)
    for (auto it = g_triggers.begin(); it != g_triggers.end();) it = it->second == stage ? std::next(it) : g_triggers.erase(it);
    g_stage = stage;
    for (auto& r : g_rules) r.latched = false;
  }
  // L1: the player's position (the workshop's player pointer); regions keep their state while it is unknown
  static V3 player{}; static bool have_player = false;
  float pp[3];
  if (stage_workshop::player_position(pp)) { player = {pp[0], pp[1], pp[2]}; have_player = true; }
  Atmosphere target = defaults();
  std::string selected, set;   // set = active rule names (partial weight marked): a change picks the fade time
  static std::string last_set;
  static float transition = 1.5f;
  static std::unordered_map<std::string, bool> was_on;
  float changed_transition = -1; int changed_priority = -100000;
  if (settings::g.light_profiles) for (auto& r : g_rules) {
    bool on = (r.stage == 0 || r.stage == stage) && (r.trigger.empty() || g_triggers.count(r.trigger));
    if (on && r.bounded && (cam.p.x < r.lo.x || cam.p.y < r.lo.y || cam.p.z < r.lo.z ||
                            cam.p.x > r.hi.x || cam.p.y > r.hi.y || cam.p.z > r.hi.z)) on = false;
    float w = 1;
    if (on && r.pbounded) {
      if (!have_player) on = false;
      else {
        const float d = inside_by(player, r.plo, r.phi) + (r.latched ? r.hysteresis : 0.f);
        w = d <= 0 ? 0.f : r.margin > 0 ? std::min(1.f, d / r.margin) : 1.f;
        r.latched = w > 0;
        on = r.latched;
      }
    }
    bool& prev = was_on[r.name];
    if (prev != on && r.priority >= changed_priority) { changed_priority = r.priority; changed_transition = r.transition; }
    prev = on;
    if (!on) continue;
    apply(target, r, w);
    selected += (selected.empty() ? "" : " + ") + r.name;
    set += r.name + (w < 1 ? "~" : "") + "|";
  }
  if (changed_transition >= 0) transition = changed_transition;   // entering or leaving a rule fades over ITS transition
  if (selected.empty()) selected = "game-derived default";
  const float dt = g_last ? float(now - g_last) * 1e-9f : 0;
  const float k = changed || !g_last || transition <= 0 ? 1 : 1 - std::exp(-dt / transition);
  g_last = now;
  g_current.sun_tint = blend(g_current.sun_tint, target.sun_tint, k);
  g_current.sky = blend(g_current.sky, target.sky, k); g_current.ground = blend(g_current.ground, target.ground, k);
  g_current.fog_colour = blend(g_current.fog_colour, target.fog_colour, k);
  g_current.sun_strength = blend(g_current.sun_strength, target.sun_strength, k);
  g_current.sky_strength = blend(g_current.sky_strength, target.sky_strength, k);
  g_current.fog_density = blend(g_current.fog_density, target.fog_density, k);
  g_current.fog_start = blend(g_current.fog_start, target.fog_start, k);
  g_current.fog_override = blend(g_current.fog_override, target.fog_override, k);
  // Azimuth uses the shortest angular path across 0/360.
  g_current.sun_azimuth += std::remainder(target.sun_azimuth - g_current.sun_azimuth, 360.f) * k;
  g_current.sun_elevation = blend(g_current.sun_elevation, target.sun_elevation, k);
  g_current.sun_direction_override = blend(g_current.sun_direction_override, target.sun_direction_override, k);
  g_current.exposure = blend(g_current.exposure, target.exposure, k);
  g_current.baked = blend(g_current.baked, target.baked, k);
  g_current.shadow_strength = blend(g_current.shadow_strength, target.shadow_strength, k);
  g_current.shadow_tint = blend(g_current.shadow_tint, target.shadow_tint, k);
  g_current.saturation = blend(g_current.saturation, target.saturation, k);
  g_current.ao_strength = blend(g_current.ao_strength, target.ao_strength, k);
  g_current.unlit_distance = target.unlit_distance;   // a switch, not faded (0 = off)
  static int last_stage = -1;
  static uint64_t since = 0;
  if (last_set != set || last_stage != stage) {
    logf("[environment] stage %d profile %s camera (%g,%g,%g) player (%g,%g,%g)%s transition %gs\n", stage, selected.c_str(),
         cam.p.x, cam.p.y, cam.p.z, player.x, player.y, player.z, have_player ? "" : " (unknown)", transition);
    last_set = set; last_stage = stage; since = now;
  }
  // LIGHT_ENV_LOG=1: the blended values 4x a second and the seconds since the last profile change (fade timing checks)
  static const bool vlog = getenv("LIGHT_ENV_LOG") != nullptr;
  static uint64_t last_vlog = 0;
  if (vlog && now - last_vlog >= 250000000ull) {
    last_vlog = now;
    logf("[environment] t+%.2f sun_strength %.4f fog_density %.6f sky %.3f,%.3f,%.3f sun_el %.2f target sun_strength %.4f\n",
         double(now - since) * 1e-9, g_current.sun_strength, g_current.fog_density, g_current.sky.x, g_current.sky.y, g_current.sky.z,
         g_current.sun_elevation, target.sun_strength);
  }
}
}
