#pragma once

#include <U8g2lib.h>
#include "beszel.h"

// Must be called once, after ST7305_U8g2::begin().
void uiBegin(U8G2 *u);

// Full-screen centred message. Used for boot steps and hard failures so the
// panel is never blank.
void uiStatus(const char *title, const char *msg);

// The dashboard. `note` is drawn in the footer's right slot: either a staleness
// string or an error. Pass rssi = 0 when WiFi is down.
void uiDashboard(const ServerStat *servers, uint8_t count, const char *note, int32_t rssi);
