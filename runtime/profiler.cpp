// PROFILE=<seconds-after-start>[,<duration>]: sampling profiler for the game thread.
// A helper thread suspends the main (game) thread ~1000x per second, records its instruction pointer, and after the
// window resolves the samples to function names with DbgHelp (needs the PDB; the build links with /DEBUG) and logs
// the top functions. Used to find where frame time goes (recompiled guest code vs runtime vs Aurora).
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include "sys.h"
#include "sched.h"
#include "rt_internal.h"
#include <set>
#include <algorithm>
#include <cstdlib>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace {
HANDLE g_main_thread = nullptr;

void run(double start_s, double dur_s) {
  Sleep((DWORD)(start_s * 1000));
  std::vector<DWORD64> samples;
  static const bool guest_stacks = getenv("PROFILE_GUEST") != nullptr;   // #152: inclusive time per guest function
  std::vector<std::vector<uint32_t>> stacks;
  samples.reserve((size_t)(dur_s * 1100));
  LARGE_INTEGER f, t0, t;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&t0);
  timeBeginPeriod(1);
  for (;;) {
    QueryPerformanceCounter(&t);
    if ((t.QuadPart - t0.QuadPart) / (double)f.QuadPart > dur_s) break;
    if (SuspendThread(g_main_thread) != (DWORD)-1) {
      CONTEXT ctx{};
      ctx.ContextFlags = CONTEXT_CONTROL;
      if (GetThreadContext(g_main_thread, &ctx)) samples.push_back(ctx.Rip);
      if (guest_stacks) {   // PROFILE_GUEST=1: the guest call chain at this sample (saved LRs up the r1 chain)
        std::vector<uint32_t> st;
        if (Cpu* c = sched_current_cpu()) {
          st.push_back(c->lr);
          uint32_t sp = c->r[1];
          for (int i = 0; i < 40 && sp >= 0x80000000u && sp < RT_RAM_END - 8; ++i) {
            const uint32_t back = ld32(sp), lr = ld32(sp + 4);
            if (lr >= 0x80000000u && lr < 0x81800000u) st.push_back(lr);
            if (back <= sp) break;
            sp = back;
          }
        }
        stacks.push_back(std::move(st));
      }
      ResumeThread(g_main_thread);
    }
    Sleep(1);
  }
  HANDLE proc = GetCurrentProcess();
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
  SymInitialize(proc, nullptr, TRUE);
  std::map<std::string, int> counts;
  alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
  for (DWORD64 ip : samples) {
    auto* si = (SYMBOL_INFO*)buf;
    si->SizeOfStruct = sizeof(SYMBOL_INFO);
    si->MaxNameLen = 255;
    DWORD64 disp = 0;
    std::string name = SymFromAddr(proc, ip, &disp, si) ? std::string(si->Name) : "?";
    if (name == "?") {
      HMODULE mod = nullptr;
      char mname[MAX_PATH] = "?";
      if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             (LPCSTR)ip, &mod))
        GetModuleFileNameA(mod, mname, MAX_PATH);
      name = std::string("[") + (strrchr(mname, '\\') ? strrchr(mname, '\\') + 1 : mname) + "]";
    }
    ++counts[name];
  }
  std::vector<std::pair<int, std::string>> v;
  for (auto& [n, c] : counts) v.push_back({c, n});
  std::sort(v.rbegin(), v.rend());
  logf("[profile] %zu samples over %.1f s (game thread)\n", samples.size(), dur_s);
  int guest = 0;
  for (auto& [c, n] : v) if (n.rfind("f_8", 0) == 0 || n.rfind("fe_8", 0) == 0) guest += c;
  logf("[profile] recompiled guest functions: %.1f%% of samples\n", 100.0 * guest / std::max<size_t>(1, samples.size()));
  for (size_t i = 0; i < v.size() && i < 40; ++i)
    logf("[profile] %5.1f%%  %s\n", 100.0 * v[i].first / samples.size(), v[i].second.c_str());
  if (guest_stacks && !stacks.empty()) {   // inclusive: each guest function counted once per sample it is on the stack
    if (const char* out = getenv("PROFILE_GUEST_OUT")) {   // the raw stacks (innermost first), one sample per line
      if (FILE* f = fopen(out, "w")) {
        for (auto& st : stacks) {
          for (uint32_t lr : st) if (const GuestFunc* g = rt_containing_func(lr)) fprintf(f, "%08X ", g->addr);
          fprintf(f, "\n");
        }
        fclose(f);
      }
    }
    std::map<uint32_t, int> incl;
    for (auto& st : stacks) {
      std::set<uint32_t> seen;
      for (uint32_t lr : st)
        if (const GuestFunc* g = rt_containing_func(lr)) seen.insert(g->addr);
      for (uint32_t a : seen) ++incl[a];
    }
    std::vector<std::pair<int, uint32_t>> w;
    for (auto& [a, n] : incl) w.push_back({n, a});
    std::sort(w.rbegin(), w.rend());
    logf("[profile] guest call chains (inclusive, %% of samples):\n");
    for (size_t i = 0; i < w.size() && i < 60; ++i) logf("[profile-incl] %5.1f%%  fn_%08X\n", 100.0 * w[i].first / stacks.size(), w[i].second);
  }
}
}  // namespace

void profiler_start_if_requested() {
  const char* p = getenv("PROFILE");
  if (!p) return;
  double start = atof(p), dur = 10;
  if (const char* c = strchr(p, ',')) dur = atof(c + 1);
  DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_main_thread, 0, FALSE,
                  DUPLICATE_SAME_ACCESS);
  std::thread(run, start, dur).detach();
}
