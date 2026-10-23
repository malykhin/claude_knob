#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <lvgl.h>

#include "board.h"
#include "display.h"

static Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    SCREEN_CS, SCREEN_SCLK, SCREEN_SDIO0, SCREEN_SDIO1, SCREEN_SDIO2, SCREEN_SDIO3);

#if defined(PANEL_CO5300)
static Arduino_CO5300 *gfx = new Arduino_CO5300(
    bus, SCREEN_RST, 0 /* rotation */, SCREEN_WIDTH, SCREEN_HEIGHT);
#elif defined(PANEL_SH8601)
static Arduino_SH8601 *gfx = new Arduino_SH8601(
    bus, SCREEN_RST, 0 /* rotation */, SCREEN_WIDTH, SCREEN_HEIGHT);
#else
#error "Select a panel in board.h"
#endif

// LVGL renders in 1/10-screen chunks.
static uint16_t draw_buf[SCREEN_WIDTH * SCREEN_HEIGHT / 10];

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)px,
                            lv_area_get_width(area), lv_area_get_height(area));
    lv_display_flush_ready(disp);
}

// The AMOLED controller only accepts windows that start and end on even pixels.
static void rounder_cb(lv_event_t *e)
{
    lv_area_t *a = (lv_area_t *)lv_event_get_param(e);
    a->x1 &= ~1;
    a->y1 &= ~1;
    a->x2 |= 1;
    a->y2 |= 1;
}

static uint32_t tick_cb() { return millis(); }

void display_init()
{
    pinMode(SCREEN_EN, OUTPUT);
    digitalWrite(SCREEN_EN, HIGH);

    gfx->begin(40000000);
    gfx->fillScreen(0x0000);

    lv_init();
    lv_tick_set_cb(tick_cb);
    lv_display_t *d = lv_display_create(SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_display_set_flush_cb(d, flush_cb);
    lv_display_set_buffers(d, draw_buf, NULL, sizeof(draw_buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_add_event_cb(d, rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);
}

void display_set_brightness(uint8_t level) { gfx->setBrightness(level); }
