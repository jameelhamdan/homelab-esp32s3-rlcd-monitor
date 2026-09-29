// Beszel server monitor for ESP32-S3 + 4.2" reflective LCD (ST7305, 300x400).
//
// Joins WiFi, resolves the Beszel host over mDNS, authenticates against its
// PocketBase API, and renders every monitored system as a status dot plus
// CPU / MEM / DISK bars.
//
// Configure WiFi and Beszel credentials in config.h.

#include "ST7305_U8g2.h"
#include "beszel.h"
#include "config.h"
#include "ui.h"

#include <WiFi.h>
#include <time.h>

// Set to 1 to render a hardcoded dataset without touching the network.
// Useful for iterating on the layout.
#define UI_DEMO 0

#define RLCD_SCK_PIN  11
#define RLCD_MOSI_PIN 12
#define RLCD_DC_PIN   5
#define RLCD_CS_PIN   40
#define RLCD_RST_PIN  41

static ST7305_U8g2 lcd(RLCD_SCK_PIN, RLCD_MOSI_PIN, RLCD_DC_PIN, RLCD_CS_PIN, RLCD_RST_PIN);
static U8G2 *u8g2 = nullptr;

enum State {
  ST_WIFI,
  ST_RESOLVE,
  ST_AUTH,
  ST_POLL,
  ST_IDLE,
};

static State    g_state = ST_WIFI;
static uint32_t g_nextAttemptMs = 0;   // backoff gate for the retrying states
static uint32_t g_backoffMs = BACKOFF_MIN_MS;

static ServerStat g_servers[BESZEL_MAX_SERVERS];
static uint8_t    g_count = 0;
static bool       g_haveData = false;

static uint32_t g_lastPollMs = 0;
static uint32_t g_lastHistoryMs = 0;
static uint32_t g_lastGoodMs = 0;
static int      g_lastDrawnMinute = -1;
static uint32_t g_lastSig = 0;
static bool     g_everRendered = false;

static uint32_t g_wifiStartedMs = 0;

// --- helpers ---------------------------------------------------------------

static void resetBackoff()
{
  g_backoffMs = BACKOFF_MIN_MS;
}

static void applyBackoff()
{
  g_nextAttemptMs = millis() + g_backoffMs;
  g_backoffMs *= 2;
  if (g_backoffMs > BACKOFF_MAX_MS) {
    g_backoffMs = BACKOFF_MAX_MS;
  }
}

static bool backoffElapsed()
{
  return (int32_t)(millis() - g_nextAttemptMs) >= 0;
}

static int currentMinute()
{
  time_t now = time(nullptr);
  struct tm tmNow;
  if (now > 1700000000 && localtime_r(&now, &tmNow)) {
    return tmNow.tm_min;
  }
  return -1;
}

// Footer note: the freshness of the data, or the current error if there is one.
static void buildNote(char *dst, size_t dstSize)
{
  const char *err = beszelLastError();
  if (err && *err) {
    snprintf(dst, dstSize, "%s", err);
    return;
  }

  if (g_lastGoodMs == 0) {
    snprintf(dst, dstSize, "%s", "");
    return;
  }

  uint32_t ageSec = (millis() - g_lastGoodMs) / 1000UL;
  if (ageSec < 60) {
    snprintf(dst, dstSize, "%lus ago", (unsigned long)ageSec);
  } else {
    snprintf(dst, dstSize, "%lum ago", (unsigned long)(ageSec / 60UL));
  }
}

// Fingerprint of everything the dashboard actually draws, at the precision it
// draws it. Polling at 5 s while agents report once a minute means most frames
// are pixel-identical; this lets us skip those refreshes.
static uint32_t frameSignature()
{
  uint32_t h = 2166136261UL;
  auto mix = [&h](uint32_t v) {
    h ^= v;
    h *= 16777619UL;
  };

  mix((uint32_t)(currentMinute() + 1));   // the header clock
  mix(g_count);

  for (uint8_t i = 0; i < g_count; i++) {
    const ServerStat &s = g_servers[i];
    for (const char *p = s.name;   *p; p++) mix((uint8_t)*p);
    for (const char *p = s.status; *p; p++) mix((uint8_t)*p);
    mix((uint32_t)(s.cpu  * 10.0f));
    mix((uint32_t)(s.mem  * 10.0f));
    mix((uint32_t)(s.disk * 10.0f));
    mix((uint32_t)(s.temp * 10.0f));
    mix(s.uptimeSec / 60);                // shown to the minute at finest
    mix(s.histCount);
    for (uint8_t k = 0; k < s.histCount; k++) mix(s.cpuHist[k]);
  }
  return h;
}

static void render()
{
  char note[48];
  buildNote(note, sizeof(note));

  int32_t rssi = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;
  uiDashboard(g_servers, g_count, note, rssi);

  g_lastDrawnMinute = currentMinute();
  g_lastSig = frameSignature();
  g_everRendered = true;
}

// Redraw only if the visible content would differ. The footer's "Ns ago" is
// deliberately excluded: while polls are succeeding the data is fresh by
// definition, and the failure paths call render() directly so staleness and
// error text still reach the screen.
static void renderIfChanged()
{
  if (g_everRendered && frameSignature() == g_lastSig) {
    return;
  }
  render();
}

// --- demo data -------------------------------------------------------------

#if UI_DEMO
static void loadDemoData()
{
  struct {
    const char *name;
    const char *status;
    float cpu, mem, disk, temp;
    uint32_t uptime;
  } demo[] = {
    {"NERV",                "up",      1.4f, 20.5f,  6.7f, 43.0f, 4761511},
    {"PC",                  "up",      9.7f, 46.0f, 71.5f, 27.9f,   14159},
    {"a-very-long-hostname","up",     99.9f, 97.2f, 88.0f, 71.0f,    7200},
    {"backup-vm",           "down",    0.0f,  0.0f,  0.0f,  0.0f,       0},
    {"staging",             "paused",  0.0f,  0.0f,  0.0f,  0.0f,       0},
  };

  g_count = sizeof(demo) / sizeof(demo[0]);
  for (uint8_t i = 0; i < g_count; i++) {
    ServerStat &s = g_servers[i];
    snprintf(s.name, sizeof(s.name), "%s", demo[i].name);
    snprintf(s.status, sizeof(s.status), "%s", demo[i].status);
    s.up             = strcmp(demo[i].status, "up") == 0;
    s.cpu            = demo[i].cpu;
    s.mem            = demo[i].mem;
    s.disk           = demo[i].disk;
    s.temp           = demo[i].temp;
    s.uptimeSec      = demo[i].uptime;
    s.bandwidthBytes = 0;
    s.updatedEpoch   = 0;

    // A synthetic trace with a couple of bursts, so the chart shape is visible.
    if (s.up) {
      s.histCount = CPU_HIST_LEN;
      for (uint8_t k = 0; k < CPU_HIST_LEN; k++) {
        float base = s.cpu * 0.6f;
        float burst = (k % 17 < 3) ? s.cpu * 2.2f : 0.0f;
        float wobble = (float)((k * 7919) % 13) * 0.3f;
        float v = base + burst + wobble;
        s.cpuHist[k] = (uint8_t)(v > 100 ? 100 : v);
      }
    } else {
      s.histCount = 0;
    }
  }
  g_haveData = true;
  g_lastGoodMs = millis();
}
#endif

// --- setup / loop ----------------------------------------------------------

void setup()
{
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[boot] beszel monitor");

  lcd.begin(0, U8G2_R0);
  u8g2 = lcd.getU8g2();
  uiBegin(u8g2);

#if UI_DEMO
  loadDemoData();
  render();
  return;
#endif

  uiStatus("BESZEL", "connecting to WiFi");

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(DEVICE_HOSTNAME);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  g_wifiStartedMs = millis();

  g_state = ST_WIFI;
}

void loop()
{
#if UI_DEMO
  delay(1000);
  return;
#endif

  uint32_t now = millis();

  // A dropped link invalidates the resolved address, so start over from WiFi.
  if (WiFi.status() != WL_CONNECTED && g_state != ST_WIFI) {
    Serial.println("[wifi] link lost");
    beszelForgetHost();
    beszelForgetToken();
    g_state = ST_WIFI;
    g_wifiStartedMs = now;
    resetBackoff();
  }

  switch (g_state) {
    case ST_WIFI:
      if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[wifi] connected, ip %s\n", WiFi.localIP().toString().c_str());
        configTzTime(TZ_STRING, NTP_SERVER);
        g_state = ST_RESOLVE;
        if (!g_haveData) {
          uiStatus("BESZEL", "resolving " BESZEL_MDNS_NAME ".local");
        }
      } else if (now - g_wifiStartedMs > 20000UL) {
        // Nudge a stuck association rather than waiting forever.
        Serial.println("[wifi] retrying association");
        WiFi.disconnect();
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        g_wifiStartedMs = now;
      }
      break;

    case ST_RESOLVE:
      if (!backoffElapsed()) break;
      if (beszelResolveHost()) {
        resetBackoff();
        g_state = ST_AUTH;
        if (!g_haveData) {
          uiStatus("BESZEL", "authenticating");
        }
      } else {
        applyBackoff();
        if (!g_haveData) {
          uiStatus("BESZEL", beszelLastError());
        }
      }
      break;

    case ST_AUTH:
      if (!backoffElapsed()) break;
      if (beszelAuth()) {
        resetBackoff();
        g_state = ST_POLL;
      } else {
        applyBackoff();
        if (g_haveData) {
          render();
        } else {
          uiStatus("BESZEL", beszelLastError());
        }
      }
      break;

    case ST_POLL: {
      if (!backoffElapsed()) break;

      int n = beszelFetch(g_servers, BESZEL_MAX_SERVERS);
      g_lastPollMs = millis();

      if (n >= 0) {
        g_count = (uint8_t)n;
        g_haveData = true;
        g_lastGoodMs = g_lastPollMs;
        resetBackoff();
        Serial.printf("[beszel] %d systems\n", n);

        // Charts refresh on their own, slower clock. Systems that are down,
        // paused or pending are skipped -- there is nothing to plot and no
        // reason to spend the round trip.
        bool historyDue = (g_lastHistoryMs == 0) ||
                          (g_lastPollMs - g_lastHistoryMs >= HISTORY_INTERVAL_MS);

        for (uint8_t i = 0; i < g_count; i++) {
          ServerStat &sv = g_servers[i];
          if (!sv.up) continue;
          // histCount == 0 means this system is new, or just came back up, so
          // give it a chart now rather than making it wait out the interval.
          if (!historyDue && sv.histCount != 0) continue;
          if (!beszelFetchHistory(sv)) {
            Serial.printf("[beszel] history failed for %s\n", sv.name);
          }
        }

        if (historyDue) {
          g_lastHistoryMs = g_lastPollMs;
        }

        renderIfChanged();
        g_state = ST_IDLE;
      } else {
        Serial.printf("[beszel] fetch failed: %s\n", beszelLastError());
        applyBackoff();
        if (n == BESZEL_ERR_NO_HOST) {
          g_state = ST_RESOLVE;
        }
        // Keep the last known data on screen; the footer explains it is stale.
        if (g_haveData) {
          render();
        } else {
          uiStatus("BESZEL", beszelLastError());
        }
      }
      break;
    }

    case ST_IDLE:
      if (now - g_lastPollMs >= POLL_INTERVAL_MS) {
        g_state = ST_POLL;
      } else {
        // Otherwise redraw only when the displayed clock would change. This is
        // a reflective panel showing near-static content; there is nothing to
        // gain from refreshing at 1 Hz.
        int minute = currentMinute();
        if (minute >= 0 && minute != g_lastDrawnMinute) {
          render();
        }
      }
      break;
  }

  delay(50);
}
