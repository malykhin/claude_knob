#include <Arduino.h>

#include "board.h"
#include "leds.h"
#include "model.h"

static const uint8_t pins[LED_COUNT] = LED_PINS;
static uint32_t flash_until = 0;

// Perceptual brightness 0..255 -> PWM, inverted for the active-low wiring.
static void put(int i, float level)
{
    level = constrain(level, 0.0f, 1.0f);
    ledcWrite(pins[i], 255 - (uint32_t)(level * level * 255));
}

void leds_init()
{
    for (int i = 0; i < LED_COUNT; i++) {
        ledcAttach(pins[i], 5000, 8);
        put(i, 0);
    }
}

void leds_flash() { flash_until = millis() + 120; }

static float wave(uint32_t t, uint32_t period) { return 0.5f - 0.5f * cosf(2 * PI * (t % period) / period); }

void leds_update(bool highlight)
{
    uint32_t t = millis();
    if ((int32_t)(flash_until - t) > 0) {
        for (int i = 0; i < LED_COUNT; i++) put(i, 1);
        return;
    }
    if (!model.hostSeen) {
        // Waiting for knobd: slow shared breathing.
        float b = 0.35f * wave(t, 4000);
        for (int i = 0; i < LED_COUNT; i++) put(i, b);
        return;
    }

    int base = (model.sel / LED_COUNT) * LED_COUNT;
    for (int i = 0; i < LED_COUNT; i++) {
        int idx = base + i;
        if (idx >= model.count) {
            put(i, 0);
            continue;
        }
        bool sel = highlight && idx == model.sel;
        float level;
        switch (model.items[idx].state) {
        case ST_RUNNING: level = (sel ? 0.25f : 0.1f) + (sel ? 0.75f : 0.35f) * wave(t + idx * 300, 1400); break;
        case ST_PENDING: level = (t / 125) % 2 ? 1.0f : 0.0f; break;
        case ST_DONE: level = sel ? 1.0f : 0.55f; break;
        default: level = sel ? 0.45f : 0.12f; break;
        }
        put(i, level);
    }
}
