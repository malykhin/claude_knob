#pragma once

// LED k shows session k of the current page of 4 (the page containing the selection).
void leds_init();
// `highlight`: show which session is selected (false while nothing is focused).
void leds_update(bool highlight);
// Briefly lights all LEDs (e.g. on a press).
void leds_flash();
