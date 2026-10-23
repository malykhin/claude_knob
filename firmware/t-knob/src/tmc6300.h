#pragma once

// Three half-bridges driven by MCPWM at 20 kHz with complementary outputs and dead time.
void tmc6300_init();
// Phase duty cycles in [0, 1].
void tmc6300_set_duty(float a, float b, float c);
// Gates on/off. While off all six outputs are low (motor coasts).
void tmc6300_enable(bool on);
