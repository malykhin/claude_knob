#include <Arduino.h>
#include <lvgl.h>

#include "model.h"
#include "ui.h"

// One arc segment per session around the edge; the selected one is thicker.
#define RING_SIZE 372
#define ARC_W 12
#define ARC_W_SEL 28
#define GAP_DEG 6

static lv_obj_t *arcs[MAX_SESSIONS];
static lv_obj_t *lbl_name, *lbl_state, *lbl_pos;

static lv_color_t color_for(SessionState s)
{
    switch (s) {
    case ST_RUNNING: return lv_color_hex(0x3B82F6);
    case ST_PENDING: return lv_color_hex(0xF59E0B);
    case ST_DONE: return lv_color_hex(0x22C55E);
    default: return lv_color_hex(0x5B616B);
    }
}

static const char *text_for(SessionState s)
{
    switch (s) {
    case ST_RUNNING: return "Running";
    case ST_PENDING: return "Needs you";
    case ST_DONE: return "Done";
    default: return "Idle";
    }
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(l, 270);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, y);
    lv_label_set_text(l, "");
    return l;
}

// Running segments breathe, pending ones blink.
static void animate(lv_timer_t *)
{
    uint32_t t = millis();
    for (int i = 0; i < model.count && i < MAX_SESSIONS; i++) {
        lv_opa_t opa = LV_OPA_COVER;
        if (model.items[i].state == ST_RUNNING)
            opa = 110 + (lv_opa_t)(145 * (0.5f + 0.5f * sinf(t / 280.0f)));
        else if (model.items[i].state == ST_PENDING)
            opa = (t / 450) % 2 ? LV_OPA_COVER : 70;
        lv_obj_set_style_arc_opa(arcs[i], opa, LV_PART_INDICATOR);
    }
}

void ui_init()
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < MAX_SESSIONS; i++) {
        lv_obj_t *a = lv_arc_create(scr);
        lv_obj_set_size(a, RING_SIZE, RING_SIZE);
        lv_obj_center(a);
        lv_obj_remove_style(a, NULL, LV_PART_KNOB);
        lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_arc_rounded(a, false, LV_PART_INDICATOR);
        lv_arc_set_rotation(a, 270); // 0° at 12 o'clock
        lv_obj_add_flag(a, LV_OBJ_FLAG_HIDDEN);
        arcs[i] = a;
    }

    lbl_name = make_label(scr, &lv_font_montserrat_28, lv_color_white(), -14);
    lbl_state = make_label(scr, &lv_font_montserrat_20, lv_color_white(), 28);
    lbl_pos = make_label(scr, &lv_font_montserrat_14, lv_color_hex(0x8A9099), 70);

    lv_timer_create(animate, 40, NULL);
}

void ui_refresh()
{
    int n = model.count;
    int seg = n > 0 ? 360 / n : 360;
    int gap = n > 1 ? GAP_DEG : 0;

    for (int i = 0; i < MAX_SESSIONS; i++) {
        lv_obj_t *a = arcs[i];
        if (i >= n) {
            lv_obj_add_flag(a, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        bool sel = i == model.sel;
        int start = i * seg + gap / 2;
        int end = n == 1 ? 360 : (i + 1) * seg - gap / 2;
        lv_arc_set_angles(a, start, end);
        lv_obj_set_style_arc_color(a, color_for(model.items[i].state), LV_PART_INDICATOR);
        lv_obj_set_style_arc_width(a, sel ? ARC_W_SEL : ARC_W, LV_PART_INDICATOR);
        lv_obj_remove_flag(a, LV_OBJ_FLAG_HIDDEN);
    }

    if (!model.hostSeen) {
        lv_label_set_text(lbl_name, "Claude Knob");
        lv_obj_set_style_text_color(lbl_state, lv_color_hex(0x8A9099), 0);
        lv_label_set_text(lbl_state, "Waiting for knobd...");
        lv_label_set_text(lbl_pos, "");
    } else if (n == 0) {
        lv_label_set_text(lbl_name, "No sessions");
        lv_obj_set_style_text_color(lbl_state, lv_color_hex(0x8A9099), 0);
        lv_label_set_text(lbl_state, "Start claude in Terminal");
        lv_label_set_text(lbl_pos, "");
    } else {
        const Session &s = model.items[model.sel];
        lv_label_set_text(lbl_name, s.name);
        lv_obj_set_style_text_color(lbl_state, color_for(s.state), 0);
        lv_label_set_text(lbl_state, text_for(s.state));
        lv_label_set_text_fmt(lbl_pos, "%d / %d", model.sel + 1, n);
    }
}
