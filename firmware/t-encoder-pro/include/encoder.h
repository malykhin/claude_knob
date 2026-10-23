#pragma once

void encoder_init();
// Detents turned since the last call (positive = clockwise).
int encoder_take_steps();
// True once per button press (debounced).
bool encoder_take_press();
