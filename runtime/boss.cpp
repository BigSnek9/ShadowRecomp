// Boss runtime (overnight D, docs/bosses/BOSS-SUPPORT.md + BOSS-PHASE1-RESULTS.md). Phase 1 probe: BOSS_LOG=1 logs Black
// Bull 0210's mode units (START update fn_80236248, WAIT update fn_80234D78): call counts, the mode value (boss +0x14),
// and the WAIT inputs (+0x174 vs the range +0xE8) - why a boss placed in another stage stays idle.
#include "sys.h"
#include <cmath>
#include <cstdlib>
#include <unordered_map>

void f_80236248(Cpu&);
void f_80234D78(Cpu&);
void f_8023514C(Cpu&);
void f_802352EC(Cpu&);
void f_8023520C(Cpu&);
void f_802353C8(Cpu&);
void f_802354F4(Cpu&);
void f_802357B0(Cpu&);
void f_8023599C(Cpu&);
void f_80235C98(Cpu&);
void f_80235E50(Cpu&);
void f_802360A8(Cpu&);
void f_80236394(Cpu&);
void f_802350B4(Cpu&);

namespace {
bool log_on() { static const bool on = getenv("BOSS_LOG") != nullptr; return on; }
void report(const char* unit, Cpu& c, uint64_t& calls) {
  ++calls;
  static uint64_t last = 0;
  const uint64_t now = sys_time_ns();
  if (!log_on() || now - last < 1000000000ull) return;
  last = now;
  const uint32_t boss = ld32(c.r[3] + 0xC);
  if (boss < 0x80000000u || boss >= RT_RAM_END) return;
  static const bool dump = getenv("BOSS_DUMP") != nullptr;   // probe: float triplets near the placed position (x ~ 0, z ~ 500)
  static int dumped = 0;
  if (dump && dumped++ < 3) {
    for (uint32_t o = 0; o + 12 <= 0x400; o += 4) {
      const float x = (float)ldf32(boss + o), y = (float)ldf32(boss + o + 4), z = (float)ldf32(boss + o + 8);
      if (std::fabs(x) < 400 && y > -50 && y < 400 && std::fabs(z - 500) < 400) logf("[boss] %s +%03X (%.1f, %.1f, %.1f)\n", unit, o, x, y, z);
    }
    const uint32_t p1 = ld32(boss + 0xC);
    logf("[boss] boss %08X +0xC %08X +0x3C %08X\n", boss, p1, ld32(boss + 0x3C));
  }
  logf("[boss] %s calls %llu mode %d dist? %.1f range? %.1f +0x178 %.1f +0x172 %u\n", unit, (unsigned long long)calls, (int)ld32(boss + 0x14),
       (float)ldf32(boss + 0x174), (float)ldf32(boss + 0xE8), (float)ldf32(boss + 0x178), ld8(boss + 0x172));
}
uint64_t g_start = 0, g_wait = 0;
uint64_t g_units[12] = {};
}  // namespace

// START waits for its start motion (motion 3, fn_80416F28) and then sets mode 2 + flag +0x172; in another stage that
// motion never reports done (its start camera / event is the boss stage's), so after 3 s the runtime does the same.
namespace bb0210 { void anchor(uint32_t vp); void chase_guard(uint32_t vp); }   // boss_vanilla.cpp (#130)
void hle_BB0210Start(Cpu& c) {
  report("START", c, g_start);
  const uint32_t boss = ld32(c.r[3] + 0xC);
  if (boss >= 0x80000000u && boss < RT_RAM_END) bb0210::anchor(boss);
  f_80236248(c);
  static std::unordered_map<uint32_t, int> frames;
  if (boss < 0x80000000u || boss >= RT_RAM_END || ld32(0x80584720) == 210) return;   // its own stage: untouched
  if ((int)ld32(boss + 0x14) != 0) { frames.erase(boss); return; }
  static const bool force = getenv("BOSS_FORCE_START") != nullptr;   // probe only: off by default
  if (force && ++frames[boss] == 180) {
    st32(boss + 0x14, 2);
    st8(boss + 0x172, 1);
    logf("[boss] Black Bull 0210 %08X: START did not finish in 3 s (no boss-stage start event) -> mode 2\n", boss);
  }
}
void hle_BB0210Wait(Cpu& c) {
  report("WAIT", c, g_wait);
  const uint32_t boss = ld32(c.r[3] + 0xC);
  if (boss >= 0x80000000u && boss < RT_RAM_END) bb0210::anchor(boss);
  f_80234D78(c);
}
// the other mode units (probe: which one runs after START outside stage 210)
// Phase 2 leash (outside its own stage 210): its CHASE only covers a short distance (it hovered 1200 units away for good),
// so while chasing and farther than 500 from the player it is moved toward the player (xz, ~6 units a frame) and its
// height eases to the player's + 35, until its own chase / attacks take over. Boss +0xFC / +0x108 = its position,
// +0x138 = the player's (dump, BOSS_DUMP=1).
// #130: OFF unless BOSS_LEASH=1 - +0xFC / +0x108 are its CHASE point / home, and the drag (360 u/s, no fly motion) was
// the 'too fast' boss. The cause (CHASE points round stage 210's origin) is fixed in boss_vanilla.cpp.
void hle_BB0210_CHASE(Cpu& c) {
  report("CHASE", c, g_units[0]);
  const uint32_t boss = ld32(c.r[3] + 0xC);
  if (boss >= 0x80000000u && boss < RT_RAM_END) bb0210::anchor(boss);
  f_8023514C(c);
  if (boss >= 0x80000000u && boss < RT_RAM_END) bb0210::chase_guard(boss);
  static const bool leash = getenv("BOSS_LEASH") != nullptr && getenv("BOSS_NO_LEASH") == nullptr;
  if (boss < 0x80000000u || boss >= RT_RAM_END || ld32(0x80584720) == 210 || !leash) return;
  const float bx = (float)ldf32(boss + 0xFC), by = (float)ldf32(boss + 0x100), bz = (float)ldf32(boss + 0x104);
  const float px = (float)ldf32(boss + 0x138), py = (float)ldf32(boss + 0x13C), pz = (float)ldf32(boss + 0x140);
  const float dx = px - bx, dz = pz - bz, d = std::sqrt(dx * dx + dz * dz);
  if (!(d > 500 && d < 20000)) return;
  const float step = std::fmin(6.f, d - 400);
  const float nx = bx + dx / d * step, nz = bz + dz / d * step, ny = by + (py + 35 - by) * 0.02f;
  for (uint32_t o : {0xFCu, 0x108u}) { stf32(boss + o, nx); stf32(boss + o + 4, ny); stf32(boss + o + 8, nz); }
  static uint64_t last = 0;
  if (log_on() && sys_time_ns() - last > 1000000000ull) { last = sys_time_ns(); logf("[boss] leash: %.0f from the player -> (%.0f, %.0f, %.0f)\n", d, nx, ny, nz); }
}
void hle_BB0210_DIZZY(Cpu& c) { report("DIZZY", c, g_units[1]); f_802352EC(c); }
void hle_BB0210_ESCAPE(Cpu& c) { report("ESCAPE", c, g_units[2]); f_8023520C(c); }
void hle_BB0210_GET_COMBO(Cpu& c) { report("GET_COMBO", c, g_units[3]); f_802353C8(c); }
void hle_BB0210_GET_ATK(Cpu& c) { report("GET_ATK", c, g_units[4]); f_802354F4(c); }
void hle_BB0210_BELOW_HALF(Cpu& c) { report("BELOW_HALF", c, g_units[5]); f_802357B0(c); }
void hle_BB0210_DEATH(Cpu& c) { report("DEATH", c, g_units[6]); f_8023599C(c); }
void hle_BB0210_ATK_BREATH(Cpu& c) { report("ATK_BREATH", c, g_units[7]); f_80235C98(c); }
void hle_BB0210_ATK_WAVERING(Cpu& c) { report("ATK_WAVERING", c, g_units[8]); f_80235E50(c); }
void hle_BB0210_ATK_CLOSERANGE(Cpu& c) { report("ATK_CLOSERANGE", c, g_units[9]); f_802360A8(c); }
void hle_BB0210_MOVETEST(Cpu& c) { report("MOVETEST", c, g_units[10]); f_80236394(c); }
void hle_BB0210_EDIT(Cpu& c) { report("EDIT", c, g_units[11]); f_802350B4(c); }

// HP bar (BOSS-SUPPORT.md 1.4): the HUD builds the boss gauge TGiHPGaugeBoss only when flag 0x8057D6A0 + 0xB1 is set
// (fn_800C0488); a boss placed in a normal slot leaves it clear. After each SET file load (stage_workshop
// hle_SetLoadFile): a boss record (list 0, type 0xB4..0xBE) in the table -> set the flag (BOSS_NO_HPBAR=1: leave it).
namespace boss {
void on_set_loaded() {
  if (getenv("BOSS_NO_HPBAR")) return;
  const uint32_t m = ld32(0x805EE6D4);
  if (m < 0x80000000u || m >= RT_RAM_END) return;
  const uint32_t n = ld32(m), tab = ld32(m + 0x18);
  if (n > 8192 || tab < 0x80000000u || tab + n * 0x20 > RT_RAM_END) return;
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t rec = ld32(tab + i * 0x20);
    if (rec < 0x80000000u || rec >= RT_RAM_END - 0x30) continue;
    if (ld8(rec + 0x20) == 0 && ld8(rec + 0x21) >= 0xB4 && ld8(rec + 0x21) <= 0xBE) {
      const uint8_t was = ld8(0x8057D6A0 + 0xB1);
      st8(0x8057D6A0 + 0xB1, 1);
      logf("[boss] boss record 00/%02X in stage %u -> HP-bar flag 0x8057D751 %u -> 1\n", ld8(rec + 0x21), ld32(0x80584720), was);
      return;
    }
  }
}
}  // namespace boss
