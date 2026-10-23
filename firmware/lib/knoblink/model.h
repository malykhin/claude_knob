#pragma once
#include <Arduino.h>

#define MAX_SESSIONS 12

enum SessionState : uint8_t { ST_IDLE, ST_RUNNING, ST_PENDING, ST_DONE };

struct Session {
    char id[48];
    char name[28];
    SessionState state;
};

struct Model {
    Session items[MAX_SESSIONS];
    int count = 0;
    int sel = 0;           // knob position
    int focus = -1;        // session whose terminal is in front (from the host), -1 = none
    bool hostSeen = false; // received at least one snapshot
};

extern Model model;
