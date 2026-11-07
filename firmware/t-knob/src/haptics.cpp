#include <Arduino.h>
#include <Preferences.h>

#include "haptics.h"

#define PARAMS_VERSION 2

HapticParams haptic_params;

static int count = 0, pos = 0;
static float origin = 0;
static uint32_t click_until_us = 0;
static float click_dir = 0;

// Travel: a smoothstep trajectory from where the knob is to the target detent, followed by
// a position controller. Stiff enough to turn the wheel, soft enough to stop for a hand.
#define TRAVEL_KP 6.0f                       // volts per radian of lag
#define TRAVEL_KD 0.04f
#define TRAVEL_LIMIT 2.0f                    // volts
#define TRAVEL_ABORT (25.0f * DEG_TO_RAD)    // lag that means someone is holding it
#define TRAVEL_DONE (1.0f * DEG_TO_RAD)
// Pushing this far into an end wall counts as a "wall push" (with hysteresis back to the wall).
#define WALL_PUSH (6.0f * DEG_TO_RAD)
static bool wall_pushed = false, wall_push_event = false;
static bool travelling = false;
static const char *travel_result = "none";
static float travel_from = 0, travel_to = 0;
static uint32_t travel_start_us = 0, travel_us = 0;

static float width() { return haptic_params.width_deg * DEG_TO_RAD; }

void haptics_load()
{
    Preferences p;
    p.begin("haptic", true);
    if (p.getUChar("ver", 0) == PARAMS_VERSION && p.getBytesLength("params") == sizeof(HapticParams))
        p.getBytes("params", &haptic_params, sizeof(HapticParams));
    p.end();
}

void haptics_save()
{
    Preferences p;
    p.begin("haptic", false);
    p.putBytes("params", &haptic_params, sizeof(HapticParams));
    p.putUChar("ver", PARAMS_VERSION);
    p.end();
}

void haptics_reset()
{
    haptic_params = HapticParams();
    Preferences p;
    p.begin("haptic", false);
    p.clear();
    p.end();
}

void haptics_set(int n, int p, float angle)
{
    if (n != count) travelling = false; // the target index may mean another session now
    count = n < 0 ? 0 : n;
    p = count == 0 ? 0 : constrain(p, 0, count - 1);
    if (p != pos) {
        pos = p;
        origin = angle - pos * width();
    }
}

void haptics_shift(float rad) { origin += rad; }

void haptics_travel_to(int p, float angle)
{
    if (count == 0) return;
    pos = constrain(p, 0, count - 1);
    travel_from = angle;
    travel_to = origin + pos * width();
    float dist = fabsf(travel_to - travel_from);
    travel_us = (uint32_t)(1e6f * fminf(0.12f + dist / 4.0f, 0.6f)); // ~0.25 s for one position
    travel_start_us = micros();
    travelling = true;
}

bool haptics_travelling() { return travelling; }
const char *haptics_travel_result() { return travel_result; }

bool haptics_take_wall_push()
{
    bool e = wall_push_event;
    wall_push_event = false;
    return e;
}

int haptics_count() { return count; }
int haptics_pos() { return pos; }
float haptics_origin() { return origin; }
float haptics_width() { return width(); }

float haptics_update(float angle, float velocity, bool *moved)
{
    *moved = false;
    if (count == 0) {
        origin = angle; // free spin; position 0 sits wherever the knob is
        return 0;
    }

    const HapticParams &p = haptic_params;
    float w = width();

    if (travelling) {
        uint32_t elapsed = micros() - travel_start_us;
        float s = fminf(1.0f, (float)elapsed / travel_us);
        float target = travel_from + (travel_to - travel_from) * s * s * (3 - 2 * s);
        float err = target - angle;
        bool held = fabsf(err) > TRAVEL_ABORT;
        bool arrived = s >= 1 && fabsf(err) < TRAVEL_DONE;
        bool timed_out = elapsed > travel_us + 400000;
        if (held || arrived || timed_out) {
            travelling = false;
            travel_result = arrived ? "arrived" : held ? "held" : "timeout";
            if (!arrived) origin = angle - pos * w; // stay where the hand put it, keep the index
        } else {
            return constrain(TRAVEL_KP * err, -TRAVEL_LIMIT, TRAVEL_LIMIT) - TRAVEL_KD * velocity;
        }
    }

    float d = angle - (origin + pos * w);
    float snap = w * p.snap;
    int before = pos;
    while (d > snap && pos < count - 1) {
        pos++;
        d -= w;
    }
    while (d < -snap && pos > 0) {
        pos--;
        d += w;
    }
    if (pos != before) {
        *moved = true;
        click_dir = pos > before ? 1.0f : -1.0f;
        click_until_us = micros() + (uint32_t)(p.click_ms * 1000);
    }

    float ad = fabsf(d), sign = d > 0 ? 1.0f : -1.0f;
    float dead = p.dead_deg * DEG_TO_RAD;
    float half = w * 0.5f;
    bool past_end = (pos == 0 && d < 0) || (pos == count - 1 && d > 0);

    if (past_end && ad > half + WALL_PUSH && !wall_pushed) {
        wall_pushed = true;
        wall_push_event = true;
        // a tick back toward the user's hand confirms it
        click_dir = -sign;
        click_until_us = micros() + (uint32_t)(p.click_ms * 1000);
    } else if (!past_end || ad < half) {
        wall_pushed = false;
    }

    float t = 0;
    if (ad >= dead) {
        float x = (ad - dead) / (half - dead); // 0 at the center, 1 at the boundary
        if (past_end) {
            // No neighbor to fall into: keep the spring rising, then hit the wall.
            t = -sign * p.peak * fminf(x, 1.0f);
            if (ad > half) t -= sign * p.wall * (ad - half);
        } else {
            // Blend of a linear spring (peaks at the boundary) and a sine hump
            // (peaks halfway, back to zero at the boundary), both scaled to `peak`.
            float lin = fminf(x, 1.0f);
            float sine = sinf(PI * lin);
            t = -sign * p.peak * ((1 - p.shape) * lin + p.shape * sine);
        }
    }

    // The tick: a short push in the direction of travel right after crossing over.
    if (p.click > 0 && (int32_t)(click_until_us - micros()) > 0) t += click_dir * p.click;

    return t - p.kd * velocity;
}
