#include <Arduino.h>
#include <Preferences.h>

#include "board.h"
#include "foc.h"
#include "mt6701.h"
#include "tmc6300.h"

#define ALIGN_VOLTS 2.0f
#define TORQUE_LIMIT (SUPPLY_VOLTS * 0.45f)
#define VEL_SAMPLE_US 1000
#define VEL_TF 0.006f // seconds
#define CAL_VERSION 1

static FocCalibration cal = {false, 1, 0, 0};

static float raw_angle = 0; // last good reading, [0, 2pi)
static int32_t turns = 0;
static bool pressed = false;
static uint8_t field = 0;
static uint32_t crc_errors = 0;

static float vel = 0, vel_prev_angle = 0;
static uint32_t vel_ts = 0;

static float norm_angle(float a)
{
    a = fmodf(a, 2 * PI);
    return a < 0 ? a + 2 * PI : a;
}

static float raw_unwrapped() { return turns * 2 * PI + raw_angle; }

static void read_sensor()
{
    Mt6701Reading r = mt6701_read();
    if (!r.ok) {
        crc_errors++;
        return;
    }
    float d = r.angle - raw_angle;
    if (d > PI) turns--;
    else if (d < -PI) turns++;
    raw_angle = r.angle;
    pressed = r.pressed;
    field = r.field;
}

// Puts a voltage vector of magnitude `uq` on the q-axis of electrical angle `el`.
static void apply_voltage(float uq, float el)
{
    float s = sinf(el), c = cosf(el);
    float alpha = -uq * s, beta = uq * c;
    const float half = SUPPLY_VOLTS / 2, k = 0.8660254f; // sqrt(3)/2
    float ua = alpha + half;
    float ub = -0.5f * alpha + k * beta + half;
    float uc = -0.5f * alpha - k * beta + half;
    tmc6300_set_duty(ua / SUPPLY_VOLTS, ub / SUPPLY_VOLTS, uc / SUPPLY_VOLTS);
}

static void settle(uint32_t ms)
{
    uint32_t until = millis() + ms;
    while ((int32_t)(millis() - until) < 0) {
        read_sensor();
        delay(1);
    }
}

FocCalibration foc_calibrate()
{
    const float start = 3 * PI / 2; // vector along the alpha axis: rotor electrical angle 0
    const int steps = 250;
    FocCalibration c = {false, 1, 0, 0};

    tmc6300_enable(true);
    apply_voltage(ALIGN_VOLTS, start);
    settle(500);
    float a0 = raw_unwrapped();

    // One electrical revolution forward: the rotor should follow by 2pi / pole pairs.
    for (int i = 1; i <= steps; i++) {
        apply_voltage(ALIGN_VOLTS, start + 2 * PI * i / steps);
        settle(2);
    }
    settle(200);
    c.moved = raw_unwrapped() - a0;
    c.dir = c.moved > 0 ? 1 : -1;

    for (int i = steps - 1; i >= 0; i--) {
        apply_voltage(ALIGN_VOLTS, start + 2 * PI * i / steps);
        settle(2);
    }
    settle(400);
    c.zero = norm_angle(c.dir * MOTOR_POLE_PAIRS * raw_angle);

    apply_voltage(0, 0);
    float expected = 2 * PI / MOTOR_POLE_PAIRS;
    c.ok = fabsf(c.moved) > expected * 0.5f && fabsf(c.moved) < expected * 1.5f;
    if (!c.ok) tmc6300_enable(false);

    cal = c;
    Preferences p;
    p.begin("foc", false);
    p.putUChar("ver", c.ok ? CAL_VERSION : 0);
    p.putChar("dir", c.dir);
    p.putFloat("zero", c.zero);
    p.putFloat("moved", c.moved);
    p.end();
    return c;
}

bool foc_init(bool force)
{
    mt6701_init();
    tmc6300_init();
    for (int i = 0; i < 5; i++) read_sensor();
    turns = 0;

    Preferences p;
    p.begin("foc", true);
    bool stored = p.getUChar("ver", 0) == CAL_VERSION;
    if (stored) {
        cal.ok = true;
        cal.dir = p.getChar("dir", 1);
        cal.zero = p.getFloat("zero", 0);
        cal.moved = p.getFloat("moved", 0);
    }
    p.end();

    if (!stored || force) foc_calibrate();
    if (cal.ok) {
        tmc6300_enable(true);
        apply_voltage(0, 0);
    }
    vel_prev_angle = foc_angle();
    vel_ts = micros();
    return cal.ok;
}

FocCalibration foc_calibration() { return cal; }

void foc_update()
{
    read_sensor();
    uint32_t now = micros();
    uint32_t dt_us = now - vel_ts;
    if (dt_us >= VEL_SAMPLE_US) {
        float dt = dt_us * 1e-6f;
        float a = foc_angle();
        float v = (a - vel_prev_angle) / dt;
        vel += (v - vel) * (dt / (dt + VEL_TF));
        vel_prev_angle = a;
        vel_ts = now;
    }
}

float foc_angle() { return cal.dir * raw_unwrapped(); }
float foc_velocity() { return vel; }
bool foc_pressed() { return pressed; }
uint8_t foc_field() { return field; }
uint32_t foc_crc_errors() { return crc_errors; }

void foc_set_torque(float uq)
{
    if (!cal.ok) return;
    uq = constrain(uq, -TORQUE_LIMIT, TORQUE_LIMIT);
    float el = norm_angle(cal.dir * MOTOR_POLE_PAIRS * raw_angle - cal.zero);
    apply_voltage(uq, el);
}
