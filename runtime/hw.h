#pragma once
#include <cstdint>
#include <vector>

namespace hw {
struct State {
  uint32_t pi_raw, pi_intmsk, pi_fifo_base, pi_fifo_end, pi_fifo_wptr, pi_misc[64];
  uint16_t vi[0x80];
  uint32_t vi_int_pending, vi_vcount, vi_frame;
  uint32_t di_sr, di_cvr, di_cmd[3], di_mar, di_len, di_cr, di_imm;
  bool di_pending_irq;
};
extern State S;

// devices
extern bool g_trace_dsp;
extern bool g_audio_trace;
void dsp_ucode_booted(uint32_t iram_mmem, uint32_t len); void dsp_ucode_mail(uint32_t m); uint32_t dsp_take_cpu_mail(bool* valid);
void dsp_cpu_mail_written(); void dsp_cpu_interrupt(); void dsp_post_mail(uint32_t m); void dsp_raise_dspint(); void dsp_raise_aidint();
extern uint8_t* g_aram;
void dsp_init();   uint16_t dsp_read16(uint32_t off);  void dsp_write16(uint32_t off, uint16_t v);  bool dsp_irq();
uint32_t ai_read(uint32_t off);  void ai_write(uint32_t off, uint32_t v);  bool ai_irq();
void si_init();    uint32_t si_reg_read(uint32_t off); void si_reg_write(uint32_t off, uint32_t v);
void si_dump(); void si_vsync(); uint32_t si_buf_read(uint32_t off); void si_buf_write(uint32_t off, uint32_t v); bool si_irq();
uint16_t mi_read16(uint32_t off);  void mi_write16(uint32_t off, uint16_t v);  bool mi_irq();
uint16_t cp_read16(uint32_t off);  void cp_write16(uint32_t off, uint16_t v);
uint16_t pe_read16(uint32_t off);  void pe_write16(uint32_t off, uint16_t v);
uint32_t cp_pe_intsr();
uint32_t efb_peek(uint32_t ea, int size);  void efb_poke(uint32_t ea, uint32_t v, int size);
void gx_init();
void vsync();
void tick_devices(uint64_t now_ns);
void ai_dma_tick(uint64_t now_ns);
void audio_process_cmdlist(uint32_t addr, uint32_t size);
void audio_submit_dma(uint32_t addr, uint32_t bytes);
int audio_queue_ms();
bool ai_audio_behind(uint64_t now_ns);   // the game owes an audio block (hw_dsp.cpp) - frame_rate.cpp yields to it   // host audio queued (ms); a virtual queue in --quiet runs (audio.cpp)
uint16_t ai_dma_remaining();
void vi_xfb_changed();
}
