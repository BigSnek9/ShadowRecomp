// System-level services shared by the runtime modules.
#pragma once
#include "rt_internal.h"
#include <string>
#include <vector>
#include <cstdio>

typedef void (*HookFn)(Cpu&);
HookFn sys_find_hook(uint32_t addr);
[[noreturn]] void sys_fatal();
void sys_backtrace(Cpu& c);
const GuestFunc* rt_containing_func(uint32_t addr);

// logging
extern bool g_trace_mmio;
void logf(const char* fmt, ...);

// time
extern double g_time_scale;
uint64_t sys_time_ns();            // monotonic host time
uint64_t sys_timebase();           // emulated 40.5 MHz timebase

// memory / boot
bool boot_init(const std::string& disc_root);
extern std::string g_disc_root;

// virtual disc
bool disc_init(const std::string& root);
const std::vector<uint8_t>& disc_fst();
bool disc_read(uint64_t offset, uint8_t* dst, uint32_t len);
std::string disc_describe(uint64_t offset);
bool disc_seg_range(uint64_t offset, uint64_t* start, uint64_t* len);
extern bool g_trace_disc;
extern bool g_quiet;                  // --quiet (tests): window in the background, no sound, user input ignored
extern bool g_boot_story;             // --story: boot straight into the save's story progress (game_hooks.cpp)
extern int g_boot_stage;              // --stage N (tests): boot straight into stage N (game_hooks.cpp)
extern bool g_play_stage;             // --play-stage N: the same start for players (saves written, no auto-press) (main.cpp)
extern bool g_skip_movies;            // auto-tap START while a movie (.sfd) is being streamed
extern uint64_t g_last_movie_read_ns;   // returns false if out of range

// hardware
void hw_init();
void hw_tick(Cpu& c);                       // called from rt_poll
void hw_set_vi_rate(double hz);              // retrace rate (0 = native 59.94 Hz)
void hw_raise_pi(uint32_t bit);
bool hw_irq_pending();
uint32_t hw_irq_status();
void hw_wgp_write(uint32_t v, int size);    // write-gather pipe
void hw_wgp_flush();

// exceptions
enum { EXC_EXTERNAL = 4, EXC_DECREMENTER = 8 };
void sys_deliver_exception(Cpu& c, int which);

void sys_set_watch(bool v);
void sys_dump_state(const char* why);   // backtrace + threads + GX state to the log
[[noreturn]] void sys_exit_guarded(int code);   // #153: stop audio, flush the log, TerminateProcess (no teardown: never blocks on the GPU)
