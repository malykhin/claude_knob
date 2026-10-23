#include <Arduino.h>

#include "board.h"
#include "encoder.h"

// Quadrature decoding in a pin-change interrupt. The T-Encoder Pro's detents
// sit at both rest states (AB = 00 and 11), so one detent is two quarter-steps.
static const int8_t QUARTER[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};

static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint8_t prev_ab;
static volatile int8_t acc;
static volatile int steps;

static inline uint8_t read_ab()
{
    return (digitalRead(KNOB_DATA_A) << 1) | digitalRead(KNOB_DATA_B);
}

static void IRAM_ATTR on_change()
{
    portENTER_CRITICAL_ISR(&mux);
    uint8_t ab = read_ab();
    if (ab != prev_ab) {
        int8_t a = acc + QUARTER[(prev_ab << 2) | ab];
        prev_ab = ab;
        if (ab == 0b00 || ab == 0b11) {
            if (a >= 2) steps = steps + 1;
            else if (a <= -2) steps = steps - 1;
            a = 0;
        }
        acc = a;
    }
    portEXIT_CRITICAL_ISR(&mux);
}

void encoder_init()
{
    pinMode(KNOB_DATA_A, INPUT_PULLUP);
    pinMode(KNOB_DATA_B, INPUT_PULLUP);
    pinMode(KNOB_KEY, INPUT_PULLUP);
    prev_ab = read_ab();
    attachInterrupt(digitalPinToInterrupt(KNOB_DATA_A), on_change, CHANGE);
    attachInterrupt(digitalPinToInterrupt(KNOB_DATA_B), on_change, CHANGE);
}

int encoder_take_steps()
{
    portENTER_CRITICAL(&mux);
    int s = steps;
    steps = 0;
    portEXIT_CRITICAL(&mux);
    return KNOB_REVERSE ? -s : s;
}

bool encoder_take_press()
{
    static bool stable_down = false;
    static bool last_raw = false;
    static uint32_t changed_at = 0;

    bool raw = digitalRead(KNOB_KEY) == LOW;
    uint32_t now = millis();
    if (raw != last_raw) {
        last_raw = raw;
        changed_at = now;
    }
    if (raw != stable_down && now - changed_at > 25) {
        stable_down = raw;
        return stable_down;
    }
    return false;
}
