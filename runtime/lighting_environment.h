#pragma once
#include "lighting_core.h"
#include <string_view>

namespace lighting {
struct Atmosphere {
  V3 sun_tint{1, 1, 1};
  V3 sky{}, ground{}, fog_colour{};
  float sky_override = 0;
  float sun_strength = 1, sky_strength = 1;
  float fog_density = 0, fog_start = 0, fog_override = 0;
  float sun_azimuth = 0, sun_elevation = 45, sun_direction_override = 0;
  float exposure = 1;          // engine-lit pixels x this (MATCHING-THE-ORIGINAL.md 5.1)
  float unlit_distance = 8000; // #106: beyond it (fading over the last 20 %) surfaces keep the game's own colour: the horizon backdrop / skyline glow is not re-lit (a profile can set 0 = off)
  float baked = 0;             // share of the baked vertex light kept (0 = full replace)    // beyond it surfaces keep the game's colour (sky / backdrops, 5.2); 0 = off
  // the real-time layer on top of the game's lighting (profile keys from tools/lighting/make_profiles.py overlay_looks;
  // defaults = its Stylized look): shadow_strength 0..1 = how much light a full shadow takes, shadow_tint = the colour left
  // in shadow, saturation 0..2, ao_strength 0..1
  float shadow_strength = 0.45f;
  V3 shadow_tint{0.68f, 0.68f, 0.85f};
  float saturation = 1.1f, ao_strength = 0.87f;
};
// The profile + the user's look preset and sliders (settings light_look / light_exposure ...), what the shader gets.
// Replace mode: the profile values as they are (the looks are for the overlay).
struct Look {
  float exposure = 1, sun = 1, saturation = 1, ao = 1, fill = 1;
  V3 gi{1, 1, 1};   // bounce tint where the sky is hidden (overlay GI)
  V3 shadow{0.45f, 0.5f, 0.62f};   // multiplier left in full shadow = 1 - strength x (1 - tint / 2)
};
Look look();
// Game-thread API: resolve stage + camera-area profiles, then smooth in elapsed seconds.
void update_environment(const Cam& camera);
const Atmosphere& atmosphere();
int environment_stage();
// A game event hook can activate a named profile condition without changing shader code.
void set_environment_trigger(std::string_view name, bool active);
Module* environment_module();
}
