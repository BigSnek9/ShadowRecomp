// Guest thread scheduler glue: each OS thread runs on its own host fiber.
#pragma once
#include "sys.h"
#include <csetjmp>

struct ExcFrame {
  jmp_buf jb;
  ExcFrame* prev;
  uint32_t which;
};
extern ExcFrame* g_exc_top;     // exception frames of the *current* fiber

constexpr uint32_t kSentinelBase = 0xFFF00000u;   // srr0 marker: "resume at the interrupt site"
inline uint32_t sentinel_for(int fid, int n) { return kSentinelBase | ((uint32_t)fid << 8) | (uint32_t)n; }
inline bool is_sentinel(uint32_t v) { return (v & 0xFFF00000u) == kSentinelBase; }
inline int sentinel_fid(uint32_t v) { return (int)((v >> 8) & 0xFFF); }

void sched_init(Cpu& main_cpu);
int sched_current_id();
void sched_register_owner(uint32_t ctx);      // ctx (any alias) is now owned by the running fiber
Cpu* sched_current_cpu();
void sys_install_crash_handlers();
extern bool g_trace_sched;
