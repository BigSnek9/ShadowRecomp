// Homing rework (docs/gameplay/MOVEMENT-REWORK.md section 3; the agents/claude/research/movement/homing-targets.md).
// The game's homing pick fn_80206814 calls the nearest-target helper fn_801E848C(out, list, from) twice: at 0x80206938
// on the filtered candidates (A) and at 0x802069A4 on the stick-cone subset (B); B wins when it found one, else A.
// Without stick a third call at 0x80206A7C picks from the front half-space (facing) - treated like A.
// settings hm_mode 0 = Original (untouched). 1 = Reworked: at A every candidate is scored (direction to the stick, or
// Shadow's facing without stick; distance; enemies first) and the list is reduced to the winner for the original call,
// so the game's own handle / lock-on / line-of-sight / chain logic runs on it; at B the list is emptied so A's pick
// stands. HOMING_LOG=1 logs each pick.
#include "settings_ui.h"
#include "sys.h"
#include <cmath>
#include <cstdlib>
#include <vector>
#include <algorithm>

void f_801E848C(Cpu&);
void f_801E79E0(Cpu&);
void f_80057858(Cpu&);
void f_80057778(Cpu&);
void f_8008A130(Cpu&);
void f_80057728(Cpu&);

namespace movement { uint32_t player(); }

namespace {
struct Cand { uint32_t slot, target, obj; float d, cosang; bool enemy; float score; };

// the list's entries: begin = fn_801E79E0(list + 4), end = list + 0x10
bool entries(Cpu& c, uint32_t list, uint32_t& begin, uint32_t& end) {
  if (list < 0x80000000u || list >= RT_RAM_END - 0x20) return false;
  const Cpu saved = c;
  c.r[3] = list + 4;
  f_801E79E0(c);
  begin = c.r[3];
  c = saved;
  end = ld32(list + 0x10);
  return begin >= 0x80000000u && end >= begin && end - begin < 4 * 256;
}
// an entry is a reference: the game turns it into a handle (fn_80057858 init, fn_80057778, fn_8008A130), takes the object,
// asks it for its position (vtable +8: out, object, from) and releases the handles (fn_80057728(h, -1)) - done the same way
bool position(Cpu& c, uint32_t entry, uint32_t from, float out[3], uint32_t& obj) {
  const Cpu saved = c;
  const uint32_t sp = (c.r[1] - 0x100) & ~0xFu;
  c.r[1] = sp;
  c.r[3] = sp + 0x18; f_80057858(c);
  c.r[3] = sp + 0x10; c.r[4] = entry; f_80057778(c);
  c.r[3] = sp + 0x18; c.r[4] = sp + 0x10; f_8008A130(c);
  c.r[3] = sp + 0x10; c.r[4] = 0xFFFFFFFFu; f_80057728(c);
  obj = ld32(sp + 0x18);
  bool ok = false;
  if (obj >= 0x80000000u && obj < RT_RAM_END - 0x40) {
    const uint32_t vt = ld32(obj), fn = vt >= 0x80000000u && vt < RT_RAM_END ? ld32(vt + 8) : 0;
    if (fn >= 0x80000000u && fn < 0x80600000u) {
      c.r[3] = sp + 0x20; c.r[4] = obj; c.r[5] = from;
      c.lr = 0x801E8544u;
      call_addr(c, fn);
      for (int i = 0; i < 3; ++i) out[i] = (float)ldf32(sp + 0x20 + 4 * i);
      ok = std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
    }
  }
  c.r[1] = sp; c.r[3] = sp + 0x18; c.r[4] = 0xFFFFFFFFu; f_80057728(c);
  c = saved;
  return ok;
}
}  // namespace

void hle_HomingNearest(Cpu& c) {
  const uint32_t lr = c.lr, list = c.r[4], from = c.r[5];
  const bool pickA = lr == 0x80206938u || lr == 0x80206A7Cu, pickB = lr == 0x802069A4u;   // A / no-stick C: score; B: empty
  { static const bool log = getenv("HOMING_LOG") != nullptr; static int n = 0;
    static std::vector<uint32_t> seen; bool fresh = std::find(seen.begin(), seen.end(), lr) == seen.end(); if (fresh) seen.push_back(lr);
    if (log && (pickA || pickB) && n++ < 60) { uint32_t b = 0, e = 0; entries(c, c.r[4], b, e); logf("[homing] nearest() from %08X, %u entries, mode %d\n", lr, (e - b) / 4, settings::g.hm_mode); } }
  if (settings::g.hm_mode != 1 || (!pickA && !pickB)) { f_801E848C(c); return; }
  uint32_t begin = 0, end = 0;
  if (!entries(c, list, begin, end) || begin == end) { f_801E848C(c); return; }
  if (pickB) {   // the cone subset: emptied, so the game falls back to A (our pick)
    const uint32_t e = ld32(list + 0x10);
    st32(list + 0x10, begin);
    f_801E848C(c);
    st32(list + 0x10, e);
    return;
  }
  const uint32_t p = movement::player();
  static const bool dlog = getenv("HOMING_LOG") != nullptr;
  if (p < 0x80000000u || p >= RT_RAM_END - 0x200 || from < 0x80000000u || from >= RT_RAM_END) {
    if (dlog) logf("[homing] skip: player %08X from %08X\n", p, from);
    f_801E848C(c); return;
  }
  const float px = (float)ldf32(from), py = (float)ldf32(from + 4), pz = (float)ldf32(from + 8);
  // reference direction: the stick in world space (P+0xB8, its length = the tilt), else Shadow's facing (yaw P+0x10, 0 = -z)
  float rx = (float)ldf32(p + 0xB8), rz = (float)ldf32(p + 0xC0);
  const float tilt = std::sqrt(rx * rx + rz * rz);
  if (tilt < 0.25f) { const float yaw = (float)ldf32(p + 0x10); rx = -std::sin(yaw); rz = -std::cos(yaw); }
  else { rx /= tilt; rz /= tilt; }
  std::vector<Cand> cs;
  float dmax = 1;
  for (uint32_t s = begin; s < end && cs.size() < 32; s += 4) {
    const uint32_t t = ld32(s);
    float q[3];
    uint32_t obj = 0;
    if (!position(c, t, from, q, obj)) continue;
    const float dx = q[0] - px, dy = q[1] - py, dz = q[2] - pz;
    const float d = std::sqrt(dx * dx + dy * dy + dz * dz), h = std::sqrt(dx * dx + dz * dz);
    const float cosang = h > 1e-3f ? (dx * rx + dz * rz) / h : 1.f;
    const bool enemy = (ld32(obj + 0x30) & 0x70000u) != 0;   // the Target's flags: faction bits = an enemy
    cs.push_back({s, t, obj, d, cosang, enemy, 0});
    dmax = std::fmax(dmax, d);
  }
  if (cs.empty()) { if (dlog) logf("[homing] skip: no candidate position (first vtable %08X)\n", ld32(begin)); f_801E848C(c); return; }
  size_t best = 0;
  for (size_t i = 0; i < cs.size(); ++i) {
    Cand& k = cs[i];
    k.score = 1.0f * k.cosang + 0.8f * (1 - k.d / (dmax * 1.05f)) + (k.enemy ? 0.6f : 0.f);
    if (k.score > cs[best].score) best = i;
  }
  // the winner first, the list reduced to it for the game's own pick
  const uint32_t first = ld32(begin), win = cs[best].slot, wt = cs[best].target;
  st32(begin, wt); st32(win, first);
  const uint32_t e = ld32(list + 0x10);
  st32(list + 0x10, begin + 4);
  f_801E848C(c);
  st32(list + 0x10, e);
  st32(win, wt); st32(begin, first);
  static const bool log = getenv("HOMING_LOG") != nullptr;
  if (log) {
    logf("[homing] reworked pick of %zu: target %08X d %.0f cos %.2f enemy %d score %.2f (flags %08X)\n", cs.size(), wt, cs[best].d,
         cs[best].cosang, (int)cs[best].enemy, cs[best].score, ld32(cs[best].obj + 0x30));
    for (const Cand& k : cs)
      if (k.target != wt) logf("[homing]   other %08X d %.0f cos %.2f enemy %d score %.2f\n", k.target, k.d, k.cosang, (int)k.enemy, k.score);
  }
}
