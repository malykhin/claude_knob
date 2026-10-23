// Pin map for the LilyGO T-Encoder Pro, from LilyGO's pin_config.h
// (github.com/Xinyuan-LilyGO/T-Encoder-Pro). Swap this file for another board.
#pragma once

// Two panel revisions ship with this board. Newer units (2025+) use the CO5300.
// If the screen stays black, switch to the other one.
#define PANEL_CO5300 // TFD12MASBCTB4_V0_07
// #define PANEL_SH8601 // DXQ120MYB2416A

#define SCREEN_SDIO0 11
#define SCREEN_SDIO1 13
#define SCREEN_SDIO2 7
#define SCREEN_SDIO3 14
#define SCREEN_SCLK 12
#define SCREEN_CS 10
#define SCREEN_RST 4
#define SCREEN_EN 3
#define SCREEN_WIDTH 390
#define SCREEN_HEIGHT 390

#define KNOB_DATA_A 1
#define KNOB_DATA_B 2
#define KNOB_KEY 0 // active low
// Flip if clockwise moves the selection backwards.
#define KNOB_REVERSE 0

#define BUZZER_DATA 17
// Set to 0 to silence the detent tick (the pending chirp stays on).
#define BUZZER_TICK 1
