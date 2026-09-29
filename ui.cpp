#include "ui.h"
#include "config.h"

#include <time.h>

// Panel is 300 x 400, portrait, 1 bit per pixel.
#define SCR_W        300
#define SCR_H        400
#define MARGIN_L     8
#define MARGIN_R     292

#define HEADER_H     26
#define BODY_TOP     32
#define FOOTER_LINE  378
#define FOOTER_BASE  392
#define BODY_BOTTOM  FOOTER_LINE

// A reporting server occupies this much vertical space; a non-reporting one
// collapses to a single line.
#define BLOCK_H      67
#define OFFLINE_H    24

// CPU history chart, in place of what used to be the CPU bar.
#define CHART_Y_OFF  15
#define CHART_H      19
#define FONT_TINY    u8g2_font_4x6_tr

// Bar geometry. Label sits left of the bar, percentage right of it.
#define BAR_X        34
#define BAR_W        228
#define BAR_H        9

// Compact-mode value columns (right edges).
#define COL_CPU      196
#define COL_MEM      244
#define COL_DSK      292
#define COMPACT_ROW_H 26

#define FONT_TITLE   u8g2_font_helvB12_tr
#define FONT_NAME    u8g2_font_helvB08_tr
#define FONT_SMALL   u8g2_font_6x12_tf

static U8G2 *g_u = nullptr;

void uiBegin(U8G2 *u)
{
  g_u = u;
}

// --- small helpers ---------------------------------------------------------

static void drawRightStr(int16_t xRight, int16_t y, const char *s)
{
  g_u->drawStr(xRight - g_u->getStrWidth(s), y, s);
}

static void drawCenterStr(int16_t y, const char *s)
{
  g_u->drawStr((SCR_W - g_u->getStrWidth(s)) / 2, y, s);
}

// Copy `src` into `dst`, trimming with an ellipsis until it fits `maxWidth`.
// Assumes the caller has already selected the font.
static void fitStr(char *dst, size_t dstSize, const char *src, int16_t maxWidth)
{
  snprintf(dst, dstSize, "%s", src);
  if (g_u->getStrWidth(dst) <= maxWidth) return;

  size_t len = strlen(dst);
  while (len > 1) {
    len--;
    dst[len] = '\0';
    char probe[32];
    snprintf(probe, sizeof(probe), "%s..", dst);
    if (g_u->getStrWidth(probe) <= maxWidth) {
      snprintf(dst, dstSize, "%s", probe);
      return;
    }
  }
}

static void formatUptime(char *dst, size_t dstSize, uint32_t sec)
{
  uint32_t days = sec / 86400UL;
  uint32_t hours = (sec % 86400UL) / 3600UL;
  uint32_t mins = (sec % 3600UL) / 60UL;

  if (days > 0) {
    snprintf(dst, dstSize, "%lud %luh", (unsigned long)days, (unsigned long)hours);
  } else if (hours > 0) {
    snprintf(dst, dstSize, "%luh %lum", (unsigned long)hours, (unsigned long)mins);
  } else {
    snprintf(dst, dstSize, "%lum", (unsigned long)mins);
  }
}

// --- chrome ----------------------------------------------------------------

// Four ascending bars, filled according to signal strength.
static void drawWifiIcon(int16_t x, int16_t yBottom, int32_t rssi)
{
  int bars = 0;
  if (rssi != 0) {
    if (rssi > -60)      bars = 4;
    else if (rssi > -70) bars = 3;
    else if (rssi > -80) bars = 2;
    else                 bars = 1;
  }

  for (int i = 0; i < 4; i++) {
    int16_t h = 3 + i * 3;
    int16_t bx = x + i * 4;
    int16_t by = yBottom - h;
    if (i < bars) {
      g_u->drawBox(bx, by, 3, h);
    } else {
      g_u->drawFrame(bx, by, 3, h);
    }
  }
}

static void drawHeader(int32_t rssi)
{
  g_u->setFont(FONT_TITLE);
  g_u->drawStr(MARGIN_L, 17, "BESZEL");

  drawWifiIcon(MARGIN_R - 15, 17, rssi);

  // Only show a clock once NTP has actually set the system time.
  time_t now = time(nullptr);
  struct tm tmNow;
  if (now > 1700000000 && localtime_r(&now, &tmNow)) {
    char clock[8];
    strftime(clock, sizeof(clock), "%H:%M", &tmNow);
    g_u->setFont(FONT_SMALL);
    drawRightStr(MARGIN_R - 22, 16, clock);
  }

  g_u->drawHLine(0, HEADER_H, SCR_W);
}

static void drawFooter(const ServerStat *servers, uint8_t count, const char *note)
{
  g_u->drawHLine(0, FOOTER_LINE, SCR_W);
  g_u->setFont(FONT_SMALL);

  uint8_t up = 0;
  for (uint8_t i = 0; i < count; i++) {
    if (servers[i].up) up++;
  }

  char left[24];
  snprintf(left, sizeof(left), "%u/%u up", up, count);
  g_u->drawStr(MARGIN_L, FOOTER_BASE, left);

  if (note && *note) {
    char fitted[32];
    fitStr(fitted, sizeof(fitted), note, MARGIN_R - MARGIN_L - g_u->getStrWidth(left) - 10);
    drawRightStr(MARGIN_R, FOOTER_BASE, fitted);
  }
}

// --- server rendering ------------------------------------------------------

static void drawStatusDot(int16_t cx, int16_t cy, bool up)
{
  if (up) {
    g_u->drawDisc(cx, cy, 4);
  } else {
    g_u->drawCircle(cx, cy, 4);
  }
}

static void drawBarRow(int16_t y, const char *label, float pct)
{
  if (pct < 0)   pct = 0;
  if (pct > 100) pct = 100;

  g_u->setFont(FONT_SMALL);
  g_u->drawStr(MARGIN_L, y + BAR_H - 1, label);

  g_u->drawFrame(BAR_X, y, BAR_W, BAR_H);
  int16_t inner = (int16_t)(((BAR_W - 2) * pct) / 100.0f + 0.5f);
  if (inner > 0) {
    g_u->drawBox(BAR_X + 1, y + 1, inner, BAR_H - 2);
  }

  char pctStr[8];
  snprintf(pctStr, sizeof(pctStr), "%d%%", (int)(pct + 0.5f));
  drawRightStr(MARGIN_R, y + BAR_H - 1, pctStr);
}

// CPU history as a filled area chart. The vertical axis auto-scales to the
// window's peak: on a fixed 0-100 axis an idle server is a flat line at the
// bottom, which tells you nothing. The ceiling is printed under the label so
// the scaling is never a surprise.
static void drawCpuChart(int16_t top, const ServerStat &s)
{
  const int16_t y = top + CHART_Y_OFF;

  g_u->setFont(FONT_SMALL);
  g_u->drawStr(MARGIN_L, top + 22, "CPU");

  char pctStr[8];
  snprintf(pctStr, sizeof(pctStr), "%d%%", (int)(s.cpu + 0.5f));
  drawRightStr(MARGIN_R, top + 26, pctStr);

  g_u->drawFrame(BAR_X, y, BAR_W, CHART_H);

  if (s.histCount == 0) {
    g_u->setFont(FONT_TINY);
    g_u->drawStr(BAR_X + 6, y + 12, "no history yet");
    return;
  }

  uint8_t peak = 0;
  for (uint8_t i = 0; i < s.histCount; i++) {
    if (s.cpuHist[i] > peak) peak = s.cpuHist[i];
  }
  const uint8_t scale = peak < 5 ? 5 : peak;

  g_u->setFont(FONT_TINY);
  char scaleStr[8];
  snprintf(scaleStr, sizeof(scaleStr), "%u%%", (unsigned)scale);
  g_u->drawStr(MARGIN_L, top + 31, scaleStr);

  const int16_t innerX = BAR_X + 1;
  const int16_t innerY = y + 1;
  const int16_t innerW = BAR_W - 2;
  const int16_t innerH = CHART_H - 2;

  for (int16_t px = 0; px < innerW; px++) {
    uint16_t idx = (s.histCount <= 1)
                     ? 0
                     : (uint16_t)((uint32_t)px * (s.histCount - 1) / (uint32_t)(innerW - 1));
    int16_t h = (int16_t)(((uint32_t)s.cpuHist[idx] * innerH) / scale);
    if (h > innerH) h = innerH;
    if (h > 0) {
      g_u->drawVLine(innerX + px, innerY + innerH - h, h);
    }
  }
}

// Full block: name, temp/uptime, CPU chart, memory and disk bars.
static int16_t drawServerBlock(int16_t top, const ServerStat &s)
{
  drawStatusDot(14, top + 7, true);

  g_u->setFont(FONT_NAME);
  char name[24];
  fitStr(name, sizeof(name), s.name, 150);
  g_u->drawStr(24, top + 11, name);

  // Temperature and uptime share the right side of the name row.
  g_u->setFont(FONT_SMALL);
  char meta[32];
  char up[16];
  formatUptime(up, sizeof(up), s.uptimeSec);
  if (s.temp > 0) {
    snprintf(meta, sizeof(meta), "%d\xB0" "C  up %s", (int)(s.temp + 0.5f), up);
  } else {
    snprintf(meta, sizeof(meta), "up %s", up);
  }
  drawRightStr(MARGIN_R, top + 11, meta);

  drawCpuChart(top, s);
  drawBarRow(top + 37, "MEM", s.mem);
  drawBarRow(top + 49, "DSK", s.disk);

  g_u->drawHLine(0, top + BLOCK_H - 3, SCR_W);
  return BLOCK_H;
}

// A system that is not reporting collapses to one line. Deliberately no bars
// and no percentages: the numbers Beszel still holds are stale, and drawing
// them would present a dead server as a healthy idle one.
static int16_t drawOfflineRow(int16_t top, const ServerStat &s)
{
  drawStatusDot(14, top + 10, false);

  g_u->setFont(FONT_NAME);
  char name[24];
  fitStr(name, sizeof(name), s.name, 150);
  g_u->drawStr(24, top + 14, name);

  // "down 2h 14m" reads better than a bare "offline"; paused and pending are
  // states the user chose or is waiting on, not failures.
  char label[32];
  time_t now = time(nullptr);
  if (s.updatedEpoch > 0 && now > 1700000000 && (uint32_t)now > s.updatedEpoch) {
    char since[16];
    formatUptime(since, sizeof(since), (uint32_t)now - s.updatedEpoch);
    snprintf(label, sizeof(label), "%s %s", s.status, since);
  } else {
    snprintf(label, sizeof(label), "%s", s.status);
  }

  g_u->setFont(FONT_SMALL);
  drawRightStr(MARGIN_R, top + 14, label);

  g_u->drawHLine(0, top + OFFLINE_H - 3, SCR_W);
  return OFFLINE_H;
}

// One line per server, no bars. Used when the detailed layout would overflow.
static void drawCompact(const ServerStat *servers, uint8_t count)
{
  g_u->setFont(FONT_SMALL);
  g_u->drawStr(24, BODY_TOP + 9, "SYSTEM");
  drawRightStr(COL_CPU, BODY_TOP + 9, "CPU");
  drawRightStr(COL_MEM, BODY_TOP + 9, "MEM");
  drawRightStr(COL_DSK, BODY_TOP + 9, "DSK");
  g_u->drawHLine(0, BODY_TOP + 13, SCR_W);

  int16_t top = BODY_TOP + 14;
  for (uint8_t i = 0; i < count; i++) {
    if (top + COMPACT_ROW_H > BODY_BOTTOM) break;
    const ServerStat &s = servers[i];

    drawStatusDot(14, top + 9, s.up);

    g_u->setFont(FONT_NAME);
    char name[24];
    fitStr(name, sizeof(name), s.name, 140);
    g_u->drawStr(24, top + 13, name);

    g_u->setFont(FONT_SMALL);
    if (s.up) {
      char v[8];
      snprintf(v, sizeof(v), "%d%%", (int)(s.cpu + 0.5f));
      drawRightStr(COL_CPU, top + 13, v);
      snprintf(v, sizeof(v), "%d%%", (int)(s.mem + 0.5f));
      drawRightStr(COL_MEM, top + 13, v);
      snprintf(v, sizeof(v), "%d%%", (int)(s.disk + 0.5f));
      drawRightStr(COL_DSK, top + 13, v);
    } else {
      drawRightStr(COL_DSK, top + 13, s.status);
    }

    g_u->drawHLine(0, top + COMPACT_ROW_H - 3, SCR_W);
    top += COMPACT_ROW_H;
  }
}

// --- public ----------------------------------------------------------------

void uiStatus(const char *title, const char *msg)
{
  if (!g_u) return;

  g_u->clearBuffer();
  g_u->setDrawColor(1);

  g_u->setFont(FONT_TITLE);
  drawCenterStr(SCR_H / 2 - 12, title);

  if (msg && *msg) {
    g_u->setFont(FONT_SMALL);
    drawCenterStr(SCR_H / 2 + 10, msg);
  }

  g_u->sendBuffer();
}

void uiDashboard(const ServerStat *servers, uint8_t count, const char *note, int32_t rssi)
{
  if (!g_u) return;

  g_u->clearBuffer();
  g_u->setDrawColor(1);

  drawHeader(rssi);

  if (count == 0) {
    g_u->setFont(FONT_SMALL);
    drawCenterStr(SCR_H / 2, "no systems");
  } else {
    // Does the detailed layout fit? Offline servers are cheap, so a mix of
    // healthy and down machines can still earn bars.
    int16_t needed = 0;
    for (uint8_t i = 0; i < count; i++) {
      needed += servers[i].up ? BLOCK_H : OFFLINE_H;
    }

    if (needed <= BODY_BOTTOM - BODY_TOP) {
      int16_t top = BODY_TOP;
      for (uint8_t i = 0; i < count; i++) {
        top += servers[i].up ? drawServerBlock(top, servers[i])
                             : drawOfflineRow(top, servers[i]);
      }
    } else {
      drawCompact(servers, count);
    }
  }

  drawFooter(servers, count, note);
  g_u->sendBuffer();
}
