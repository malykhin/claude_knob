#pragma once
#include <stdint.h>

// Field-oriented control of the gimbal motor, voltage mode.
// Angles are in radians; positive angle is the direction positive torque turns the rotor.

struct FocCalibration {
    bool ok;
    int8_t dir;   // sensor direction relative to the motor's electrical rotation
    float zero;   // electrical angle offset
    float moved;  // mechanical travel during the electrical sweep (expected 2pi / pole pairs)
};

// Loads the stored calibration, or calibrates (the knob twitches ~50 degrees) when
// there is none or `force` is set. Returns false when the motor isn't usable;
// the sensor still works then, there's just no torque.
bool foc_init(bool force);
FocCalibration foc_calibrate();
FocCalibration foc_calibration();

// Call as often as possible.
void foc_update();
float foc_angle();    // unwrapped
float foc_velocity(); // rad/s, low-pass filtered
bool foc_pressed();   // MT6701 push flag
uint8_t foc_field();  // MT6701 field status, 0 = ok
uint32_t foc_crc_errors();

// Torque command in volts (q-axis). Ignored when uncalibrated.
void foc_set_torque(float uq);
