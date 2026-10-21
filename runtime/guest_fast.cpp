// #152 #7: native fast paths for the game's hottest engine plumbing - identical results, always on.
// docs/perf/OPTIMISATION.md.
//
// fn_803A0AAC = CodeWarrior's __dynamic_cast(obj r3, vtable-pointer offset r4, target type r5, source type r6,
// throw-on-failure r7). It walks the object's class-hierarchy table comparing type NAME strings byte by byte - slow,
// and called for nearly every object message. Its answer depends only on read-only data: the object's vtable (at
// obj + r4) and the type infos r5 / r6. So it is memoised per (vtable, r4, r5, r6): either "fails" or "obj + delta".
// A miss, a vtable outside the static data, or a failure that throws (r7 != 0: bad_cast) runs the original.
// DCAST_CHECK=1: every call also runs the original and any difference is logged (validation).
#include "sys.h"
#include <cstdint>
#include <cstdlib>
#include <unordered_map>

void f_803A0AAC(Cpu&);   // __dynamic_cast (recompiled original)

namespace {
struct Key {
  uint32_t vt, off, dst, src;
  bool operator==(const Key& o) const { return vt == o.vt && off == o.off && dst == o.dst && src == o.src; }
};
struct KeyHash {
  size_t operator()(const Key& k) const {
    uint64_t h = k.vt * 0x9E3779B97F4A7C15ull ^ (uint64_t(k.dst) << 7) ^ (uint64_t(k.src) * 0xC2B2AE3D27D4EB4Full) ^ k.off;
    return size_t(h ^ (h >> 29));
  }
};
constexpr int32_t kFail = INT32_MIN;
std::unordered_map<Key, int32_t, KeyHash> g_memo;
uint64_t g_hits = 0, g_misses = 0;
}  // namespace

void hle_DynamicCast(Cpu& c) {
  static const bool check = getenv("DCAST_CHECK") != nullptr;
  static const bool off = getenv("DCAST_OFF") != nullptr;   // A/B: the original only
  if (off) { f_803A0AAC(c); return; }
  const uint32_t obj = c.r[3];
  if (!obj) { c.r[3] = 0; return; }   // the original's first test
  const uint32_t vt = ld32(obj + c.r[4]);
  const Key k{vt, c.r[4], c.r[5], c.r[6]};
  const bool cacheable = vt >= 0x80003000u && vt < 0x80600000u;   // a static vtable (code / data sections)
  if (cacheable && !check) {
    const auto it = g_memo.find(k);
    if (it != g_memo.end() && (it->second != kFail || (int16_t)c.r[7] == 0)) {
      c.r[3] = it->second == kFail ? 0 : obj + uint32_t(it->second);
      ++g_hits;
      return;
    }
  }
  ++g_misses;
  const auto memo = cacheable ? g_memo.find(k) : g_memo.end();
  f_803A0AAC(c);   // the original (also throws bad_cast where it must)
  const int32_t got = c.r[3] ? int32_t(c.r[3] - obj) : kFail;
  if (!cacheable) return;
  if (check && memo != g_memo.end() && memo->second != got) {
    static int n = 0;
    if (n++ < 50) logf("[dcast] MISMATCH vt %08X off %X dst %08X src %08X: memo %d, original %d\n", vt, k.off, k.dst, k.src, memo->second, got);
  }
  g_memo[k] = got;
  static uint64_t last = 0;
  if (check && sys_time_ns() - last > 5000000000ull) {
    last = sys_time_ns();
    logf("[dcast] %zu memo entries, %llu calls checked\n", g_memo.size(), (unsigned long long)g_misses);
  }
}
