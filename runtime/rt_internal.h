#pragma once
#include "ppc_rt.h"
struct GuestFunc { uint32_t addr, size; void (*fn)(Cpu&); const char* name; };
extern const GuestFunc g_functab[];
extern const unsigned g_functab_count;
struct MultiFunc { uint32_t addr, size; void (*fn)(Cpu&, uint32_t); };
extern const MultiFunc g_multi[];
