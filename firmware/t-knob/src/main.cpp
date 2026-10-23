// Claude Knob firmware for the LilyGO T-Knob: one haptic detent per Claude Code
// session, walls past the first and last, LEDs for session states. Silent: no buzzer.
#include <Arduino.h>

#include "board.h"
#include "foc.h"
#include "haptics.h"
#include "leds.h"
#include "link.h"
#include "model.h"

#define UI_EVERY_MS 5
// After you turn the knob, the host's view of the focused window lags behind (it is busy
// focusing what you picked). Ignore its focus reports for this long.
#define LOCAL_GRACE_MS 1000
#define DEBUG_EVERY_MS 100
// Vibration used to get attention when a session starts waiting for the user
// (strength is haptic_params.nudge).
#define NUDGE_MS 180

Model model;

static const float user_dir = KNOB_REVERSE ? -1.0f : 1.0f;
static uint32_t nudge_until = 0;
static uint32_t last_ui = 0, last_debug = 0;
static bool debug = false;
static uint32_t debug_every = DEBUG_EVERY_MS;
static float last_torque = 0;
static uint32_t last_local_move = 0;

static bool user_active() { return millis() - last_local_move < LOCAL_GRACE_MS; }

// "No selection": the user is looking at something other than a Claude session.
static bool unselected() { return model.hostSeen && model.count > 0 && model.focus < 0 && !user_active(); }

static float user_angle() { return user_dir * foc_angle(); }

static void send_json(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Serial.print(buf);
    Serial.print('\n');
}

static void report_info()
{
    FocCalibration c = foc_calibration();
    const HapticParams &p = haptic_params;
    send_json("{\"t\":\"info\",\"board\":\"t-knob\",\"cal\":%s,\"dir\":%d,\"zero\":%.3f,\"moved\":%.3f,"
              "\"width\":%.1f,\"peak\":%.2f,\"shape\":%.2f,\"click\":%.2f,\"click_ms\":%.1f,\"wall\":%.1f,"
              "\"kd\":%.3f,\"dead\":%.2f,\"snap\":%.2f,\"nudge\":%.2f}",
              c.ok ? "true" : "false", c.dir, c.zero, c.moved,
              p.width_deg, p.peak, p.shape, p.click, p.click_ms, p.wall, p.kd, p.dead_deg, p.snap, p.nudge);
}

// Board-specific commands, for tuning over the serial port:
//   {"t":"tune","peak":1.5,"shape":0.8,...}  {"t":"save"}  {"t":"reset"}  {"t":"info"}
//   {"t":"debug","on":true,"ms":20}  {"t":"recal"}  {"t":"nudge"}
//   {"t":"shift","deg":5} (moves the detent grid: the motor should follow)
static void on_command(JsonDocument &doc)
{
    const char *t = doc["t"] | "";
    if (!strcmp(t, "tune")) {
        HapticParams &p = haptic_params;
        p.width_deg = doc["width"] | p.width_deg;
        p.peak = doc["peak"] | p.peak;
        p.wall = doc["wall"] | p.wall;
        p.kd = doc["kd"] | p.kd;
        p.dead_deg = doc["dead"] | p.dead_deg;
        p.snap = doc["snap"] | p.snap;
        p.shape = constrain(doc["shape"] | p.shape, 0.0f, 1.0f);
        p.click = doc["click"] | p.click;
        p.click_ms = doc["click_ms"] | p.click_ms;
        p.nudge = doc["nudge"] | p.nudge;
        report_info();
    } else if (!strcmp(t, "save")) {
        haptics_save();
        report_info();
    } else if (!strcmp(t, "reset")) {
        haptics_reset();
        report_info();
    } else if (!strcmp(t, "debug")) {
        debug = doc["on"] | true;
        debug_every = doc["ms"] | DEBUG_EVERY_MS;
    } else if (!strcmp(t, "recal")) {
        foc_calibrate();
        haptics_set(haptics_count(), haptics_pos(), user_angle());
        report_info();
    } else if (!strcmp(t, "info")) {
        report_info();
    } else if (!strcmp(t, "shift")) {
        haptics_shift((doc["deg"] | 0.0f) * DEG_TO_RAD);
    } else if (!strcmp(t, "nudge")) {
        nudge_until = millis() + NUDGE_MS;
    }
}

static bool take_press()
{
    static bool stable = false, last_raw = false;
    static uint32_t changed_at = 0;
    bool raw = foc_pressed() || digitalRead(BOOT_BTN) == LOW;
    uint32_t now = millis();
    if (raw != last_raw) {
        last_raw = raw;
        changed_at = now;
    }
    if (raw != stable && now - changed_at > 30) {
        stable = raw;
        return stable;
    }
    return false;
}

void setup()
{
    pinMode(BOOT_BTN, INPUT_PULLUP);
    link_init();
    link_on_message(on_command);
    leds_init();
    haptics_load();
    bool force_cal = digitalRead(BOOT_BTN) == LOW;
    foc_init(force_cal);
}

static void ui_tick()
{
    int lf = link_poll();
    if (lf & (LINK_UPDATED | LINK_LOST)) {
        // model.sel follows the same session id across list changes: re-index without moving.
        int n = model.hostSeen ? model.count : 0;
        if (!haptics_travelling()) haptics_set(n, model.sel, user_angle());
    }
    // Terminal -> knob: you focused another session's window, so turn the wheel to it.
    if (!user_active() && !haptics_travelling() && model.focus >= 0 && model.focus != haptics_pos())
        haptics_travel_to(model.focus, user_angle());
    model.sel = haptics_pos();
    if (lf & LINK_NEW_PENDING) nudge_until = millis() + NUDGE_MS;
    if (take_press() && model.count > 0) {
        last_local_move = millis();
        link_send_select(model.sel, true);
        leds_flash();
    }
    leds_update(!unselected());

    uint32_t now = millis();
    if (debug && now - last_debug >= debug_every) {
        last_debug = now;
        send_json("{\"t\":\"debug\",\"angle\":%.1f,\"vel\":%.2f,\"pos\":%d,\"n\":%d,\"focus\":%d,"
                  "\"active\":%d,\"travel\":%d,\"last_travel\":\"%s\",\"torque\":%.2f,\"push\":%d,\"crc_err\":%lu}",
                  user_angle() * RAD_TO_DEG, foc_velocity(), haptics_pos(), haptics_count(), model.focus,
                  user_active(), haptics_travelling(), haptics_travel_result(), last_torque,
                  foc_pressed(), (unsigned long)foc_crc_errors());
    }
}

void loop()
{
    foc_update();

    bool moved;
    float torque = haptics_update(user_angle(), user_dir * foc_velocity(), &moved);
    uint32_t now = millis();
    float nudge = haptic_params.nudge;
    if (nudge > 0 && (int32_t)(nudge_until - now) > 0) torque += (now / 12) % 2 ? nudge : -nudge;
    last_torque = torque;
    foc_set_torque(user_dir * torque);

    // With nothing selected, pushing into an end wall selects the session at that end:
    // parked on the last session, turning forward still gets you there.
    bool wall_push = haptics_take_wall_push() && unselected();
    if (moved || wall_push) {
        last_local_move = millis();
        model.sel = haptics_pos();
        link_send_select(model.sel, false);
    }

    if (now - last_ui >= UI_EVERY_MS) {
        last_ui = now;
        ui_tick();
    }
}
