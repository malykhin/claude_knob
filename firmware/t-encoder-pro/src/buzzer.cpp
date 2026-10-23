#include <Arduino.h>

#include "board.h"
#include "buzzer.h"

static uint32_t off_at = 0;
static int chirp_step = -1;

static void beep(uint32_t freq, uint32_t ms)
{
    ledcWriteTone(BUZZER_DATA, freq);
    off_at = millis() + ms;
}

void buzzer_init()
{
    ledcAttach(BUZZER_DATA, 2000, 8);
    ledcWrite(BUZZER_DATA, 0);
}

void buzzer_tick()
{
#if BUZZER_TICK
    if (chirp_step < 0) beep(3200, 4);
#endif
}

void buzzer_chirp() { chirp_step = 0; }

// Non-blocking: call every loop.
void buzzer_loop()
{
    if (off_at && (int32_t)(millis() - off_at) >= 0) {
        ledcWriteTone(BUZZER_DATA, 0);
        off_at = 0;
        if (chirp_step == 1) {
            beep(2700, 70);
            chirp_step = -1;
        }
    }
    if (chirp_step == 0 && !off_at) {
        beep(2000, 70);
        chirp_step = 1;
    }
}
