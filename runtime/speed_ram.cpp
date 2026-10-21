// #111 Speed ram (track B, 2026-10-19). Running on the ground (Ground behaviour) at 90%+ of top speed, an enemy Shadow
// bumps into takes the hit instead of him: its contact damage is cancelled, the enemy gets a DamageCommand (the same
// kind 4 / amount 10 the game's own attacks send), and Shadow slows to 70%. Shots / attacks that do not come from the
// enemy's body (hit point more than 60 units from it) still hurt as usual. settings key speed_ram (1 = on, default).
//  * Shadow's command handler fn_800AF53C (r3 = behaviour context, +0x28 = the player; r4 = the command: +4 u16 id,
//    +7 flags (bit 0x2 = handled), +8 kind; DamageCommand id 7 (vtable 0x8053D44C): +0x14 amount, +0x18 hit point).
//  * an enemy's command receiver = its EnemyCharInfo object (its +0x38 vtable 0x8053A8B4 has the receiver fn_8019FF24
//    at +0x10, which reads the command id and queues it - like Shadow's PlayerShadow fn_800799D0), found through a
//    pointer just below the SET instance (GUN Soldier -0x3B0, Bk Soldier -0x3D0; quiet-run probe); delivered like the
//    game does: receiver(char info, command).
//  * per frame from custom_stage_rules.cpp hle_CsPlayerUpdate (speed_ram::frame): the ram state (+0.4 s grace).
// RAM_LOG=1 logs. Proof: tests/RESULTS.md (#111). docs/stage-workshop/research/speed-ram-111.md.
#include "sys.h"
#include "settings_ui.h"
#include <cmath>
#include <cstdlib>
#include <cstring>

void f_800AF53C(Cpu&);
void f_801D9C84(Cpu&);   // command allocation (r3 size)
void f_801DB804(Cpu&);   // DamageCommand ctor (r3 cmd, r4 &hit point, r5 kind, f1 amount)
namespace movement { uint32_t player(); }

namespace {
bool ram(uint32_t a) { return a >= 0x80000000u && a < RT_RAM_END; }
constexpr uint32_t kCharTable = 0x804F1834;      // + id * 0x2C, +4 = top-speed multiplier (top speed = 750 u/s x it)
constexpr uint32_t kEnemyHandler = 0x8019FF24;   // EnemyCharInfo (vtable 0x8053A8B4) +0x10: an enemy's command receiver
float speed_of(uint32_t p) { return std::hypot((float)ldf32(p + 0x18), (float)ldf32(p + 0x20)); }
float top_of(uint32_t p) { const uint32_t id = ld32(p + 0x64); return id < 32 ? 750.0f * (float)ldf32(kCharTable + id * 0x2C + 4) : 750.0f; }
uint32_t behaviour_id(uint32_t p) {   // executer p+0x22C -> current behaviour +0x30 -> id +0x10 (movement.cpp behaviour())
  const uint32_t ex = ld32(p + 0x22C), b = ram(ex) ? ld32(ex + 0x30) : 0;
  return ram(b) ? ld32(b + 0x10) : 0xFFFFFFFFu;
}
void put_f(uint32_t a, float v) { uint32_t u; memcpy(&u, &v, 4); st32(a, u); }
uint32_t g_ram_player = 0;   // Shadow while ramming (or in the grace after it)
int g_grace = 0;
// the live SET enemy whose body is at the hit point, and its command receiver
uint32_t enemy_at(float x, float z, uint32_t& character) {
  const uint32_t m = ld32(0x805EE6D4);
  if (!ram(m)) return 0;
  const uint32_t n = ld32(m), tab = ld32(m + 0x18);
  uint32_t best = 0; float bd = 60.f;
  for (uint32_t i = 0; i < n && i < 4096 && ram(tab); ++i) {
    const uint32_t e = tab + i * 0x20, rec = ld32(e), inst = ld32(e + 0xC);
    if (!ram(rec) || !ram(inst) || ld8(rec + 0x20) != 0 || ld8(rec + 0x21) < 0x64 || ld8(rec + 0x21) > 0xBE) continue;
    const float d = std::hypot((float)ldf32(inst + 0x40) - x, (float)ldf32(inst + 0x48) - z);
    if (d < bd) { bd = d; best = inst; }
  }
  character = 0;
  if (best)
    for (uint32_t off = 4; off <= 0x800 && !character; off += 4) {   // a pointer below the instance to its EnemyCharInfo
      const uint32_t o = ld32(best - off), vt = ram(o) ? ld32(o + 0x38) : 0;
      if (vt >= 0x804A0000u && vt < 0x80560000u && ld32(vt + 0x10) == kEnemyHandler) character = o;
    }
  return best;
}
}  // namespace

void hle_RamShadowMessage(Cpu& c) {
  static const bool log = getenv("RAM_LOG") != nullptr;
  const uint32_t ctx = c.r[3], cmd = c.r[4];
  if (!g_ram_player || !settings::g.speed_ram || !ram(cmd) || ld16(cmd + 4) != 7 || !ram(ctx) || ld32(ctx + 0x28) != g_ram_player) {
    f_800AF53C(c);
    return;
  }
  const float hx = (float)ldf32(cmd + 0x18), hz = (float)ldf32(cmd + 0x20);
  uint32_t character = 0;
  const uint32_t enemy = enemy_at(hx, hz, character);
  if (!enemy || !character) {   // not a body contact (a shot, a hazard): the hit counts as usual
    if (log) logf("[ram] hit at %.0f,%.0f not from an enemy body (enemy %08X, receiver %08X): normal damage\n", hx, hz, enemy, character);
    f_800AF53C(c);
    return;
  }
  st8(cmd + 7, ld8(cmd + 7) | 2);   // handled: Shadow takes nothing
  const uint32_t p = g_ram_player;
  // the enemy takes the hit: DamageCommand(kind 4, amount 10) from Shadow's position, delivered like the game does
  const Cpu keep = c;
  const uint32_t sp = (c.r[1] - 0x80) & ~0xFu;
  c.r[1] = sp;
  for (int i = 0; i < 3; ++i) st32(sp + 0x20 + i * 4, ld32(p + i * 4));   // Shadow's position (x, y, z)
  c.r[3] = 0x24; f_801D9C84(c);
  const uint32_t dmg = c.r[3];
  if (ram(dmg)) {
    c.r[3] = dmg; c.r[4] = sp + 0x20; c.r[5] = 4; c.f[1] = 10.0;
    f_801DB804(c);
    c.r[3] = character; c.r[4] = dmg;
    call_addr(c, ld32(ld32(character + 0x38) + 0x10));
  }
  c = keep;
  for (int i = 0; i < 3; i += 2) put_f(p + 0x18 + i * 4, (float)ldf32(p + 0x18 + i * 4) * 0.7f);   // the bump
  if (log) logf("[ram] bump: enemy %08X (receiver %08X) takes the hit, Shadow %.0f -> %.0f u/s\n", enemy, character, speed_of(p) / 0.7f, speed_of(p));
}

namespace speed_ram {
void frame(Cpu&) {
  const uint32_t p = movement::player();
  if (!ram(p) || ld32(p + 0x38) != 0x80521C50u) { g_ram_player = 0; return; }   // Shadow (PlayerShadow vtable at +0x38)
  const bool ramming = settings::g.speed_ram && (ld32(p + 0xA8) & 4) && behaviour_id(p) == 1 && speed_of(p) >= 0.9f * top_of(p);
  static const bool log = getenv("RAM_LOG") != nullptr;
  if (ramming) {
    if (!g_ram_player && log) logf("[ram] ram on: speed %.0f / top %.0f\n", speed_of(p), top_of(p));
    g_ram_player = p; g_grace = 24;
  } else if (g_ram_player && --g_grace <= 0) {
    g_ram_player = 0;
    if (log) logf("[ram] ram off\n");
  }
}
}  // namespace speed_ram
