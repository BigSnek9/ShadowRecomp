// GameCube hardware register emulation (LLE-style MMIO).
#include "sys.h"
#include "hw.h"
#include "sched.h"
#include "input.h"
#include "settings_ui.h"
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <chrono>
#include <mutex>
#include <ctime>

bool g_trace_mmio = false;
bool g_trace_disc = false;
bool g_skip_movies = false;
bool g_movie_user_skip = false;   // #149: the player pressed START during a movie (settings_ui.cpp, skip_any_cutscene)
uint64_t g_last_movie_read_ns = 0;
// P0 (user: New Game's opening movie skipped by itself): a skip request belongs to the movie playback it was made in.
// g_movie_start_ns names the playback (set when a movie starts streaming); g_movie_skip_for = the playback the START
// press was made in; a request for another playback or older than 2 s is dropped, never applied to the next movie.
uint64_t g_movie_start_ns = 0, g_movie_skip_for = 0, g_movie_skip_ns = 0;
bool g_movie_skip_jumped = false;   // a skipped movie is streaming its tail (settings_ui blacks out the picture)

// ------------------------------------------------------------------ time
// Virtual emulation clock: real time scaled by g_time_scale (fast-forward for testing).
double g_time_scale = 1.0;
// Thread-safe (2026-10-17): helper threads (the lighting occlusion worker, render-worker callbacks) call this too. Two
// threads updating last_real / virt unsynchronised made (real - last_real) negative = +2^64 ns on the virtual clock:
// the guest timebase leapt, the SDK's alarms (the DVD timeout) fired -> frozen frames / the DVD fatal-error screen.
uint64_t sys_time_ns() {
  using namespace std::chrono;
  static std::mutex mtx;
  std::lock_guard<std::mutex> lock(mtx);
  static uint64_t last_real = 0, virt = 0;
  uint64_t real = (uint64_t)duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
  if (last_real && real < last_real) real = last_real;   // never backwards
  if (!last_real) { last_real = real; virt = real; }
  virt += (uint64_t)((double)(real - last_real) * g_time_scale);
  last_real = real;
  return virt;
}
static uint64_t g_t0;
uint64_t sys_timebase() {
  // 40.5 MHz timebase (bus clock / 4)
  uint64_t ns = sys_time_ns() - g_t0;
  return (ns / 1000) * 405 / 10 + ((ns % 1000) * 405 / 10000);
}

// ------------------------------------------------------------------ state
namespace hw {
State S;
}
using hw::S;

static std::vector<uint8_t>& ram_vec();

#define HWLOG(...) do { if (g_trace_mmio) logf(__VA_ARGS__); } while (0)

static uint32_t pi_intsr();

// ---------------------------------------------------------------- PI / MI
static uint32_t pi_read(uint32_t off) {
  switch (off) {
    case 0x00: return pi_intsr();
    case 0x04: return S.pi_intmsk;
    case 0x0C: return S.pi_fifo_base;
    case 0x10: return S.pi_fifo_end;
    case 0x14: return S.pi_fifo_wptr;
    case 0x2C: return 0x246500B1;   // chip revision
    default: return S.pi_misc[(off >> 2) & 0x3F];
  }
}
static void pi_write(uint32_t off, uint32_t v) {
  switch (off) {
    case 0x00:  // clear raw bits
      S.pi_raw &= ~v;
      if (v & (1u << 1)) { /* reset switch */ }
      break;
    case 0x04: S.pi_intmsk = v & 0x3FFF; break;
    case 0x0C: S.pi_fifo_base = v; break;
    case 0x10: S.pi_fifo_end = v; break;
    case 0x14: S.pi_fifo_wptr = v; break;
    case 0x24: HWLOG("PI reset %08X\n", v); break;
    default: S.pi_misc[(off >> 2) & 0x3F] = v;
  }
}

// ---------------------------------------------------------------- VI
static uint16_t vi_read16(uint32_t off) {
  switch (off) {
    case 0x2C: return (uint16_t)S.vi_vcount;
    case 0x2E: return 0;
    default: return S.vi[off >> 1];
  }
}
static void vi_write16(uint32_t off, uint16_t v) {
  S.vi[off >> 1] = v;
  if (off >= 0x30 && off <= 0x3E && !(off & 2)) {
    // interrupt registers: bit 15 of HI half is the status flag, cleared by writing 0
    // (stored as written; status clear is implemented in vi_write16 by masking)
  }
  if (off >= 0x30 && off <= 0x3E && !(off & 2)) {
    // writing 0 to bit 15 clears the pending flag
    if (!(v & 0x8000)) S.vi_int_pending &= ~(1u << ((off - 0x30) >> 2));
  }
  if (off == 0x1C || off == 0x1E || off == 0x24 || off == 0x26) hw::vi_xfb_changed();
}

// ---------------------------------------------------------------- DI
static void di_update() {}
static void di_start(uint32_t cr) {
  uint32_t cmd = S.di_cmd[0];
  uint32_t op = cmd >> 24;
  bool dma = cr & 2;
  uint32_t mar = S.di_mar & 0x1FFFFFFF;
  uint32_t len = S.di_len;
  HWLOG("DI cmd %08X %08X %08X dma=%d mar=%08X len=%X\n", S.di_cmd[0], S.di_cmd[1], S.di_cmd[2], dma, mar, len);
  switch (op) {
    case 0xA8: {  // read
      uint64_t off = (uint64_t)S.di_cmd[1] << 2;
      uint32_t n = S.di_cmd[2];
      if (n > len && dma) n = len;
      if (dma) {
        {   // movie streaming (pre-rendered cutscenes): widescreen shows them pillarboxed (game::cutscene_active)
          std::string nm = disc_describe(off);
          if (nm.size() > 4 && nm.compare(nm.size() - 4, 4, ".sfd") == 0) {
            static std::string s_last;
            if (nm != s_last && sys_time_ns() - g_last_movie_read_ns > 2000000000ull) logf("[movie] streaming %s\n", nm.c_str());
            if (nm != s_last || sys_time_ns() - g_last_movie_read_ns > 2000000000ull) {   // a new playback: no skip carried over
              g_movie_start_ns = sys_time_ns();
              if (g_movie_user_skip) logf("[skip] request from an earlier movie dropped (%s starts)\n", nm.c_str());
              g_movie_user_skip = false;
            }
            if (nm != s_last) s_last = nm;
            g_last_movie_read_ns = sys_time_ns();
          }
        }
        if (g_skip_movies) {
          std::string nm = disc_describe(off);
          // --fast: stop auto-pressing once a stage's geometry archive loads (START would pause the stage)
          extern bool g_mash;
          // FAST_KEEP_EVENTS=1: also stop at the first in-engine cutscene file, so it plays (testing cutscenes)
          static const bool keep_events = getenv("FAST_KEEP_EVENTS") != nullptr;
          if (g_mash && keep_events && nm.find("event") != std::string::npos) {
            g_mash = false;
            logf("[fast] cutscene load (%s): auto-input off\n", nm.c_str());
          }
          if (g_mash && nm.size() > 8 && nm.compare(nm.size() - 8, 8, "_dat.one") == 0) {
            g_mash = false;
            logf("[fast] stage load (%s): auto-input off\n", nm.c_str());
          }
        }
        if (g_trace_disc) { Cpu* cc = sched_current_cpu(); if (cc) sys_backtrace(*cc); }
        if (g_trace_disc) logf("[disc] read %s +%llX len %X -> %08X\n", disc_describe(off).c_str(), (unsigned long long)(off), n, mar);
        rt_watch_dma("DI", mar, n, off);
        // --skip-movies: story movies can't be skipped by a button press (only already-seen ones), so after the
        // header region jump the stream to the file's tail; Sofdec then hits end-of-stream within a second.
        uint64_t fs = 0, fl = 0;
        static uint64_t s_skip_file = ~0ull, s_skip_delta = 0;
        bool zero = false;
        uint64_t rd = off;
        // #149 "Skip any cutscene": START (Tab) during a movie (settings_ui.cpp sets g_movie_user_skip) skips that movie
        // the same way, also the first time
        static uint64_t s_user_skip_file = ~1ull;
        static uint64_t s_skip_playback = 0;   // the skip state belongs to one playback: a replay of the same movie plays again
        if (s_skip_playback != g_movie_start_ns) { s_skip_playback = g_movie_start_ns; s_user_skip_file = ~1ull; s_skip_file = ~0ull; }
        const bool seg = disc_seg_range(off, &fs, &fl);
        if (g_movie_user_skip && (g_movie_skip_for != g_movie_start_ns || sys_time_ns() - g_movie_skip_ns > 2000000000ull)) {
          g_movie_user_skip = false;   // made in another playback, or never matched a read of it: stale
          logf("[skip] stale movie skip request dropped\n");
        }
        if (seg && g_movie_user_skip && s_user_skip_file != fs) {
          std::string nm = disc_describe(off);
          if (nm.size() > 4 && nm.compare(nm.size() - 4, 4, ".sfd") == 0) {
            s_user_skip_file = fs;
            g_movie_user_skip = false;
            logf("[skip] movie %s skipped (skip-any option)\n", nm.c_str());
          }
        }
        static const bool model_log = getenv("MODEL_LOG") != nullptr;   // tests (#156): when Shadow's model archive is read
        if (model_log && seg) {
          std::string nm = disc_describe(off);
          if (nm.find("shadow.one") != std::string::npos) logf("[model] read %s +%llX len %X\n", nm.c_str(), (unsigned long long)(off - fs), n);
        }
        static const bool movie_log = getenv("MOVIE_LOG") != nullptr;   // tests: every movie read (offset in the file)
        if (movie_log && seg && g_last_movie_read_ns && sys_time_ns() - g_last_movie_read_ns < 1000000ull)
          logf("[movie] read +%llX of %llX len %X%s\n", (unsigned long long)(off - fs), (unsigned long long)fl, n, s_user_skip_file == fs ? " (skip)" : "");
        if ((g_skip_movies || s_user_skip_file == fs) && seg) {
          std::string nm = disc_describe(off);
          bool sfd = nm.size() > 4 && nm.compare(nm.size() - 4, 4, ".sfd") == 0;
          if (sfd && fl > 0x400000 && off - fs >= 0x100000) {
            if (s_skip_file != fs) {
              s_skip_file = fs;
              s_skip_delta = (fs + fl - 0x80000) > off ? (fs + fl - 0x80000) - off : 0;
              logf("[skip] movie %s: jumping stream +%llX to tail\n", nm.c_str(), (unsigned long long)s_skip_delta);
              g_movie_skip_jumped = true;   // the decoder shows garbage until it resyncs: the screen is blacked out
            }
            rd = off + s_skip_delta;
            if (rd >= fs + fl) zero = true;
            else if (rd + n > fs + fl) { memset(g_ram + (mar & RT_RAM_MASK), 0, n); n = (uint32_t)(fs + fl - rd); }
          }
        }
        if (zero) memset(g_ram + (mar & RT_RAM_MASK), 0, n);
        else disc_read(rd, g_ram + (mar & RT_RAM_MASK), n);
        S.di_len = 0;
      } else {
        S.di_imm = 0;
      }
      break;
    }
    case 0x12: {  // inquiry
      if (dma) {
        uint8_t id[0x20] = {0};
        id[2] = 0x00; id[3] = 0x00;
        memcpy(g_ram + (mar & RT_RAM_MASK), id, std::min<uint32_t>(len, 0x20));
      }
      break;
    }
    case 0xE0: S.di_imm = 0; break;           // request error: no error
    case 0xE1: case 0xE2: case 0xE4: S.di_imm = 0; break;   // audio stream
    case 0xE3: S.di_imm = 0; break;           // stop motor
    case 0xAB: S.di_imm = 0; break;           // seek
    case 0x8B: break;                         // reset
    case 0xA9: break;
    default:
      HWLOG("DI: unhandled command %08X\n", cmd);
      break;
  }
  if (dma) S.di_len = 0;       // remaining length
  S.di_cr = 0;                 // transfer complete
  S.di_sr |= (1u << 4);        // TCINT
  S.di_pending_irq = true;
}
static uint32_t di_read_(uint32_t off);
static uint32_t di_read(uint32_t off) {
  uint32_t v = di_read_(off);
  HWLOG("DI rd %02X -> %08X\n", off, v);
  return v;
}
static uint32_t di_read_(uint32_t off) {
  switch (off) {
    case 0x00: return S.di_sr;
    case 0x04: return S.di_cvr;
    case 0x08: return S.di_cmd[0];
    case 0x0C: return S.di_cmd[1];
    case 0x10: return S.di_cmd[2];
    case 0x14: return S.di_mar;
    case 0x18: return S.di_len;
    case 0x1C: return S.di_cr;
    case 0x20: return S.di_imm;
    case 0x24: return 0;
  }
  return 0;
}
static void di_write(uint32_t off, uint32_t v) {
  HWLOG("DI wr %02X <- %08X\n", off, v);
  switch (off) {
    case 0x00: {
      uint32_t clr = v & ((1u << 2) | (1u << 4) | (1u << 6));
      S.di_sr &= ~clr;
      S.di_sr = (S.di_sr & ~((1u << 1) | (1u << 3) | (1u << 5))) | (v & ((1u << 1) | (1u << 3) | (1u << 5)));
      S.di_sr = (S.di_sr & ~1u) | (v & 1u);
      break;
    }
    case 0x04: {
      uint32_t clr = v & 2;  // CVRINT write-1-clear
      S.di_cvr &= ~clr;
      S.di_cvr = (S.di_cvr & ~4u) | (v & 4u);
      break;
    }
    case 0x08: S.di_cmd[0] = v; break;
    case 0x0C: S.di_cmd[1] = v; break;
    case 0x10: S.di_cmd[2] = v; break;
    case 0x14: S.di_mar = v; break;
    case 0x18: S.di_len = v; break;
    case 0x1C:
      S.di_cr = v & 7;
      if (v & 1) di_start(v);
      break;
    case 0x20: S.di_imm = v; break;
  }
}
static bool di_irq() {
  return ((S.di_sr & (1u << 4)) && (S.di_sr & (1u << 3))) || ((S.di_sr & (1u << 2)) && (S.di_sr & (1u << 1))) ||
         ((S.di_sr & (1u << 6)) && (S.di_sr & (1u << 5))) || ((S.di_cvr & 2) && (S.di_cvr & 4));
}

// ---------------------------------------------------------------- EXI
static uint8_t g_sram[64];
static void sram_init() {
  memset(g_sram, 0, sizeof g_sram);
  g_sram[0x13] = 0x04 | 0x00;     // flags: stereo, NTSC (video mode 0)
  g_sram[0x12] = 0;               // language: English
  // OSSramEx.flashIDCheckSum[2] (0x3A/0x3B) = ~sum(flashID[chan]); flash IDs are zero -> 0xFF. CARD mount checks this.
  g_sram[0x3A] = 0xFF;
  g_sram[0x3B] = 0xFF;
  uint16_t c1 = 0, c2 = 0;
  for (int i = 0x0C; i < 0x14; i += 2) {
    uint16_t w = (uint16_t)((g_sram[i] << 8) | g_sram[i + 1]);
    c1 = (uint16_t)(c1 + w);
    c2 = (uint16_t)(c2 + (uint16_t)~w);
  }
  g_sram[0] = c1 >> 8; g_sram[1] = c1 & 0xFF; g_sram[2] = c2 >> 8; g_sram[3] = c2 & 0xFF;
}

static uint32_t rtc_now() {
  // GC RTC counts seconds since 2000-01-01
  std::time_t t = std::time(nullptr);
  return (uint32_t)(t - 946684800);
}

struct ExiChan {
  uint32_t csr = 0, mar = 0, len = 0, cr = 0, data = 0;
  int dev = -1;                // selected device (CS bits)
  uint32_t cmd = 0;            // latched device command
  bool have_cmd = false;
  uint32_t addr = 0;           // current device address
  bool write = false;
};
static ExiChan g_exi[3];

static int exi_cs(uint32_t csr) {
  uint32_t cs = (csr >> 7) & 7;
  if (!cs) return -1;
  return cs == 1 ? 0 : cs == 2 ? 1 : 2;
}

static uint32_t exi_dev_read_byte(ExiChan& ch, int chn, int dev) {
  if (chn == 0 && dev == 1) {   // RTC / SRAM
    if ((ch.cmd & 0x7FFFFF00u) == 0x20000000u && !(ch.cmd & 0x100)) {
      uint32_t v = rtc_now();
      uint32_t b = (v >> (24 - 8 * (ch.addr & 3))) & 0xFF;
      ch.addr++;
      return b;
    }
    if ((ch.cmd & 0x7FFFFF00u) == 0x20000100u) {
      uint32_t a = ch.addr++;
      return a < 64 ? g_sram[a] : 0;
    }
  }
  return 0;
}

namespace exicard { void select(); uint8_t transfer(uint8_t in); bool deselect(); }

static void exi_xfer(ExiChan& ch, int chn, uint32_t cr) {
  int dev = exi_cs(ch.csr);
  bool dma = cr & 2;
  uint32_t rw = (cr >> 2) & 3;      // 0 read, 1 write
  uint32_t n = dma ? ch.len : (((cr >> 4) & 3) + 1);
  if (dev < 0) { HWLOG("EXI%d xfer with no device\n", chn); return; }

  if (chn == 0 && dev == 0) {       // memory card in slot A: byte stream (see exi_card.cpp)
    if (dma) {
      uint8_t* p = g_ram + (ch.mar & RT_RAM_MASK);
      for (uint32_t i = 0; i < n; ++i) {
        uint8_t out = exicard::transfer(rw == 1 ? p[i] : 0);
        if (rw == 0) p[i] = out;
      }
    } else {
      uint32_t v = 0;
      for (uint32_t i = 0; i < n; ++i) {
        uint8_t in = rw == 1 ? (uint8_t)(ch.data >> (24 - 8 * i)) : 0;
        v = (v << 8) | exicard::transfer(in);
      }
      if (rw == 0) ch.data = v << (8 * (4 - n));
    }
    return;
  }

  if (!dma && rw == 1 && !ch.have_cmd) {
    // first immediate write after select = command word
    uint32_t v = ch.data;
    ch.cmd = v;
    ch.have_cmd = true;
    ch.addr = 0;
    if (chn == 0 && dev == 1) {
      if ((v & 0x7FFFFF00u) == 0x20000100u) ch.addr = 0;               // SRAM read
      else if ((v & 0x3FFFFF00u) == 0x20000000u) ch.addr = 0;
      ch.write = (v & 0x80000000u) != 0;
      uint32_t a = ((v & 0x3FFFFF00u) >> 6);
      if ((v & 0x7FFFFF00u) == 0x20000100u || (v & 0x7FFFFF00u) == 0x20000000u) { /* read */ }
      (void)a;
    }
    HWLOG("EXI%d dev%d cmd %08X\n", chn, dev, v);
    return;
  }
  // memory card / other devices present? ID probe handled: no device -> zeros
  if (rw == 0) {
    if (dma) {
      uint8_t* dst = g_ram + (ch.mar & RT_RAM_MASK);
      for (uint32_t i = 0; i < n; ++i) dst[i] = (uint8_t)exi_dev_read_byte(ch, chn, dev);
    } else {
      uint32_t v = 0;
      for (uint32_t i = 0; i < n; ++i) v = (v << 8) | exi_dev_read_byte(ch, chn, dev);
      ch.data = v << (8 * (4 - n));
    }
  } else {
    // data writes to device: SRAM writes ignored (apart from sync)
    HWLOG("EXI%d dev%d write len %u data %08X\n", chn, dev, n, ch.data);
  }
}

static uint32_t exi_read(uint32_t off) {
  int chn = off / 0x14;
  if (chn > 2) return 0;
  ExiChan& ch = g_exi[chn];
  switch (off % 0x14) {
    case 0x00: {
      uint32_t v = ch.csr;
      // EXT (bit 12): device inserted. Slot A (channel 0) has the memory card; slot B is empty.
      if (chn == 0) v |= (1u << 12);
      return v;
    }
    case 0x04: return ch.mar;
    case 0x08: return ch.len;
    case 0x0C: return ch.cr;
    case 0x10: return ch.data;
  }
  return 0;
}
static void exi_write(uint32_t off, uint32_t v) {
  int chn = off / 0x14;
  if (chn > 2) return;
  ExiChan& ch = g_exi[chn];
  switch (off % 0x14) {
    case 0x00: {
      uint32_t clr = v & ((1u << 1) | (1u << 3) | (1u << 11));
      uint32_t status = ch.csr & ((1u << 1) | (1u << 3) | (1u << 11));
      status &= ~clr;
      uint32_t old_cs = (ch.csr >> 7) & 7;
      ch.csr = (v & ~((1u << 1) | (1u << 3) | (1u << 11) | (1u << 12))) | status;
      uint32_t new_cs = (ch.csr >> 7) & 7;
      if (new_cs != old_cs) {
        ch.have_cmd = false; ch.cmd = 0; ch.addr = 0;
        if (chn == 0 && old_cs == 1 && exicard::deselect()) ch.csr |= (1u << 1);   // card EXI interrupt
        if (chn == 0 && new_cs == 1) exicard::select();
      }
      break;
    }
    case 0x04: ch.mar = v; break;
    case 0x08: ch.len = v; break;
    case 0x0C:
      ch.cr = v & 0x3F;
      if (v & 1) {
        exi_xfer(ch, chn, v);
        ch.cr &= ~1u;
        ch.csr |= (1u << 3);   // TCINT
      }
      break;
    case 0x10: ch.data = v; break;
  }
}
static bool exi_irq() {
  for (int i = 0; i < 3; ++i) {
    uint32_t c = g_exi[i].csr;
    if (((c >> 3) & 1) && ((c >> 2) & 1)) return true;   // TC
    if (((c >> 1) & 1) && (c & 1)) return true;           // EXI
    if (((c >> 11) & 1) && ((c >> 10) & 1)) return true;  // EXT
  }
  return false;
}

// ---------------------------------------------------------------- SI / AI / DSP / MI / CP / PE
static uint32_t si_read(uint32_t off) {
  if (off >= 0x80) { return hw::si_buf_read(off - 0x80); }
  return hw::si_reg_read(off);
}
static void si_write(uint32_t off, uint32_t v) {
  if (off >= 0x80) { hw::si_buf_write(off - 0x80, v); return; }
  hw::si_reg_write(off, v);
}

// ---------------------------------------------------------------- dispatch
static std::vector<uint8_t> g_unknown_seen;
static void log_unknown(const char* what, uint32_t ea, uint32_t v, int size, bool wr) {
  static int count = 0;
  if (count++ < 400) logf("[hw] %s %s %08X size %d val %08X\n", wr ? "WR" : "RD", what, ea, size, v);
}

static uint32_t reg_read(uint32_t pa, int size) {
  uint32_t reg = pa & 0xFFFF;
  uint32_t blk = reg & 0xFC00;
  uint32_t shift = 0;
  uint32_t v = 0;
  bool wide = size == 4;
  switch (reg >> 12) {
    case 0x0:  // CP
      v = hw::cp_read16(reg & 0xFFF); if (wide) v = (v << 16) | hw::cp_read16((reg & 0xFFF) + 2); return v;
    case 0x1:  // PE
      v = hw::pe_read16(reg & 0xFFF); if (wide) v = (v << 16) | hw::pe_read16((reg & 0xFFF) + 2); return v;
    case 0x2:  // VI
      if (size == 4) return ((uint32_t)vi_read16(reg & 0xFF) << 16) | vi_read16((reg & 0xFF) + 2);
      return vi_read16(reg & 0xFF);
    case 0x3: {  // PI / MI is at 0x4000
      return pi_read(reg & 0xFF);
    }
    case 0x4:  // MI
      return hw::mi_read16(reg & 0xFFF) | (wide ? 0 : 0);
    case 0x5:  // DSP / AR / AI-DMA
      v = hw::dsp_read16(reg & 0xFFF);
      if (wide) v = (v << 16) | hw::dsp_read16((reg & 0xFFF) + 2);
      return v;
    case 0x6:
      switch (reg & 0xFC00) {
        case 0x6000: return di_read(reg & 0xFF);
        case 0x6400: return si_read(reg & 0x1FF);
        case 0x6800: return exi_read(reg & 0xFF);
        case 0x6C00: return hw::ai_read(reg & 0xFF);
      }
      break;
  }
  log_unknown("reg", pa, 0, size, false);
  return 0;
}

static void reg_write(uint32_t pa, uint32_t v, int size) {
  uint32_t reg = pa & 0xFFFF;
  switch (reg >> 12) {
    case 0x0:
      if (size == 4) { hw::cp_write16(reg & 0xFFF, v >> 16); hw::cp_write16((reg & 0xFFF) + 2, v & 0xFFFF); }
      else hw::cp_write16(reg & 0xFFF, v);
      return;
    case 0x1:
      if (size == 4) { hw::pe_write16(reg & 0xFFF, v >> 16); hw::pe_write16((reg & 0xFFF) + 2, v & 0xFFFF); }
      else hw::pe_write16(reg & 0xFFF, v);
      return;
    case 0x2:
      if (size == 4) { vi_write16(reg & 0xFF, v >> 16); vi_write16((reg & 0xFF) + 2, v & 0xFFFF); }
      else vi_write16(reg & 0xFF, v);
      return;
    case 0x3:
      pi_write(reg & 0xFF, v);
      return;
    case 0x4:
      hw::mi_write16(reg & 0xFFF, v);
      return;
    case 0x5:
      if (size == 4) { hw::dsp_write16(reg & 0xFFF, v >> 16); hw::dsp_write16((reg & 0xFFF) + 2, v & 0xFFFF); }
      else hw::dsp_write16(reg & 0xFFF, v);
      return;
    case 0x6:
      switch (reg & 0xFC00) {
        case 0x6000: di_write(reg & 0xFF, v); return;
        case 0x6400: si_write(reg & 0x1FF, v); return;
        case 0x6800: exi_write(reg & 0xFF, v); return;
        case 0x6C00: hw::ai_write(reg & 0xFF, v); return;
      }
      break;
    case 0x8:  // write-gather pipe is at 0xCC008000
      hw_wgp_write(v, size);
      return;
  }
  log_unknown("reg", pa, v, size, true);
}

uint8_t g_lc[16384];
static uint8_t g_aram_dummy;

uint32_t mmio_read(uint32_t ea, int size) {
  // low physical RAM (translation off in exception vectors)
  if (ea < RT_RAM_SIZE || RT_IN_RAM(ea)) {
    uint32_t o = ea & RT_RAM_MASK;
    uint32_t v = 0;
    for (int i = 0; i < size; ++i) v = (v << 8) | g_ram[o + i];
    return v;
  }
  if (ea >= 0xE0000000u && ea < 0xE0004000u) {
    uint32_t o = ea & 0x3FFF;
    uint32_t v = 0;
    for (int i = 0; i < size; ++i) v = (v << 8) | g_lc[(o + i) & 0x3FFF];
    return v;
  }
  if ((ea & 0xFFFF0000u) == 0xCC000000u || (ea & 0xFFFF0000u) == 0x0C000000u) {
    return reg_read(ea, size);
  }
  if ((ea & 0xFF000000u) == 0xC8000000u || (ea & 0xFF000000u) == 0x08000000u) {
    return hw::efb_peek(ea, size);
  }
  static int cnt = 0;
  if (cnt++ < 50) logf("[mem] unmapped read %08X size %d\n", ea, size);
  return 0;
}

void mmio_write(uint32_t ea, uint32_t v, int size) {
  if ((ea & 0xFFFFF000u) == 0xCC008000u) { hw_wgp_write(v, size); return; }   // #152: the GX FIFO first (most MMIO writes)
  if (ea < RT_RAM_SIZE || RT_IN_RAM(ea)) {
    uint32_t o = ea & RT_RAM_MASK;
    for (int i = 0; i < size; ++i) g_ram[o + i] = (uint8_t)(v >> (8 * (size - 1 - i)));
    return;
  }
  if (ea >= 0xE0000000u && ea < 0xE0004000u) {
    uint32_t o = ea & 0x3FFF;
    for (int i = 0; i < size; ++i) g_lc[(o + i) & 0x3FFF] = (uint8_t)(v >> (8 * (size - 1 - i)));
    return;
  }
  if ((ea & 0xFFFF0000u) == 0xCC000000u || (ea & 0xFFFF0000u) == 0x0C000000u) {
    reg_write(ea, v, size);
    return;
  }
  if ((ea & 0xFF000000u) == 0xC8000000u || (ea & 0xFF000000u) == 0x08000000u) {
    hw::efb_poke(ea, v, size);
    return;
  }
  static int cnt = 0;
  if (cnt++ < 50) logf("[mem] unmapped write %08X size %d val %08X\n", ea, size, v);
}

uint64_t mem_read64_slow(uint32_t ea) {
  uint64_t hi = mmio_read(ea, 4), lo = mmio_read(ea + 4, 4);
  return (hi << 32) | lo;
}
void mem_write64_slow(uint32_t ea, uint64_t v) {
  if ((ea & 0xFFFFF000u) == 0xCC008000u) { hw_wgp_write((uint32_t)(v >> 32), 4); hw_wgp_write((uint32_t)v, 4); return; }
  mmio_write(ea, (uint32_t)(v >> 32), 4);
  mmio_write(ea + 4, (uint32_t)v, 4);
}

void dcbz(uint32_t ea) {
  ea &= ~31u;
  if (RT_IN_RAM(ea)) { memset(g_ram + RT_OFF(ea), 0, 32); return; }
  if (ea >= 0xE0000000u && ea < 0xE0004000u) { memset(g_lc + (ea & 0x3FFF), 0, 32); return; }
  if (ea < RT_RAM_SIZE) { memset(g_ram + ea, 0, 32); return; }
}
void rt_dcbi(uint32_t) {}

// ---------------------------------------------------------------- interrupt aggregation
static uint32_t pi_intsr() {
  uint32_t v = S.pi_raw;
  if (di_irq()) v |= (1u << 2);
  if (hw::si_irq()) v |= (1u << 3);
  if (exi_irq()) v |= (1u << 4);
  if (hw::ai_irq()) v |= (1u << 5);
  if (hw::dsp_irq()) v |= (1u << 6);
  if (hw::mi_irq()) v |= (1u << 7);
  if (S.vi_int_pending) {
    for (int i = 0; i < 4; ++i)
      if ((S.vi_int_pending & (1u << i)) && (S.vi[(0x30 >> 1) + 2 * i] & 0x1000)) { v |= (1u << 8); }
  }
  v |= hw::cp_pe_intsr();
  return v;
}
uint32_t hw_irq_status() { return pi_intsr() & S.pi_intmsk; }
bool hw_irq_pending() { return hw_irq_status() != 0; }
void hw_raise_pi(uint32_t bit) { S.pi_raw |= bit; }

// ---------------------------------------------------------------- VI timing & ticks
static uint64_t g_next_vsync_ns;
static const uint64_t kNativeFrameNs = 16683333;   // 59.94 Hz
// Retrace period. The game's main loop waits for a retrace as well as for its own 1/rate pacing (fn_801E1BD4), so a
// frame-rate target above 60 needs the retraces to come faster too (what Dolphin's "VBI frequency override" does).
static uint64_t kFrameNs = kNativeFrameNs;
void hw_set_vi_rate(double hz) { kFrameNs = hz > 0 ? (uint64_t)(1e9 / hz) : kNativeFrameNs; }

void hw_init() {
  g_t0 = sys_time_ns();
  sram_init();
  memset(&S, 0, sizeof S);
  S.pi_intmsk = 0;
  S.di_cvr = 0;           // cover closed
  g_next_vsync_ns = sys_time_ns() + kFrameNs;
  hw::dsp_init();
  hw::si_init();
  hw::gx_init();
}

void hw_tick(Cpu& c) {
  uint64_t now = sys_time_ns();
  if (now >= g_next_vsync_ns) {
    // one retrace
    int lag = (int)((now - g_next_vsync_ns) / kFrameNs);
    g_next_vsync_ns += kFrameNs * (1 + (lag > 3 ? lag : 0));
    for (int i = 0; i < 4; ++i)
      if (S.vi[(0x30 >> 1) + 2 * i] & 0x1000) {
        S.vi[(0x30 >> 1) + 2 * i] |= 0x8000;
        S.vi_int_pending |= (1u << i);
      }
    S.vi_frame++;
    hw::vsync();
  }
  // beam position
  uint64_t into = kFrameNs - (g_next_vsync_ns > now ? g_next_vsync_ns - now : 0);
  S.vi_vcount = 1 + (uint32_t)(into * 262 / kFrameNs);
  hw::tick_devices(now);
}
