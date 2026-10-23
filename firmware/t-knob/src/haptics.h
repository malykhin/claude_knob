#pragma once

// Detents: `count` positions `width` apart with walls past the first and last one.
struct HapticParams {
    float width_deg = 30;  // angle between positions
    float peak = 1.2f;     // volts of the strongest pull toward a detent center
    // Detent profile: 0 = linear spring that flips at the boundary (crisp, "digital"),
    // 1 = sinusoid that fades to zero at the boundary (smooth, like a ball detent).
    float shape = 0.6f;
    float click = 0.6f;    // volts of the "tick" pulse when crossing into a new position, 0 = off
    float click_ms = 6;    // tick length
    float wall = 10.0f;    // extra volts per radian past an end position
    float kd = 0.025f;     // damping, volts per rad/s
    float dead_deg = 0.4f; // no spring torque this close to a detent center
    float snap = 0.55f;    // switch to the neighbor past this fraction of `width`
    float nudge = 0.5f;    // volts of the "a session needs you" vibration, 0 = off
};

extern HapticParams haptic_params;

// Loads tuned parameters saved with haptics_save(), if any.
void haptics_load();
void haptics_save();
void haptics_reset();

// Updates the number of positions. When `pos` differs from the current position the
// detents are re-anchored around `angle`, so the knob doesn't move; only the index changes.
void haptics_set(int count, int pos, float angle);
// Moves the detent grid; the motor should then turn the knob by `rad` (a self-test).
void haptics_shift(float rad);
int haptics_count();
int haptics_pos();

// Turns the knob to position `p` with the motor (the user switched windows by hand).
// Gives up, staying at `p` logically, if a hand holds the wheel.
void haptics_travel_to(int p, float angle);
bool haptics_travelling();
// Why the last travel ended: "arrived", "held", "timeout" (for the debug output).
const char *haptics_travel_result();
// True once each time the knob is pushed into an end wall (past the first or last position).
bool haptics_take_wall_push();

// Returns the torque to apply; sets *moved when the position changed.
float haptics_update(float angle, float velocity, bool *moved);
