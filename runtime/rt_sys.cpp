// Safe points, exceptions, SPRs and misc system glue.
#include "sys.h"
#include "hw.h"
#include "sched.h"
#include "gx_lle.h"
#include <cstdarg>
#include <cstdlib>
#include <unordered_map>
#include <windows.h>


// Debug call tracing: TRACE_FN=8016A108,800454F8 logs each call (r3-r6, backtrace) of up to 4 guest functions.
static uint32_t g_tf_addr[4];
static int g_tf_n = -1;
template <int I> static void tf_hook(Cpu& c) {
  logf("[trace-fn] %08X r3=%08X r4=%08X r5=%08X r6=%08X lr=%08X\n", g_tf_addr[I], c.r[3], c.r[4], c.r[5], c.r[6], c.lr);
  sys_backtrace(c);
  const GuestFunc* f = rt_containing_func(g_tf_addr[I]);
  f->fn(c);
}
// tracefns.txt entry tracing: the first 20 calls of each function are logged with args + backtrace,
// after that every 1000th call (with the running count).
void rt_trace_enter(Cpu& c, uint32_t fn) {
  static std::unordered_map<uint32_t, uint64_t> counts;
  // TRACE_R3=<hex>: only (and always) log calls whose first argument is that value
  static const char* r3env = getenv("TRACE_R3");
  static const uint32_t r3want = r3env ? (uint32_t)strtoul(r3env, nullptr, 16) : 0;
  uint64_t n = ++counts[fn];
  if (r3env) { if (c.r[3] != r3want || n > 100000) return; }
  else if (n > 20 && n % 1000) return;
  const GuestFunc* g = rt_containing_func(fn);
  logf("[trace] #%llu %s r3=%08X r4=%08X r5=%08X r6=%08X\n", (unsigned long long)n, g ? g->name : "?", c.r[3], c.r[4], c.r[5], c.r[6]);
  sys_backtrace(c);
}

HookFn sys_find_hook(uint32_t addr) {
  if (g_tf_n < 0) {
    g_tf_n = 0;
    if (const char* s = getenv("TRACE_FN"))
      for (const char* p = s; *p && g_tf_n < 4;) {
        g_tf_addr[g_tf_n++] = (uint32_t)strtoul(p, nullptr, 16);
        p = strchr(p, ','); if (!p) break; ++p;
      }
  }
  static const HookFn hooks[4] = {tf_hook<0>, tf_hook<1>, tf_hook<2>, tf_hook<3>};
  for (int i = 0; i < g_tf_n; ++i) if (g_tf_addr[i] == addr) return hooks[i];
  return nullptr;
}

void logf(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fflush(stderr);
}

void sys_fatal() {
  fflush(stderr);
  TerminateProcess(GetCurrentProcess(), 3);   // #153: no teardown (it can block on the GPU driver)
  ExitProcess(3);
}

void sys_backtrace(Cpu& c) {
  uint32_t sp = c.r[1];
  fprintf(stderr, "     backtrace:");
  const GuestFunc* f = rt_containing_func(c.lr);
  fprintf(stderr, " [lr %08X %s]", c.lr, f ? f->name : "?");
  for (int i = 0; i < 24 && sp >= 0x80000000u && sp < RT_RAM_END; ++i) {
    uint32_t back = ld32(sp), lr = ld32(sp + 4);
    const GuestFunc* g = rt_containing_func(lr);
    fprintf(stderr, " %08X(%s)", lr, g ? g->name : "?");
    if (back <= sp) break;
    sp = back;
  }
  fprintf(stderr, "\n");
}

static uint64_t g_dec_expire_tb = ~0ull;
static bool g_dec_fired = true;

uint32_t rt_mftb(uint32_t tbr) {
  uint64_t tb = sys_timebase();
  return tbr == 269 ? (uint32_t)(tb >> 32) : (uint32_t)tb;
}
uint32_t rt_get_dec(Cpu&) {
  uint64_t tb = sys_timebase();
  if (g_dec_expire_tb == ~0ull) return 0x7FFFFFFF;
  return (uint32_t)(g_dec_expire_tb - tb);
}
void rt_set_dec(Cpu&, uint32_t v) {
  g_dec_expire_tb = sys_timebase() + v;
  g_dec_fired = false;
}

uint32_t rt_mfspr(Cpu& c, uint32_t spr) { return c.spr[spr & 1023]; }

static void lc_dma(Cpu& c) {
  uint32_t u = c.spr[922], l = c.spr[923];
  uint32_t len = ((u & 0x1F) << 2) | ((l >> 2) & 3);
  if (!len) len = 128;
  uint32_t mem = u & 0xFFFFFFE0u;
  uint32_t lcaddr = l & 0x0003FFE0u;
  bool load = (l >> 4) & 1;
  uint32_t bytes = len * 32;
  for (uint32_t i = 0; i < bytes; ++i) {
    if (load) g_lc[(lcaddr + i) & 0x3FFF] = ld8(mem + i);
    else st8(mem + i, g_lc[(lcaddr + i) & 0x3FFF]);
  }
}
void rt_mtspr(Cpu& c, uint32_t spr, uint32_t v) {
  spr &= 1023;
  c.spr[spr] = v;
  if (spr == 923 && (v & 2)) { lc_dma(c); c.spr[923] = v & ~2u; }
}

void rt_sc(Cpu& c, uint32_t pc) { (void)c; (void)pc; /* DCFlushRange/ICInvalidate syscall: caches are coherent here */ }

void rt_rfi(Cpu& c, uint32_t pc) {
  c.msr = c.srr1;
  if (is_sentinel(c.srr0)) {
    if (!g_exc_top) rt_unimpl(c, pc, "rfi to exception sentinel with no frame");
    longjmp(g_exc_top->jb, 1);
  }
  // real target (thread start etc.)
  call_addr(c, c.srr0);
}

void sys_deliver_exception(Cpu& c, int N) {
  Cpu saved = c;
  ExcFrame fr;
  fr.prev = g_exc_top;
  fr.which = N;
  g_exc_top = &fr;
  if (setjmp(fr.jb) == 0) {
    // emulate the exception vector template (OSExceptionVector, executed with translation off)
    uint32_t pc_ctx = ld32(0x800000C0);
    st32(pc_ctx + 0xC, c.r[3]);
    st32(pc_ctx + 0x10, c.r[4]);
    st32(pc_ctx + 0x14, c.r[5]);
    st16(pc_ctx + 0x1A2, ld16(pc_ctx + 0x1A2) | 2);
    st32(pc_ctx + 0x80, get_cr(c));
    st32(pc_ctx + 0x84, c.lr);
    st32(pc_ctx + 0x88, c.ctr);
    st32(pc_ctx + 0x8C, get_xer(c));
    uint32_t sent = sentinel_for(sched_current_id(), N);
    sched_register_owner(ld32(0x800000D4));
    st32(pc_ctx + 0x198, sent);               // srr0 (interrupted PC is not meaningful here)
    st32(pc_ctx + 0x19C, c.msr);              // srr1
    uint32_t srr1 = c.msr;
    c.srr0 = sent;
    c.srr1 = (c.msr & ~0x8000u) | 0x30;       // handler runs with IR/DR on, EE off
    c.r[3] = N;
    c.r[4] = ld32(0x800000D4);                // __OSCurrentContext
    uint32_t handler = ld32(0x80003000 + (N << 2));
    if (!(srr1 & 2)) handler = 0;             // non-recoverable
    if (!handler) rt_unimpl(c, 0, "exception without handler");
    c.msr = c.srr1;
    call_addr(c, handler);
    // handler returned without rfi: treat as return from exception
  }
  g_exc_top = fr.prev;
  c = saved;
}

static uint64_t g_irq_counts[16];
static void dump_threads() {
  gxlle::print_stats();
  hw::si_dump();
  fprintf(stderr, "     VI xfb regs: TFBL=%04X%04X BFBL=%04X%04X DCR=%04X VT=%04X ring[0..2]=%08X %08X %08X cur=%08X\n", hw::S.vi[14], hw::S.vi[15], hw::S.vi[18], hw::S.vi[19], hw::S.vi[1], hw::S.vi[0], ld32(0x805E3178), ld32(0x805E3180), ld32(0x805E3188), ld32(0x805F1604));
  fprintf(stderr, "     game frame ring: r=%u w=%u cap=%u  805F15F0=%u shown=%08X ring=[%08X %08X %08X] vi_idx=%u flags 161C=%u 1620=%u 1624=%u 1628=%u retr=%u/%u\n",
          ld32(0x805F1608), ld32(0x805F160C), ld32(0x805EDF34), ld32(0x805F15F0), ld32(0x805F1604), ld32(0x805E3178), ld32(0x805E3180),
          ld32(0x805E3188), ld32(0x805F1614), ld32(0x805F161C), ld32(0x805F1620), ld32(0x805F1624), ld32(0x805F1628), ld8(0x805EDF38), ld8(0x805EDF39));
  fprintf(stderr, "     draw-sync tokens: ring=[%04X %04X %04X] issued(805EDF40)=%04X PE_TOKEN=%04X\n", ld16(0x805F1638), ld16(0x805F163A),
          ld16(0x805F163C), ld16(0x805EDF40), hw::pe_read16(0x0E));
  {
    uint32_t tbl = ld32(0x805F0EE8);
    fprintf(stderr, "     OS interrupt handlers (table %08X): ", tbl);
    for (int i = 17; i <= 26; ++i) {
      uint32_t h = ld32(tbl + 4 * i);
      const GuestFunc* g = rt_containing_func(h);
      fprintf(stderr, "[%d]=%08X(%s) ", i, h, g ? g->name : "-");
    }
    fprintf(stderr, "\n");
  }
  fprintf(stderr, "     irq deliveries by PI bit:");
  for (int b = 0; b < 16; ++b) if (g_irq_counts[b]) fprintf(stderr, " [%d]=%llu", b, (unsigned long long)g_irq_counts[b]);
  fprintf(stderr, "\n");
  uint32_t t = ld32(0x800000DC);
  uint32_t cur = ld32(0x800000E4);
  fprintf(stderr, "     VI int regs: %04X %04X %04X %04X %04X %04X %04X %04X frame=%u pending=%X\n", hw::S.vi[24], hw::S.vi[25], hw::S.vi[26], hw::S.vi[27], hw::S.vi[28], hw::S.vi[29], hw::S.vi[30], hw::S.vi[31], hw::S.vi_frame, hw::S.vi_int_pending);
  fprintf(stderr, "     threads (current=%08X) RunQueueBits=%08X hint=%u resched=%u:\n", cur, ld32(0x805F0F38), ld32(0x805F0F3C), ld32(0x805F0F40));
  for (int n = 0; t && n < 32; ++n) {
    uint32_t state = ld16(t + 0x2C8), prio = ld32(t + 0x2D0), queue = ld32(t + 0x2DC);
    uint32_t sp = ld32(t + 4), lr = ld32(t + 0x84);
    const GuestFunc* ent = rt_containing_func(ld32(t + 0x198));
    fprintf(stderr, "       thr %08X state=%u prio=%d suspend=%d queue=%08X lr=%08X srr0=%08X(%s) r3=%08X stk:", t, state, (int)prio,
            (int)ld32(t + 0x2CC), queue, lr, ld32(t + 0x198), ent ? ent->name : "?", ld32(t + 0xC));
    for (int i = 0; i < 12 && sp >= 0x80000000u && sp < RT_RAM_END; ++i) {
      uint32_t back = ld32(sp), l = ld32(sp + 4);
      const GuestFunc* g = rt_containing_func(l);
      fprintf(stderr, " %08X(%s)", l, g ? g->name : "?");
      if (back <= sp) break;
      sp = back;
    }
    fprintf(stderr, "\n");
    t = ld32(t + 0x2FC);
  }
}

static bool g_watch = false;
void sys_set_watch(bool v) { g_watch = v; }

void sys_dump_state(const char* why) {
  fprintf(stderr, "[state] ---- %s ----\n", why);
  if (Cpu* c = sched_current_cpu()) sys_backtrace(*c);
  dump_threads();
  fflush(stderr);
}

// Hang watchdog: if no frame has been displayed (XFB copy) for 4 s, dump the scheduler/GX state to the log
// (up to 3 times per stall) so freezes reported by players can be diagnosed from the log file.
static void hang_watchdog(Cpu& c, uint32_t pc) {
  static uint64_t last_check = 0, last_change = 0, last_count = ~0ull;
  static int dumps = 0;
  static unsigned polls = 0;
  if (++polls & 63) return;   // #152: one clock read per 64 polls (the check runs every 500 ms anyway)
  uint64_t now = sys_time_ns();
  if (now - last_check < 500000000ull) return;
  last_check = now;
  uint64_t n = gxlle::xfb_count();
  if (n != last_count) { last_count = n; last_change = now; dumps = 0; return; }
  // before the first frame (boot: __init_data clears memory, files load) allow 20 s - a slow first start of a freshly
  // built exe (page-in / virus scan) took 5.3 s once and was logged as a hang (2026-10-20, 0 of 10 in a repro loop)
  const uint64_t limit = n == 0 ? 20000000000ull : 4000000000ull;
  if (now - last_change > limit * (dumps + 1) && dumps < 3) {
    ++dumps;
    const GuestFunc* f = rt_containing_func(pc);
    fprintf(stderr, "[hang] host %.0f ms: no frame for %.1f s (dump %d) - poll at %08X (%s+0x%X)\n", now * 1e-6, (now - last_change) / 1e9, dumps, pc,
            f ? f->name : "?", f ? pc - f->addr : 0);
    sys_dump_state("hang");
  }
}

void rt_poll(Cpu& c, uint32_t pc) {
  c.budget = 4000;
  hang_watchdog(c, pc);
  if (g_watch) {
    static uint64_t last = 0;
    uint64_t now = sys_time_ns();
    if (now - last > 2000000000ull) {
      last = now;
      const GuestFunc* f = rt_containing_func(pc);
      fprintf(stderr, "[watch] poll at %08X (%s+0x%X) msr=%08X pi=%08X/%08X\n", pc, f ? f->name : "?", f ? pc - f->addr : 0,
              c.msr, hw_irq_status(), hw::S.pi_intmsk);
      sys_backtrace(c);
      dump_threads();
    }
  }
  hw_tick(c);
  if (!(c.msr & 0x8000)) return;
  uint64_t tb = sys_timebase();
  if (!g_dec_fired && g_dec_expire_tb != ~0ull && tb >= g_dec_expire_tb) {
    g_dec_fired = true;
    sys_deliver_exception(c, EXC_DECREMENTER);
    return;
  }
  if (hw_irq_pending()) {
    uint32_t st = hw_irq_status();
    for (int b = 0; b < 16; ++b) if (st & (1u << b)) ++g_irq_counts[b];
    sys_deliver_exception(c, EXC_EXTERNAL);
  }
}

static LONG WINAPI crash_filter(EXCEPTION_POINTERS* ep) {
  fprintf(stderr, "\n[crash] host exception %08lX at %p\n", ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress);
  {   // which module / thread (host faults inside Dawn or the runtime look alike in the guest backtrace)
    HMODULE mod = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)ep->ExceptionRecord->ExceptionAddress, &mod))
      GetModuleFileNameA(mod, name, sizeof name);
    fprintf(stderr, "        in %s (+%llx), thread %lu\n", name, (unsigned long long)((char*)ep->ExceptionRecord->ExceptionAddress - (char*)mod), GetCurrentThreadId());
  }
  if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2)
    fprintf(stderr, "        access violation %s address %p\n", ep->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading", (void*)ep->ExceptionRecord->ExceptionInformation[1]);
  if (Cpu* c = sched_current_cpu()) sys_backtrace(*c);
  fflush(stderr);
  return EXCEPTION_CONTINUE_SEARCH;
}
// #153 (user: "very hard to close the game, it freezes"): closing never runs the process teardown. exit() / ExitProcess
// first kill every other thread (the #114 watchdog with them), then run the static destructors and DLL detach: the Dawn
// device drains the GPU and SDL closes the audio device there, which a busy or hung driver can block for minutes. Here:
// the audio output stops (on a helper thread, at most 150 ms), the log is flushed, and the process is terminated at once.
// Saves are safe: the memory card is written synchronously on the game thread (exi_card.cpp), the thread that closes,
// and settings are saved when they change. Windows releases the GPU device with the process.
void audio_stop_for_exit();   // audio.cpp
[[noreturn]] void sys_exit_guarded(int code) {
  const uint64_t t0 = GetTickCount64();
  // the window goes away at once (the kernel may need a moment to release a GPU device that is still busy)
  EnumWindows([](HWND w, LPARAM) -> BOOL {
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid == GetCurrentProcessId()) ShowWindow(w, SW_HIDE);
    return TRUE;
  }, 0);
  if (HANDLE h = CreateThread(nullptr, 0, [](LPVOID) -> DWORD { audio_stop_for_exit(); return 0; }, nullptr, 0, nullptr)) {
    WaitForSingleObject(h, 150);
    CloseHandle(h);
  }
  fprintf(stderr, "[exit] fast exit (code %d, %llu ms): process terminated, no teardown\n", code, (unsigned long long)(GetTickCount64() - t0));
  fflush(stderr);
  TerminateProcess(GetCurrentProcess(), (UINT)code);
  ExitProcess((UINT)code);   // not reached
}
static void at_exit_report() {
  fprintf(stderr, "[exit] process exiting\n");
  if (Cpu* c = sched_current_cpu()) sys_backtrace(*c);
  fflush(stderr);
}
void sys_install_crash_handlers() {
  SetUnhandledExceptionFilter(crash_filter);
  atexit(at_exit_report);
}

uint32_t g_watch_ea = 0xFFFFFFFFu;
uint32_t g_watch_mask = 0;   // nonzero: only log 32-bit writes that change one of these bits (INPUT_FILE "WATCH ea:mask")
void rt_watch_hit(uint32_t ea, uint32_t v, int size) {
  if (g_watch_mask && size == 4 && !((ld32(ea) ^ v) & g_watch_mask)) return;
  static int hits = 0;
  if (++hits > 600) return;   // a hot field would flood the log
  fprintf(stderr, "[watch-ea] write %08X <- %08X (size %d)\n", ea, v, size);
  if (Cpu* c = sched_current_cpu()) sys_backtrace(*c);
  fflush(stderr);
}

void rt_watch_dma(const char* what, uint32_t phys, uint32_t len, uint64_t src) {
  uint32_t w = g_watch_ea & RT_RAM_MASK;
  if (g_watch_ea != 0xFFFFFFFFu && w >= (phys & RT_RAM_MASK) && w < (phys & RT_RAM_MASK) + len)
    fprintf(stderr, "[watch-ea] DMA %s covers %08X: dest=%08X len=%X src=%llX\n", what, g_watch_ea, phys, len, (unsigned long long)src);
}
