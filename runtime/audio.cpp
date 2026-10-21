// High-level emulation of the Dolphin SDK "AX" DSP microcode and the host audio output.
//
// The game (via the SDK's AX library) hands the DSP a command list every 5 ms. We walk the voice parameter
// blocks (AXPB), decode their ARAM sample data, resample, apply volume ramps, mix to L/R/S (+aux buses) and
// write the final stereo frame into main memory for the AI DMA.
#include "sys.h"
#include "hw.h"
#include "settings_ui.h"
#include <SDL3/SDL.h>
#include "gx_lle.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>
#include <unordered_map>
#include <string>
void audio_mix_extra(int16_t* pcm, size_t n);   // below: logic VM one-shot sounds

namespace hw {
extern uint8_t* g_aram;
}

namespace {
constexpr int kFrame = 160;             // samples per AX frame at 32 kHz
constexpr int kSub = 32;                // 1 ms sub-blocks

// ---- guest memory helpers (big endian)
inline uint16_t R16(uint32_t a) { return (uint16_t)ld16(a); }
inline void W16(uint32_t a, uint32_t v) { st16(a, v); }
inline uint32_t R32h(uint32_t a) { return ((uint32_t)R16(a) << 16) | R16(a + 2); }

// ---- ARAM access
inline uint8_t aram8(uint32_t byte) { return hw::g_aram && byte < 0x01000000u ? hw::g_aram[byte] : 0; }
inline int16_t aram16(uint32_t byte) {
  return (int16_t)(((uint16_t)aram8(byte) << 8) | aram8(byte + 1));
}

// PB field offsets (bytes)
enum {
  PB_NEXT = 0x00, PB_SRCSEL = 0x08, PB_MIXCTRL = 0x0C, PB_STATE = 0x0E,
  PB_MIX = 0x12,            // 18 u16: vL dL vR dR aAL daAL aAR daAR aBL daBL aBR daBR aBS daBS vS dS aAS daAS
  PB_UPD_NUM = 0x44, PB_UPD_DATA = 0x4E,
  PB_VE = 0x64,
  PB_LOOPFLAG = 0x6E, PB_FORMAT = 0x70, PB_LOOPADDR = 0x72, PB_ENDADDR = 0x76, PB_CURADDR = 0x7A,
  PB_ADPCM_COEF = 0x7E, PB_ADPCM_GAIN = 0x9E, PB_ADPCM_PS = 0xA0, PB_ADPCM_YN1 = 0xA2, PB_ADPCM_YN2 = 0xA4,
  PB_SRC_RATIO = 0xA6, PB_SRC_FRAC = 0xAA, PB_SRC_LAST = 0xAC,
  PB_ADPCMLOOP_PS = 0xB4, PB_ADPCMLOOP_YN1 = 0xB6, PB_ADPCMLOOP_YN2 = 0xB8,
};

struct Accum {
  int32_t l[kFrame], r[kFrame], s[kFrame];
  void clear() { memset(this, 0, sizeof *this); }
};
Accum g_main, g_auxA, g_auxB;
uint64_t g_frames = 0;

// A decoded sample source for one voice (streaming).
struct Src {
  uint32_t pb;
  uint16_t fmt, loopflag;
  uint32_t cur, loop, end;
  // adpcm state
  int16_t coef[16];
  int32_t yn1, yn2;
  uint16_t ps;
  bool ended = false;
  uint32_t loops = 0;

  void load(uint32_t pbaddr) {
    pb = pbaddr;
    fmt = R16(pb + PB_FORMAT);
    loopflag = R16(pb + PB_LOOPFLAG);
    loop = R32h(pb + PB_LOOPADDR);
    end = R32h(pb + PB_ENDADDR);
    cur = R32h(pb + PB_CURADDR);
    for (int i = 0; i < 16; ++i) coef[i] = (int16_t)R16(pb + PB_ADPCM_COEF + 2 * i);
    ps = R16(pb + PB_ADPCM_PS);
    yn1 = (int16_t)R16(pb + PB_ADPCM_YN1);
    yn2 = (int16_t)R16(pb + PB_ADPCM_YN2);
  }
  void store() {
    W16(pb + PB_CURADDR, cur >> 16);
    W16(pb + PB_CURADDR + 2, cur & 0xFFFF);
    W16(pb + PB_ADPCM_PS, ps);
    W16(pb + PB_ADPCM_YN1, (uint16_t)yn1);
    W16(pb + PB_ADPCM_YN2, (uint16_t)yn2);
  }

  // return the next sample at the current position and advance by one sample
  int16_t next() {
    if (ended) return 0;
    int16_t out = 0;
    switch (fmt) {
      case 10:  // PCM16: address in samples
        out = aram16(cur * 2);
        break;
      case 25:  // PCM8: address in bytes
        out = (int16_t)((int8_t)aram8(cur) << 8);
        break;
      default: {  // ADPCM: address in nibbles, 16 nibbles per 8-byte frame, first two are the header
        uint32_t frame_nib = cur & ~15u;
        uint32_t pos = cur & 15u;
        if (pos < 2) {  // header nibbles: read the frame header and skip to the first sample
          uint8_t hdr = aram8(frame_nib >> 1);
          ps = hdr;
          cur += 2 - pos;
          pos = 2;
        }
        uint8_t byte = aram8((cur >> 1));
        int nib = (cur & 1) ? (byte & 0xF) : (byte >> 4);
        if (nib >= 8) nib -= 16;
        int scale = 1 << (ps & 0xF);
        int idx = (ps >> 4) & 7;
        int32_t c1 = coef[idx * 2], c2 = coef[idx * 2 + 1];
        int32_t v = (((int32_t)nib * scale) << 11) + 0x400 + c1 * yn1 + c2 * yn2;
        v >>= 11;
        v = std::clamp(v, -32768, 32767);
        yn2 = yn1;
        yn1 = v;
        out = (int16_t)v;
        break;
      }
    }
    // advance and handle loop / end (end address is inclusive)
    if (cur == end) {
      if (loopflag) {
        cur = loop;
        ++loops;
        if (fmt == 0) {   // ADPCM: restore the loop predictor state
          ps = R16(pb + PB_ADPCMLOOP_PS);
          yn1 = (int16_t)R16(pb + PB_ADPCMLOOP_YN1);
          yn2 = (int16_t)R16(pb + PB_ADPCMLOOP_YN2);
        }
      } else {
        ended = true;
      }
    } else {
      ++cur;
    }
    return out;
  }
};

inline int32_t volmul(int32_t sample, uint32_t vol) { return (int32_t)(((int64_t)sample * vol) >> 15); }

void apply_updates(uint32_t pb, int sub, uint32_t& data_ptr) {
  uint16_t n = R16(pb + PB_UPD_NUM + 2 * sub);
  for (uint16_t i = 0; i < n; ++i) {
    uint16_t off = R16(data_ptr);
    uint16_t val = R16(data_ptr + 2);
    data_ptr += 4;
    if (off < 0xF4 / 2) W16(pb + off * 2, val);
  }
}

// Volume sliders (settings: Master / Sound effects / Music / Dialogue, % of the original). Voice classes:
//  - sound effects: DSP-ADPCM voices (the sound banks).
//  - streams: 16-bit PCM voices - the game's CRI ADX player decodes on the CPU into PCM ring buffers in ARAM.
//    The player splits a stereo stream into two voices panned hard left / right (music: ARAM 0x402000 L,
//    0x403000 R in stg0502), while a mono stream is one voice sent to both sides (dialogue: ARAM 0x406000, centre).
//    So: PCM to L only or R only = music, PCM to both = dialogue.
// AUDIO_CLASS=1 logs voice-frames and peak level per class every ~5 s (to check the split).
enum { kSfx, kMusic, kDialogue, kClasses };
int32_t g_gain_q15[kClasses] = {32768, 32768, 32768};   // Q15
uint32_t g_class_count[kClasses], g_class_peak[kClasses];

// VOICE_LOG=1 (debugging): when every voice starts / stops, its length in 5 ms blocks and whether the game stopped it
// (PB_STATE written by the guest) or it played to its end. Used to compare sound lifetimes at different frame rates.
struct VoiceTrack { uint32_t blocks = 0; uint32_t fmt = 0; uint32_t loopflag = 0; uint32_t end = 0; };
std::unordered_map<uint32_t, VoiceTrack> g_voice_track;

void render_voice(uint32_t pb) {
  static const bool vlog = getenv("VOICE_LOG") != nullptr;
  if (R16(pb + PB_STATE) != 1) {
    if (vlog) {
      auto it = g_voice_track.find(pb);
      if (it != g_voice_track.end()) {
        logf("[voice] pb=%08X STOPPED BY GAME after %u blocks (%.0f ms) fmt=%u loop=%u ve=%04X\n", pb, it->second.blocks,
             it->second.blocks * 5.0, it->second.fmt, it->second.loopflag, R16(pb + PB_VE));
        g_voice_track.erase(it);
      }
    }
    return;
  }
  Src src;
  src.load(pb);
  if (vlog) {
    auto ins = g_voice_track.try_emplace(pb);
    VoiceTrack& vt = ins.first->second;
    if (ins.second) {
      vt.fmt = src.fmt; vt.loopflag = src.loopflag; vt.end = src.end;
      logf("[voice] pb=%08X start fmt=%u loop=%u cur=%08X end=%08X ratio=%08X ve=%04X/%d\n", pb, src.fmt, src.loopflag, src.cur, src.end,
           R32h(pb + PB_SRC_RATIO), R16(pb + PB_VE), (int)(int16_t)R16(pb + PB_VE + 2));
    }
    ++vt.blocks;
  }
  const bool pcm = src.fmt == 10 || src.fmt == 25;
  const int cls = !pcm ? kSfx : (R16(pb + PB_MIXCTRL) & 3) == 3 ? kDialogue : kMusic;
  const int32_t cgain = g_gain_q15[cls];
  ++g_class_count[cls];
  uint16_t mixctrl = R16(pb + PB_MIXCTRL);
  uint32_t ratio = R32h(pb + PB_SRC_RATIO);
  uint32_t frac = R16(pb + PB_SRC_FRAC);
  uint16_t srcsel = R16(pb + PB_SRCSEL);
  uint32_t upd_ptr = R32h(pb + PB_UPD_DATA);
  uint32_t veVol = R16(pb + PB_VE);
  int32_t veDelta = (int16_t)R16(pb + PB_VE + 2);
  uint32_t mv[9]; int32_t md[9];       // L R S AAL AAR AAS ABL ABR ABS volumes + deltas
  static const int mixoff[9] = {0, 2, 14, 4, 6, 16, 8, 10, 12};     // vL vR vS aAL aAR aAS aBL aBR aBS (index in u16)
  for (int i = 0; i < 9; ++i) {
    mv[i] = R16(pb + PB_MIX + 2 * mixoff[i]);
    md[i] = (int16_t)R16(pb + PB_MIX + 2 * (mixoff[i] + 1));
  }
  const bool ramp_main = mixctrl & 0x8, ramp_a = mixctrl & 0x40, ramp_b = mixctrl & 0x800;
  const bool ramp_as = mixctrl & 0x100, ramp_bs = mixctrl & 0x2000;
  const bool dest[9] = {(bool)(mixctrl & 1), (bool)(mixctrl & 2), (bool)(mixctrl & 4), (bool)(mixctrl & 0x10),
                        (bool)(mixctrl & 0x20), (bool)(mixctrl & 0x80), (bool)(mixctrl & 0x200),
                        (bool)(mixctrl & 0x400), (bool)(mixctrl & 0x1000)};
  const bool ramp[9] = {ramp_main, ramp_main, ramp_main, ramp_a, ramp_a, ramp_as, ramp_b, ramp_b, ramp_bs};

  int16_t s0, s1;
  if (R16(pb + PB_SRC_LAST) != 0x5A5A) {          // first block for this voice: prime the interpolation window
    s0 = src.next();
    s1 = src.next();
    W16(pb + PB_SRC_LAST, 0x5A5A);
  } else {
    s0 = (int16_t)R16(pb + PB_SRC_LAST + 4);
    s1 = (int16_t)R16(pb + PB_SRC_LAST + 6);
  }
  if (ratio == 0) ratio = 0x10000;

  for (int sub = 0; sub < kFrame / kSub; ++sub) {
    apply_updates(pb, sub, upd_ptr);
    // re-read volumes if an update touched them
    for (int i = 0; i < 9; ++i) {
      mv[i] = R16(pb + PB_MIX + 2 * mixoff[i]);
      md[i] = (int16_t)R16(pb + PB_MIX + 2 * (mixoff[i] + 1));
    }
    veVol = R16(pb + PB_VE);
    veDelta = (int16_t)R16(pb + PB_VE + 2);
    uint32_t v_start[9], v_end[9];
    for (int i = 0; i < 9; ++i) {
      v_start[i] = mv[i];
      int32_t e = (int32_t)mv[i] + (ramp[i] ? md[i] : 0);
      v_end[i] = (uint32_t)std::clamp(e, 0, 0xFFFF);
    }
    uint32_t ve_start = veVol, ve_end = (uint32_t)std::clamp((int32_t)veVol + veDelta, 0, 0xFFFF);
    for (int n = 0; n < kSub; ++n) {
      int idx = sub * kSub + n;
      // resample: linear interpolation between s0 and s1
      int32_t sample;
      if (srcsel == 2 || ratio == 0x10000 && frac == 0) {
        sample = s0;
        // advance one sample
        s0 = s1; s1 = src.next();
      } else {
        sample = s0 + (int32_t)(((int64_t)(s1 - s0) * frac) >> 16);
        uint32_t acc = frac + ratio;
        int steps = acc >> 16;
        frac = acc & 0xFFFF;
        for (int k = 0; k < steps; ++k) { s0 = s1; s1 = src.next(); }
      }
      uint32_t ve = ve_start + (int32_t)(((int64_t)ve_end - ve_start) * n / kSub);
      sample = volmul(sample, ve);
      if (cgain != 32768) sample = (int32_t)(((int64_t)sample * cgain) >> 15);
      g_class_peak[cls] = std::max<uint32_t>(g_class_peak[cls], (uint32_t)std::abs(sample));
      auto vol_at = [&](int i) -> uint32_t {
        return v_start[i] + (int32_t)(((int64_t)v_end[i] - v_start[i]) * n / kSub);
      };
      if (dest[0]) g_main.l[idx] += volmul(sample, vol_at(0));
      if (dest[1]) g_main.r[idx] += volmul(sample, vol_at(1));
      if (dest[2]) g_main.s[idx] += volmul(sample, vol_at(2));
      if (dest[3]) g_auxA.l[idx] += volmul(sample, vol_at(3));
      if (dest[4]) g_auxA.r[idx] += volmul(sample, vol_at(4));
      if (dest[5]) g_auxA.s[idx] += volmul(sample, vol_at(5));
      if (dest[6]) g_auxB.l[idx] += volmul(sample, vol_at(6));
      if (dest[7]) g_auxB.r[idx] += volmul(sample, vol_at(7));
      if (dest[8]) g_auxB.s[idx] += volmul(sample, vol_at(8));
    }
    // ramped volumes persist
    for (int i = 0; i < 9; ++i) W16(pb + PB_MIX + 2 * mixoff[i], v_end[i]);
    W16(pb + PB_VE, ve_end);
  }
  src.store();
  W16(pb + PB_SRC_FRAC, frac);
  W16(pb + PB_SRC_LAST + 4, (uint16_t)s0);
  W16(pb + PB_SRC_LAST + 6, (uint16_t)s1);
  if (src.ended) {
    W16(pb + PB_STATE, 0);       // non-looping voice reached its end: stopped
    if (vlog) {
      auto it = g_voice_track.find(pb);
      if (it != g_voice_track.end()) {
        logf("[voice] pb=%08X played to its END after %u blocks (%.0f ms) fmt=%u\n", pb, it->second.blocks, it->second.blocks * 5.0, it->second.fmt);
        g_voice_track.erase(it);
      }
    }
  }
}

void out_stereo(uint32_t buf, uint32_t sbuf) {
  for (int i = 0; i < kFrame; ++i) {
    int32_t l = std::clamp(g_main.l[i], -32768, 32767);
    int32_t r = std::clamp(g_main.r[i], -32768, 32767);
    st16(buf + i * 4, (uint16_t)l);
    st16(buf + i * 4 + 2, (uint16_t)r);
    if (sbuf) st16(sbuf + i * 2, (uint16_t)std::clamp(g_main.s[i], -32768, 32767));
  }
}

// aux bus exchange with the CPU side effect processors: buffers are 3 x 160 s32 (L, R, S)
void aux_exchange(Accum& acc, uint32_t in_buf, uint32_t out_buf) {
  if (out_buf) {   // previous CPU processed output is added into the main mix
    for (int i = 0; i < kFrame; ++i) {
      g_main.l[i] += (int32_t)ld32(out_buf + i * 4);
      g_main.r[i] += (int32_t)ld32(out_buf + kFrame * 4 + i * 4);
      g_main.s[i] += (int32_t)ld32(out_buf + kFrame * 8 + i * 4);
    }
  }
  if (in_buf) {
    for (int i = 0; i < kFrame; ++i) {
      st32(in_buf + i * 4, (uint32_t)acc.l[i]);
      st32(in_buf + kFrame * 4 + i * 4, (uint32_t)acc.r[i]);
      st32(in_buf + kFrame * 8 + i * 4, (uint32_t)acc.s[i]);
    }
  }
  acc.clear();
}

// ---- host output ----------------------------------------------------------------------------------
SDL_AudioStream* g_stream = nullptr;
bool g_audio_failed = false;
int g_audio_skipped = 0;   // #161 replayed blocks skipped (the [audio] summary)
}  // namespace

namespace hw {
bool g_audio_trace = false;

void audio_process_cmdlist(uint32_t addr, uint32_t size_words) {
  static uint32_t pb_base = 0;
  g_main.clear();
  g_gain_q15[kSfx] = std::clamp(settings::g.vol_sfx, 0, 200) * 32768 / 100;
  g_gain_q15[kMusic] = std::clamp(settings::g.vol_music, 0, 200) * 32768 / 100;
  g_gain_q15[kDialogue] = std::clamp(settings::g.vol_dialogue, 0, 200) * 32768 / 100;
  static const bool class_log = getenv("AUDIO_CLASS") != nullptr;
  if (class_log && g_frames % 1000 == 0) {   // ~5 s: voice-frames and peak level per class since the last line
    logf("[audio-class] sfx voices=%u peak=%u | music voices=%u peak=%u | dialogue voices=%u peak=%u\n",
         g_class_count[kSfx], g_class_peak[kSfx], g_class_count[kMusic], g_class_peak[kMusic],
         g_class_count[kDialogue], g_class_peak[kDialogue]);
    for (int i = 0; i < kClasses; ++i) g_class_count[i] = g_class_peak[i] = 0;
  }
  // aux accumulators are kept across the list; they are cleared by the aux commands
  uint32_t a = addr;
  uint32_t end = addr + size_words * 2;
  if (g_audio_trace && (g_frames % 200) == 0) logf("[audio] frame %llu cmdlist %08X size %u\n", (unsigned long long)g_frames, addr, size_words);
  while (a < end) {
    uint16_t op = R16(a);
    a += 2;
    switch (op) {
      case 0: a += 4; break;                                   // studio / depop state
      case 7: case 0x11: a += 4; break;                        // surround buffer setup
      case 2: pb_base = R32h(a); a += 4; break;                // voice parameter block chain
      case 3: {                                                // process the voices
        uint32_t pb = pb_base;
        int guard = 0;
        while (pb && guard++ < 64) {
          render_voice(pb);
          pb = R32h(pb + PB_NEXT);
        }
        break;
      }
      case 4: {                                                // aux A: in, out
        uint32_t in = R32h(a), out = R32h(a + 4);
        a += 8;
        aux_exchange(g_auxA, in, out);
        break;
      }
      case 5: {
        uint32_t in = R32h(a), out = R32h(a + 4);
        a += 8;
        aux_exchange(g_auxB, in, out);
        break;
      }
      case 0x12: a += 8; break;                                // compressor (not emulated)
      case 0x13: a += 24; break;
      case 0x10: a += 8; break;
      case 0xE: {                                              // final output
        uint32_t sbuf = R32h(a), buf = R32h(a + 4);
        a += 8;
        out_stereo(buf, sbuf);
        break;
      }
      case 0xF: a = end; break;
      default:
        logf("[audio] unknown AX command %04X at %08X\n", op, a - 2);
        a = end;
        break;
    }
  }
  ++g_frames;
  if (g_audio_trace && (g_frames % 400) == 0) {
    static uint64_t t0 = sys_time_ns(), last = t0;
    uint64_t now = sys_time_ns();
    int nv = 0; uint32_t pbw = pb_base; int guard = 0;
    while (pbw && guard++ < 64) {
      if (R16(pbw + PB_STATE) == 1) {
        ++nv;
        logf("[audio]   voice %08X fmt=%u loop=%u cur=%08X loopaddr=%08X end=%08X ratio=%08X srcsel=%u mix=%04X vL=%04X vR=%04X ve=%04X\n",
             pbw, R16(pbw + PB_FORMAT), R16(pbw + PB_LOOPFLAG), R32h(pbw + PB_CURADDR), R32h(pbw + PB_LOOPADDR),
             R32h(pbw + PB_ENDADDR), R32h(pbw + PB_SRC_RATIO), R16(pbw + PB_SRCSEL), R16(pbw + PB_MIXCTRL),
             R16(pbw + PB_MIX), R16(pbw + PB_MIX + 4), R16(pbw + PB_VE));
      }
      pbw = R32h(pbw + PB_NEXT);
    }
    logf("[audio] %llu AX frames, %.1f frames/s over last interval, active voices=%d\n", (unsigned long long)g_frames,
         400.0 / ((now - last) / 1e9), nv);
    last = now;
  }
}

// --quiet runs: a virtual host queue (bytes), drained at 32 kHz real time, fed like the real one
static double g_vq = 32000 / 10 * 4;
static uint64_t g_vq_last = 0;
int audio_queue_ms() {
  const int per_ms = 32000 * 4 / 1000;
  if (g_quiet) {
    uint64_t now = sys_time_ns();
    if (!g_vq_last) g_vq_last = now;   // starts draining at the first query (the device plays its prebuffer)
    double q = std::max(0.0, g_vq - (now - g_vq_last) / 1e9 * 32000 * 4);
    return (int)(q / per_ms);
  }
  return g_stream ? SDL_GetAudioStreamQueued(g_stream) / per_ms : -1;
}

void audio_submit_dma(uint32_t addr, uint32_t bytes) {
  if (g_audio_failed) return;
  if (g_time_scale > 1.2) return;           // fast-forward: audio is muted
  // AUDIO_STATS=1: every 2 s, how the host queue fared (under-runs padded with silence, blocks dropped, queue level).
  // In --quiet runs no device is opened; a virtual queue drained at exactly 32 kHz real time stands in for it.
  static const bool stats = getenv("AUDIO_STATS") != nullptr;
  static int st_pads = 0, st_drops = 0, st_blocks = 0, st_min_q = 1 << 30;
  static uint64_t st_t0 = 0;
  if (stats) {
    uint64_t now = sys_time_ns();
    if (!st_t0) st_t0 = now;
    if (now - st_t0 > 2000000000ull) {
      logf("[audio-stats] %d blocks in %.1f s (%.0f ms of audio), %d under-runs padded, %d dropped / skipped, min queue %d ms\n", st_blocks,
           (now - st_t0) / 1e9, st_blocks * (bytes / 4) / 32.0, st_pads, st_drops, st_min_q / 128);
      st_pads = st_drops = st_blocks = 0;
      st_min_q = 1 << 30;
      st_t0 = now;
    }
  }
  // #161 (user: "audio crunch still exists"): one feed for the device and the quiet runs' virtual queue.
  // Before: under 40 ms queued, 40 ms of SILENCE was pushed ahead of the real audio (a gap on every near-miss), and a
  // block the busy game replayed was played as silence (another gap). Now a replayed block is skipped (the cushion
  // covers it; hw_dsp.cpp keeps ~120 ms queued) and silence is inserted only when the queue is really empty (< 8 ms).
  // The device plays exactly 32 kHz (no rate games: the AI clock itself is driven by this queue).
  // AUDIO_DUMP records exactly what is queued, inserted silence included.
  bytes &= ~3u;
  addr |= 0x80000000u;
  std::vector<int16_t> pcm(bytes / 2);
  for (uint32_t i = 0; i < bytes / 2; ++i) pcm[i] = (int16_t)ld16(addr + i * 2);
  audio_mix_extra(pcm.data(), pcm.size());
  if (!g_quiet) {
    if (!g_stream) {
      if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        logf("[audio] SDL audio init failed: %s\n", SDL_GetError());
        g_audio_failed = true;
        return;
      }
      SDL_AudioSpec spec{};
      spec.format = SDL_AUDIO_S16;
      spec.channels = 2;
      spec.freq = 32000;
      g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
      if (!g_stream) {
        logf("[audio] cannot open audio device: %s\n", SDL_GetError());
        g_audio_failed = true;
        return;
      }
      SDL_ResumeAudioStreamDevice(g_stream);
      // prebuffer ~100 ms of silence so scheduling jitter on the emulation side does not cause under-runs
      std::vector<int16_t> pre(32000 / 10 * 2, 0);
      SDL_PutAudioStreamData(g_stream, pre.data(), (int)(pre.size() * 2));
      logf("[audio] host audio stream opened (32 kHz stereo)\n");
    }
    // Master volume (settings, % of the original) on what goes to the speakers only - the game's buffers stay as mixed.
    if (int mv = std::clamp(settings::g.vol_master, 0, 200); mv != 100)
      for (auto& v : pcm) v = (int16_t)std::clamp(v * mv / 100, -32768, 32767);
    if (settings::g.mute_background && gxlle::g_window) {   // #151 "Mute when in the background": unfocused or minimised
      const SDL_WindowFlags fl = SDL_GetWindowFlags(gxlle::g_window);
      if (!(fl & SDL_WINDOW_INPUT_FOCUS) || (fl & SDL_WINDOW_MINIMIZED)) std::fill(pcm.begin(), pcm.end(), int16_t(0));
    }
  }
  static FILE* dump = getenv("AUDIO_DUMP") ? fopen(getenv("AUDIO_DUMP"), "wb") : nullptr;
  const int per_ms = 32000 * 4 / 1000;
  // the queue now (bytes): the device's, or the virtual one drained at 32 kHz
  int queued;
  if (g_quiet) {
    const uint64_t now = sys_time_ns();
    if (!g_vq_last) { g_vq_last = now; g_vq = 32000 / 10 * 4; }   // the device's ~100 ms prebuffer
    g_vq = std::max(0.0, g_vq - (now - g_vq_last) / 1e9 * 32000 * 4);
    g_vq_last = now;
    queued = (int)g_vq;
  } else {
    queued = SDL_GetAudioStreamQueued(g_stream);
  }
  auto put = [&](const int16_t* d, size_t n) {
    if (g_quiet) g_vq += double(n * 2);
    else SDL_PutAudioStreamData(g_stream, d, (int)(n * 2));
    if (dump) { fwrite(d, 2, n, dump); fflush(dump); }
  };
  ++st_blocks;
  st_min_q = std::min(st_min_q, queued);
  {
    // Replayed block: when the game is busy (saving, loading) and has not mixed the next audio frame yet, the AI DMA
    // replays the same few milliseconds (a buzzing burst). An exact repeat of the previous non-silent block from the
    // same buffer is skipped; the cushion plays on. (Games alternate DMA buffers: compare per address.)
    static std::unordered_map<uint32_t, std::vector<int16_t>> last_by_addr;
    bool silent = true;
    for (int16_t v : pcm) if (v) { silent = false; break; }
    auto& prev = last_by_addr[addr];
    if (!silent && pcm == prev) {
      static int n = 0;
      if (n++ < 20) logf("[audio] repeated block at %08X (game busy) -> skipped (queue %d ms)\n", addr, queued / per_ms);
      ++st_drops;
      ++g_audio_skipped;
      return;
    }
    prev = pcm;
    if (last_by_addr.size() > 16) last_by_addr.clear();
  }
  const bool underrun = queued < 8 * per_ms;
  if (underrun) {   // really empty: a short silence so the device has something to play (the only audible gap left)
    std::vector<int16_t> pad(32000 / 50 * 2, 0);
    put(pad.data(), pad.size());
    ++st_pads;
  }
  if (!g_quiet) {   // #151 diagnostic (always on, quiet unless something goes wrong): under-runs while in the background
    static uint64_t bg_t0 = 0;
    static int bg_pads = 0, bg_blocks = 0, bg_lines = 0;
    const SDL_WindowFlags fl = gxlle::g_window ? SDL_GetWindowFlags(gxlle::g_window) : 0;
    const bool bg = gxlle::g_window && (!(fl & SDL_WINDOW_INPUT_FOCUS) || (fl & SDL_WINDOW_MINIMIZED));
    const uint64_t now = sys_time_ns();
    if (!bg) bg_t0 = 0;
    else {
      if (!bg_t0) { bg_t0 = now; bg_pads = bg_blocks = 0; }
      ++bg_blocks;
      bg_pads += underrun;
      if (now - bg_t0 > 2000000000ull) {
        if (bg_pads && bg_lines < 100) {
          ++bg_lines;
          logf("[bg-audio] window in the background%s: %d under-runs in %.1f s, %.0f ms of audio made, queue %d ms\n",
               (fl & SDL_WINDOW_MINIMIZED) ? " (minimised)" : "", bg_pads, (now - bg_t0) / 1e9, bg_blocks * (bytes / 4) / 32.0, queued / per_ms);
        }
        bg_t0 = now; bg_pads = bg_blocks = 0;
      }
    }
  }
  if (queued < 300 * per_ms) put(pcm.data(), pcm.size());
  else ++st_drops;
  {   // #161: always on, one line per 2 s that had skipped / padded audio (the per-block lines stop after 20)
    static uint64_t a_t0 = 0;
    static int a_pads = 0, a_lines = 0;
    a_pads += underrun;
    const uint64_t now = sys_time_ns();
    if (!a_t0) a_t0 = now;
    if (now - a_t0 > 2000000000ull) {
      static int drops_seen = 0;
      const int skipped = g_audio_skipped - drops_seen;
      drops_seen = g_audio_skipped;
      if ((skipped || a_pads) && a_lines < 200) {
        ++a_lines;
        logf("[audio] last 2 s: %d replayed blocks skipped, %d empty-queue gaps (queue %d ms)\n", skipped, a_pads, queued / per_ms);
      }
      a_pads = 0;
      a_t0 = now;
    }
  }
  {
    size_t loud = 0;
    for (int16_t v : pcm) if (v > 30000 || v < -30000) ++loud;
    if (loud > pcm.size() / 8) {
      static int n = 0;
      if (n++ < 40) logf("[audio] LOUD block: %zu/%zu samples at full scale (t=%.2fs, addr %08X)\n", loud, pcm.size(),
                         sys_time_ns() / 1e9, addr);
    }
  }
}
}  // namespace hw

// ---- one-shot extra sounds (logic VM 'Play sound', stage_workshop.cpp ACT 25): a WAV file (PCM 8/16-bit, mono or
// stereo, any rate) decoded once, resampled to 32 kHz stereo and mixed into what goes to the speakers (and into
// AUDIO_DUMP in --quiet runs, which is how tests hear it). Volume = the SFX setting.
#include <fstream>
#include <mutex>
namespace {
struct Extra { std::vector<int16_t> pcm; size_t pos = 0; };
std::mutex g_extra_mx;
std::vector<Extra> g_extra;
std::unordered_map<std::string, std::vector<int16_t>> g_extra_cache;

bool load_wav(const std::string& path, std::vector<int16_t>& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (d.size() < 44 || memcmp(d.data(), "RIFF", 4) || memcmp(d.data() + 8, "WAVE", 4)) return false;
  uint16_t fmt = 0, ch = 0, bits = 0; uint32_t rate = 0; const uint8_t* data = nullptr; uint32_t len = 0;
  for (size_t o = 12; o + 8 <= d.size();) {
    const uint32_t sz = d[o + 4] | d[o + 5] << 8 | d[o + 6] << 16 | (uint32_t)d[o + 7] << 24;
    if (!memcmp(d.data() + o, "fmt ", 4) && o + 24 <= d.size()) {
      fmt = d[o + 8] | d[o + 9] << 8; ch = d[o + 10] | d[o + 11] << 8;
      rate = d[o + 12] | d[o + 13] << 8 | d[o + 14] << 16 | (uint32_t)d[o + 15] << 24; bits = d[o + 22] | d[o + 23] << 8;
    } else if (!memcmp(d.data() + o, "data", 4)) { data = d.data() + o + 8; len = (uint32_t)std::min<size_t>(sz, d.size() - o - 8); }
    o += 8 + sz + (sz & 1);
  }
  if (fmt != 1 || !data || !rate || (ch != 1 && ch != 2) || (bits != 8 && bits != 16)) return false;
  const uint32_t frames = len / (ch * bits / 8);
  auto sample = [&](uint32_t i, int c) -> float {
    const uint32_t k = i * ch + (ch == 2 ? c : 0);
    return bits == 16 ? (int16_t)(data[k * 2] | data[k * 2 + 1] << 8) / 32768.f : (data[k] - 128) / 128.f;
  };
  const double step = rate / 32000.0;
  const uint32_t n = (uint32_t)(frames / step);
  out.resize((size_t)n * 2);
  for (uint32_t i = 0; i < n; ++i) {
    const double s = i * step; const uint32_t a = (uint32_t)s, b = std::min(a + 1, frames - 1); const float t = float(s - a);
    for (int c = 0; c < 2; ++c) out[(size_t)i * 2 + c] = (int16_t)std::clamp((sample(a, c) * (1 - t) + sample(b, c) * t) * 32767.f, -32768.f, 32767.f);
  }
  return true;
}
}  // namespace

bool audio_play_file(const char* path) {
  std::lock_guard<std::mutex> lk(g_extra_mx);
  auto it = g_extra_cache.find(path);
  if (it == g_extra_cache.end()) {
    std::vector<int16_t> pcm;
    if (!load_wav(path, pcm)) { logf("[audio] play sound: cannot read %s (WAV PCM 8/16-bit needed)\n", path); return false; }
    it = g_extra_cache.emplace(path, std::move(pcm)).first;
  }
  if (g_extra.size() < 16) g_extra.push_back({it->second, 0});
  logf("[audio] play sound %s (%.2f s)\n", path, it->second.size() / 2 / 32000.0);
  return true;
}
// mixes the playing extra sounds into n stereo samples (interleaved), advancing them
void audio_mix_extra(int16_t* pcm, size_t n) {
  std::lock_guard<std::mutex> lk(g_extra_mx);
  if (g_extra.empty()) return;
  const float vol = std::clamp(settings::g.vol_sfx, 0, 200) / 100.f;
  for (auto& e : g_extra)
    for (size_t i = 0; i < n && e.pos < e.pcm.size(); ++i, ++e.pos)
      pcm[i] = (int16_t)std::clamp(pcm[i] + e.pcm[e.pos] * vol, -32768.f, 32767.f);
  g_extra.erase(std::remove_if(g_extra.begin(), g_extra.end(), [](const Extra& e) { return e.pos >= e.pcm.size(); }), g_extra.end());
}

// #153 fast exit: stop the output before the process is terminated (no buzz from a half-played buffer)
void audio_stop_for_exit() {
  if (g_stream) SDL_PauseAudioStreamDevice(g_stream);
}

// #161 never latch silent: hw_dsp.cpp calls this when the host queue has not drained for a second while the game's
// audio runs (the device stopped consuming - device change, driver hiccup): clear and resume the stream.
namespace hw {
void audio_recover_stalled_device() {
  if (!g_stream) return;
  SDL_ClearAudioStream(g_stream);
  SDL_ResumeAudioStreamDevice(g_stream);
  logf("[audio] the output stopped draining for 1 s - stream cleared and resumed\n");
}
}  // namespace hw
