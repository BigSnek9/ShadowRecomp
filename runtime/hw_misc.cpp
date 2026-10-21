// MI and small device stubs. GX/CP/PE/WGP live in gx_lle.cpp, audio in hw_dsp.cpp.
#include "sys.h"
#include "hw.h"
#include "gx_lle.h"
#include <cstring>

namespace hw {
static uint16_t g_mi[0x80];
uint16_t mi_read16(uint32_t off) { return g_mi[(off >> 1) & 0x7F]; }
void mi_write16(uint32_t off, uint16_t v) { g_mi[(off >> 1) & 0x7F] = v; }
bool mi_irq() { return false; }

void tick_devices(uint64_t now) { ai_dma_tick(now); gxlle::gp_run(); }
void dsp_cpu_interrupt() {}
}
