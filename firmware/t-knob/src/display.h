#pragma once

// GC9A01 240x240 round LCD on the top breakout: a dial with one mark per session, spaced
// like the haptic detents and starting at 12 o'clock, a pointer that follows the knob, and
// the selected session's name and state in the middle.
//
// Frames go out in 8-row strips over DMA, one strip rendered per display_step(), so the
// motor loop never waits more than a fraction of a millisecond for the screen.
void display_init();

// Latest state to show, from the UI tick. Angles are in the haptics' frame (user_angle()):
// `pointer` is where the knob is, detent k sits at `origin + k * width`.
void display_update(bool highlight, float pointer, float origin, float width);

// Call from every loop() iteration.
void display_step();
