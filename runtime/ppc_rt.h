// Runtime support for statically-recompiled Gekko code.
#pragma once
#include <cstdint>
#include <cstring>
#include <cmath>
#include <climits>

#if defined(__clang__) || defined(__GNUC__)
#define RT_LIKELY(x) __builtin_expect(!!(x), 1)
#define RT_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define RT_NOINLINE __attribute__((noinline))
#else
#define RT_LIKELY(x) (x)
#define RT_UNLIKELY(x) (x)
#define RT_NOINLINE
#endif

struct Cpu {
  uint32_t r[32];
  double f[32];    // ps0
  double ps1[32];  // ps1
  uint32_t lr, ctr;
  uint32_t xer_ca, xer_ov, xer_so, xer_bc;
  uint8_t cr[8];   // each nibble: LT=8 GT=4 EQ=2 SO=1
  uint32_t fpscr, msr, srr0, srr1;
  uint32_t gqr[8];
  uint32_t sr[16];
  uint32_t reserve;
  int32_t budget;
  uint32_t spr[1024];
  int id;          // host-side thread id (for debugging)
};

// ---- debug store watchpoint (set with --watch-ea) ----------------------------------
extern uint32_t g_watch_ea;
void rt_watch_hit(uint32_t ea, uint32_t v, int size);
void rt_watch_dma(const char* what, uint32_t phys, uint32_t len, uint64_t src);
#define RT_WATCH(ea, v, sz) do { if (RT_UNLIKELY((uint32_t)(g_watch_ea - ((ea) & RT_RAM_MASK)) < (uint32_t)(sz))) rt_watch_hit((ea), (uint32_t)(v), (sz)); } while (0)

// ---- guest memory -------------------------------------------------------
// Guest MEM1 window (#60, 2026-10-16: GameCube 24 MiB -> 64 MiB). The size the GAME is told (boot info, arena) is
// g_mem1_size (SHADOW_MEM1_MB, boot.cpp); the window is always RT_RAM_SIZE so every check below can use it.
#define RT_RAM_SIZE 0x04000000u
#define RT_RAM_MASK (RT_RAM_SIZE - 1u)
#define RT_RAM_END (0x80000000u + RT_RAM_SIZE)
extern uint32_t g_mem1_size;          // MEM1 size reported to the game (bytes, <= RT_RAM_SIZE)
extern uint8_t* g_ram;                 // RT_RAM_SIZE mirror of MEM1, stored in guest (big-endian) byte order
extern uint8_t g_lc[16384];            // locked cache (0xE0000000)

uint32_t mmio_read(uint32_t ea, int size);
void mmio_write(uint32_t ea, uint32_t val, int size);
uint64_t mem_read64_slow(uint32_t ea);
void mem_write64_slow(uint32_t ea, uint64_t v);

static inline uint32_t bswap32(uint32_t v) { return __builtin_bswap32(v); }
static inline uint16_t bswap16(uint16_t v) { return __builtin_bswap16(v); }
static inline uint64_t bswap64(uint64_t v) { return __builtin_bswap64(v); }

// Fast path: cached/uncached MEM1 (0x80/0xC0 + < RT_RAM_SIZE).
#define RT_IN_RAM(ea) (((ea) & (0xBFFFFFFFu & ~RT_RAM_MASK)) == 0x80000000u)
#define RT_OFF(ea) ((ea) & RT_RAM_MASK)

static inline uint8_t ld8(uint32_t ea) {
  if (RT_LIKELY(RT_IN_RAM(ea))) return g_ram[RT_OFF(ea)];
  return (uint8_t)mmio_read(ea, 1);
}
static inline uint32_t ld16(uint32_t ea) {
  if (RT_LIKELY(RT_IN_RAM(ea))) { uint16_t v; memcpy(&v, g_ram + RT_OFF(ea), 2); return bswap16(v); }
  return (uint16_t)mmio_read(ea, 2);
}
static inline uint32_t ld16s(uint32_t ea) { return (uint32_t)(int32_t)(int16_t)ld16(ea); }
static inline uint32_t ld16r(uint32_t ea) { return bswap16((uint16_t)ld16(ea)); }
static inline uint32_t ld32(uint32_t ea) {
  if (RT_LIKELY(RT_IN_RAM(ea))) { uint32_t v; memcpy(&v, g_ram + RT_OFF(ea), 4); return bswap32(v); }
  return mmio_read(ea, 4);
}
static inline uint32_t ld32r(uint32_t ea) { return bswap32(ld32(ea)); }
static inline uint64_t ld64(uint32_t ea) {
  if (RT_LIKELY(RT_IN_RAM(ea))) { uint64_t v; memcpy(&v, g_ram + RT_OFF(ea), 8); return bswap64(v); }
  return mem_read64_slow(ea);
}
static inline void st8(uint32_t ea, uint32_t v) {
  RT_WATCH(ea, v, 1);
  if (RT_LIKELY(RT_IN_RAM(ea))) { g_ram[RT_OFF(ea)] = (uint8_t)v; return; }
  mmio_write(ea, v & 0xFF, 1);
}
static inline void st16(uint32_t ea, uint32_t v) {
  RT_WATCH(ea, v, 2);
  if (RT_LIKELY(RT_IN_RAM(ea))) { uint16_t x = bswap16((uint16_t)v); memcpy(g_ram + RT_OFF(ea), &x, 2); return; }
  mmio_write(ea, v & 0xFFFF, 2);
}
static inline void st16r(uint32_t ea, uint32_t v) { st16(ea, bswap16((uint16_t)v)); }
static inline void st32(uint32_t ea, uint32_t v) {
  RT_WATCH(ea, v, 4);
  if (RT_LIKELY(RT_IN_RAM(ea))) { uint32_t x = bswap32(v); memcpy(g_ram + RT_OFF(ea), &x, 4); return; }
  mmio_write(ea, v, 4);
}
static inline void st32r(uint32_t ea, uint32_t v) { st32(ea, bswap32(v)); }
static inline void st64(uint32_t ea, uint64_t v) {
  RT_WATCH(ea, v, 8);
  if (RT_LIKELY(RT_IN_RAM(ea))) { uint64_t x = bswap64(v); memcpy(g_ram + RT_OFF(ea), &x, 8); return; }
  mem_write64_slow(ea, v);
}

static inline uint64_t bits64(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static inline double from_bits64(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }
static inline double ldf32(uint32_t ea) { uint32_t u = ld32(ea); float f; memcpy(&f, &u, 4); return (double)f; }
static inline double ldf64(uint32_t ea) { return from_bits64(ld64(ea)); }
static inline void stf32(uint32_t ea, double d) { float f = (float)d; uint32_t u; memcpy(&u, &f, 4); st32(ea, u); }
static inline void stf64(uint32_t ea, double d) { st64(ea, bits64(d)); }
void dcbz(uint32_t ea);
void rt_dcbi(uint32_t ea);

// ---- integer helpers -------------------------------------------------------
static inline uint32_t rotl32(uint32_t v, uint32_t n) { n &= 31; return n ? (v << n) | (v >> (32 - n)) : v; }

static inline void cr0(Cpu& c, uint32_t v) {
  int32_t s = (int32_t)v;
  c.cr[0] = (uint8_t)((s < 0 ? 8 : s > 0 ? 4 : 2) | (c.xer_so & 1));
}
static inline void cr1(Cpu& c) { c.cr[1] = (uint8_t)((c.fpscr >> 28) & 0xF); }
static inline void cmp_s(Cpu& c, int crf, int32_t a, int32_t b) {
  c.cr[crf] = (uint8_t)((a < b ? 8 : a > b ? 4 : 2) | (c.xer_so & 1));
}
static inline void cmp_u(Cpu& c, int crf, uint32_t a, uint32_t b) {
  c.cr[crf] = (uint8_t)((a < b ? 8 : a > b ? 4 : 2) | (c.xer_so & 1));
}
static inline uint32_t CRB(const Cpu& c, int bi) { return (c.cr[bi >> 2] >> (3 - (bi & 3))) & 1; }
static inline void CRS(Cpu& c, int bi, uint32_t v) {
  uint8_t m = (uint8_t)(1u << (3 - (bi & 3)));
  if (v) c.cr[bi >> 2] |= m; else c.cr[bi >> 2] &= (uint8_t)~m;
}
static inline uint32_t get_cr(const Cpu& c) {
  uint32_t v = 0;
  for (int i = 0; i < 8; ++i) v = (v << 4) | (c.cr[i] & 0xF);
  return v;
}
static inline void set_crf(Cpu& c, uint32_t v, uint32_t fxm) {
  for (int i = 0; i < 8; ++i)
    if (fxm & (0x80u >> i)) c.cr[i] = (uint8_t)((v >> (28 - 4 * i)) & 0xF);
}
static inline uint32_t get_xer(const Cpu& c) {
  return (c.xer_so << 31) | (c.xer_ov << 30) | (c.xer_ca << 29) | (c.xer_bc & 0x7F);
}
static inline void set_xer(Cpu& c, uint32_t v) {
  c.xer_so = v >> 31; c.xer_ov = (v >> 30) & 1; c.xer_ca = (v >> 29) & 1; c.xer_bc = v & 0x7F;
}
static inline void set_ov(Cpu& c, uint32_t v) { c.xer_ov = v & 1; if (v & 1) c.xer_so = 1; }
static inline uint32_t addc_(Cpu& c, uint32_t a, uint32_t b) {
  uint64_t t = (uint64_t)a + b; c.xer_ca = (uint32_t)(t >> 32); return (uint32_t)t;
}
static inline uint32_t adde_(Cpu& c, uint32_t a, uint32_t b) {
  uint64_t t = (uint64_t)a + b + c.xer_ca; c.xer_ca = (uint32_t)(t >> 32); return (uint32_t)t;
}
static inline uint32_t subfc_(Cpu& c, uint32_t a, uint32_t b) {
  uint64_t t = (uint64_t)(uint32_t)~a + b + 1; c.xer_ca = (uint32_t)(t >> 32); return (uint32_t)t;
}
static inline uint32_t subfe_(Cpu& c, uint32_t a, uint32_t b) {
  uint64_t t = (uint64_t)(uint32_t)~a + b + c.xer_ca; c.xer_ca = (uint32_t)(t >> 32); return (uint32_t)t;
}
static inline uint32_t sraw_(Cpu& c, uint32_t v, uint32_t n) {
  int32_t s = (int32_t)v;
  if (n >= 32) { c.xer_ca = (s < 0); return (uint32_t)(s >> 31); }
  if (n == 0) { c.xer_ca = 0; return v; }
  c.xer_ca = (s < 0) && ((v & ((1u << n) - 1)) != 0);
  return (uint32_t)(s >> n);
}
void tw_(Cpu& c, uint32_t to, uint32_t a, uint32_t b, uint32_t pc);

// ---- floating point ------------------------------------------------------
static inline double rs(double d) { return (double)(float)d; }
// dtscale.txt instructions: per-frame increments scaled to this frame's length in 1/60 s (frame_rate.cpp; 1 at 60 fps)
extern double g_dt60;
static inline double rt_dt60() { return g_dt60; }
double fres_(double x);
double frsqrte_(double x);
void fcmp_(Cpu& c, int crf, double a, double b);
double fctiw_(double x, bool trunc);
void mtfsf_(Cpu& c, uint32_t fm, uint32_t val);

// paired singles
void psq_l(Cpu& c, int d, uint32_t ea, int w, int i);
void psq_st(Cpu& c, int s, uint32_t ea, int w, int i);
void ps_cmp(Cpu& c, int crf, int a, int b, int kind);
#define PS_DECL(n) void n(Cpu& c, int d, int a, int b, int cc);
PS_DECL(ps_sum0) PS_DECL(ps_sum1) PS_DECL(ps_muls0) PS_DECL(ps_muls1) PS_DECL(ps_madds0)
PS_DECL(ps_madds1) PS_DECL(ps_div) PS_DECL(ps_sub) PS_DECL(ps_add) PS_DECL(ps_sel)
PS_DECL(ps_res) PS_DECL(ps_mul) PS_DECL(ps_rsqrte) PS_DECL(ps_msub) PS_DECL(ps_madd)
PS_DECL(ps_nmsub) PS_DECL(ps_nmadd)

// ---- control flow / system -----------------------------------------------
void call_addr(Cpu& c, uint32_t addr);    // indirect call via function table
void rt_poll(Cpu& c, uint32_t pc);        // periodic safe point (interrupts, thread yield)
void rt_fallthrough(Cpu& c, uint32_t addr);
[[noreturn]] void rt_unimpl(Cpu& c, uint32_t pc, const char* what);
void rt_trace_enter(Cpu& c, uint32_t fn);   // debug: entry of a function listed in tracefns.txt
// Function coverage: every generated function counts its entries here, indexed by (address - 0x80000000) / 4
// (one increment per call). INPUT_FILE "COV reset" / "COV dump <file>" + tools/cov_diff.py find the functions that
// run only during some action (opening the pause menu, a menu choice...). See NOTES-GAME.md "Finding the code".
constexpr uint32_t kCovWords = 0x00500000u / 4;   // text ends below 0x80500000
extern uint32_t g_cov[kCovWords];
#define RT_COV(addr) (++g_cov[((addr) - 0x80000000u) >> 2])
void rt_rfi(Cpu& c, uint32_t pc);
void rt_sc(Cpu& c, uint32_t pc);
uint32_t rt_mftb(uint32_t tbr);
uint32_t rt_get_dec(Cpu& c);
void rt_set_dec(Cpu& c, uint32_t v);
uint32_t rt_mfspr(Cpu& c, uint32_t spr);
void rt_mtspr(Cpu& c, uint32_t spr, uint32_t v);
