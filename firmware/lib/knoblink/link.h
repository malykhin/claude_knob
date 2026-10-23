#pragma once
#include <ArduinoJson.h>

// Host link: newline-delimited JSON over USB serial, shared by all boards.

// LINK_FOCUS: the focused session changed (another session, or none). Not set when only
// its index moved because sessions before it came or went.
enum { LINK_UPDATED = 1, LINK_NEW_PENDING = 2, LINK_LOST = 4, LINK_FOCUS = 8 };

void link_init();
// Reads serial. Returns LINK_* flags.
int link_poll();
void link_send_select(int idx, bool press);
// Called for any message that isn't a "sessions" snapshot (board-specific commands).
void link_on_message(void (*handler)(JsonDocument &doc));
