// Frame rate: the game's own variable time step, driven at any rate.
//
// Shadow's main loop (fn_801E1EB8) paces itself in fn_801E1BD4: it spins on OSGetTime until a whole number of
// 1/rate periods has passed and hands the game "periods x 1/rate" as the frame's time step. The rate lives at
// lbl_8057D6A0+0x50 (60, or 50 on PAL; rewritten every frame from the video mode just before the wait), +0x10 is
// the minimum number of periods per frame and +0x34 the maximum (3 = a 50 ms step at 60). Movement uses that step,
// so raising the rate makes the game simulate and draw more often - real frames, not interpolation.
//
// The video retrace stays at the console's 59.94 Hz: audio, controller polling and the game's retrace-counted timers
// all hang off it (running it faster made them speed up). What limited the frame rate to the retrace was RenderWare's
// frame queue (fn_80494C6C, "show raster"): finished frames wait in a 3-slot ring that the retrace callback empties
// one per retrace, with a GX breakpoint keeping the GPU a frame behind the display. This runtime presents every frame
// the moment it is copied out, so with a custom rate the queue is bypassed: the frame is copied, fenced with its
// draw-sync token, flushed and the copy buffers swapped exactly as the original does - only the wait, the ring entry
// and the breakpoint are left out.
//
// The game's frame counters (+0x88/+0x8C/+0x90, bumped once per frame) also drive animations that ignore the time
// step; they keep counting 60 per second at any rate.
//
// Remaining known differences above 60 (being worked through): objects whose own code adds a fixed amount per frame.
#include "sys.h"
#include "gx_lle.h"
#include "settings_ui.h"
#include "cheats.h"
#include "texture_pack.h"
#include "hw.h"
#include "game.h"
#include "stage_workshop.h"
#include "movement.h"
#include "object_distance.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <cstring>

void f_801E1BD4(Cpu&);   // frame pacing wait
void f_8037866C(Cpu&);   // OSSetThreadPriority(thread, prio)
void widescreen_refresh(Cpu&);   // widescreen.cpp: re-apply FOV / aspect to live cameras
void f_80494C6C(Cpu&);   // RenderWare: queue the finished frame (sleeps while the ring is full)
void f_8039528C(Cpu&);   // GXFlush
void f_80396400(Cpu&);   // GXCopyDisp(dest, clear)
void f_8039561C(Cpu&);   // GXSetDrawSync(token)
void f_80394228(Cpu&);   // GXDisableBreakPt
void f_8032BB24(Cpu&);   // the game's pre-retrace callback

namespace {
constexpr uint32_t kTiming = 0x8057D6A0;
constexpr uint32_t kXfbCur = 0x805F1600, kXfbA = 0x805F15F8, kXfbB = 0x805F15FC, kCopyClear = 0x805F15E0;
constexpr uint32_t kAltMode = 0x805F15F0, kBpArmed = 0x805F1618, kToken = 0x805EDF40;
uint32_t g_ticks60 = 1;   // whole 1/60 s ticks elapsed during the current game frame (custom rate)
float fl32(uint32_t a) { uint32_t v = ld32(a); float r; memcpy(&r, &v, 4); return r; }

// 0 = the game's own pacing (60), otherwise the target rate (unlimited = 1000 Hz periods)
int target_rate() {
  static const int env_rate = getenv("GAME_RATE") ? atoi(getenv("GAME_RATE")) : -1;
  // Going back from the custom path (frame queue bypassed) to the original queue mid-session hung (120 -> 60 in the
  // menu) and scrolling through the values crashed (each pass through 60 switched paths). So once a custom rate
  // was used this session, 60 stays on the custom path at 60. A session that only ever runs at 60 uses the original.
  static bool used_custom = false;
  int fps = env_rate >= 0 ? env_rate : settings::g.fps_target;
  // #122: in-stage events and story scenes animate from the game's 60-per-second frame counters, not from the
  // time step, so above 60 fps a cutscene repeats frames (one in four or five held at 165, measured) - judder. While a
  // cutscene plays the rate is held at cutscene_fps_cap (default 60; 0 = no cap).
  const int cap = settings::g.cutscene_fps_cap;
  if (cap > 0 && (fps <= 0 || fps > cap) && game::cutscene_active()) fps = cap;
  if (fps == 60) return used_custom ? 60 : 0;
  used_custom = true;
  if (fps <= 0) return 1000;
  return std::clamp(fps, 20, 1000);
}
}  // namespace

double g_dt60 = 1;

uint32_t g_plr_owner = 0;   // the player object hle_PlayerMoveStep last moved (PLAYER_LOG)
void hle_FramePace(Cpu& c) {
  stage_workshop::frame_begin(c);
  int rate = target_rate();
  gxlle::g_gp_no_halt = rate != 0;   // safety: a GP parked >100 ms passes its breakpoint (gx_lle.cpp gp_run)
  if (rate) {
    st32(kTiming + 0x50, (uint32_t)rate);
    // keep the longest single step at the original 50 ms (3 periods of 1/60 s)
    st32(kTiming + 0x34, (uint32_t)std::max(3, rate / 20));
  }
  // Controller samples are latched per retrace and consumed by the read; at more than one game frame per retrace the
  // game's pad code saw "no new data" and dropped presses (menus ignored A at 120 fps). Sample every game frame.
  if (rate) hw::si_vsync();
  uint32_t before[3];
  for (int i = 0; i < 3; ++i) before[i] = ld32(kTiming + 0x88 + 4 * i);
  // In the original the main thread sleeps on RenderWare's frame queue every frame, which is when loader and disc
  // threads get to run. Without the queue it only spins in this wait, starving them (a disc read timed out -> the
  // "An error has occurred" screen). So it waits at the lowest priority: anything ready runs first, as if it slept.
  uint32_t self = ld32(0x800000E4), prio = self ? ld32(self + 0x2D4) : 0;
  bool yield = rate && self;
  if (yield) { Cpu saved = c; c.r[3] = self; c.r[4] = 31; f_8037866C(c); c = saved; }
  uint64_t t0 = sys_time_ns();
  // Audio first. The sound threads share the one host thread with rendering; at a saturated frame rate (unlimited,
  // or a light menu at hundreds of fps) they answered the audio interrupts too late and the output stuttered. When
  // the game owes an audio block (hw_dsp.cpp ai_audio_behind), stay at this lowest priority and take safe points
  // (interrupts, thread switches) for up to 4 ms so they catch up. This is before the game's own wait reads the clock,
  // so the frame's measured length - and game time - include it. With spare time per frame it never triggers.
  if (yield) {
    static const bool no_audio_first = getenv("FR_NO_AUDIO_FIRST") != nullptr;   // A/B tests
    uint64_t start = sys_time_ns(), now = start;
    while (!no_audio_first && hw::ai_audio_behind(now) && now - start < 4000000ull) {
      c.budget = 0;
      rt_poll(c, 0x801E1BD4u);
      now = sys_time_ns();
    }
    static uint64_t waited_ns = 0, since = start;
    waited_ns += now - start;
    static const bool stats = getenv("AUDIO_STATS") != nullptr;
    if (stats && start - since > 2000000000ull) {
      logf("[audio-first] main loop yielded %.0f ms in the last %.1f s\n", waited_ns / 1e6, (start - since) / 1e9);
      waited_ns = 0;
      since = start;
    }
  }
  f_801E1BD4(c);
  gxlle::g_prof_ns[3] += sys_time_ns() - t0;
  if (yield) {
    uint32_t r3 = c.r[3], r4 = c.r[4];
    c.r[3] = self; c.r[4] = prio;
    f_8037866C(c);
    c.r[3] = r3; c.r[4] = r4;
  }
  // dtscale.txt: per-frame object steps (moving platforms etc.) scaled by this frame's length in 1/60 s
  g_dt60 = 1;
  static const bool no_dtscale = getenv("FR_NO_DTSCALE") != nullptr;   // bisecting
  if (rate && !no_dtscale) g_dt60 = std::min(3.0, (double)ld32(kTiming + 0x0C) * 60.0 / rate);   // elapsed periods of 1/rate s
  if (rate) {
    static double acc = 0;
    acc += (double)ld32(kTiming + 0x0C) * 60.0 / rate;   // elapsed periods of 1/rate s -> 1/60 s units
    uint32_t n = (uint32_t)acc;
    acc -= n;
    g_ticks60 = n;
    static const bool no_counters = getenv("FR_NO_COUNTERS") != nullptr;   // bisecting
    if (!no_counters)
      for (int i = 0; i < 3; ++i)
        if (ld32(kTiming + 0x88 + 4 * i) != before[i]) st32(kTiming + 0x88 + 4 * i, before[i] + n);
  }
  cheats::apply_frame();
  texture_pack::tick();
  if (!getenv("NO_CAM_REFRESH")) widescreen_refresh(c);
  // OBJ_LOG=<vtable hex>:<offset hex> (debugging): stage time + that float field of every live object of the class,
  // once per frame - for measuring how fast an object moves per second at different frame rates.
  static const char* olog = getenv("OBJ_LOG");
  if (olog) {
    static uint32_t vt = (uint32_t)strtoul(olog, nullptr, 16), off = strchr(olog, ':') ? (uint32_t)strtoul(strchr(olog, ':') + 1, nullptr, 16) : 0;
    static uint32_t objs[8], nobj = 0, wait = 0;
    if (wait-- == 0) {
      wait = 120; nobj = 0;
      for (uint32_t a = 0x80600000; a < 0x80000000u + g_mem1_size && nobj < 8; a += 4) if (ld32(a) == vt) objs[nobj++] = a;
    }
    char line[512]; int n = snprintf(line, sizeof line, "[obj] st=%.4f", fl32(0x8057D734));
    for (uint32_t i = 0; i < nobj; ++i) if (ld32(objs[i]) == vt) n += snprintf(line + n, sizeof line - n, " %08X:%.3f", objs[i], fl32(objs[i] + off));
    if (nobj) logf("%s\n", line);
  }
  // PLAYER_LOG=1 (debugging): one line per frame with the player's position, velocity, flags. The player object is
  // found by its Player::PlayerShadow vtable (80521C50), which sits 0x38 into the object the behaviours use.
  static const char* plog = getenv("PLAYER_LOG");
  extern uint32_t g_plr_owner;   // set by hle_PlayerMoveStep: the moving player, found at once (the scan below can lag)
  static uint32_t o = 0, scan_wait = 0;
  if (plog && g_plr_owner && ld32(g_plr_owner + 0x38) == 0x80521C50) o = g_plr_owner;
  if (plog && (!o || ld32(o + 0x38) != 0x80521C50) && scan_wait-- == 0) {
    o = 0;
    scan_wait = 60;
    for (uint32_t a = 0x80600000; a < 0x80000000u + g_mem1_size; a += 4)
      if (ld32(a) == 0x80521C50 && ld32(a + 4) == 0x53686164) { o = a - 0x38; break; }   // "Shad"
  }
  // PLAYER_DUMP=1 (research, start yaw): 3 s after the first player line, every word of the player object 0..0x800 once
  static const bool pdump = getenv("PLAYER_DUMP") != nullptr;
  static int pdump_wait = pdump ? std::max(1, atoi(getenv("PLAYER_DUMP")) > 1 ? atoi(getenv("PLAYER_DUMP")) : 180) : 0;
  if (pdump && plog && o && ld32(o + 0x38) == 0x80521C50 && pdump_wait >= 0 && pdump_wait-- == 0) {
    for (uint32_t off = 0; off < 0x800; off += 4) {
      const uint32_t w = ld32(o + off); float f; memcpy(&f, &w, 4);
      logf("[pdump] +%03X %08X %g\n", off, w, std::isfinite(f) ? f : 0.f);
    }
  }
  if (plog && o && ld32(o + 0x38) == 0x80521C50) {
    auto f = [](uint32_t a) { uint32_t v = ld32(a); float r; memcpy(&r, &v, 4); return r; };
    uint32_t beh = ld32(o + 0x238);
    logf("[plr] t=%u dt=%.4f pos=%.1f,%.1f,%.1f vel=%.1f,%.1f,%.1f acc=%.1f,%.1f,%.1f a0=%X a4=%X fl=%X,%X,%X,%X lk=%u,%u t8c=%.2f beh=%08X\n",
         ld32(kTiming + 0x88), f(ld32(o + 0x60)), f(o), f(o + 4), f(o + 8), f(o + 0x18), f(o + 0x1C), f(o + 0x20),
         f(o + 0x24), f(o + 0x28), f(o + 0x2C), ld32(o + 0xA0), ld32(o + 0xA4), ld32(o + 0xA8), ld32(o + 0xAC),
         ld32(o + 0xB0), ld32(o + 0xB4), ld32(o + 0x88), ld32(o + 0x90), f(o + 0x8C), beh ? ld32(beh) : 0);
  }
}

void hle_RwShowRaster(Cpu& c) {
  static const bool no_bypass = getenv("FR_NO_BYPASS") != nullptr;   // bisecting
  if (!target_rate() || ld32(kAltMode) != 0 || no_bypass) { f_80494C6C(c); return; }
  // custom rate: no queue. Leave the breakpoint off (the GPU must never wait for a retrace).
  if (ld32(kBpArmed)) { f_80394228(c); st32(kBpArmed, 0); }
  f_8039528C(c);
  c.r[3] = ld32(kXfbCur);
  c.r[4] = ld32(kCopyClear) & 0xFF;
  f_80396400(c);
  uint32_t token = ld16(kToken);
  c.r[3] = token;
  f_8039561C(c);
  st16(kToken, (token + 1) % 0xE000);
  f_8039528C(c);
  st32(kXfbCur, ld32(kXfbCur) == ld32(kXfbA) ? ld32(kXfbB) : ld32(kXfbA));
  c.r[3] = 1;
}


// Player ground contact (fn_8009728C, the player's move + collision step, called by every player behaviour).
void f_8009728C(Cpu&);
// After moving, the floor test is two sweeps, the frame's movement sweep and a
// short sweep down to the probe's hit, and the player stays grounded if either finds floor. A collision seam a few
// units long past the third dash panel defeats the down sweep; at 60 fps the next frame's 12.5-unit movement sweep
// spans it, at 120 two 6-unit frames both fall inside it -> grounded cleared. So the original effectively ignores
// floor gaps shorter than one 1/60 s frame; this keeps that as time: losing the floor while grounded and not moving
// up (jumps unaffected) is ignored until it has lasted 1/60 s.
namespace {
struct GroundGrace { uint32_t owner; float lost; };
GroundGrace g_grace[4];
}
void hle_PlayerMoveStep(Cpu& c) {
  float dt = fl32(kTiming + 0x18);
  static const bool off = getenv("FR_NO_GROUNDFIX") != nullptr;   // bisecting
  bool fix = !off && dt > 0 && dt < 1.0f / 60;
  uint32_t owner = ld32(c.r[3]);
  if (owner >= 0x80000000u && owner < RT_RAM_END - 0x400 && ld32(owner + 0x38) == 0x80521C50) g_plr_owner = owner;
  stage_workshop::before_player(owner);
  movement::frame(owner);
  object_distance::apply();   // slider changes reach the records not created yet
  bool was_grounded = ld32(owner + 0xA8) & 4;   // flag 2
  game::player_stepped();
  f_8009728C(c);
  stage_workshop::after_player(owner, dt);
  GroundGrace* g = nullptr;
  for (auto& e : g_grace) if (e.owner == owner) g = &e;
  if (!g) for (auto& e : g_grace) if (!e.owner) { g = &e; g->owner = owner; break; }
  if (!g) { g = &g_grace[0]; g->owner = owner; g->lost = 0; }
  uint32_t flags = ld32(owner + 0xA8);
  if (flags & 4) { g->lost = 0; return; }
  if (!fix || !was_grounded) { g->lost = 0; return; }
  float up = fl32(owner + 0x18) * fl32(owner + 0x1C8) + fl32(owner + 0x1C) * fl32(owner + 0x1CC) +
             fl32(owner + 0x20) * fl32(owner + 0x1D0);
  g->lost += dt;
  if (up <= 1.0f && g->lost < 1.0f / 60 - 1e-4f) st32(owner + 0xA8, flags | 4);
}
void f_800979E8(Cpu&);   // player ray cast (returns hit distance in f1, < 0 = none)
void hle_PlayerRay(Cpu& c) {   // MOVE_LOG debugging only
  uint32_t lr = c.lr;
  f_800979E8(c);
  static const bool log = getenv("MOVE_LOG") != nullptr;
  if (log && lr >= 0x8009728C && lr < 0x800979A0) logf("[ray] from %08X -> %.3f\n", lr, c.f[1]);
}
void f_800986F0(Cpu&);   // player sweep (returns hit in r3; out: position r3-arg, normal ...)
void hle_PlayerSweep(Cpu& c) {   // MOVE_LOG debugging only
  uint32_t lr = c.lr, out = c.r[3], from = c.r[8], vec = c.r[9];
  auto f = [](uint32_t a) { uint32_t v = ld32(a); float r; memcpy(&r, &v, 4); return r; };
  float v0 = f(vec), v1 = f(vec + 4), v2 = f(vec + 8), p1 = f(from + 4);
  f_800986F0(c);
  static const bool log = getenv("MOVE_LOG") != nullptr;
  if (log && lr >= 0x8009728C && lr < 0x800979A0)
    logf("[sweep] from %08X y=%.3f vec=%.3f,%.3f,%.3f -> %u out=%.3f,%.3f,%.3f\n", lr, p1, v0, v1, v2, c.r[3] & 0xFF,
         f(out), f(out + 4), f(out + 8));
}

// Rocket ride (Rocket::Rocket, fn_80115C60, called every frame while flying): position += velocity (+0x4C) through
// the shared vector add fn_8040CFF8, then "arrived" when the target is within one step. One step per frame -> the
// ride flew fps/60 times too fast. The velocity is scaled to this frame's length for the duration of the call
// (g_dt60 = 1 at 60 fps, so 60 is unchanged).
void f_80115C60(Cpu&);
void hle_RocketStep(Cpu& c) {
  uint32_t vel = c.r[3] + 0x4C, saved[3];
  for (int i = 0; i < 3; ++i) saved[i] = ld32(vel + 4 * i);
  if (g_dt60 != 1)
    for (int i = 0; i < 3; ++i) {
      float v = fl32(vel + 4 * i) * (float)g_dt60;
      uint32_t w; memcpy(&w, &v, 4); st32(vel + 4 * i, w);
    }
  f_80115C60(c);
  for (int i = 0; i < 3; ++i) st32(vel + 4 * i, saved[i]);
}

// The sound library's per-frame update (fn_801D82EC -> fn_803C46C0) advances its clock (envelopes, sequencing,
// repeating menu sounds) by one tick per call: with a custom rate it runs once per elapsed 1/60 s instead.
void f_801D82EC(Cpu&);
// The sound manager's per-sound update (fn_801D5ED0, called once per game frame from fn_801D6094): every live sound entry
// counts a frame countdown down by one per call (byte +0x2C of its state, fn_801D6CF4) and the voice is stopped when it
// runs out. Unthrottled, sounds lasted 60/fps of their length: at 240 fps a 440 ms menu sound was cut off after 125 ms.
// Like hle_SoundFrame it runs once per elapsed 1/60 s (exactly once per frame at 60 fps).
void f_801D5ED0(Cpu&);
void hle_SoundEntries(Cpu& c) {
  static const bool no_sound = getenv("FR_NO_SOUND") != nullptr;   // bisecting
  if (!target_rate() || no_sound) { f_801D5ED0(c); return; }
  const uint32_t r3 = c.r[3], r4 = c.r[4], lr = c.lr;
  for (uint32_t i = 0; i < g_ticks60 && i < 4; ++i) {
    c.r[3] = r3; c.r[4] = r4; c.lr = lr;
    f_801D5ED0(c);
  }
}

// SND_LOG=1 (debugging): every sound request (fn_801D560C: r4 low 16 bits = sound id) with the time and caller.
void f_801D560C(Cpu&);
void hle_SndRequest(Cpu& c) {
  static const bool on = getenv("SND_LOG") != nullptr;
  if (on) logf("[snd] t=%.3f id=%04X r5=%X r6=%X lr=%08X\n", (double)sys_time_ns() / 1e9, c.r[4] & 0xFFFF, c.r[5], c.r[6], c.lr);
  f_801D560C(c);
}

void hle_SoundFrame(Cpu& c) {
  static const bool no_sound = getenv("FR_NO_SOUND") != nullptr;   // bisecting
  if (!target_rate() || no_sound) { f_801D82EC(c); return; }
  for (uint32_t i = 0; i < g_ticks60 && i < 4; ++i) f_801D82EC(c);
}
