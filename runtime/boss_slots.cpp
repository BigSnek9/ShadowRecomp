// Two different bosses in one stage (#104, 2026-10-19). Every boss load() registers its archive in the
// resource manager under the SAME key 0x16 (e.g. Egg Breaker fn_802EAF00: fn_8001299C(fn_800122D4(), 0x16,
// new ResourceOneFile("enemy/boss/EggMecaOne.one"))). fn_8001299C refuses a key that already exists (returns 0, no
// insert), so only the FIRST boss loaded owns key 0x16; a second boss's archive is never findable (nor loaded), and its
// code reads the first boss's rows by index (quiet run: Egg Breaker cls 00BB looked up HeavyDog0510One.one, a missing
// row -> fn_80169364 walked a garbage atomic list forever, fn_802A58F0 -> fn_80456C54).
// Fix:
//  * a refused boss archive is registered under a key of its own (0x100..) so the manager owns (and frees) it; the
//    manager's load loop only runs on the loading screen, so the boss's base update steps its loading until it is in;
//  * "which boss is running": the boss base update fn_8027DEB0 (r3 = the boss) marks the object as a boss and pairs it
//    with the archive whose load() call site is nearest its init function (vtable +0x10; each boss's load and init live
//    in its own code block - logged once per boss as "[boss-slot]"); the task-tree walker (game_hooks per-node call ->
//    stage_workshop::node_enter / leave) then tells us when that object (and its child nodes) runs;
//  * key 0x16's map node then holds the running boss's archive, and the owner's archive at any other time;
//  * a boss's helper tasks run as separate nodes (Egg Breaker: fn_80179A34 -> fn_802A7C9C -> BossCtrlUnit
//    fn_80291E0C): while a boss's own node runs, the return addresses of its key-0x16 lookups are learned as its code;
//    a lookup outside any boss node goes to the boss whose learned code is within 0x6000 of a frame on the call stack
//    (the shared boss framework 0x8027D000.. / 0x80290000.. is never counted as one boss's code).
// Proven (quiet runs 2026-10-19, build_rouge): Egg Breaker + Blue Falcon + soldiers - no hang in 30 s, each boss looks
// up and draws only its own archive; Egg Breaker alone / + soldiers / stage 511 unchanged (fix inactive). BOSS_SLOT_LOG=1.
// Inactive (no change at all) while only one archive was offered for 0x16, i.e. in every original stage.
#include "sys.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

void f_8001299C(Cpu&);   // ResourceManager: register (r3 manager, r4 key, r5 ResourceOneFile) -> r3 1 / 0 (key taken)
void f_800124E4(Cpu&);   // ResourceManager: find (r3 out iterator, r4 manager, r5 &key)
void f_8027DEB0(Cpu&);   // boss base update (r3 boss)
namespace boss_arena { void step(uint32_t boss); }   // boss_arena.cpp

namespace {
bool ram(uint32_t a) { return a >= 0x80000000u && a < RT_RAM_END; }
constexpr uint32_t kBossKey = 0x16;
constexpr uint32_t kExtraKey = 0x60;    // keys for the 2nd.. boss archive: unused small keys (the game uses 7, 0xC, 0x16 .. 0x28; 0x100 collided - keys are not 32-bit)
struct Arch { uint32_t file, site; std::string path; };
std::vector<Arch> g_arch;                          // every archive offered for key 0x16 (the first one owns the key)
std::unordered_map<uint32_t, uint32_t> g_boss;     // boss object -> its archive
std::vector<uint32_t> g_stack;                     // the archive wanted by each running task node (0 = not a boss)
uint32_t g_want = 0;                               // the archive key 0x16 should give now (0 = the owner's)
uint32_t g_node = 0;                               // key 0x16's map node (learned at the first find)
std::unordered_map<uint32_t, std::vector<uint32_t>> g_code;   // archive -> return addresses seen in its boss's own lookups
constexpr uint32_t kBossCodeLo = 0x80200000u, kNear = 0x6000;   // boss classes' own code lies above kBossCodeLo
// shared boss framework, never one boss's own code: boss base (fn_8027DEB0 ..) and the BossCtrlUnit classes
bool shared_code(uint32_t a) { return (a >= 0x8027D000u && a < 0x80280000u) || (a >= 0x80290000u && a < 0x80293000u); }
template <class F> void frames(uint32_t sp, F f) {   // the guest call stack's return addresses (back chain)
  for (int i = 0; i < 32; ++i) { if (!ram(sp)) return; sp = ld32(sp); if (!ram(sp)) return; const uint32_t lr = ld32(sp + 4); if (lr >= kBossCodeLo && lr < 0x804AA000u && !shared_code(lr)) f(lr); }
}
// #125: archives with the same path hold the same data (the three Egg Breakers each load EggMecaOne.one from
// their own call site) - one family. Code learning and the lookup guess work per family, otherwise the shared Egg
// Breaker code is "specific" to none of them and a helper's lookup fell to the owner's archive (hang with 2-3 Egg Breakers).
uint32_t family(uint32_t file) {
  const Arch* me = nullptr;
  for (const auto& a : g_arch) if (a.file == file) { me = &a; break; }
  if (!me) return file;
  for (const auto& a : g_arch) if (a.path == me->path) return a.file;
  return file;
}
void learn(uint32_t sp) {   // inside a boss's own update: its code
  auto& v = g_code[family(g_want)];
  frames(sp, [&](uint32_t lr) { for (uint32_t x : v) if (x == lr) return; if (v.size() < 256) v.push_back(lr); });
}
bool specific(uint32_t arch, uint32_t lr) {   // seen for this boss only
  for (const auto& [k, v] : g_code) if (k != arch) for (uint32_t x : v) if (x == lr) return false;
  return true;
}
uint32_t guess(uint32_t sp) {   // a lookup outside any boss node (e.g. a boss's own helper task): the boss whose code is on the stack
  uint32_t hit = 0;
  frames(sp, [&](uint32_t lr) {   // the innermost frame near any boss's code decides; the NEAREST code wins (#125: the
    if (hit) return;                // Heavy Dog 0410 and Blue Falcon code blocks are neighbours, the first match was the wrong dog)
    uint32_t best = kNear;
    for (const auto& [arch, v] : g_code) for (uint32_t x : v) {
      const uint32_t d = x > lr ? x - lr : lr - x;
      if (d < best && specific(arch, x)) { best = d; hit = arch; }
    }
  });
  return hit;
}
bool active() { return g_arch.size() > 1; }
std::string guest_str(uint32_t a) {
  std::string s;
  if (!ram(a)) return s;
  for (uint32_t i = 0; i < 96; ++i) { const uint8_t ch = ld8(a + i); if (!ch) break; s += char(ch); }
  return s;
}
bool ours(uint32_t file) { for (const auto& a : g_arch) if (a.file == file) return true; return false; }
void apply(uint32_t want) {   // key 0x16's node -> that archive (0 = the owner's)
  if (!g_node || !ram(g_node + 0x10)) return;
  const uint32_t f = want ? want : g_arch.front().file, cur = ld32(g_node + 0x10);
  if (cur != f && ours(cur)) st32(g_node + 0x10, f);
}
const Arch* archive_for(uint32_t boss) {
  const uint32_t vt = ld32(boss + 0x18);
  if (!ram(vt)) return nullptr;
  const uint32_t init = ld32(vt + 0x10);
  const Arch* best = nullptr;
  uint32_t bd = 0xFFFFFFFFu;
  for (const auto& a : g_arch) { const uint32_t d = a.site > init ? a.site - init : init - a.site; if (d < bd) { bd = d; best = &a; } }
  logf("[boss-slot] boss %08X vtable %08X init %08X -> %s (load site %08X)\n", boss, vt, init, best ? best->path.c_str() : "?", best ? best->site : 0);
  return best;
}
}  // namespace

namespace boss_slots {
// the task-tree walker's per-node call (game_hooks.cpp via stage_workshop::node_enter / node_leave)
void enter(uint32_t obj) {
  if (!active()) return;
  g_stack.push_back(g_want);
  const auto it = g_boss.find(obj);
  if (it != g_boss.end()) { g_want = it->second; apply(g_want); }   // child nodes of a boss keep its archive
}
void leave() {
  if (!active() || g_stack.empty()) return;
  const uint32_t was = g_want;
  g_want = g_stack.back();
  g_stack.pop_back();
  if (was != g_want) apply(g_want);
}
}  // namespace boss_slots

void hle_BossSlotRegister(Cpu& c) {
  const uint32_t mgr = c.r[3], key = c.r[4], file = c.r[5], site = c.lr;
  f_8001299C(c);
  if (key != kBossKey || !ram(file)) return;
  const bool owner = c.r[3] != 0;
  if (owner) { g_arch.clear(); g_boss.clear(); g_stack.clear(); g_code.clear(); g_want = 0; g_node = 0; }   // a new stage
  uint32_t extra = 0;
  if (!owner) {   // refused: register it under a key of its own so the manager owns (and frees) it like any other
    const Cpu keep = c;
    for (uint32_t k = kExtraKey; k < kExtraKey + 16 && !extra; ++k) { c.r[3] = mgr; c.r[4] = k; c.r[5] = file; f_8001299C(c); if (c.r[3]) extra = k; }
    c = keep; c.r[3] = 0;   // the caller sees the original answer
    if (!extra) { logf("[boss-slot] no free key for %s - left as the game had it\n", guest_str(ld32(file + 0x10)).c_str()); return; }
  }
  g_arch.push_back({file, site, guest_str(ld32(file + 0x10))});
  logf("[boss-slot] key 0x16 %s %s (load site %08X)", owner ? "owner" : "taken -", g_arch.back().path.c_str(), site);
  if (extra) logf(" -> its own key 0x%X", extra);
  logf("\n");
}

void hle_BossSlotFind(Cpu& c) {
  const uint32_t out = c.r[3], keyp = c.r[5];
  f_800124E4(c);
  if (!active() || !ram(keyp) || ld32(keyp) != kBossKey || !ram(out)) return;
  const uint32_t node = ld32(out);
  if (!ram(node) || !ram(node + 0x10) || !ours(ld32(node + 0x10))) return;   // only key 0x16's node holds one of ours
  g_node = node;
  if (g_want) { learn(c.r[1]); apply(g_want); return; }
  const uint32_t g = guess(c.r[1]);
  static const bool dbg = getenv("BOSS_SLOT_LOG") != nullptr;
  static int n = 0;
  if (dbg && g && n++ < 20) logf("[boss-slot] lookup outside a boss node (lr %08X) -> its boss's archive\n", c.lr);
  apply(g);
}

void hle_BossBaseUpdate(Cpu& c) {
  const uint32_t boss = c.r[3];
  if (!active() || !ram(boss)) { f_8027DEB0(c); if (ram(boss)) boss_arena::step(boss); return; }
  auto it = g_boss.find(boss);
  if (it == g_boss.end()) { const Arch* a = archive_for(boss); it = g_boss.emplace(boss, a ? family(a->file) : 0).first; }
  const uint32_t file = it->second;
  if (file && ld32(file + 4) != 3) {   // the manager's load loop (fn_80012A30) only runs on the loading screen
    const Cpu keep = c;
    c.r[3] = file;
    call_addr(c, ld32(ld32(file) + 0xC));
    c = keep;
  }
  boss_slots::enter(boss);
  f_8027DEB0(c);
  boss_slots::leave();
  boss_arena::step(boss);   // #123 the boss card's arena (boss_arena.cpp)
  // BOSS_POSDUMP=1 (research, #123 arena leash): every word of each boss object 0..0x800 at its 120th and 900th update
  static const bool posdump = getenv("BOSS_POSDUMP") != nullptr;
  if (posdump) {
    static std::unordered_map<uint32_t, int> upd;
    const int k = ++upd[boss];
    if (k == 120 || k == 900)
      for (uint32_t off = 0; off < 0x800; off += 4) { const uint32_t w = ld32(boss + off); float f; memcpy(&f, &w, 4);
        logf("[bdump] %08X u%d +%03X %08X %g\n", boss, k, off, w, std::isfinite(f) ? f : 0.f); }
  }
  static const bool dbg = getenv("BOSS_SLOT_LOG") != nullptr;
  static int n = 0;
  if (dbg && n < 40 && (n++ % 4) == 0) {
    std::string st;
    for (const auto& x : g_arch) { char t[96]; snprintf(t, sizeof t, " %s=%u", x.path.substr(x.path.rfind('/') + 1).c_str(), ld32(x.file + 4)); st += t; }
    for (const auto& [k, v] : g_code) { char t[48]; snprintf(t, sizeof t, " code[%08X]=%zu", k, v.size()); st += t; }
    logf("[boss-slot] update boss %08X inited %u node %08X states%s\n", boss, ld8(boss + 0x30), g_node, st.c_str());
  }
}
