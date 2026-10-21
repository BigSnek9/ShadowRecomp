// #152 #2: what the extra objects of a wider object distance cost on the game thread.
// fn_801A6EC4 = an enemy task's per-frame update (docs/stage-workshop/research/enemy-ai-59.md): wrapped here.
// PERF_OBJ=1: once a second, how many enemy updates ran and the time spent inside them (ms per second).
#include "sys.h"
#include "object_distance.h"
#include "stage_workshop.h"
#include <cstring>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>
#include <unordered_map>
#include "settings_ui.h"

void f_801A6EC4(Cpu&);   // the enemy task update (recompiled original)

// ENEMY_REC=1 (research): for each new enemy, the offsets inside the enemy and its modules (e+4..e+0x10) that hold a
// pointer to a loaded SET record, and that record's first floats (its position) next to the player's position
static void enemy_rec_research(uint32_t task) {
  static int logged = 0;
  if (logged >= 12) return;
  const uint32_t e = ld32(task + 4);
  if (e < 0x80000000u || e >= RT_RAM_END - 0x400) return;
  static uint32_t seen[64]; static int nseen = 0;
  for (int i = 0; i < nseen; ++i) if (seen[i] == e) return;
  if (nseen < 64) seen[nseen++] = e;
  ++logged;
  float pp[3] = {};
  stage_workshop::player_position(pp);
  logf("[enemy-rec] enemy %08X (task %08X), player %.0f %.0f %.0f\n", e, task, pp[0], pp[1], pp[2]);
  {   // the SET table: entries are 0x20 bytes, record pointer at +0; look for pointers to entries / records near the task
    const uint32_t m = ld32(0x805EE6D4), n = m >= 0x80000000u && m < RT_RAM_END ? ld32(m) : 0, tab = n ? ld32(m + 0x18) : 0;
    auto fl = [](uint32_t a) { uint32_t u = ld32(a); float x; memcpy(&x, &u, 4); return x; };
    if (logged == 1 && tab)
      for (uint32_t i = 0; i < n && i < 6; ++i) {
        const uint32_t r = ld32(tab + i * 0x20);
        logf("[enemy-rec] record %u %08X key %02X%02X byte %u: %.0f %.0f %.0f %.0f %.0f %.0f %.0f %.0f\n", i, r, ld8(r + 0x21), ld8(r + 0x20),
             ld8(r + 0x23), fl(r), fl(r + 4), fl(r + 8), fl(r + 0xC), fl(r + 0x10), fl(r + 0x14), fl(r + 0x18), fl(r + 0x1C));
      }
    for (uint32_t off = 0; off < 0x800 && tab; off += 4) {
      const uint32_t v = ld32(task + off);
      if (v >= tab && v < tab + n * 0x20 && ((v - tab) & 0x1F) == 0)
        logf("[enemy-rec]   task+%03X -> table entry %u (record %08X)\n", off, (v - tab) / 0x20, ld32(v));
      else if (object_distance::original_byte(v) >= 0)
        logf("[enemy-rec]   task+%03X -> record %08X\n", off, v);
    }
  }
  {   // the enemy's current position: float triples near its record's position (enemy - 4 = its table entry)
    auto fl = [](uint32_t a) { uint32_t u = ld32(a); float x; memcpy(&x, &u, 4); return x; };
    const uint32_t ent = ld32(e - 4), rec = ent >= 0x80000000u && ent < RT_RAM_END ? ld32(ent) : 0;
    if (object_distance::original_byte(rec) >= 0) {
      const float rx = fl(rec), ry = fl(rec + 4), rz = fl(rec + 8);
      const uint32_t mods[5] = {e, ld32(e + 4), ld32(e + 8), ld32(e + 0xC), ld32(e + 0x10)};
      for (int b = 0; b < 5; ++b) {   // one level deeper: heap pointers in the first 0x200 bytes of each module
        if (mods[b] < 0x80000000u || mods[b] >= RT_RAM_END - 0x200) continue;
        for (uint32_t po = 0; po < 0x200; po += 4) {
          const uint32_t q = ld32(mods[b] + po);
          if (q < 0x80700000u || q >= RT_RAM_END - 0x200 || q == e) continue;
          for (uint32_t off = 0; off < 0x1F4; off += 4) {
            const float x = fl(q + off), y = fl(q + off + 4), z = fl(q + off + 8);
            if (std::fabs(x - rx) < 150 && std::fabs(y - ry) < 150 && std::fabs(z - rz) < 150 && (x != 0 || z != 0))
              logf("[enemy-rec]   pos2? %s+%03X -> +%03X = %.1f %.1f %.1f (record %.1f %.1f %.1f)\n",
                   b == 0 ? "e" : b == 1 ? "ctrl" : b == 2 ? "move" : b == 3 ? "disp" : "status", po, off, x, y, z, rx, ry, rz);
          }
        }
      }
      for (int b = 0; b < 5; ++b) {
        if (mods[b] < 0x80000000u || mods[b] >= RT_RAM_END - 0x1000) continue;
        for (uint32_t off = 0; off < 0xFF4; off += 4) {
          const float x = fl(mods[b] + off), y = fl(mods[b] + off + 4), z = fl(mods[b] + off + 8);
          if (std::fabs(x - rx) < 150 && std::fabs(y - ry) < 150 && std::fabs(z - rz) < 150 && (x != 0 || z != 0))
            logf("[enemy-rec]   pos? %s+%03X = %.1f %.1f %.1f (record %.1f %.1f %.1f)\n", b == 0 ? "e" : b == 1 ? "ctrl" : b == 2 ? "move" : b == 3 ? "disp" : "status",
                 off, x, y, z, rx, ry, rz);
        }
      }
    }
  }
  const uint32_t bases[5] = {e, ld32(e + 4), ld32(e + 8), ld32(e + 0xC), ld32(e + 0x10)};
  for (int b = 0; b < 5; ++b) {
    const uint32_t base = bases[b];
    if (base < 0x80000000u || base >= RT_RAM_END - 0x400) continue;
    for (uint32_t off = 0; off < 0x400; off += 4) {
      const uint32_t v = ld32(base + off);
      const int ob = object_distance::original_byte(v);
      if (ob < 0) continue;
      auto f = [&](uint32_t a) { uint32_t u = ld32(a); float x; memcpy(&x, &u, 4); return x; };
      logf("[enemy-rec]   %s+%03X -> record %08X (byte %d now %d): floats %.0f %.0f %.0f | %.0f %.0f %.0f\n",
           b == 0 ? "e" : b == 1 ? "ctrl" : b == 2 ? "move" : b == 3 ? "disp" : "status", off, v, ob, ld8(v + 0x23),
           f(v), f(v + 4), f(v + 8), f(v + 0xC), f(v + 0x10), f(v + 0x14));
    }
  }
}

// ENEMY_MOVE=1 (research): snapshot the enemy object + its 4 modules (0x800 bytes each) at first sight, and 3 s later
// report the float triples that moved by a walking distance (candidates for the live world position)
static void enemy_move_research(uint32_t task) {
  struct Snap { uint32_t e, base[5]; uint64_t t; std::vector<uint32_t> words[5]; bool done; };
  static std::vector<Snap> snaps;
  const uint32_t e = ld32(task + 4);
  if (e < 0x80000000u || e >= RT_RAM_END - 0x800) return;
  const uint64_t now = sys_time_ns();
  for (auto& s : snaps) {
    if (s.e != e) continue;
    if (s.done || now - s.t < 3000000000ull) return;
    s.done = true;
    auto fl = [](uint32_t u) { float x; memcpy(&x, &u, 4); return x; };
    for (int b = 0; b < 5; ++b) {
      if (s.words[b].empty() || ld32(e + 4 * b) != (b ? s.base[b] : e)) continue;
      for (uint32_t i = 0; i + 2 < s.words[b].size(); ++i) {
        const float x0 = fl(s.words[b][i]), y0 = fl(s.words[b][i + 1]), z0 = fl(s.words[b][i + 2]);
        const float x1 = fl(ld32(s.base[b] + i * 4)), y1 = fl(ld32(s.base[b] + i * 4 + 4)), z1 = fl(ld32(s.base[b] + i * 4 + 8));
        const float d = std::hypot(x1 - x0, z1 - z0);
        if (std::isfinite(d) && d > 5 && d < 3000 && std::fabs(x1) > 20 && std::fabs(x1) < 100000 && std::fabs(z1) < 100000 && std::fabs(y1) < 100000)
          logf("[enemy-move] enemy %08X %s+%03X: (%.0f %.0f %.0f) -> (%.0f %.0f %.0f) moved %.0f\n", e,
               b == 0 ? "e" : b == 1 ? "ctrl" : b == 2 ? "move" : b == 3 ? "disp" : "status", i * 4, x0, y0, z0, x1, y1, z1, d);
      }
    }
    return;
  }
  if (snaps.size() >= 8) return;
  Snap s{e, {e, ld32(e + 4), ld32(e + 8), ld32(e + 0xC), ld32(e + 0x10)}, now, {}, false};
  for (int b = 0; b < 5; ++b) {
    if (s.base[b] < 0x80000000u || s.base[b] >= RT_RAM_END - 0x800) continue;
    for (uint32_t o = 0; o < 0x800; o += 4) s.words[b].push_back(ld32(s.base[b] + o));
  }
  snaps.push_back(std::move(s));
}

// ENEMY_MTX=1 (research): RenderWare-style matrices (3 unit vectors + a world position at +0x30) inside the enemy, its
// modules and one pointer level below them: where the drawn model's transform (= its live position) lives
static void enemy_mtx_research(uint32_t task) {
  static int logged = 0;
  if (logged >= 10) return;
  const uint32_t e = ld32(task + 4);
  if (e < 0x80000000u || e >= RT_RAM_END - 0x800) return;
  static uint32_t seen[32]; static int nseen = 0;
  for (int i = 0; i < nseen; ++i) if (seen[i] == e) return;
  if (nseen < 32) seen[nseen++] = e;
  ++logged;
  auto fl = [](uint32_t a) { uint32_t u = ld32(a); float x; memcpy(&x, &u, 4); return x; };
  auto unit = [&](uint32_t a) { const float x = fl(a), y = fl(a + 4), z = fl(a + 8); const float l = x * x + y * y + z * z; return std::isfinite(l) && std::fabs(l - 1) < 0.02f; };
  auto scan = [&](uint32_t base, uint32_t len, const char* what, uint32_t via) {
    if (base < 0x80000000u || base >= RT_RAM_END - len - 0x40) return;
    for (uint32_t o = 0; o < len; o += 4)
      if (unit(base + o) && unit(base + o + 0x10) && unit(base + o + 0x20)) {
        const float x = fl(base + o + 0x30), y = fl(base + o + 0x34), z = fl(base + o + 0x38);
        if (std::isfinite(x) && std::fabs(x) + std::fabs(z) > 20 && std::fabs(x) < 1e5f && std::fabs(z) < 1e5f)
          logf("[enemy-mtx] enemy %08X %s%s+%03X (+%03X): pos %.0f %.0f %.0f\n", e, what, via ? " ->" : "", via, o, x, y, z);
      }
  };
  const char* names[5] = {"e", "ctrl", "move", "disp", "status"};
  for (int b = 0; b < 5; ++b) {
    const uint32_t m = b ? ld32(e + 4 * b) : e;
    scan(m, 0x800, names[b], 0);
    if (m < 0x80000000u || m >= RT_RAM_END - 0x200) continue;
    for (uint32_t po = 0; po < 0x200; po += 4) {
      const uint32_t q = ld32(m + po);
      if (q >= 0x80700000u && q < RT_RAM_END - 0x200 && q != e) scan(q, 0x100, names[b], po);
    }
  }
}

// #152 #2 far-enemy update rate (an Enhancement: only with "Original game logic" off and object distance above 100 %).
// An enemy whose spawn point is further from the player than its ORIGINAL activation range + 500 units exists only
// because of the wider object distance. Enemies stay near their spawn (they wander 30-70 units; ENEMY_POS research),
// so such an enemy is far from the player. It updates every 2nd frame (every 3rd beyond twice that distance) with the
// skipped frame time added (f1 = dt), so it moves / animates the same amount over time; it is still drawn every frame.
// When it comes back in range, the time it is owed is added to its next update. Running phase (task+8 == 4) only.
static uint64_t g_far_skipped = 0, g_far_run = 0;
static bool far_enemy_skip(Cpu& c) {
  struct State { uint32_t n = 0; double owed = 0; };
  static std::unordered_map<uint32_t, State> st;
  const uint32_t task = c.r[3];
  const auto& g = settings::g;
  if (g.original_logic || g.draw_objects <= 100 || ld32(task + 8) != 4) {
    if (!st.empty() && g.original_logic) st.clear();
    return false;
  }
  const uint32_t e = ld32(task + 4);
  const uint32_t ent = e >= 0x80000004u && e < RT_RAM_END ? ld32(e - 4) : 0;
  const uint32_t rec = ent >= 0x80000000u && ent < RT_RAM_END - 0x30 ? ld32(ent) : 0;
  const int ob = object_distance::original_byte(rec);
  float pp[3];
  int every = 1;
  if (ob > 0 && stage_workshop::player_position(pp)) {
    auto fl = [](uint32_t a) { uint32_t u = ld32(a); float x; memcpy(&x, &u, 4); return x; };
    const float dx = fl(rec) - pp[0], dy = fl(rec + 4) - pp[1], dz = fl(rec + 8) - pp[2];
    const float d = std::sqrt(dx * dx + dy * dy + dz * dz), r = ob * 100.f + 500.f;
    static const bool flog = getenv("FAR_LOG") != nullptr;   // research: distance / original range of the enemies updating
    if (flog) {
      static int hist[6]; static uint64_t t0 = 0;
      const float q = d / (ob * 100.f + 50.f);
      ++hist[q < 0.5f ? 0 : q < 1 ? 1 : q < 1.5f ? 2 : q < 2 ? 3 : q < 3 ? 4 : 5];
      if (!t0) t0 = sys_time_ns();
      if (sys_time_ns() - t0 > 2000000000ull) {
        logf("[far] spawn distance / original range: <0.5 %d, <1 %d, <1.5 %d, <2 %d, <3 %d, >=3 %d (updates in 2 s)\n", hist[0], hist[1], hist[2], hist[3], hist[4], hist[5]);
        for (int& h : hist) h = 0;
        t0 = sys_time_ns();
      }
    }
    if (d > 2 * r) every = 3;
    else if (d > r) every = 2;
  }
  auto it = st.find(task);
  if (every == 1) {   // in range: full rate; pay out any time it is owed
    if (it != st.end()) {
      c.f[1] += it->second.owed;
      st.erase(it);
    }
    return false;
  }
  State& s = st[task];
  s.owed += c.f[1];
  if (++s.n % every) { ++g_far_skipped; return true; }   // skipped this frame
  c.f[1] = s.owed;    // this update covers the skipped frames too
  s.owed = 0;
  ++g_far_run;
  return false;
}

void hle_EnemyTaskUpdate(Cpu& c) {
  static const bool mtx_research = getenv("ENEMY_MTX") != nullptr;
  if (ld32(c.r[3] + 8) == 4 && mtx_research) enemy_mtx_research(c.r[3]);
  static const bool pos_research = getenv("ENEMY_POS") != nullptr;   // research: which of e[0]+0x48 / +0xB4 / +0xD4 moves
  if (pos_research && ld32(c.r[3] + 8) == 4) {
    static uint64_t last = 0;
    static uint32_t shown[16]; static int ns = 0;
    const uint64_t now = sys_time_ns();
    if (now - last > 2000000000ull) { last = now; ns = 0; }
    const uint32_t e = ld32(c.r[3] + 4), s = ld32(e), ent = ld32(e - 4), rec = ent >= 0x80000000u && ent < RT_RAM_END ? ld32(ent) : 0;
    bool dup = false;
    for (int i = 0; i < ns; ++i) dup |= shown[i] == e;
    if (!dup && ns < 6 && s >= 0x80000000u && s < RT_RAM_END - 0x100 && object_distance::original_byte(rec) >= 0) {
      shown[ns++] = e;
      auto fl = [](uint32_t a) { uint32_t u = ld32(a); float x; memcpy(&x, &u, 4); return x; };
      auto d = [&](uint32_t a) { return std::hypot(fl(a) - fl(rec), fl(a + 8) - fl(rec + 8)); };
      float pp[3] = {};
      stage_workshop::player_position(pp);
      logf("[enemy-pos] enemy %08X spawn %.0f %.0f %.0f | +6C %.0f %.0f %.0f | +D4 %.0f %.0f %.0f | player %.0f %.0f %.0f\n", e, fl(rec), fl(rec + 4),
           fl(rec + 8), fl(s + 0x6C), fl(s + 0x70), fl(s + 0x74), fl(s + 0xD4), fl(s + 0xD8), fl(s + 0xDC), pp[0], pp[1], pp[2]);
      (void)d;
    }
  }
  static const bool vt_research = getenv("ENEMY_VT") != nullptr;   // research: the move module's vtable / update function
  if (vt_research) {
    static uint32_t seen[32]; static int n = 0;
    const uint32_t e = ld32(c.r[3] + 4), mv = e >= 0x80000000u && e < RT_RAM_END ? ld32(e + 8) : 0;
    static int dumped = 0;
    if (dumped < 4 && e >= 0x80000000u && e < RT_RAM_END - 0x40) {
      static uint32_t dseen[8];
      bool k = false;
      for (int i = 0; i < dumped; ++i) k |= dseen[i] == e;
      if (!k) {
        dseen[dumped++] = e;
        logf("[enemy-vt] enemy %08X words: %08X %08X %08X %08X %08X %08X %08X %08X | task: %08X %08X %08X %08X\n", e, ld32(e), ld32(e + 4), ld32(e + 8),
             ld32(e + 0xC), ld32(e + 0x10), ld32(e + 0x14), ld32(e + 0x18), ld32(e + 0x1C), ld32(c.r[3]), ld32(c.r[3] + 4), ld32(c.r[3] + 8), ld32(c.r[3] + 0xC));
      }
    }
    if (mv >= 0x80000000u && mv < RT_RAM_END - 0x20 && n < 32) {
      const uint32_t vt0 = ld32(mv), vtC = ld32(mv + 0xC);
      bool known = false;
      for (int i = 0; i < n; ++i) known |= seen[i] == vt0;
      if (!known) {
        seen[n++] = vt0;
        logf("[enemy-vt] enemy %08X move %08X: *(move+0) = %08X -> [+0x0C] %08X [+0x10] %08X | *(move+0xC) = %08X -> [+0x0C] %08X\n", e, mv, vt0,
             vt0 >= 0x80000000u && vt0 < RT_RAM_END ? ld32(vt0 + 0xC) : 0, vt0 >= 0x80000000u && vt0 < RT_RAM_END ? ld32(vt0 + 0x10) : 0, vtC,
             vtC >= 0x80000000u && vtC < RT_RAM_END ? ld32(vtC + 0xC) : 0);
      }
    }
  }
  static const bool move_research = getenv("ENEMY_MOVE") != nullptr;
  if (ld32(c.r[3] + 8) == 4 && move_research) enemy_move_research(c.r[3]);
  static const bool research = getenv("ENEMY_REC") != nullptr;
  if (ld32(c.r[3] + 8) == 4 && research) enemy_rec_research(c.r[3]);
  if (far_enemy_skip(c)) return;
  static const bool perf = getenv("PERF_OBJ") != nullptr;
  if (!perf) { f_801A6EC4(c); return; }
  static uint64_t win0 = 0, spent = 0, calls = 0;
  const uint64_t t0 = sys_time_ns();
  f_801A6EC4(c);
  const uint64_t t1 = sys_time_ns();
  spent += t1 - t0;
  ++calls;
  if (!win0) win0 = t0;
  if (t1 - win0 >= 1000000000ull) {
    logf("[perf-obj] enemy task updates: %llu per s, %.1f ms per s inside them (far: %llu skipped, %llu run)\n", (unsigned long long)calls,
         spent / 1e6, (unsigned long long)g_far_skipped, (unsigned long long)g_far_run);
    win0 = t1; spent = 0; calls = 0; g_far_skipped = g_far_run = 0;
  }
}
