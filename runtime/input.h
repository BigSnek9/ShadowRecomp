#pragma once
#include <cstdint>

enum PadButton : uint16_t {
  PAD_LEFT = 0x0001, PAD_RIGHT = 0x0002, PAD_DOWN = 0x0004, PAD_UP = 0x0008, PAD_Z = 0x0010,
  PAD_R = 0x0020, PAD_L = 0x0040,
  PAD_A = 0x0100, PAD_B = 0x0200, PAD_X = 0x0400, PAD_Y = 0x0800, PAD_START = 0x1000,
};

struct PadState {
  uint16_t buttons;
  int8_t stick_x, stick_y, c_x, c_y;     // -127..127, +y = up
  uint8_t trig_l, trig_r;
};

PadState input_poll();
void input_add_script(const char* spec);
extern bool g_mash;
extern uint16_t g_user_buttons;   // the player's buttons at the last poll (pad/keys/INPUT_FILE; 0 while a PC menu is open)
extern uint16_t g_auto_buttons;   // buttons held by game hooks (--stage presses A in Select Mode); OR'd into the pad
bool input_take_start_press();   // true once per START press (gameplay hooks)
