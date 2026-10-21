// Extra sound banks for bosses placed outside their stages (2026-10-20, tools/stage_editor/sound_banks.py).
// The sound manager loads its banks through vtable slot 0x1C fn_801F2770(this, bank 0..7, stage); bank 7 = the stage
// bank, its file picked by stage number from the table at 8053E1B8 ({stage, "sound/B7x_....mlt"} pairs, -1 ends) and
// loaded by slot 0x18 fn_801F29D4(this, path) (the bank number is inside the file, MLT header byte 0x24). The build
// writes mods/sound_banks.txt ('<bank> sound/<file>.mlt <bytes>' lines, the boss banks renumbered 8..15, their sound ids
// merged into the stage's AudioData table); right after bank 7 is loaded each of them is loaded the same way (ARAM banks
// 8-10 are sized and 11-15 made at boot by hle_GcaxNewAramBank below). The path strings are the game's own (from the 8053E1B8 table), so no guest memory
// is allocated. SOUND_BANKS_LOG=1 logs each load.
#include "sys.h"
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <windows.h>

void f_801F2770(Cpu&);   // sound manager: load bank r4 for stage r5
void f_801F29D4(Cpu&);   // sound manager: load one bank file (r4 = path)
void f_801F1E24(Cpu&);   // sound manager: after a load (r3 = this + 0x28), as fn_801F2770 does
void f_8043B1AC(Cpu&);   // gcaxNewAramBank(r3 bank 0..15, r4 bytes): carves the bank out of the free ARAM at boot

namespace {
bool ram(uint32_t a) { return a >= 0x80000000u && a < RT_RAM_END; }
std::string gstr(uint32_t a) { std::string s; while (ram(a) && s.size() < 64) { const char ch = (char)ld8(a++); if (!ch) break; s += ch; } return s; }
struct Extra { int bank; std::string file; uint32_t bytes; };
std::vector<Extra> read_list() {
  std::vector<Extra> out;
  std::string s;
  if (const char* e = getenv("SOUND_BANKS")) s = e;   // tests: 'bank file;bank file'
  else {
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string dir = exe;
    dir = dir.substr(0, dir.find_last_of("\\/"));
    std::ifstream f(dir + "\\mods\\sound_banks.txt");
    if (!f) return out;
    std::stringstream ss;
    ss << f.rdbuf();
    s = ss.str();
  }
  std::istringstream in(s);
  std::string line;
  while (std::getline(in, line, line.find(';') != std::string::npos ? ';' : '\n')) {
    std::istringstream l(line);
    int bank = 0;
    std::string file;
    uint32_t bytes = 0;
    if (l >> bank >> file) { l >> bytes; out.push_back({bank, file, bytes}); }
  }
  return out;
}
uint32_t path_ptr(const std::string& file) {   // the game's own string for this file in the stage-bank table
  for (uint32_t a = 0x8053E1B8u; ld32(a) != 0xFFFFFFFFu && a < 0x8053E400u; a += 8)
    if (gstr(ld32(a + 4)) == file) return ld32(a + 4);
  return 0;
}
}  // namespace

void hle_SoundBankLoad(Cpu& c) {
  const uint32_t self = c.r[3], bank = c.r[4];
  f_801F2770(c);
  if (bank != 7) return;
  static const bool log = getenv("SOUND_BANKS_LOG") != nullptr;
  const uint32_t result = c.r[3];
  for (const auto& [b, file, bytes] : read_list()) {
    const uint32_t p = path_ptr(file);
    if (!p) { logf("[sound-banks] %s: not in the game's bank table - skipped\n", file.c_str()); continue; }
    const Cpu keep = c;
    c.r[3] = self; c.r[4] = p; f_801F29D4(c);
    const uint32_t r = c.r[3];
    c.r[3] = self + 0x28; f_801F1E24(c);
    c = keep;
    if (log) logf("[sound-banks] bank %d <- %s (load returned %u)\n", b, file.c_str(), r);
  }
  c.r[3] = result;
}

// ARAM bank sizes are compiled in (files/sound/GCAX.conf is a tool file the game never reads); banks 8 / 9 / 10 get
// 500 bytes, 11-15 none. At boot each bank is made by gcaxNewAramBank(bank, bytes): a bank listed in
// mods/sound_banks.txt gets the size given there, and listed banks 11-15 are made after bank 10 (below). Free ARAM after
// the game's own banks: 2968192 bytes ('AX SDLIB: aramRest'); the build keeps the boss banks inside that budget.
void hle_GcaxNewAramBank(Cpu& c) {
  const int bank = (int)c.r[3];
  const auto list = read_list();
  for (const auto& e : list)
    if (bank == e.bank && e.bytes > c.r[4]) {
      logf("[sound-banks] ARAM bank %d: %u -> %u bytes\n", e.bank, c.r[4], e.bytes);
      c.r[4] = e.bytes;
    }
  static const bool log = getenv("SOUND_BANKS_LOG") != nullptr;
  if (log) logf("[sound-banks] gcaxNewAramBank(%d, %u)\n", bank, c.r[4]);
  f_8043B1AC(c);
}
