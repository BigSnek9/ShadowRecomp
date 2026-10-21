// #154 engine limits (2026-10-21): measurement probes for the hard caps (docs/perf/ENGINE-LIMITS.md).
// BUF_LOG=1: every 5 s, the peak per-frame use of Aurora's fixed per-frame GPU buffers (vertices, indices, uniforms,
// storage, texture uploads) against their capacity - a frame that needs more than a buffer holds aborts the game
// (third_party/aurora/lib/internal.hpp ByteBuffer, '[aurora] ByteBuffer overflow' in the log), so this sizes them.
#include "sys.h"
#include <aurora/gfx.h>
#include <algorithm>
#include <cstdlib>

namespace engine_limits {
void live_log();
void frame() {
  live_log();
  static const bool on = getenv("BUF_LOG") != nullptr;
  if (!on) return;
  const AuroraStats* s = aurora_get_stats();
  if (!s) return;
  static uint32_t pv = 0, pi = 0, pu = 0, ps = 0, pt = 0, pd = 0;
  pv = std::max(pv, s->lastVertSize); pi = std::max(pi, s->lastIndexSize); pu = std::max(pu, s->lastUniformSize);
  ps = std::max(ps, s->lastStorageSize); pt = std::max(pt, s->lastTextureUploadSize); pd = std::max(pd, s->drawCallCount);
  static uint64_t next = 0;
  const uint64_t now = sys_time_ns();
  if (now < next) return;
  next = now + 5000000000ull;
  logf("[buffers] peak per frame: verts %u KiB, indices %u KiB, uniforms %u KiB, storage %u KiB, texture uploads %u KiB, draws %u\n",
       pv >> 10, pi >> 10, pu >> 10, ps >> 10, pt >> 10, pd);
  pv = pi = pu = ps = pt = pd = 0;
}
}  // namespace engine_limits

namespace engine_limits {
// The game's operator new (fn_803A0254) found no memory and the game exits (hle_guest_exit): name the request.
// Its frame keeps the caller's r31 at +0x1C; the throw (fn_803A1610) below it saved r13..r31 at +0x24C, where r31 is
// the requested size and r28 / r29 / r30 are what the caller held (fn_8011357C: r28 = element count).
void exit_report(Cpu& c) {
  uint32_t sp = c.r[1], prev = 0;
  for (int i = 0; i < 32 && sp >= 0x80000000u && sp < RT_RAM_END; ++i) {
    if (ld32(sp + 4) == 0x803A02CCu && prev) {
      const uint32_t regs = prev + 0x24C, caller = ld32(sp + 0x1C);
      logf("[limits] operator new failed: size %u (0x%08X), caller r31 %08X r28 %u r29 %08X r30 %08X\n", ld32(regs + 18 * 4),
           ld32(regs + 18 * 4), caller, ld32(regs + 15 * 4), ld32(regs + 16 * 4), ld32(regs + 17 * 4));
      return;
    }
    const uint32_t back = ld32(sp);
    if (back <= sp) break;
    prev = sp; sp = back;
  }
}
}  // namespace engine_limits

namespace engine_limits {
// LIVE_LOG=1 (#166 tests): once a second, how many SET objects have a live instance (SetManager entry +0xC) of all loaded
void live_log() {
  static const bool on = getenv("LIVE_LOG") != nullptr;
  if (!on) return;
  static uint64_t next = 0;
  const uint64_t now = sys_time_ns();
  if (now < next) return;
  next = now + 1000000000ull;
  const uint32_t m = ld32(0x805EE6D4);
  if (m < 0x80000000u || m >= RT_RAM_END) return;
  const uint32_t n = ld32(m), tab = ld32(m + 0x18);
  if (n > 8192 || tab < 0x80000000u || tab + n * 0x20 > RT_RAM_END) return;
  uint32_t live = 0;
  for (uint32_t i = 0; i < n; ++i) live += ld32(tab + i * 0x20 + 0xC) != 0;
  logf("[live] %u of %u objects live\n", live, n);
}
}  // namespace engine_limits
