#pragma once

// ---------------------------------------------------------------------------
// User configuration.
//
// These credentials are compiled into the firmware in plaintext. That is normal
// for a device like this, but keep it in mind before sharing this folder or a
// built binary.
// ---------------------------------------------------------------------------

// --- WiFi ------------------------------------------------------------------
#define WIFI_SSID           "YOUR_SSID"            // 2.4 GHz only -- the ESP32-S3 has no 5 GHz radio
#define WIFI_PASSWORD       "YOUR_WIFI_PASSWORD"

// --- Beszel ----------------------------------------------------------------
// mDNS name without the .local suffix. Beszel lives at http://monitoring.local/
#define BESZEL_MDNS_NAME    "monitoring"
// Used when mDNS resolution fails. Find it with: ping -c1 monitoring.local
#define BESZEL_FALLBACK_IP  "192.168.1.10"         // used when mDNS fails
#define BESZEL_PORT         80

// "users" for a normal account, "_superusers" for an admin account.
#define BESZEL_COLLECTION   "users"
#define BESZEL_IDENTITY     "display@example.com"
#define BESZEL_PASSWORD     "YOUR_BESZEL_PASSWORD"
// --- Behaviour -------------------------------------------------------------
// How often to refresh the live numbers. Beszel's agents only push once a
// minute, so a shorter interval does not gain resolution -- it lowers the delay
// between an agent reporting and the panel showing it. The systems endpoint is
// ~650 bytes, so this is cheap.
#define POLL_INTERVAL_MS    5000UL

// How often to refetch the CPU charts. The 1-minute history tier gains one
// sample per minute, so pulling it on every poll would be 12x the traffic for
// identical data.
#define HISTORY_INTERVAL_MS 60000UL
#define HTTP_TIMEOUT_MS     5000

// Backoff bounds applied after a failed poll or auth.
#define BACKOFF_MIN_MS      2000UL
#define BACKOFF_MAX_MS      60000UL

// --- Clock -----------------------------------------------------------------
#define NTP_SERVER          "pool.ntp.org"
// POSIX TZ string for the header clock. Examples:
//   UTC                          -> "UTC0"
//   Central European             -> "CET-1CEST,M3.5.0,M10.5.0/3"
//   US Eastern                   -> "EST5EDT,M3.2.0,M11.1.0"
//   Gulf Standard (Dubai)        -> "<+04>-4"
#define TZ_STRING           "UTC0"                 // POSIX TZ, see examples above

// Hostname this board announces on the network.
#define DEVICE_HOSTNAME     "esp32-beszel"
