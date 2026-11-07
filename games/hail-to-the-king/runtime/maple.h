#pragma once
#include <stdint.h>
/* controller state in "pressed = 1" form; maple.c inverts to the bus's active-low format */
typedef struct { uint16_t buttons; uint8_t rtrig, ltrig, joyx, joyy; } maple_pad_t;
extern maple_pad_t maple_pad;
enum {
    MAPLE_C = 0x0001, MAPLE_B = 0x0002, MAPLE_A = 0x0004, MAPLE_START = 0x0008,
    MAPLE_UP = 0x0010, MAPLE_DOWN = 0x0020, MAPLE_LEFT = 0x0040, MAPLE_RIGHT = 0x0080,
    MAPLE_Z = 0x0100, MAPLE_Y = 0x0200, MAPLE_X = 0x0400, MAPLE_D = 0x0800
};
