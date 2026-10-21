// Serial interface: GameCube controller on channel 0 (polling + command transfers).
#include "sys.h"
#include "hw.h"
#include "input.h"
#include <cstring>

namespace hw {
namespace {
struct Chan { uint32_t out = 0, inh = 0, inl = 0; };
Chan g_ch[4];
uint32_t g_poll = 0, g_comcsr = 0, g_status = 0, g_exilk = 0;
uint32_t g_buf[32];
bool g_tc = false, g_comerr = false;

// SISR holds one status byte per channel (channel 0 in the top byte):
//   0x01 under-run, 0x02 over-run, 0x04 collision, 0x08 no response, 0x10 WRST, 0x20 RDST
constexpr uint32_t kRdstBit(int ch) { return 0x20u << ((3 - ch) * 8); }
constexpr uint32_t kNorepBit(int ch) { return 0x08u << ((3 - ch) * 8); }

bool present(int ch) { return ch == 0; }

void pack_status(const PadState& p, uint8_t out[8]) {
  uint8_t b0 = 0, b1 = 0x80;
  if (p.buttons & PAD_A) b0 |= 0x01;
  if (p.buttons & PAD_B) b0 |= 0x02;
  if (p.buttons & PAD_X) b0 |= 0x04;
  if (p.buttons & PAD_Y) b0 |= 0x08;
  if (p.buttons & PAD_START) b0 |= 0x10;
  if (p.buttons & PAD_LEFT) b1 |= 0x01;
  if (p.buttons & PAD_RIGHT) b1 |= 0x02;
  if (p.buttons & PAD_DOWN) b1 |= 0x04;
  if (p.buttons & PAD_UP) b1 |= 0x08;
  if (p.buttons & PAD_Z) b1 |= 0x10;
  if (p.buttons & PAD_R) b1 |= 0x20;
  if (p.buttons & PAD_L) b1 |= 0x40;
  out[0] = b0; out[1] = b1;
  out[2] = (uint8_t)(128 + p.stick_x);
  out[3] = (uint8_t)(128 + p.stick_y);
  out[4] = (uint8_t)(128 + p.c_x);
  out[5] = (uint8_t)(128 + p.c_y);
  out[6] = p.trig_l;
  out[7] = p.trig_r;
}

// Run one command on the attached controller. Returns number of response bytes (0 = no response).
int controller_command(const uint8_t* cmd, int cmdlen, uint8_t* resp) {
  if (cmdlen < 1) return 0;
  switch (cmd[0]) {
    case 0x00: case 0xFF:                       // ID / reset
      resp[0] = 0x09; resp[1] = 0x00; resp[2] = 0x03;
      return 3;
    case 0x40: {                                // poll
      PadState p = input_poll();
      pack_status(p, resp);
      return 8;
    }
    case 0x41: case 0x42: {                     // origin / calibrate
      PadState p = input_poll();
      uint8_t st[8];
      pack_status(p, st);
      resp[0] = st[0]; resp[1] = st[1];
      resp[2] = 128; resp[3] = 128; resp[4] = 128; resp[5] = 128;   // stick origins at centre
      resp[6] = 0; resp[7] = 0; resp[8] = 0; resp[9] = 0;
      return 10;
    }
    default:
      return 0;
  }
}

uint32_t derived_comcsr() {
  uint32_t v = g_comcsr & ~((1u << 31) | (1u << 29) | (1u << 28) | 1u);
  if (g_tc) v |= (1u << 31);
  if (g_comerr) v |= (1u << 29);
  uint32_t rdst = 0;
  for (int c = 0; c < 4; ++c) if (g_status & kRdstBit(c)) rdst = 1;
  if (rdst) v |= (1u << 28);
  return v;
}
}  // namespace

void si_init() {
  memset(g_ch, 0, sizeof g_ch);
  memset(g_buf, 0, sizeof g_buf);
  g_poll = g_comcsr = g_status = g_exilk = 0;
  g_tc = g_comerr = false;
}

uint32_t si_reg_read(uint32_t off) {
  uint32_t i = (off >> 2) & 0x3F;
  if (i < 12) {
    int ch = i / 3, r = i % 3;
    if (r == 0) return g_ch[ch].out;
    if (r == 1) { g_status &= ~kRdstBit(ch); return g_ch[ch].inh; }     // reading the high word clears RDST
    return g_ch[ch].inl;
  }
  switch (i) {
    case 12: return g_poll;
    case 13: return derived_comcsr();
    case 14: return g_status;
    case 15: return g_exilk;
  }
  return 0;
}

void si_reg_write(uint32_t off, uint32_t v) {
  uint32_t i = (off >> 2) & 0x3F;
  if (i < 12) {
    int ch = i / 3, r = i % 3;
    if (r == 0) g_ch[ch].out = v;
    return;
  }
  switch (i) {
    case 12: g_poll = v; break;
    case 13: {
      if (v & (1u << 31)) g_tc = false;                       // TCINT write-1-clear
      g_comcsr = (g_comcsr & ((1u << 31) | (1u << 29) | (1u << 28))) | (v & 0x7FFFFFFEu & ~((1u << 29) | (1u << 28)));
      g_comcsr = (g_comcsr & ~0x80000000u);
      if (v & 1) {                                            // TSTART: run the transfer now
        int ch = (v >> 1) & 3;
        int outlen = (v >> 16) & 0x7F; if (!outlen) outlen = 128;
        int inlen = (v >> 8) & 0x7F; if (!inlen) inlen = 128;
        uint8_t out[128], in[128];
        for (int k = 0; k < outlen; ++k) out[k] = (uint8_t)(g_buf[k >> 2] >> (24 - 8 * (k & 3)));
        memset(in, 0, sizeof in);
        int got = present(ch) ? controller_command(out, outlen, in) : 0;
        if (got > 0) {
          g_comerr = false;
          for (int k = 0; k < inlen; ++k) {
            int sh = 24 - 8 * (k & 3);
            g_buf[k >> 2] = (g_buf[k >> 2] & ~(0xFFu << sh)) | ((uint32_t)in[k] << sh);
          }
        } else {
          g_comerr = true;
          g_status |= kNorepBit(ch);
        }
        g_tc = true;
      }
      break;
    }
    case 14: {
      // error bits are write-1-to-clear; bit 31 (WR) just latches the command registers
      uint32_t clr = v & 0x0F0F0F0Fu;
      g_status &= ~clr;
      break;
    }
    case 15: g_exilk = v; break;
  }
}

uint32_t si_buf_read(uint32_t off) { return g_buf[(off >> 2) & 0x1F]; }
void si_buf_write(uint32_t off, uint32_t v) { g_buf[(off >> 2) & 0x1F] = v; }

bool si_irq() {
  uint32_t c = derived_comcsr();
  if ((c & (1u << 31)) && (c & (1u << 30))) return true;      // TCINT & TCINTMSK
  if ((c & (1u << 28)) && (c & (1u << 27))) return true;      // RDSTINT & RDSTINTMSK
  return false;
}

// Called once per video frame: the SI hardware polls the enabled channels.
void si_vsync() {
  for (int ch = 0; ch < 4; ++ch) {
    if (!(g_poll & (0x80u >> ch))) continue;
    if (!present(ch)) { g_status |= kNorepBit(ch); continue; }
    uint8_t cmd[3] = {(uint8_t)(g_ch[ch].out >> 16), (uint8_t)(g_ch[ch].out >> 8), (uint8_t)g_ch[ch].out};
    uint8_t resp[16];
    int n = controller_command(cmd, 3, resp);
    if (n >= 8) {
      g_ch[ch].inh = ((uint32_t)resp[0] << 24) | (resp[1] << 16) | (resp[2] << 8) | resp[3];
      g_ch[ch].inl = ((uint32_t)resp[4] << 24) | (resp[5] << 16) | (resp[6] << 8) | resp[7];
      g_status |= kRdstBit(ch);
    }
  }
}
}  // namespace hw

namespace hw {
void si_dump() {
  logf("     SI: poll=%08X comcsr=%08X status=%08X ch0.out=%08X inh=%08X inl=%08X\n", g_poll, g_comcsr, g_status,
       g_ch[0].out, g_ch[0].inh, g_ch[0].inl);
}
}
