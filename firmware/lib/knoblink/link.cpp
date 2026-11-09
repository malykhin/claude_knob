#include <Arduino.h>
#include <ArduinoJson.h>

#include "link.h"
#include "model.h"

#ifndef FW_VERSION
#define FW_VERSION "0.2.0"
#endif
// The daemon re-sends a snapshot every few seconds; silence longer than this means it's gone.
#define HOST_TIMEOUT_MS 8000
#define HELLO_EVERY_MS 1000
// While connected: lets the host notice a wedged USB transmit (it resets the chip).
#define ALIVE_EVERY_MS 2000

static char line[4096];
static size_t len = 0;
static bool overflow = false;
static uint32_t last_rx = 0;
static uint32_t last_hello = 0, last_alive = 0;
static void (*extra_handler)(JsonDocument &) = nullptr;

void link_init()
{
    // The default 256-byte RX buffer is smaller than a snapshot with 3+ sessions,
    // and the tail of the line would be dropped. Must be set before begin().
    Serial.setRxBufferSize(sizeof(line));
    Serial.begin(115200);
    // Don't stall when no host has the port open.
    Serial.setTxTimeoutMs(0);
}

void link_on_message(void (*handler)(JsonDocument &doc)) { extra_handler = handler; }

static SessionState parse_state(const char *s)
{
    if (!strcmp(s, "running")) return ST_RUNNING;
    if (!strcmp(s, "pending")) return ST_PENDING;
    if (!strcmp(s, "done")) return ST_DONE;
    return ST_IDLE;
}

static int find(const Session *items, int n, const char *id)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(items[i].id, id)) return i;
    return -1;
}

static int apply_sessions(JsonDocument &doc)
{
    static Session prev[MAX_SESSIONS];
    int prev_n = model.count;
    memcpy(prev, model.items, sizeof(prev));
    char sel_id[sizeof(prev[0].id)] = "";
    if (model.hostSeen && model.sel < prev_n) strlcpy(sel_id, prev[model.sel].id, sizeof(sel_id));
    char focus_id[sizeof(prev[0].id)] = "";
    if (model.focus >= 0 && model.focus < prev_n) strlcpy(focus_id, prev[model.focus].id, sizeof(focus_id));

    int n = 0;
    for (JsonObject it : doc["items"].as<JsonArray>()) {
        if (n == MAX_SESSIONS) break;
        Session &s = model.items[n++];
        strlcpy(s.id, it["id"] | "", sizeof(s.id));
        strlcpy(s.name, it["name"] | "", sizeof(s.name));
        s.state = parse_state(it["state"] | "idle");
    }
    model.count = n;

    // Stay on the session we were on, even if others were added or removed before it.
    // The host's `sel` is only used when that session is gone (or on first contact),
    // so a snapshot that crossed a fast turn in flight can't yank the selection back.
    int keep = sel_id[0] ? find(model.items, n, sel_id) : -1;
    int sel = keep >= 0 ? keep : (int)(doc["sel"] | 0);
    model.sel = n == 0 ? 0 : constrain(sel, 0, n - 1);
    // Hosts without focus tracking don't send it: then the selection counts as focused.
    int focus = doc["focus"] | (n == 0 ? -1 : model.sel);
    model.focus = focus < n ? focus : -1;
    model.hostSeen = true;

    int flags = LINK_UPDATED;
    const char *new_focus_id = model.focus >= 0 ? model.items[model.focus].id : "";
    if (strcmp(focus_id, new_focus_id)) flags |= LINK_FOCUS;
    for (int i = 0; i < n; i++) {
        int p = find(prev, prev_n, model.items[i].id);
        if (model.items[i].state == ST_PENDING && (p < 0 || prev[p].state != ST_PENDING)) flags |= LINK_NEW_PENDING;
    }
    return flags;
}

static int apply(const char *json)
{
    JsonDocument doc;
    if (deserializeJson(doc, json)) return 0;
    const char *t = doc["t"] | "";
    if (!strcmp(t, "sessions")) {
        last_rx = millis();
        return apply_sessions(doc);
    }
    if (extra_handler) extra_handler(doc);
    return 0;
}

int link_poll()
{
    int flags = 0;
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n') {
            line[len] = 0;
            if (!overflow && len > 0) flags |= apply(line);
            len = 0;
            overflow = false;
        } else if (len < sizeof(line) - 1) {
            line[len++] = c;
        } else {
            overflow = true;
        }
    }

    uint32_t now = millis();
    if (model.hostSeen && now - last_rx > HOST_TIMEOUT_MS) {
        model.hostSeen = false;
        flags |= LINK_LOST;
    }
    if (!model.hostSeen && now - last_hello > HELLO_EVERY_MS) {
        last_hello = now;
        Serial.print("{\"t\":\"hello\",\"fw\":\"" FW_VERSION "\"}\n");
    }
    if (model.hostSeen && now - last_alive > ALIVE_EVERY_MS) {
        last_alive = now;
        Serial.print("{\"t\":\"alive\"}\n");
    }
    return flags;
}

void link_send_select(int idx, bool press)
{
    if (idx < 0 || idx >= model.count) return;
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"t\":\"%s\",\"idx\":%d,\"id\":\"%s\"}\n",
             press ? "press" : "select", idx, model.items[idx].id);
    Serial.print(buf);
}
