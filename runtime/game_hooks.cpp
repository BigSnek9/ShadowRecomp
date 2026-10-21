// Gameplay-level hooks: in-engine cutscenes ("events").
//
// Story cutscenes run as a stage-sequence phase, StageSequence::PlayEvent::Phase (vtable 8055835C, typeinfo
// 805EC250). Its vtable slots: fn_802D06B0 = destructor, fn_802D068C = per-frame update (-> fn_802D0810 on the
// impl at +4), fn_802D0668 = second per-frame hook (-> fn_802D07E0). A cutscene is "playing" while the update runs.
//
// fn_80173034 is the in-stage event player's update (this +0x28 state 0..8, +0x34 skip request); EVENT_TRACE=1
// logs both.
#include "sys.h"
#include "gx_lle.h"
#include "settings_ui.h"
#include "input.h"
#include "game.h"
#include "stage_workshop.h"
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>

void f_80173034(Cpu&);
void f_802D068C(Cpu&);
void f_802D0668(Cpu&);

namespace {
const bool g_event_trace = getenv("EVENT_TRACE") != nullptr;
uint64_t g_event_last_ns = 0;   // last PlayEvent update
uint64_t g_evmgr_last_ns = 0;   // last in-stage event player update
uint32_t g_evmgr_state = 0;
// #87: a new in-stage event player's first update (state 0, before its state 1) - the frame the scene's subtitles are
// sized from the camera window; counted as cutscene so the FOV setting is not in that window (FOV 150% gave 1.5x text
// with the second line below the picture). Only a NEW player object counts: one that idles after its event does not.
uint64_t g_evmgr_start_ns = 0;
}  // namespace

extern uint64_t g_last_movie_read_ns;   // hw.cpp: last read of a .sfd movie stream

namespace game {
// Story cutscenes in this game are played by the in-stage event player (fn_80173034; e.g. Black Doom's scenes run in
// their own small "event stage", stg0005): state 1-4 load/fade in, 5 = playing, 6-8 wrap up. The StageSequence
// PlayEvent phase is also checked.
bool cutscene_active() {
  uint64_t now = sys_time_ns();
  if (g_last_movie_read_ns && now - g_last_movie_read_ns < 1500000000ull) return true;
  if (g_event_last_ns && now - g_event_last_ns < 250000000ull) return true;
  if (g_evmgr_start_ns && now - g_evmgr_start_ns < 250000000ull) return true;   // #87: an event player's first frame
  return g_evmgr_last_ns && now - g_evmgr_last_ns < 250000000ull && g_evmgr_state >= 1 && g_evmgr_state <= 7;
}
}  // namespace game

void hle_PlayEventUpdate(Cpu& c) {
  if (g_event_trace && !game::cutscene_active())
    logf("[event] PlayEvent phase running (this=%08X impl=%08X)\n", c.r[3], ld32(c.r[3] + 4));
  g_event_last_ns = sys_time_ns();
  f_802D068C(c);
}

void hle_PlayEventUpdate2(Cpu& c) {
  static int n = 0;
  if (g_event_trace && n++ < 3) logf("[event] PlayEvent slot2 (this=%08X)\n", c.r[3]);
  f_802D0668(c);
}

// ---- Select Mode: "Unlock every stage" (settings; this session only, never saved)
//
// The save data lives twice in RAM (80576958 and 805788B4, 0x1F5C apart; the card copy and the working copy). After a
// 0xA8-byte header come per-stage blocks of 0x60 bytes at +0xA8 (80576A00): +0x00 = "reached" flag (the stage
// select grid shows a stage when it is set), then the mission records (clear flag, rank, best time, score).
// AdvReal::SelectMode::Phase (vtable 80557AC4: fn_802C4204 destructor, fn_802C41E0 update) is wrapped: while the
// option is on, every block's flag is set when the phase runs and the original flags are put back when it ends, so
// nothing reaches the memory card (the game only saves after a stage).
void f_802C41E0(Cpu&);
void f_802C4204(Cpu&);
namespace {
constexpr uint32_t kSaveCopies[2] = {0x80576958, 0x805788B4};
constexpr uint32_t kStageBlocks = 0xA8, kStageStride = 0x60, kStageCount = 59;   // the stage table's 59 rows (+0x16C8.. after them = story / weapon bits)
bool g_unlock_applied = false;
uint32_t g_unlock_saved[2][kStageCount];

void unlock_apply() {
  if (g_unlock_applied) return;
  for (int c = 0; c < 2; ++c)
    for (uint32_t i = 0; i < kStageCount; ++i) {
      uint32_t a = kSaveCopies[c] + kStageBlocks + i * kStageStride;
      g_unlock_saved[c][i] = ld32(a);
      if (!g_unlock_saved[c][i]) st32(a, 1);
    }
  g_unlock_applied = true;
  logf("[game] select mode: every stage unlocked for this visit (restored on leaving)\n");
}
void unlock_restore() {
  if (!g_unlock_applied) return;
  for (int c = 0; c < 2; ++c)
    for (uint32_t i = 0; i < kStageCount; ++i) st32(kSaveCopies[c] + kStageBlocks + i * kStageStride, g_unlock_saved[c][i]);
  g_unlock_applied = false;
  logf("[game] select mode: stage flags restored\n");
}
}  // namespace

static bool g_unlock_used = false;
bool exi_card_write_protect() { return g_boot_stage > 0 && !g_play_stage; }   // exi_card.cpp: no card writes in --stage test runs (--play-stage saves)
// the unlock never reaches a save: the game's save writers (fn_80299130 copies 0x1F5C bytes of the save data, and
// fn_8029986C) run with the real flags put back, then the unlock is applied again (real saves stay allowed)
void f_80299130(Cpu&);
void f_8029986C(Cpu&);
static void save_guarded(Cpu& c, void (*fn)(Cpu&)) {
  const bool was = g_unlock_applied;
  if (was) logf("[game] save while stages are unlocked: real flags restored for the save\n");
  unlock_restore();
  fn(c);
  if (was && settings::g.unlock_all_stages) unlock_apply();
}
void hle_SaveWrite(Cpu& c) { save_guarded(c, f_80299130); }
void hle_SaveWrite2(Cpu& c) { save_guarded(c, f_8029986C); }
static void boot_stage_from_select(Cpu& c);   // --stage N, below

void hle_SelectModeUpdate(Cpu& c) {
  if (settings::g.unlock_all_stages) { unlock_apply(); g_unlock_used = true; }
  else unlock_restore();
  f_802C41E0(c);
  boot_stage_from_select(c);
}
// constructor (fn_802C42DC): the grid is built here, so the flags must already be set
void f_802C42DC(Cpu&);
void hle_SelectModeCtor(Cpu& c) {
  if (settings::g.unlock_all_stages) { unlock_apply(); g_unlock_used = true; }
  f_802C42DC(c);
}
// --stage N (tests): boot straight into stage N with the save loaded, through the game's own Select Mode launcher.
//  1. boot screens auto-confirmed (--fast presses) until the title (AdvReal::Title::Phase, update fn_802BCB38) runs;
//  2. the title asks the menu folder for phase 6 = Select Mode: the folder update fn_802B6470 gets its state in r3,
//     whose word +0 is the requested phase (+4 the current one; jump table 805576C8: 1 title, 6 select mode, ...);
//  3. in Select Mode, A is pressed (g_auto_buttons) until the game's own A path runs: Select Mode's impl update
//     (fn_802C7B14, at 802C7804) checks it is ready for input, calls the launcher fn_802C4334, then fn_802C7AAC(impl,
//     3, 1) (state 3 = leaving for the stage, starts the fade). Calling the launcher directly is not enough: without
//     the state change Select Mode stays up (it once "worked" by timing luck).
//  4. the launcher is wrapped: after it stored the cursor's stage in the stage request lbl_80584720+0, N replaces it
//     (stage numbers as in the stage table lbl_804C4A18: 100 = Westopolis, 402 = Sky Troops, 502 = Iron Jungle...;
//     the stage files are stg%04d). The rest of the launcher (fn_802062FC resets a 0x50-byte block) does not read it.
// See NOTES-GAME.md "Starting a stage" for the call chain.
void f_802BCB38(Cpu&);   // AdvReal::Title::Phase update
void f_802B6470(Cpu&);   // menu folder update (r3 = its state: +0 requested phase, +4 current)
void f_802C4334(Cpu&);   // Select Mode: launch the selected stage
static uint32_t g_adv_state = 0;
// #117: the menu folder (fn_802B6470 state +4 = current phase) is 0 while any stage runs, whatever the player does
// (in_stage() needs the player's move step, which a ledge hang, poles and rails skip); before the folder exists: menus
bool game::menus_active() { return !g_adv_state || ld32(g_adv_state + 4) != 0; }
static int g_boot_step = 0;   // 0 waiting for the title, 1 Select Mode requested, 2 launched
bool game::stage_boot_loading() { return g_boot_stage>1 && g_boot_step<3; }
void hle_SelectLaunch(Cpu& c) {
  f_802C4334(c);
  if (g_boot_stage > 0 && g_boot_step == 1) {
    logf("[game] --stage %d: launcher ran (cursor stage %u), stage request -> %d\n", g_boot_stage, ld32(0x80584720), g_boot_stage);
    st32(0x80584720, (uint32_t)g_boot_stage);
    g_boot_step = 2;
    g_auto_buttons = 0;
    g_mash = false;
    g_skip_movies = false;   // no movie-skip START taps into the stage (they open the pause menu)
  }
}
// ---- --stage N, direct start (N > 1): no title, no Select Mode -----------------------------------------------------
// Menu folder phases at boot: 9 (start-up: save loaded, systems up) -> 10 (warning screens) -> 8 (opening movie) ->
// 1 (title) -> 6 (Select Mode) -> 0 (in game). Asking for another phase during 9 hangs; from 10 on it is safe.
// When 10 becomes current we do what Select Mode does when you press A on a stage, then ask for phase 0:
//  1. point Select Mode's cursor (lbl_80584560: +0 column, +4 row, +8/+0x10 mode flags) at the grid cell of stage N.
//     The launcher looks its stage-table row up through the 6x5 grid of row indexes at 0x804FF688 (fn_802BF3C0);
//     with the never-initialised cursor it gets row -1 and the stage starts with a bogus setup.
//  2. run the launcher fn_802C4334 (hle_SelectLaunch: play mode, stage request -> N, stage block reset). It only reads
//     globals; the CPU registers are restored afterwards because we call it from inside the folder update.
//  3. stop all automatic presses: the opening movie streamed moments ago, and the movie-skip START taps (input.cpp,
//     active 4 s after a movie read) would land in the stage and open the pause menu.
// Stages missing from the stage table, and SHADOW_BOOT_VIA_SELECT=1, use the older Select Mode route instead.
static bool boot_direct_start(Cpu& c, uint32_t state) {
  bool placed = false;
  for (uint32_t x = 0; x < 6 && !placed; ++x)
    for (uint32_t y = 0; y < 5 && !placed; ++y) {
      const int32_t row = (int32_t)ld32(0x804FF688u + x * 20 + y * 4);
      if (row >= 0 && row < 128 && (int32_t)ld32(0x804C4A18u + (uint32_t)row * 0x50) == g_boot_stage) {
        st32(0x80584560u, x); st32(0x80584564u, y); st8(0x80584568u, 0); st8(0x80584570u, 0);
        placed = true;
      }
    }
  // Not in the grid (boss / extra stages): borrow cell (0,0) for the launcher, pointing it at stage N's table row,
  // and put the original value back right after.
  uint32_t borrowed = 0;
  bool borrow = false;
  if (!placed) {
    for (uint32_t row = 0; row < 128 && !placed; ++row)
      if ((int32_t)ld32(0x804C4A18u + row * 0x50) == g_boot_stage) {
        borrowed = ld32(0x804FF688u);
        st32(0x804FF688u, row);
        st32(0x80584560u, 0); st32(0x80584564u, 0); st8(0x80584568u, 0); st8(0x80584570u, 0);
        placed = borrow = true;
      }
  }
  if (!placed) return false;
  g_mash = false;
  g_skip_movies = false;
  g_auto_buttons = 0;
  const Cpu saved = c;
  g_boot_step = 1;
  hle_SelectLaunch(c);
  c = saved;
  if (borrow) st32(0x804FF688u, borrowed);
  st32(state, 0);
  logf("[game] --stage %d: direct start (no title / Select Mode)\n", g_boot_stage);
  return true;
}
// ---- --story: continue the save's story (no title, no main menu) ------------------------------------------------------
// What the Synopsis phase does when you confirm "continue" (fn_803416C4, docs/launcher/RUNTIME-PREP.md section 2):
// start the stage sequence fn_801E5858(fn_801E2128(), 5), sequence type 1 = story fn_802CF1DC(1), 0x80584724 = 1
// (continue the current story), reset the stage block fn_802062FC(0x8057D76C) and fn_800CBBFC(0x8057D7E8, 1, 3), then
// ask the menu folder for phase 0. Run when phase 10 (warning screens) is current: the save is loaded by then.
void f_801E2128(Cpu&);
void f_801E5858(Cpu&);
void f_802CF1DC(Cpu&);
void f_802062FC(Cpu&);
void f_800CBBFC(Cpu&);
static void boot_story(Cpu& c, uint32_t state) {
  g_mash = false;
  g_skip_movies = false;
  g_auto_buttons = 0;
  // testing: TEST_STORY_PATH="5,7" = a mid-story save (story path rows at save +0x34, -1 = in progress; a finished story
  // ends with -2 and continues from the start), written into both RAM copies of the loaded save
  if (const char* e = getenv("TEST_STORY_PATH")) {
    const uint32_t live = ld32(0x805F0390u);
    const uint32_t bases[3] = {0x80576958u, 0x805788B4u, live >= 0x80000000u && live < RT_RAM_END ? live : 0x80576958u};
    uint32_t o = 0x34;
    for (const char* q = e; *q && o < 0xA4; o += 4) {
      for (uint32_t base : bases) st32(base + o, (uint32_t)atoi(q));
      const char* n = strchr(q, ',');
      if (!n) { o += 4; break; }
      q = n + 1;
    }
    for (uint32_t base : bases) { st32(base + o, 0xFFFFFFFFu); st32(base + 0x30, (o - 0x34) / 4 - 1); }   // index = the last entry
    logf("[game] --story test: story path set to %s, -1 (live save at %08X)\n", e, live);
  }
  if (const char* e = getenv("TEST_STORY_WORD")) {   // testing: "off=value" into both save copies (finding the resume word)
    unsigned off = 0; int v = 0;
    if (sscanf(e, "%x=%d", &off, &v) == 2 && off < 0x1F5C) {
      for (uint32_t base : {0x80576958u, 0x805788B4u}) st32(base + off, (uint32_t)v);
      logf("[game] --story test: save +0x%X = %d\n", off, v);
    }
  }
  // the resume point (proven): with continue = 1 the story sequence init fn_802D20C0 reads the save's story index
  // (+0x30) and starts at the story path entry path[index] (+0x34 + 4 * index, a stage-table row), the previous stage
  // = path[index - 1]. A new story (mode 0) writes index 0 / path[0] = 5 (Westopolis) instead.
  const int32_t idx = (int32_t)ld32(0x80576958u + 0x30);
  const int32_t cur = idx >= 0 && idx < 29 ? (int32_t)ld32(0x80576958u + 0x34 + 4 * (uint32_t)idx) : -1;
  logf("[game] --story: save story index %d -> row %d (stage %d)\n", idx, cur, cur >= 0 && cur < 128 ? (int32_t)ld32(0x804C4A18u + (uint32_t)cur * 0x50) : -1);
  // proven on a real mid-story card (tests/fixtures/story_mid_card.raw, index 1): story event stg0904 -> cutscene in
  // stg0100 -> Glyphic Canyon stg0201, the same order as through the game's own Synopsis phase
  const Cpu saved = c;
  c.lr = 0;
  f_801E2128(c);
  c.r[4] = 5;   // the sequence's state (fn_801E5858 stores it), as the game's continue
  f_801E5858(c);
  c.r[3] = 1;
  f_802CF1DC(c);
  st32(0x80584724u, 1);
  c.r[3] = 0x8057D6A0u + 0xCC;
  f_802062FC(c);
  c.r[3] = 0x8057D6A0u + 0x148; c.r[4] = 1; c.r[5] = 3;
  f_800CBBFC(c);
  c = saved;
  st32(state, 0);
  g_boot_step = 2;
  logf("[game] --story: continue the save's story (no title / main menu); stage request %u\n", ld32(0x80584720));
}
static void boot_via_select(uint32_t state, const char* why) {
  st32(state, 6);
  g_boot_step = 1;
  g_mash = false;
  logf("[game] --stage %d: opening Select Mode (%s)\n", g_boot_stage, why);
}
static void boot_leave_menus(Cpu& c, uint32_t state) {
  static const bool via_select = getenv("SHADOW_BOOT_VIA_SELECT") != nullptr;
  if (via_select) boot_via_select(state, "SHADOW_BOOT_VIA_SELECT");
  else if (!boot_direct_start(c, state)) boot_via_select(state, "stage not in the Select Mode grid");
}
void hle_AdvFolderUpdate(Cpu& c) {
  g_adv_state = c.r[3];
  if (g_boot_story && g_boot_step == 0 && ld32(g_adv_state + 4) == 10 && ld32(g_adv_state) == 10) boot_story(c, g_adv_state);
  if (g_boot_stage > 0 || g_boot_story) {
    const uint32_t req = ld32(g_adv_state), cur = ld32(g_adv_state + 4);
    static const bool boot_log = getenv("BOOT_LOG") != nullptr;   // phase changes, for boot work
    static uint32_t last_req = ~0u, last_cur = ~0u;
    if (boot_log && (req != last_req || cur != last_cur)) { logf("[game] adv folder: requested %u current %u\n", req, cur); last_req = req; last_cur = cur; }
    if (g_boot_stage > 1 && g_boot_step == 0 && cur == 10 && req == 10) boot_leave_menus(c, g_adv_state);
  }
  f_802B6470(c);
}
void hle_TitleUpdate(Cpu& c) {
  f_802BCB38(c);
  static int frames = 0;
  if (g_boot_stage > 0 && g_boot_step == 0) {
    g_mash = false;               // the title is up: no more automatic presses (they would open its menus)
    if (++frames >= 5 && g_adv_state) {
      // only reached if start-up never showed phase 10 (stage 1 always comes here: tests drive its Select Mode)
      if (g_boot_stage > 1) boot_leave_menus(c, g_adv_state);
      else boot_via_select(g_adv_state, "--stage 1");
    }
  }
}
static uint64_t g_player_step_ns = 0;
void game::player_stepped() {   // tools\stage.ps1 waits for the --stage line
  g_player_step_ns = sys_time_ns();
  if (g_boot_step != 2) return;
  g_boot_step = 3;
  if (g_boot_story) logf("[game] --story: player active in stage %u\n", ld32(0x80584720));
  logf("[game] --stage %d: player active\n", g_boot_stage);
  stage_workshop::memory_report(g_boot_stage);   // [mem] free MEM1 per game heap (memory budget)
  // MEM_DUMP_ACTIVE=<file> (testing): all of MEM1 at this moment, to diff game state between boot routes
  if (const char* fn = getenv("MEM_DUMP_ACTIVE"))
    if (FILE* df = fopen(fn, "wb")) { fwrite(g_ram, 1, g_mem1_size, df); fclose(df); logf("[mem] dumped MEM1 to %s\n", fn); }
}
static void boot_stage_from_select(Cpu&) {
  static int frames = 0;
  if (g_boot_stage <= 0 || g_boot_step != 1) return;
  if (g_boot_stage == 1) {   // --stage 1: just open Select Mode (tests drive it with INPUT_FILE)
    g_boot_step = 3;
    g_skip_movies = false;   // its START taps would confirm a stage (Select Mode streams from disc too)
    logf("[game] --stage 1: Select Mode open\n");
    return;
  }
  ++frames;
  g_auto_buttons = (frames >= 10 && (frames / 6) % 2) ? PAD_A : 0;   // A pulses (6 frames down / 6 up) until it launches
}

// ---- In-stage pause menu (see NOTES-GAME.md "Pause menu")
// fn_801E3404 = the pause menu's per-frame input handler (r3 = the pause object): +0x38 cursor (0 resume, 1 restart,
// 2 quit; +0x3D set = restart is skipped), +0x34 selected mission (left/right, 3 entries of 12 bytes, -1 = none).
// It returns 0 to stay paused, otherwise the choice code from fn_801E333C. The words are sprites in the pause_main
// texture atlas (CSD layout CsdFiles/pause/pause.gncp), not text.
void f_801E3404(Cpu&);
static uint64_t g_pause_ns = 0;
static uint32_t g_pause_obj = 0;
void hle_PauseUpdate(Cpu& c) {
  g_pause_ns = sys_time_ns();
  g_pause_obj = c.r[3];
  static const bool plog = getenv("PAUSE_LOG") != nullptr;
  if (plog) {   // PAUSE_LOG=1: the pause object whenever it changes (to map the mission table / text)
    static uint8_t last[0x80];
    uint8_t cur[0x80];
    for (int i = 0; i < 0x80; ++i) cur[i] = ld8(c.r[3] + i);
    if (memcmp(cur, last, sizeof cur)) {
      memcpy(last, cur, sizeof cur);
      char buf[0x80 * 3 + 64];
      int n = snprintf(buf, sizeof buf, "[pause] obj %08X:", c.r[3]);
      for (int i = 0; i < 0x80; i += 4) n += snprintf(buf + n, sizeof buf - n, "%s%08X", i % 32 ? " " : " | ", ld32(c.r[3] + i));
      logf("%s\n", buf);
    }
  }
  f_801E3404(c);
}
bool game::pause_menu_active() { return g_pause_ns && sys_time_ns() - g_pause_ns < 150000000ull; }
uint32_t game::pause_object() { return pause_menu_active() ? g_pause_obj : 0; }
bool game::in_stage() {
  return pause_menu_active() || (g_player_step_ns && sys_time_ns() - g_player_step_ns < 300000000ull);
}

void hle_SelectModeDtor(Cpu& c) {
  unlock_restore();
  f_802C4204(c);
}

void hle_EventUpdate(Cpu& c) {
  uint32_t self = c.r[3];
  g_evmgr_last_ns = sys_time_ns();
  uint32_t prev_state = g_evmgr_state;
  g_evmgr_state = ld32(self + 0x28);
  static uint32_t started_self = 0;
  if (g_evmgr_state == 0 && self != started_self) { started_self = self; g_evmgr_start_ns = g_evmgr_last_ns; }
  static uint64_t playing_since = 0;
  if (g_evmgr_state == 5 && prev_state != 5) { playing_since = g_evmgr_last_ns; input_take_start_press(); }   // drop older presses
  // "Skip any cutscene": START during playback requests the game's own skip (+0x34), which it otherwise only
  // accepts for scenes already seen.
  if (g_evmgr_state == 5 && g_evmgr_last_ns - playing_since > 500000000ull && input_take_start_press() &&
      settings::g.skip_any_cutscene && !ld8(self + 0x34)) {
    st8(self + 0x34, 1);
    logf("[game] cutscene skipped (skip-any option)\n");
  }
  if (g_event_trace) {
    static uint32_t last_self = 0, last_state = ~0u, last_id = ~0u;
    static uint8_t last_skip = 0xFF;
    uint32_t state = ld32(self + 0x28), id = ld32(0x805EF2A0);
    uint8_t skip = ld8(self + 0x34);
    if (self != last_self || state != last_state || id != last_id || skip != last_skip) {
      logf("[event] t=%.2f self=%08X state=%u id=%u skip=%u flags4=%04X cur=%d d1=%u\n", sys_time_ns() / 1e9, self, state, id, skip,
           ld32(self + 4) >> 16, (int)ld32(self + 0x2C), ld8(0x8057D6A0 + 0xD1));
      last_self = self; last_state = state; last_id = id; last_skip = skip;
    }
  }
  f_80173034(c);
}

// ---- GXBegin (fn_80395B28) - debugging aid. GXB_TRACE=1: in a stage, every distinct guest call chain that starts a
// draw is logged once (return addresses up the stack) - finds which game code draws what (CSD scenes, HUD...).
void f_80395B28(Cpu&);
void hle_GXBegin(Cpu& c) {
  static const bool trace = getenv("GXB_TRACE") != nullptr;
  if (trace && game::in_stage()) {
    static std::set<std::string> seen;
    char buf[600];
    int n = snprintf(buf, sizeof buf, "%s%08X", game::pause_menu_active() ? "PAUSED " : "", c.lr);
    uint32_t sp = c.r[1];
    static const int depth = atoi(getenv("GXB_TRACE")) > 1 ? atoi(getenv("GXB_TRACE")) : 9;   // GXB_TRACE=<frames>
    for (int i = 0; i < depth && sp >= 0x80000000u && sp < RT_RAM_END; ++i) {
      uint32_t back = ld32(sp);
      if (back <= sp || back >= RT_RAM_END) break;
      n += snprintf(buf + n, sizeof buf - n, " %08X", ld32(back + 4));
      sp = back;
    }
    if (seen.size() < 2000 && seen.insert(buf).second) logf("[gxb] fmt/prim %02X n=%u chain %s\n", c.r[3] | c.r[4], c.r[5], buf);
  }
  f_80395B28(c);
}

// ---- 16:9 HUD: each HUD element's draw is tagged in the GX FIFO (gx_lle.cpp hud_marker), so the renderer can move
// the element as a whole to the left / right screen edge. callhooks.txt routes the render-tree walker's per-object
// draw call (fn_8004EC68, bctrl at 0x8004ED60 = vtable +0x0C, r3 = the object) through here. The private commands
// (0x02 <object>, 0x03) go through the write-gather pipe like the game's own GX writes, so they stay in order.
// a "RenderUnit" render node (first word = its name string 0x804ABD88; draw fn_8004E34C) runs a boost::function stored at
// +0x28 -> +0x14; that function's first word (its manager) names the bound type, e.g. an enemy HP bar. 0 = not a unit.
static uint32_t render_unit_fn(uint32_t obj) {
  if (ld32(obj) != 0x804ABD88u) return 0;
  const uint32_t p = ld32(obj + 0x28);
  return p >= 0x80000000u && p < RT_RAM_END ? ld32(p + 0x14) : 0;
}
void hud_element_draw(Cpu& c, uint32_t target) {
  // HUD objects are tagged for the 16:9 HUD and for the PC gauges (gx_lle.cpp finds the gauge objects by their place)
  // (and for the post-processing, postfx.cpp: it runs at the first HUD object of a frame)
  const bool mark = (settings::g.widescreen || settings::g.light || settings::g.ao) &&   // #143: the PC gauges are gone; #148 contact AO; #146 widescreen: HP bars / lock-on marks (gx_lle.cpp)
                    game::in_stage();
  const uint32_t obj = c.r[3];
  static const char* gauge_test = getenv("GAUGE_TEST");   // test only: "<hero> <dark>" gauge values (full = 30000)
  if (gauge_test && game::in_stage()) {
    int hero = 0, dark = 0;
    sscanf(gauge_test, "%d %d", &hero, &dark);
    st32(0x805766C8, (uint32_t)hero);
    st32(0x805766D4, (uint32_t)dark);
  }
  static const bool hlog = getenv("HUD_LOG") != nullptr;
  if (hlog && game::in_stage()) {   // HUD_LOG: each HUD object's class (vtable) - pairs with gx_lle's "[hud] object" widget boxes
    static std::set<uint32_t> seen;
    if (seen.size() < 300 && seen.insert(obj).second) logf("[hud] object %08X vtable %08X draw %08X unit %08X\n", obj, ld32(obj), target, render_unit_fn(obj));
  }
  static const bool plog = getenv("PAUSE_LOG") != nullptr;
  if (plog) {   // render objects that draw only while paused (vtable = their class)
    static std::set<uint32_t> gameplay, seen;
    if (!game::pause_menu_active()) gameplay.insert(obj);
    else if (!gameplay.count(obj) && seen.size() < 200 && seen.insert(obj).second)
      logf("[pause] pause-only draw object %08X vtable %08X target %08X\n", obj, ld32(obj), target);
  }
  // #89: world-anchored markers are already projected to their target with the 4:3 camera - tag them (low bit of the
  // id; objects are 4-aligned) so the 16:9 HUD never snaps them to an edge. They are RenderUnits whose function is
  // bound to GiHPGaugeEnemyImpl (enemy HP bar, manager fn_800BF910) or Weapon::LockonMarkImpl (fn_80062888); found
  // through the bind types' typeinfo (rodata strings) and proven with HUD_LOG's "unit" column (tests hud89-soldier)
  const uint32_t unit = render_unit_fn(obj);
  const bool anchored = ld32(obj) == 0x8051ED98u || unit == 0x800BF910u || unit == 0x80062888u;
  const bool hp_bar = unit == 0x800BF910u;   // #146: placed with the widened window (gx_lle.cpp kSideAnchor): bit 1 too
  if (mark) { hw_wgp_write(0x02, 1); hw_wgp_write(anchored ? (obj | (hp_bar ? 3u : 1u)) : obj, 4); }
  stage_workshop::node_enter(obj);   // #35 capture: resources a SET object looks up while its node runs
  call_addr(c, target);
  stage_workshop::node_leave();
  if (mark) { hw_wgp_write(0x03, 1); hw_wgp_write(0, 4); }
}
