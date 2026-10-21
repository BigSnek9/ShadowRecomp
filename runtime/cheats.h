#pragma once
// Cheats: classic Action Replay codes as session-only toggles (cheats.cpp).
namespace cheats {
enum Id {
  INFINITE_LIVES,
  MIN_20_RINGS,
  RINGS_999,
  INFINITE_AMMO,
  INFINITE_SECONDARY_HEALTH,
  MAX_SCORE,
  MAX_HERO_SCORE,
  MAX_DARK_SCORE,
  HERO_GAUGE_ZL,
  DARK_GAUGE_ZR,
  HERO_GAUGE_ALWAYS,
  DARK_GAUGE_ALWAYS,
  INFINITE_RINGS,   // rings never go down (a hit still scatters rings to pick up)
  ALL_WEAPONS,      // the Secret Container (00/3A) holds every special weapon (session only)
  COUNT
};
struct Info { const char* name; const char* help; };
const Info& info(int i);
int by_key(const char* key);   // the named shadow_cheats.ini keys (cheat_infinite_rings ...) -> id, -1 if unknown
bool& enabled(int i);
void apply_frame();   // once per game frame (frame_rate.cpp hle_FramePace)
}
