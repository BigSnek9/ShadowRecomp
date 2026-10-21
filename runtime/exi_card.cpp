// GameCube memory card (slot A, EXI channel 0 device 0), saved as a PC file.
//
// The game's own CARD library (recompiled guest code) drives this exactly like real hardware, so formatting,
// creating, saving and loading all go through the original code paths. The card is a 16 Mbit (251 block) Nintendo
// card stored raw in <exe dir>\saves\MemoryCardA.raw (the same layout Dolphin uses for .raw cards).
//
// Command set (as the game's own CARD library sends it, cross-checked with Dolphin's open-source memory card
// emulation, EXI_DeviceMemoryCard; the names in brackets are the game's function names):
//   00 xx            -> 4-byte device ID          (EXIGetID)
//   85 xx            -> 2-byte vendor ID          (__CARDReadVendorID)
//   83 xx            -> 1-byte status             (__CARDReadStatus)
//   89               clear status                 (__CARDClearStatus)
//   81 en            enable/disable card interrupt
//   52 a1 a2 a3 a4 + latency bytes, then 512 data bytes read     (__CARDReadSegment)
//   F2 a1 a2 a3 a4, then 128 data bytes written (page program)   (__CARDWritePage)
//   F1 a1 a2         sector erase (8 KB)                          (__CARDEraseSector)
//   F4 ...           chip erase
// Program/erase complete when the device is deselected; then the card raises its EXI interrupt if enabled.
// Status is reported as READY|UNLOCKED (0x41): with the unlocked bit set the SDK skips the DSP-based card unlock.
#include "sys.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

bool exi_card_write_protect();   // game_hooks.cpp
namespace exicard {
namespace {
constexpr uint32_t kSizeMbit = 16;                           // 2 MB, 251 user blocks
constexpr uint32_t kBytes = kSizeMbit * 1024 * 1024 / 8;
constexpr uint32_t kDeviceId = kSizeMbit;                    // size in Mbit, 8 KB sectors, latency 4
constexpr uint8_t ST_READY = 0x01, ST_UNLOCKED = 0x40;

std::vector<uint8_t> g_mem;
std::string g_path;
bool g_loaded = false;
uint8_t g_status = ST_READY | ST_UNLOCKED;
bool g_irq_enabled = false;

uint8_t g_cmd = 0;
uint32_t g_pos = 0;          // bytes transferred since select
uint8_t g_args[5];
uint32_t g_addr = 0;
uint8_t g_page[512];
uint32_t g_page_len = 0;

uint32_t decode_addr() {
  return ((uint32_t)(g_args[1] & 0x7F) << 17) | ((uint32_t)g_args[2] << 9) | ((uint32_t)(g_args[3] & 3) << 7) |
         (g_args[4] & 0x7F);
}

void load() {
  if (g_loaded) return;
  g_loaded = true;
  char exe[MAX_PATH];
  GetModuleFileNameA(nullptr, exe, MAX_PATH);
  std::string dir = exe;
  dir = dir.substr(0, dir.find_last_of("\\/")) + "\\saves";
  CreateDirectoryA(dir.c_str(), nullptr);
  g_path = dir + "\\MemoryCardA.raw";
  if (const char* src = getenv("TEST_CARD")) {   // testing: start from this card (copied over this copy's own card first)
    if (CopyFileA(src, g_path.c_str(), FALSE)) logf("[card] TEST_CARD %s copied in\n", src);
  }
  g_mem.assign(kBytes, 0xFF);   // erased flash; the game offers to format a blank card
  if (FILE* f = fopen(g_path.c_str(), "rb")) {
    size_t n = fread(g_mem.data(), 1, kBytes, f);
    fclose(f);
    logf("[card] loaded %s (%zu bytes)\n", g_path.c_str(), n);
  } else {
    logf("[card] no card file yet, new blank card: %s\n", g_path.c_str());
  }
}

// write a range back to the card file immediately (crash-safe saves)
void flush(uint32_t off, uint32_t len) {
  if (exi_card_write_protect()) {   // "Unlock every stage" was used this session: keep saves in memory only
    static int n = 0;
    if (n++ < 3) logf("[card] save kept in memory only (stage unlock used this session; the card file is untouched)\n");
    return;
  }
  FILE* f = fopen(g_path.c_str(), "r+b");
  if (!f) {
    f = fopen(g_path.c_str(), "wb");
    if (!f) { logf("[card] cannot write %s\n", g_path.c_str()); return; }
    fwrite(g_mem.data(), 1, kBytes, f);
    fclose(f);
    return;
  }
  fseek(f, (long)off, SEEK_SET);
  fwrite(g_mem.data() + off, 1, len, f);
  fclose(f);
}
}  // namespace

void select() {
  load();
  g_pos = 0;
  g_cmd = 0;
  g_page_len = 0;
}

// One byte on the wire. `in` is what the CPU sends (dummy on reads); returns what the card drives back.
uint8_t transfer(uint8_t in) {
  uint32_t pos = g_pos++;
  if (pos == 0) {
    g_cmd = in;
    if (in == 0x89) g_status = (uint8_t)((g_status & ~0x18) | ST_READY);   // clear program/erase errors
    return 0xFF;
  }
  switch (g_cmd) {
    case 0x00:   // device ID: 1 dummy byte after the command, then the 32-bit ID
      return pos >= 2 ? (uint8_t)(kDeviceId >> (24 - 8 * ((pos - 2) & 3))) : 0;
    case 0x85:   // vendor ID (0x0000: not a Samsung "fast" card -> 128-byte pages)
      return 0x00;
    case 0x83:
      return g_status;
    case 0x81:
      if (pos == 1) g_irq_enabled = (in & 1) != 0;
      return 0;
    case 0x52:   // read array
      if (pos <= 4) { g_args[pos] = in; if (pos == 4) g_addr = decode_addr(); return 0; }
      return pos >= 5 + 4 ? g_mem[g_addr++ % kBytes] : 0;   // 4 latency bytes, then data
    case 0xF2:   // page program
      if (pos <= 4) { g_args[pos] = in; if (pos == 4) g_addr = decode_addr(); return 0; }
      if (g_page_len < sizeof g_page) g_page[g_page_len++] = in;
      return 0;
    case 0xF1:   // sector erase
      if (pos <= 2) g_args[pos] = in;
      return 0;
    default:
      return 0;
  }
}

// Deselect: commit program/erase. Returns true when the card raises its EXI interrupt.
bool deselect() {
  bool irq = false;
  if (g_cmd == 0xF2 && g_page_len) {
    uint32_t a = g_addr % kBytes;
    uint32_t n = std::min<uint32_t>(g_page_len, kBytes - a);
    memcpy(&g_mem[a], g_page, n);
    flush(a, n);
    irq = true;
  } else if (g_cmd == 0xF1 && g_pos >= 3) {
    uint32_t a = (((uint32_t)(g_args[1] & 0x7F) << 17) | ((uint32_t)g_args[2] << 9)) % kBytes;
    a &= ~(8192u - 1);
    memset(&g_mem[a], 0xFF, 8192);
    flush(a, 8192);
    irq = true;
  } else if (g_cmd == 0xF4) {
    memset(g_mem.data(), 0xFF, kBytes);
    flush(0, kBytes);
    irq = true;
  }
  g_cmd = 0;
  g_pos = 0;
  g_page_len = 0;
  return irq && g_irq_enabled;
}
}  // namespace exicard
