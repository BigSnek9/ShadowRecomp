// High-level replacements for guest functions (see overrides.txt).
#include "sys.h"
#include <string>
#include <cstring>

void hle_nop(Cpu&) {}

namespace stage_workshop { void memory_report(int stage); }   // stage_workshop.cpp: every heap's free / largest block
namespace engine_limits { void exit_report(Cpu& c); }       // engine_limits.cpp: a failed operator new's size
void hle_guest_exit(Cpu& c) {
  fprintf(stderr, "[guest] exit()/_ExitProcess called, status=%d\n", (int)c.r[3]);
  sys_backtrace(c);
  engine_limits::exit_report(c);
  stage_workshop::memory_report(-1);   // #154: the heaps at the moment the game gave up (an allocation that failed)
  fflush(stderr);
  sys_fatal();
}

// ---- OSReport / debug output capture --------------------------------------------------------------
// Guest varargs (Dolphin SDK ABI): integer args in r4..r10 then the stack (r1+8...), doubles in f1..f8.
static std::string guest_cstr(uint32_t ea) {
  std::string s;
  for (int i = 0; i < 1024; ++i) { char ch = (char)ld8(ea + i); if (!ch) break; s += ch; }
  return s;
}

std::string guest_format(Cpu& c, uint32_t fmt_ea, int first_int_reg) {
  std::string fmt = guest_cstr(fmt_ea), out;
  int ireg = first_int_reg, freg = 1;
  uint32_t stack = c.r[1] + 8;
  auto next_int = [&]() -> uint32_t {
    if (ireg <= 10) return c.r[ireg++];
    uint32_t v = ld32(stack); stack += 4; return v;
  };
  auto next_dbl = [&]() -> double {
    if (freg <= 8) return c.f[freg++];
    double d = ldf64(stack); stack += 8; return d;
  };
  for (size_t i = 0; i < fmt.size(); ++i) {
    if (fmt[i] != '%') { out += fmt[i]; continue; }
    std::string spec = "%";
    ++i;
    while (i < fmt.size() && strchr("-+ #0123456789.*lhLqjzt", fmt[i])) {
      if (fmt[i] == '*') spec += std::to_string((int)next_int()); else spec += fmt[i];
      ++i;
    }
    if (i >= fmt.size()) break;
    char cv = fmt[i];
    char buf[512];
    std::string base;
    for (char ch : spec) if (!strchr("lhLqjzt", ch)) base += ch;
    switch (cv) {
      case '%': out += '%'; break;
      case 'd': case 'i': snprintf(buf, sizeof buf, (base + "d").c_str(), (int)next_int()); out += buf; break;
      case 'u': snprintf(buf, sizeof buf, (base + "u").c_str(), next_int()); out += buf; break;
      case 'x': case 'X': case 'o': snprintf(buf, sizeof buf, (base + cv).c_str(), next_int()); out += buf; break;
      case 'p': snprintf(buf, sizeof buf, "%08X", next_int()); out += buf; break;
      case 'c': out += (char)next_int(); break;
      case 's': { std::string s = guest_cstr(next_int()); snprintf(buf, sizeof buf, (base + "s").c_str(), s.c_str()); out += buf; break; }
      case 'f': case 'g': case 'e': case 'G': case 'E': snprintf(buf, sizeof buf, (base + cv).c_str(), next_dbl()); out += buf; break;
      default: out += spec; out += cv; break;
    }
  }
  return out;
}

void hle_OSReport(Cpu& c) {
  std::string s = guest_format(c, c.r[3], 4);
  fprintf(stderr, "[OSReport] %s", s.c_str());
  if (s.empty() || s.back() != '\n') fputc('\n', stderr);
  fflush(stderr);
}
