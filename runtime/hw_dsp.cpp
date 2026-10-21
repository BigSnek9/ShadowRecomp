// DSP mailbox / ARAM / AI register emulation.
// The DSP is high-level emulated: only the boot handshake and ARAM DMA live here;
// the audio microcode itself is handled in audio.cpp (later).
#include "sys.h"
#include "hw.h"
#include <cstring>
#include <cstdlib>
#include <vector>
#include <algorithm>

namespace hw {
bool g_trace_dsp = false;
static uint16_t g_dsp[0x80];
static uint32_t g_ai_cr, g_ai_vol, g_ai_cnt, g_ai_inttim;
uint8_t* g_aram = nullptr;
static const uint32_t kAramSize = 16 * 1024 * 1024;

// mailboxes
static uint32_t g_mail_to_dsp = 0;      // bit31 = valid (not yet read by DSP)
static uint32_t g_mail_from_dsp = 0;    // bit31 = valid (not yet read by CPU)

void dsp_post_mail(uint32_t m) { g_mail_from_dsp = m | 0x80000000u; }
uint32_t dsp_take_cpu_mail(bool* valid) {
  *valid = (g_mail_to_dsp & 0x80000000u) != 0;
  uint32_t v = g_mail_to_dsp & 0x7FFFFFFFu;
  g_mail_to_dsp = 0;
  return v;
}

void dsp_init() {
  memset(g_dsp, 0, sizeof g_dsp);
  g_ai_cr = 0;
  if (!g_aram) g_aram = (uint8_t*)calloc(1, kAramSize + 64);
}

// ---- AI DMA: streams 32-byte blocks (8 stereo frames) to the DAC at 32 or 48 kHz ----------------
static bool g_ai_dma_active = false;
static uint64_t g_ai_dma_end_ns = 0;
static uint32_t g_ai_dma_blocks_left = 0;
static uint64_t ai_dma_period_ns() {
  uint32_t blocks = g_dsp[0x1B] & 0x7FFF;
  uint64_t rate = (g_ai_cr & 0x40) ? 32000 : 48000;      // AICR bit 6: DSP sample rate (set = 32 kHz)
  return (uint64_t)blocks * 8 * 1000000000ull / rate;
}
static void ai_dma_start() {
  g_ai_dma_active = true;
  g_ai_dma_blocks_left = g_dsp[0x1B] & 0x7FFF;
  g_ai_dma_end_ns = sys_time_ns() + ai_dma_period_ns();
}
// The game answers every AI interrupt by programming the next buffer (AX: AIInitDMA -> DMA address regs 0x18/0x19)
// and mixing the one after. With a strict wall clock (AI_WALLCLOCK=1) the old buffer was replayed whenever that
// answer came late: the guest shares one host thread with GX translation + Aurora, which run in multi-millisecond
// chunks, so at very high frame rates (Select Mode ~900 fps / ~340 fps unlimited) 16% of the output was repeated
// 5 ms blocks ("slowdown and crunch"), and heavy scenes above 60 fps get some too ("wavy").
// Now the host audio queue drives it: the next AI interrupt comes as soon as the game has answered the last one and
// the queue is below its target. The game mixes a little ahead; delays of a few ms are absorbed by the queue instead
// of repeating or dropping audio. The device consumes at exactly 32 kHz, so speed and pitch stay exact.
// If the game stops answering for 40 ms (it paused its audio), the clock carries on as before.
static bool g_ai_reprog = true;      // the DMA address was written since the last AI interrupt
static uint64_t g_ai_stall_ns = 0;   // a block is wanted but the game has not answered since then
static int g_ai_stalls = 0, g_ai_stall_max_ms = 0, g_ai_forced = 0;
static void ai_dma_block_done(uint64_t now) {
  // buffer finished: raise AIDINT, immediately restart with whatever the callback programs
  audio_submit_dma(((uint32_t)g_dsp[0x18] << 16 | g_dsp[0x19]) & 0x03FFFFE0, (g_dsp[0x1B] & 0x7FFF) * 32);
  g_dsp[0x05] |= (1u << 3);
  g_ai_reprog = false;
  static const bool stats = getenv("AUDIO_STATS") != nullptr;
  static uint64_t t0 = now;
  if (stats && now - t0 > 2000000000ull) {
    logf("[ai-clock] %d late answers (longest %d ms, %d gave up), queue %d ms\n", g_ai_stalls, g_ai_stall_max_ms, g_ai_forced,
         audio_queue_ms());
    g_ai_stalls = g_ai_stall_max_ms = g_ai_forced = 0;
    t0 = now;
  }
}
// The game is behind on audio right now: a block is wanted and its answer to the last AI interrupt is pending
// (but not given up on). frame_rate.cpp then lets the sound threads run before starting the next frame.
bool ai_audio_behind(uint64_t now) {
  return g_ai_dma_active && g_ai_stall_ns && now - g_ai_stall_ns < 40000000ull;
}
void ai_dma_tick(uint64_t now) {
  if (!g_ai_dma_active) return;
  static const bool wallclock = getenv("AI_WALLCLOCK") != nullptr;
  const int q = wallclock ? -1 : audio_queue_ms();
  if (q >= 0) {
    // #161: 120 ms (was 70): heavy frames (explosions, loads at 4K / 240 fps) kept the game's answer late longer than
    // the cushion lasted -> gaps. Latency +50 ms, pitch / speed unchanged (the device plays exactly 32 kHz).
    constexpr int kTargetMs = 120;
    {   // never latch silent: if the queue has not drained for 1 s while it is full, the device stopped consuming
      static int last_q = -1;
      static uint64_t still_since = 0;
      if (q >= kTargetMs && q >= last_q) { if (!still_since) still_since = now; }
      else still_since = 0;
      last_q = q;
      if (still_since && now - still_since > 1000000000ull && !g_quiet) {
        extern void audio_recover_stalled_device();
        audio_recover_stalled_device();
        still_since = 0;
      }
    }
    if (q >= kTargetMs) return;   // enough queued: the device is playing it
    if (!g_ai_reprog) {           // a block is wanted but the game has not answered the last interrupt yet
      if (!g_ai_stall_ns) { g_ai_stall_ns = now; ++g_ai_stalls; }
      if (now - g_ai_stall_ns < 40000000ull) return;
      ++g_ai_forced;
    }
    if (g_ai_stall_ns) {
      g_ai_stall_max_ms = std::max(g_ai_stall_max_ms, (int)((now - g_ai_stall_ns) / 1000000));
      g_ai_stall_ns = 0;
    }
    ai_dma_block_done(now);
    g_ai_dma_end_ns = now + ai_dma_period_ns();   // (ai_dma_remaining)
    return;
  }
  // no host queue to go by (device not open) or AI_WALLCLOCK: real-time blocks
  if (now < g_ai_dma_end_ns) return;
  ai_dma_block_done(now);
  g_ai_dma_end_ns += ai_dma_period_ns();
  if (g_ai_dma_end_ns < now) g_ai_dma_end_ns = now + ai_dma_period_ns();
}
uint16_t ai_dma_remaining() {
  if (!g_ai_dma_active) return 0;
  uint64_t now = sys_time_ns();
  uint64_t per = ai_dma_period_ns();
  if (now >= g_ai_dma_end_ns || per == 0) return 0;
  return (uint16_t)((g_ai_dma_end_ns - now) * (g_dsp[0x1B] & 0x7FFF) / per);
}

// ---- DSP ROM / ucode mail protocol (high level) --------------------------------
enum UcodeKind { UC_ROM, UC_AX };
static UcodeKind g_uc = UC_ROM;
static uint32_t g_rom_pending = 0;
static uint32_t g_rom_iram_mmem = 0, g_rom_iram_len = 0, g_rom_iram_dsp = 0;

static void dsp_post_mail_irq(uint32_t m) {
  dsp_post_mail(m);
  g_dsp[0x05] |= (1u << 7);          // DSPINT: the ucode raises an interrupt after sending
}

static void dsp_reset() {
  g_mail_to_dsp = 0;
  g_mail_from_dsp = 0;
  g_uc = UC_ROM;
  g_rom_pending = 0;
  dsp_post_mail(0x8071FEED);         // IROM: ready for a ucode upload
}

static bool g_next_is_cmdlist = false;
static uint32_t g_cmdlist_size = 0;

void dsp_ucode_booted(uint32_t, uint32_t) {
  g_next_is_cmdlist = false;
  dsp_post_mail_irq(0xDCD10000u);          // DSP_INIT
}

static int g_upload_mails = 0;

void dsp_ucode_mail(uint32_t m) {
  if (g_upload_mails > 0) {
    if (--g_upload_mails == 0) dsp_post_mail_irq(0xDCD10000u);     // new task booted: DSP_INIT
    return;
  }
  if (g_next_is_cmdlist) {
    g_next_is_cmdlist = false;
    audio_process_cmdlist(m, g_cmdlist_size);
    dsp_post_mail_irq(0xDCD10002u);         // DSP_YIELD: work finished
    return;
  }
  uint32_t hi = (m >> 16) & 0x7FFF;
  if (hi == 0x3ABE) {                       // 0xBABExxxx: a command list follows
    g_cmdlist_size = m & 0xFFFF;
    g_next_is_cmdlist = true;
  } else if (hi == 0x4DD1) {                // 0xCDD1xxxx: task control
    switch (m & 0xFFFF) {
      case 0: dsp_post_mail_irq(0xDCD10001u); break;   // MAIL_RESUME -> DSP_RESUME ack
      case 1: g_upload_mails = 8; break;                // MAIL_NEW_UCODE: eight parameter mails follow
      case 2: g_uc = UC_ROM; dsp_post_mail(0x8071FEED); break;   // MAIL_RESET
      case 3: break;                                    // MAIL_CONTINUE
      default: break;
    }
  } else if (g_trace_dsp) {
    logf("[dsp] unhandled ucode mail %08X\n", m);
  }
}

void dsp_cpu_mail_written() {
  bool valid;
  uint32_t m = dsp_take_cpu_mail(&valid);
  if (!valid) return;
  if (g_trace_dsp) logf("[dsp] cpu->dsp mail %08X (%s)\n", m, g_uc == UC_ROM ? "rom" : "ucode");
  if (g_uc == UC_ROM) {
    if (g_rom_pending) {
      uint32_t cmd = g_rom_pending;
      g_rom_pending = 0;
      switch (cmd) {
        case 0xA001: g_rom_iram_mmem = m; break;
        case 0xC002: g_rom_iram_dsp = m; break;
        case 0xA002: g_rom_iram_len = m; break;
        case 0xB002: break;
        case 0xD001:
          g_uc = UC_AX;
          if (g_trace_dsp) logf("[dsp] boot ucode: iram_mmem=%08X len=%X dsp_iram=%X start=%X\n", g_rom_iram_mmem,
                                g_rom_iram_len, g_rom_iram_dsp, m);
          dsp_ucode_booted(g_rom_iram_mmem, g_rom_iram_len);
          break;
        default: break;
      }
    } else if ((m & 0x7FFF0000u) == 0x00F30000u) {
      g_rom_pending = m & 0xFFFF;
    }
  } else {
    dsp_ucode_mail(m);
  }
}

static void aram_dma() {
  uint32_t mm = (((uint32_t)g_dsp[0x10] << 16) | g_dsp[0x11]) & 0x03FFFFFF;
  uint32_t ar = (((uint32_t)g_dsp[0x12] << 16) | g_dsp[0x13]) & 0x03FFFFFF;
  uint32_t cnt = ((uint32_t)g_dsp[0x14] << 16) | g_dsp[0x15];
  bool to_main = cnt & 0x80000000u;
  uint32_t len = cnt & 0x7FFFFFFFu;
  if (ar + len > kAramSize) len = ar < kAramSize ? kAramSize - ar : 0;
  if (to_main) { rt_watch_dma("ARAM->MEM", mm, len, ar); memcpy(g_ram + (mm & RT_RAM_MASK), g_aram + ar, len); }
  else memcpy(g_aram + ar, g_ram + (mm & RT_RAM_MASK), len);
  g_dsp[0x14] = 0; g_dsp[0x15] = 0;
  g_dsp[0x05] |= (1u << 5);   // ARINT
}

uint16_t dsp_read16(uint32_t off) {
  uint32_t i = (off >> 1) & 0x7F;
  switch (off) {
    case 0x00: return (uint16_t)((g_mail_to_dsp >> 16) & 0xFFFF);
    case 0x02: return (uint16_t)g_mail_to_dsp;
    case 0x04: return (uint16_t)((g_mail_from_dsp >> 16) & 0xFFFF);
    case 0x06: {
      uint16_t v = (uint16_t)g_mail_from_dsp;
      g_mail_from_dsp &= 0x7FFFFFFFu;     // reading the low half consumes the mail
      return v;
    }
    case 0x16: return g_dsp[i] | 1;      // AR_MODE: ARAM controller ready
    case 0x3A: return ai_dma_remaining();
    default: return g_dsp[i];
  }
}

void dsp_write16(uint32_t off, uint16_t v) {
  uint32_t i = (off >> 1) & 0x7F;
  switch (off) {
    case 0x00: g_mail_to_dsp = (g_mail_to_dsp & 0x0000FFFFu) | ((uint32_t)(v & 0x7FFF) << 16); return;
    case 0x02: g_mail_to_dsp = (g_mail_to_dsp & 0xFFFF0000u) | v; g_mail_to_dsp |= 0x80000000u; dsp_cpu_mail_written(); return;
    case 0x0A: {
      uint16_t old = g_dsp[i];
      uint16_t w1c = (1u << 3) | (1u << 5) | (1u << 7);
      uint16_t nv = (uint16_t)((v & ~w1c) | (old & w1c & ~v));
      bool init_fall = (old & (1u << 11)) && !(v & (1u << 11));
      g_dsp[i] = nv & ~1u;
      if (v & 1) dsp_reset();
      if (v & (1u << 1)) { g_dsp[i] &= ~(1u << 1); dsp_cpu_interrupt(); }
      if (init_fall) dsp_post_mail(0x8071FEED);
      return;
    }
    case 0x36: {  // AI DMA length / enable
      bool was = g_dsp[i] & 0x8000;
      g_dsp[i] = v;
      if ((v & 0x8000) && !was) ai_dma_start();
      else if (!(v & 0x8000)) g_ai_dma_active = false;
      return;
    }
    case 0x2A:   // AR_DMA_CNT low: start
      g_dsp[i] = v;
      aram_dma();
      return;
    case 0x30:   // AI DMA start address (high / low): the game programmed the next buffer (ai_dma_tick)
    case 0x32:
      g_dsp[i] = v;
      g_ai_reprog = true;
      return;
    default:
      g_dsp[i] = v;
  }
}

bool dsp_irq() {
  uint16_t cr = g_dsp[0x05];
  return ((cr & (1u << 3)) && (cr & (1u << 4))) || ((cr & (1u << 5)) && (cr & (1u << 6))) ||
         ((cr & (1u << 7)) && (cr & (1u << 8)));
}
void dsp_raise_dspint() { g_dsp[0x05] |= (1u << 7); }
void dsp_raise_aidint() { g_dsp[0x05] |= (1u << 3); }

// The AI sample counter advances in real time while the interface is playing.
static uint64_t g_ai_play_ns = 0;
static uint32_t ai_counter_now() {
  if (!(g_ai_cr & 1)) return g_ai_cnt;
  uint64_t rate = (g_ai_cr & 2) ? 48000 : 32000;
  uint64_t el = sys_time_ns() - g_ai_play_ns;
  return g_ai_cnt + (uint32_t)(el * rate / 1000000000ull);
}
uint32_t ai_read(uint32_t off) {
  switch (off) { case 0: return g_ai_cr; case 4: return g_ai_vol; case 8: return ai_counter_now(); case 0xC: return g_ai_inttim; }
  return 0;
}
void ai_write(uint32_t off, uint32_t v) {
  switch (off) {
    case 0: {
      uint32_t cur = ai_counter_now();
      bool was_play = g_ai_cr & 1;
      g_ai_cr = (g_ai_cr & (1u << 3)) | (v & ~(1u << 3) & ~(1u << 5));
      if (v & (1u << 3)) g_ai_cr &= ~(1u << 3);
      if (v & (1u << 5)) { g_ai_cnt = 0; g_ai_play_ns = sys_time_ns(); }       // SCRESET
      else { g_ai_cnt = cur; g_ai_play_ns = sys_time_ns(); }
      (void)was_play;
      break;
    }
    case 4: g_ai_vol = v; break;
    case 8: g_ai_cnt = v; g_ai_play_ns = sys_time_ns(); break;
    case 0xC: g_ai_inttim = v; break;
  }
}
bool ai_irq() { return (g_ai_cr & (1u << 3)) && (g_ai_cr & (1u << 2)); }
}
