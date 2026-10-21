// Core helpers: FP, paired singles, quantisation, dispatch.
#include "rt_internal.h"
#include "sys.h"
#include <cstdio>
#include <cstdlib>
#include <algorithm>

// ---------------------------------------------------------------- floating point
double fres_(double x) {
  if (x == 0.0) return std::copysign(INFINITY, x);
  return 1.0 / x;
}
double frsqrte_(double x) {
  if (x == 0.0) return INFINITY;
  if (x < 0.0) return NAN;
  return 1.0 / std::sqrt(x);
}
void fcmp_(Cpu& c, int crf, double a, double b) {
  uint8_t v;
  if (std::isnan(a) || std::isnan(b)) v = 1;
  else if (a < b) v = 8;
  else if (a > b) v = 4;
  else v = 2;
  c.cr[crf] = v;
  c.fpscr = (c.fpscr & ~0xF000u) | ((uint32_t)v << 12);
}
double fctiw_(double x, bool trunc) {
  int32_t r;
  if (std::isnan(x)) r = INT32_MIN;
  else if (x >= 2147483648.0) r = INT32_MAX;
  else if (x <= -2147483649.0) r = INT32_MIN;
  else r = (int32_t)(trunc ? std::trunc(x) : std::nearbyint(x));
  return from_bits64(0xFFF8000000000000ull | (uint32_t)r);
}
void mtfsf_(Cpu& c, uint32_t fm, uint32_t val) {
  uint32_t m = 0;
  for (int i = 0; i < 8; ++i)
    if (fm & (0x80u >> i)) m |= 0xFu << (28 - 4 * i);
  c.fpscr = (c.fpscr & ~m) | (val & m);
}

void tw_(Cpu& c, uint32_t to, uint32_t a, uint32_t b, uint32_t pc) {
  int32_t sa = (int32_t)a, sb = (int32_t)b;
  bool t = ((to & 16) && sa < sb) || ((to & 8) && sa > sb) || ((to & 4) && a == b) ||
           ((to & 2) && a < b) || ((to & 1) && a > b);
  if (t) rt_unimpl(c, pc, "trap instruction taken");
}

// ---------------------------------------------------------------- paired singles
static inline double sel0(double a) { return a; }
#define PS2(NAME, E0, E1)                                         \
  void NAME(Cpu& c, int d, int a, int b, int cc) {                \
    double a0 = c.f[a], a1 = c.ps1[a], b0 = c.f[b], b1 = c.ps1[b]; \
    double c0 = c.f[cc], c1 = c.ps1[cc];                          \
    (void)a0; (void)a1; (void)b0; (void)b1; (void)c0; (void)c1;  \
    double r0 = (E0), r1 = (E1);                                  \
    c.f[d] = rs(r0); c.ps1[d] = rs(r1);                           \
  }
PS2(ps_sum0, a0 + b1, c1)
PS2(ps_sum1, c0, a0 + b1)
PS2(ps_muls0, a0 * c0, a1 * c0)
PS2(ps_muls1, a0 * c1, a1 * c1)
PS2(ps_madds0, std::fma(a0, c0, b0), std::fma(a1, c0, b1))
PS2(ps_madds1, std::fma(a0, c1, b0), std::fma(a1, c1, b1))
PS2(ps_div, a0 / b0, a1 / b1)
PS2(ps_sub, a0 - b0, a1 - b1)
PS2(ps_add, a0 + b0, a1 + b1)
PS2(ps_sel, (a0 >= 0.0 ? c0 : b0), (a1 >= 0.0 ? c1 : b1))
PS2(ps_res, fres_(b0), fres_(b1))
PS2(ps_mul, a0 * c0, a1 * c1)
PS2(ps_rsqrte, frsqrte_(b0), frsqrte_(b1))
PS2(ps_msub, std::fma(a0, c0, -b0), std::fma(a1, c1, -b1))
PS2(ps_madd, std::fma(a0, c0, b0), std::fma(a1, c1, b1))
PS2(ps_nmsub, -std::fma(a0, c0, -b0), -std::fma(a1, c1, -b1))
PS2(ps_nmadd, -std::fma(a0, c0, b0), -std::fma(a1, c1, b1))

void ps_cmp(Cpu& c, int crf, int a, int b, int kind) {
  // kind: 0 = cmpu0, 1 = cmpo0, 2 = cmpu1, 3 = cmpo1
  double x = (kind >= 2) ? c.ps1[a] : c.f[a];
  double y = (kind >= 2) ? c.ps1[b] : c.f[b];
  fcmp_(c, crf, x, y);
}

static inline int scale6(uint32_t v) { return (int)((int32_t)(v << 26) >> 26); }
// #152: 2^e for e = -32..32 (exact powers of two: the same doubles std::ldexp(1, e) gives), built once - psq_l / psq_st
// called ldexp on every paired-single load / store, ~5 % of the game thread at the user's settings
static const struct Pow2 { double v[65]; Pow2() { for (int e = -32; e <= 32; ++e) v[e + 32] = std::ldexp(1.0, e); } } kPow2;

void psq_l(Cpu& c, int d, uint32_t ea, int w, int i) {
  uint32_t gqr = c.gqr[i];
  int type = (gqr >> 16) & 7;
  if (type < 4) {   // #152: plain floats (the common case) - the same loads without the per-element switch
    c.f[d] = ldf32(ea);
    c.ps1[d] = w ? 1.0 : (double)ldf32(ea + 4);
    return;
  }
  int sc = scale6(gqr >> 24);
  const double m = kPow2.v[32 - sc];   // 2^-sc (sc -32..31)
  auto rd = [&](uint32_t a, double& out) -> int {
    switch (type) {
      case 4: out = (double)ld8(a) * m; return 1;
      case 5: out = (double)ld16(a) * m; return 2;
      case 6: out = (double)(int8_t)ld8(a) * m; return 1;
      case 7: out = (double)(int16_t)ld16(a) * m; return 2;
      default: out = ldf32(a); return 4;
    }
  };
  double v0, v1 = 1.0;
  int sz = rd(ea, v0);
  if (!w) rd(ea + sz, v1);
  c.f[d] = v0;
  c.ps1[d] = w ? 1.0 : v1;
}

static inline uint32_t quant(double v, int type, double m) {
  double s = v * m;
  switch (type) {
    case 4: return (uint32_t)(s <= 0 ? 0 : s >= 255 ? 255 : (int)s);
    case 5: return (uint32_t)(s <= 0 ? 0 : s >= 65535 ? 65535 : (int)s);
    case 6: return (uint32_t)(int8_t)(s <= -128 ? -128 : s >= 127 ? 127 : (int)s);
    case 7: return (uint32_t)(int16_t)(s <= -32768 ? -32768 : s >= 32767 ? 32767 : (int)s);
  }
  return 0;
}

void psq_st(Cpu& c, int s, uint32_t ea, int w, int i) {
  uint32_t gqr = c.gqr[i];
  int type = gqr & 7;
  if (type < 4) {   // #152: plain floats - the same stores without the per-element switch
    stf32(ea, c.f[s]);
    if (!w) stf32(ea + 4, c.ps1[s]);
    return;
  }
  int sc = scale6(gqr >> 8);
  const double m = kPow2.v[32 + sc];   // 2^sc
  auto wr = [&](uint32_t a, double v) -> int {
    switch (type) {
      case 4: case 6: st8(a, quant(v, type, m)); return 1;
      case 5: case 7: st16(a, quant(v, type, m)); return 2;
      default: stf32(a, v); return 4;
    }
  };
  int sz = wr(ea, c.f[s]);
  if (!w) wr(ea + sz, c.ps1[s]);
}

// ---------------------------------------------------------------- dispatch
static const GuestFunc** g_fast = nullptr;   // not used for now

static const GuestFunc* find_func(uint32_t addr) {
  unsigned lo = 0, hi = g_functab_count;
  while (lo < hi) {
    unsigned mid = (lo + hi) >> 1;
    if (g_functab[mid].addr < addr) lo = mid + 1; else hi = mid;
  }
  if (lo < g_functab_count && g_functab[lo].addr == addr) return &g_functab[lo];
  return nullptr;
}

const GuestFunc* rt_containing_func(uint32_t addr) {
  unsigned lo = 0, hi = g_functab_count;
  while (lo < hi) {
    unsigned mid = (lo + hi) >> 1;
    if (g_functab[mid].addr <= addr) lo = mid + 1; else hi = mid;
  }
  if (lo == 0) return nullptr;
  const GuestFunc* f = &g_functab[lo - 1];
  return (addr < f->addr + f->size) ? f : nullptr;
}

// open-addressed cache of addr -> fn
struct CacheEnt { uint32_t addr; void (*fn)(Cpu&); };
static CacheEnt g_cache[1 << 14];

void call_addr(Cpu& c, uint32_t addr) {
  if (addr < RT_RAM_SIZE && addr >= 0x100u) addr |= 0x80000000u;   // real-mode (physical) alias
  else if ((addr & 0xC0000000u) == 0xC0000000u && addr < 0xC0000000u + RT_RAM_SIZE) addr &= ~0x40000000u;   // uncached alias
  CacheEnt& e = g_cache[(addr >> 2) & ((1 << 14) - 1)];
  if (RT_LIKELY(e.addr == addr && e.fn)) { e.fn(c); return; }
  if (auto h = sys_find_hook(addr)) { e.addr = addr; e.fn = h; h(c); return; }
  const GuestFunc* f = find_func(addr);
  if (!f) {
    const GuestFunc* in = rt_containing_func(addr);
    if (in) {   // inside a multi-entry function?
      for (const MultiFunc* m = g_multi; m->fn; ++m)
        if (m->addr == in->addr) { m->fn(c, addr); return; }
    }
    // log for the generator: functions that need multi-entry support
    static uint32_t last_logged = 0;
    if (in && addr != last_logged) {
      last_logged = addr;
      FILE* mf = fopen("dispatch_miss.txt", "a");
      if (mf) { fprintf(mf, "%08X  # %08X inside %s\n", in->addr, addr, in->name); fclose(mf); }
    }
    char buf[256];
    snprintf(buf, sizeof buf, "indirect call to non-function 0x%08X (inside %s+0x%X)", addr,
             in ? in->name : "?", in ? addr - in->addr : 0);
    rt_unimpl(c, c.lr, buf);
  }
  e.addr = addr; e.fn = f->fn;
  f->fn(c);
}

void rt_fallthrough(Cpu& c, uint32_t addr) {
  // Function ended without a terminating branch: continue into the next one.
  call_addr(c, addr);
}

void rt_unimpl(Cpu& c, uint32_t pc, const char* what) {
  const GuestFunc* in = rt_containing_func(pc);
  fprintf(stderr, "\n[rt] FATAL unimplemented @%08X (%s+0x%X): %s\n", pc, in ? in->name : "?",
          in ? pc - in->addr : 0, what);
  fprintf(stderr, "     lr=%08X ctr=%08X r1=%08X r3=%08X r4=%08X r5=%08X r30=%08X r31=%08X\n", c.lr, c.ctr, c.r[1],
          c.r[3], c.r[4], c.r[5], c.r[30], c.r[31]);
  fprintf(stderr, "     r12=%08X r0=%08X r6=%08X r7=%08X r29=%08X [r31+0x1338]=%08X\n", c.r[12], c.r[0], c.r[6], c.r[7], c.r[29],
          ld32(c.r[31] + 0x1338));
  sys_backtrace(c);
  fflush(stderr);
  sys_fatal();
}

// ---------------------------------------------------------------- function coverage (ppc_rt.h RT_COV)
uint32_t g_cov[kCovWords];
void cov_reset() { std::fill(g_cov, g_cov + kCovWords, 0u); }
// "address count name" for every function entered since the last reset (multi-entry functions by entry point).
int cov_dump(const char* path) {
  FILE* f = fopen(path, "w");
  if (!f) return -1;
  int n = 0;
  for (uint32_t i = 0; i < kCovWords; ++i) {
    if (!g_cov[i]) continue;
    uint32_t addr = 0x80000000u + i * 4;
    const GuestFunc* g = rt_containing_func(addr);
    fprintf(f, "%08X %u %s\n", addr, g_cov[i], g ? g->name : "?");
    ++n;
  }
  fclose(f);
  return n;
}
