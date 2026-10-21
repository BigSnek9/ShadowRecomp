#pragma once
#include <cstdint>
struct Cpu;
namespace draw_distance {
// Called for a known screen camera BEFORE RwCameraBeginUpdate builds its frustum.
void refresh_camera(uint32_t camera, Cpu& cpu);
void finish_camera(uint32_t camera, Cpu& cpu);
}
