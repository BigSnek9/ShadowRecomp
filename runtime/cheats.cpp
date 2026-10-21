// Cheats (F1 / PC Settings > Cheats). The classic US Action Replay codes for this game (GUPE8P), decrypted offline
// with Dolphin's GCNcrypt port (work/ardecrypt), re-expressed as RAM writes applied once per game frame, plus the two
// code-patch cheats done as hooks around the patched functions. Cheats are session-only (never saved to the ini);
// note that autosave can still record scores/lives earned while one is on.
//
//   lives        80576704 (s32)     rings        8057670C (s32)     score     80576710 (s32)
//   dark score   80576714 (s32)     hero score   80576718 (s32)     timer     8057D734 (f32, the "low time" code)
//   dark gauge   805766D4 (s32, full = 30000)    hero gauge 805766C8 (s32, full = 30000)
//   pad buttons  8056ED4E (u16; Z+R = 0x1020, Z+L = 0x2020 in the original codes)
//   fn_8016EF38  adds to an ammo counter (AR code: nop its store)     fn_801B64F8 returns a "secondary health"
//   (AR code: load a constant from r2+0x50 instead)
#include "cheats.h"
#include "sys.h"
#include "game.h"
#include <cstdlib>
#include <cstring>
#include <vector>

void f_8016EF38(Cpu&);
void f_801B64F8(Cpu&);

namespace cheats {
namespace {
bool g_on[COUNT];
const Info g_info[COUNT] = {
    {"Infinite lives", "Lives stay at 9."},
    {"Always at least 20 rings", "You can still collect more. (Safe for Circus Park's 400-ring mission.)"},
    {"999 rings", "Warning: auto-completes Circus Park's Hero mission (needs 400 rings)."},
    {"Infinite ammo", "Weapons never run out."},
    {"Infinite vehicle / secondary health", "Vehicles and other secondary health bars stay full."},
    {"Max score", "Score set to 99,999,999."},
    {"Max Hero score", "Hero points maxed."},
    {"Max Dark score", "Dark points maxed."},
    {"Full Hero gauge (hold Z+L)", "Hold Z and L to fill the Hero gauge."},
    {"Full Dark gauge (hold Z+R)", "Hold Z and R to fill the Dark gauge."},
    {"Full Hero gauge (always)", "Hero gauge always full."},
    {"Full Dark gauge (always)", "Dark gauge always full."},
    {"Infinite rings", "Rings never go down (a hit still scatters rings you can pick up)."},
    {"All weapons in the Special Weapon Box", "The Secret Container holds every special weapon (this session only)."},
};
// named keys for the launcher (shadow_cheats.ini key=0/1, live); cheatN=0/1 by index still works
const struct { const char* key; Id id; } kKeys[] = {
    {"cheat_infinite_rings", INFINITE_RINGS}, {"cheat_hero_gauge", HERO_GAUGE_ALWAYS}, {"cheat_dark_gauge", DARK_GAUGE_ALWAYS},
    {"cheat_infinite_ammo", INFINITE_AMMO}, {"cheat_all_weapons", ALL_WEAPONS}, {"cheat_infinite_lives", INFINITE_LIVES},
};
int32_t g_rings_last = -1;
uint32_t g_rings_stage = 0;
inline void w32(uint32_t a, uint32_t v) { st32(a, v); }
}  // namespace

const Info& info(int i) { return g_info[i]; }
int by_key(const char* key) {
  for (const auto& k : kKeys)
    if (!strcmp(key, k.key)) return k.id;
  return -1;
}
bool& enabled(int i) { return g_on[i]; }

// testing: TEST_POKE="secs:addr=value,..." writes a RAM word once, secs after the stage started (cheat scenarios)
static void test_pokes() {
  static std::vector<std::pair<double, std::pair<uint32_t, int32_t>>> pokes;
  static bool parsed = false;
  static uint64_t t0 = 0;
  if (!parsed) {
    parsed = true;
    if (const char* e = getenv("TEST_POKE"))
      for (const char* q = e; *q;) {
        double s; unsigned a; int v;
        if (sscanf(q, "%lf:%x=%d", &s, &a, &v) == 3) pokes.push_back({s, {a, v}});
        const char* n = strchr(q, ',');
        if (!n) break;
        q = n + 1;
      }
  }
  if (pokes.empty() || !game::in_stage()) return;
  if (!t0) t0 = sys_time_ns();
  const double t = (sys_time_ns() - t0) * 1e-9;
  for (auto it = pokes.begin(); it != pokes.end();)
    if (t >= it->first) { st32(it->second.first, (uint32_t)it->second.second); logf("[test] poke %08X = %d at %.1f s\n", it->second.first, it->second.second, t); it = pokes.erase(it); }
    else ++it;
}

void apply_frame() {
  test_pokes();
  if (g_on[INFINITE_LIVES] && (int32_t)ld32(0x80576704) < 9) w32(0x80576704, 9);
  if (g_on[MIN_20_RINGS] && (int32_t)ld32(0x8057670C) < 20) w32(0x8057670C, 20);
  if (g_on[RINGS_999]) w32(0x8057670C, 999);
  if (g_on[INFINITE_RINGS]) {   // never down: a loss is undone (per stage; a new stage starts from its own count)
    const uint32_t stage = ld32(0x80584720);
    const int32_t r = (int32_t)ld32(0x8057670C);
    if (stage != g_rings_stage || !game::in_stage()) { g_rings_stage = stage; g_rings_last = r; }   // loading / a new stage: its own count
    else if (r < g_rings_last) {
      static int n = 0;
      if (n++ < 5) logf("[cheats] infinite rings: %d -> %d (loss undone)\n", r, g_rings_last);
      w32(0x8057670C, (uint32_t)g_rings_last);
    }
    else g_rings_last = r;
  } else g_rings_last = -1, g_rings_stage = 0;
  if (g_on[MAX_SCORE]) w32(0x80576710, 99999999);
  if (g_on[MAX_HERO_SCORE]) w32(0x80576718, 9999999);
  if (g_on[MAX_DARK_SCORE]) w32(0x80576714, 9999999);
  uint16_t pad = ld16(0x8056ED4E);
  if (g_on[HERO_GAUGE_ALWAYS] || (g_on[HERO_GAUGE_ZL] && pad == 0x2020)) w32(0x805766C8, 30000);
  if (g_on[DARK_GAUGE_ALWAYS] || (g_on[DARK_GAUGE_ZR] && pad == 0x1020)) w32(0x805766D4, 30000);
}
}  // namespace cheats

// ammo += delta (fn_8016EF38): with the cheat, a decrease is undone
void hle_AmmoAdd(Cpu& c) {
  uint32_t p = c.r[3];
  uint32_t before = ld32(p);
  f_8016EF38(c);
  if (cheats::enabled(cheats::INFINITE_AMMO) && (int32_t)ld32(p) < (int32_t)before) st32(p, before);
}

// secondary health getter (fn_801B64F8): with the cheat it returns the constant the AR code loads (r2+0x50)
void hle_SecondaryHealth(Cpu& c) {
  uint32_t r2 = c.r[2];
  f_801B64F8(c);
  if (cheats::enabled(cheats::INFINITE_SECONDARY_HEALTH)) {
    uint32_t u = ld32(r2 + 0x50);
    float f;
    memcpy(&f, &u, 4);
    c.f[1] = f;
  }
}

// Secret Container (00/3A, the editor's "Special Weapon Box"): fn_80278A78 reads its SET parameters; the weapon it
// holds is the 4-byte parameter stored at +0x94 (fn_802786B0 passes it to the pickup fn_8027B338 / fn_80254384).
// With "All weapons" each box created takes the next special weapon (57-67: Samurai Sword .. Shadow Rifle) in turn,
// so a stage's boxes hand out every one; RAM only, nothing is saved.
void f_80278A78(Cpu&);
void hle_SecretContainerParams(Cpu& c) {
  const uint32_t box = c.r[3];
  f_80278A78(c);
  if (!cheats::enabled(cheats::ALL_WEAPONS)) return;
  static int next = 0;
  static std::vector<std::pair<uint32_t, uint32_t>> given;   // box -> its weapon (the parameters are read more than once)
  static uint32_t stage = 0;
  if (ld32(0x80584720) != stage) { stage = ld32(0x80584720); given.clear(); next = 0; }
  uint32_t w = 0;
  for (const auto& g : given)
    if (g.first == box) w = g.second;
  if (!w) { w = 57 + (uint32_t)(next++ % 11); given.push_back({box, w}); }
  logf("[cheats] secret container %08X: weapon %u -> %u\n", box, ld32(box + 0x94), w);
  st32(box + 0x94, w);
}
