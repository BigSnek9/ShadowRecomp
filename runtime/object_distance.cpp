// Object draw distance (settings draw_objects, %; the research/activation-distance-tests.md): every SET record has
// an activation byte at +0x23; the game creates / keeps an object while the player is within 100 x that byte (+ a margin)
// - the activation scan, the update range fn_800C996C, the create range fn_800C9938 and the other readers all use it.
// User test 2026-10-18: scaling only the update range kept objects longer but did not SPAWN them sooner. So the byte
// itself is scaled live in the loaded record table (SetManager *0x805EE6D4: count at +0, 0x20-byte entries at *(+0x18),
// record pointer at entry +0), exactly what the editor's build-side activationScale does to the file:
// everything x s, rings / items / decor / breakables x min(3, 1.5 s), triggers / checkpoints / goal / generators /
// mission objects never (server.py ACTIVATION_PRESETS['wider']); byte 0 stays 0, at most 255; capped at 3x ('max 255'
// measured 8.5-21 fps). The original byte of each record is remembered; a slider change re-applies from it, and a byte
// someone else wrote (the workshop VM's spawn sets 255) becomes that record's new original. 100 % = the file's bytes.
#include "object_distance.h"
#include "settings_ui.h"
#include "sys.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <unordered_map>

namespace object_distance {
namespace {
#include "object_distance_classes.inc"
float factor(uint32_t rec, int pct) {
  if (pct == 100) return 1;
  const uint16_t key = uint16_t((ld8(rec + 0x20) << 8) | ld8(rec + 0x21));   // the object id in memory: list +0x20, type +0x21
  for (uint16_t k : kDrawVanilla) if (k == key) return 1;
  const float s = pct / 100.f;
  for (uint16_t k : kDrawDecor) if (k == key) return std::min(3.f, s * 1.5f);
  return s;
}
struct Rec { uint8_t orig, written; };
std::unordered_map<uint32_t, Rec> g_recs;
uint32_t g_table = 0;
}  // namespace

void apply() {
  const uint32_t m = ld32(0x805EE6D4);
  if (m < 0x80000000u || m >= RT_RAM_END) return;
  const uint32_t n = ld32(m), tab = ld32(m + 0x18);
  if (n > 8192 || tab < 0x80000000u || tab + n * 0x20 > RT_RAM_END) return;
  if (tab != g_table) { g_recs.clear(); g_table = tab; }   // a new stage / table
  static const bool keys = getenv("OBJDIST_KEYS") != nullptr;
  static uint32_t keyed = 0;
  if (keys && n && keyed != n) {   // the class keys of the table (list << 8 | type), checks the class table
    keyed = n;
    for (uint32_t i = 0; i < n && i < 400; i += 7) {
      const uint32_t rec = ld32(tab + i * 0x20);
      if (rec >= 0x80000000u && rec < RT_RAM_END - 0x30)
        logf("[objdist] rec %u key %02X%02X (+0x20 %02X +0x21 %02X) byte %u\n", i, ld8(rec + 0x21), ld8(rec + 0x20), ld8(rec + 0x20), ld8(rec + 0x21), ld8(rec + 0x23));
    }
  }
  const int pct = std::clamp(settings::g.draw_objects, 100, 300);
  uint32_t changed = 0, scaled = 0;
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t rec = ld32(tab + i * 0x20);
    if (rec < 0x80000000u || rec >= RT_RAM_END - 0x30) continue;
    const uint8_t now = ld8(rec + 0x23);
    auto [it, fresh] = g_recs.try_emplace(rec, Rec{now, now});
    Rec& r = it->second;
    if (!fresh && now != r.written) r.orig = now;   // written by someone else since: their value is the new original
    const float k = factor(rec, pct);
    const uint8_t want = r.orig == 0 ? 0 : uint8_t(std::min(255.f, std::round(r.orig * k)));
    if (k != 1) ++scaled;
    if (want != now) { st8(rec + 0x23, want); ++changed; }
    r.written = want;
  }
  static const bool log = getenv("OBJDIST_LOG") != nullptr;
  static int last_pct = -1;
  static uint32_t last_n = 0;
  if (log && (changed || pct != last_pct || n != last_n))
    logf("[objdist] %d%%: %u records, %u scaled, %u bytes rewritten\n", pct, n, scaled, changed);
  last_pct = pct; last_n = n;
}
int original_byte(uint32_t rec) {
  const auto it = g_recs.find(rec);
  return it == g_recs.end() ? -1 : it->second.orig;
}
}  // namespace object_distance
