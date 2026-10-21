// Memory setup, DOL loading and the post-apploader machine state.
#include "sys.h"
#include "hw.h"
#include <windows.h>
#include <cstring>
#include <vector>
#include <algorithm>

uint8_t* g_ram = nullptr;
uint32_t g_mem1_size = 0x04000000u;   // #60: SHADOW_MEM1_MB=24..64 (default 64; 24 = the GameCube)

static std::vector<uint8_t> slurp(const std::string& p) {
  std::vector<uint8_t> v;
  FILE* f = fopen(p.c_str(), "rb");
  if (!f) return v;
  fseek(f, 0, SEEK_END);
  v.resize(ftell(f));
  fseek(f, 0, SEEK_SET);
  fread(v.data(), 1, v.size(), f);
  fclose(f);
  return v;
}
static uint32_t be32(const uint8_t* p) { return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static void wr32(uint32_t ea, uint32_t v) { st32(ea, v); }

bool boot_init(const std::string& root) {
  if (const char* mb = getenv("SHADOW_MEM1_MB")) g_mem1_size = std::clamp<uint32_t>((uint32_t)atoi(mb), 24, RT_RAM_SIZE >> 20) << 20;
  g_ram = (uint8_t*)VirtualAlloc(nullptr, RT_RAM_SIZE + 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  if (!g_ram) { logf("cannot allocate guest RAM\n"); return false; }
  if (!disc_init(root)) return false;
  auto dol = slurp(root + "/sys/main.dol");
  if (dol.size() < 0x100) { logf("cannot read main.dol\n"); return false; }
  uint32_t bss = be32(&dol[0xD8]), bsssz = be32(&dol[0xDC]);
  memset(g_ram + (bss & RT_RAM_MASK), 0, bsssz);
  for (int i = 0; i < 18; ++i) {
    uint32_t off = be32(&dol[i * 4]), addr = be32(&dol[0x48 + i * 4]), sz = be32(&dol[0x90 + i * 4]);
    if (!sz) continue;
    memcpy(g_ram + (addr & RT_RAM_MASK), &dol[off], sz);
  }

  // disc header + boot info
  auto boot = slurp(root + "/sys/boot.bin");
  auto bi2 = slurp(root + "/sys/bi2.bin");
  auto fst = disc_fst();   // the disc's file table, patched for mods (runtime/disc.cpp)
  memcpy(g_ram, boot.data(), 0x20);
  wr32(0x80000018, 0);
  wr32(0x8000001C, 0xC2339F3D);
  wr32(0x80000020, 0x0D15EA5E);
  wr32(0x80000024, 1);
  wr32(0x80000028, g_mem1_size);      // physical memory size (the OS arena / heap grow with it)
  wr32(0x8000002C, 0x00000003);        // retail hardware
  uint32_t bi2_addr = 0x80000000u + g_mem1_size - 0x2000;
  memcpy(g_ram + (bi2_addr & RT_RAM_MASK), bi2.data(), std::min<size_t>(bi2.size(), 0x2000));
  uint32_t fst_addr = (bi2_addr - (uint32_t)fst.size()) & ~31u;
  memcpy(g_ram + (fst_addr & RT_RAM_MASK), fst.data(), fst.size());
  wr32(0x80000034, fst_addr);          // arena hi
  wr32(0x80000038, fst_addr);
  wr32(0x8000003C, (uint32_t)fst.size());
  wr32(0x800000CC, 0);                 // NTSC
  wr32(0x800000D0, 0x01000000);        // ARAM 16 MB
  wr32(0x800000EC, 0x80000000u + g_mem1_size);
  wr32(0x800000F0, g_mem1_size);
  wr32(0x800000F4, bi2_addr);
  wr32(0x800000F8, 162000000);
  wr32(0x800000FC, 486000000);
  if (g_mem1_size != 0x01800000u) logf("[mem] guest MEM1 %u MB (GameCube: 24)\n", g_mem1_size >> 20);
  hw_init();
  return true;
}
