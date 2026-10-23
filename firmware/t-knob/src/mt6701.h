#pragma once
#include <stdint.h>

struct Mt6701Reading {
    bool ok;        // CRC matched
    float angle;    // radians, [0, 2pi)
    bool pressed;   // "push" status bit (z-axis field change)
    uint8_t field;  // 0 normal, 1 too strong, 2 too weak
    bool overspeed;
};

void mt6701_init();
Mt6701Reading mt6701_read();
