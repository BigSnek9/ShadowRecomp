#pragma once
#include <cstdint>
// Object draw distance (object_distance.cpp): scales the loaded SET records' activation bytes by settings draw_objects.
namespace object_distance {
void apply();   // after a SET file is loaded and once per frame (cheap: one pass over the record table)
int original_byte(uint32_t rec);   // #152: a loaded SET record's own activation byte (before scaling), -1 = not a record
}
