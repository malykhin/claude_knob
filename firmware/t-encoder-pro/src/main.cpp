// Claude Knob firmware: shows Claude Code sessions as a ring of segments,
// and tells the desktop daemon (knobd) which one to focus.
#include <Arduino.h>
#include <lvgl.h>

#include "buzzer.h"
#include "display.h"
#include "encoder.h"
#include "link.h"
#include "model.h"
#include "ui.h"

#define BRIGHT 220
#define DIM 40
// Dim the AMOLED after this long without input, unless a session needs attention.
#define DIM_AFTER_MS 60000

Model model;

static uint32_t last_activity = 0;
static uint32_t last_turn = 0;
static int brightness = -1;

static bool any_pending()
{
    for (int i = 0; i < model.count; i++)
        if (model.items[i].state == ST_PENDING) return true;
    return false;
}

static void update_brightness()
{
    bool idle = millis() - last_activity > DIM_AFTER_MS && !any_pending();
    int want = idle ? DIM : BRIGHT;
    if (want != brightness) {
        brightness = want;
        display_set_brightness(want);
    }
}

void setup()
{
    link_init();
    buzzer_init();
    encoder_init();
    display_init();
    ui_init();
    ui_refresh();
    last_activity = millis();
}

void loop()
{
    bool dirty = false;

    int lf = link_poll();
    if (lf) dirty = true;
    // Follow the window you focused by hand, unless you're turning the knob right now.
    if ((lf & LINK_UPDATED) && model.focus >= 0 && millis() - last_turn > 1000) model.sel = model.focus;
    if (lf & LINK_NEW_PENDING) {
        buzzer_chirp();
        last_activity = millis();
    }

    int steps = encoder_take_steps();
    if (steps) {
        last_activity = millis();
        last_turn = millis();
        if (model.count > 0) {
            int n = model.count;
            model.sel = ((model.sel + steps) % n + n) % n;
            link_send_select(model.sel, false);
            buzzer_tick();
            dirty = true;
        }
    }

    if (encoder_take_press()) {
        last_activity = millis();
        link_send_select(model.sel, true);
    }

    if (dirty) ui_refresh();
    update_brightness();
    buzzer_loop();
    lv_timer_handler();
    delay(2);
}
