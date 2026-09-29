#include "beszel.h"
#include "config.h"

#include <stdarg.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClient.h>

static IPAddress s_host;
static bool      s_hostValid = false;
static char      s_hostStr[20] = "-";

// PocketBase hands back a JWT; they run 300-500 chars in practice.
static char s_token[640] = {0};

static char s_error[48] = {0};

static void setError(const char *fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  vsnprintf(s_error, sizeof(s_error), fmt, args);
  va_end(args);
}

const char *beszelLastError()
{
  return s_error;
}

// --- host discovery --------------------------------------------------------

bool beszelHaveHost()
{
  return s_hostValid;
}

const char *beszelHostString()
{
  return s_hostStr;
}

void beszelForgetHost()
{
  s_hostValid = false;
  strcpy(s_hostStr, "-");
}

bool beszelResolveHost()
{
  static bool mdnsStarted = false;
  if (!mdnsStarted) {
    mdnsStarted = MDNS.begin(DEVICE_HOSTNAME);
  }

  IPAddress found;
  if (mdnsStarted) {
    found = MDNS.queryHost(BESZEL_MDNS_NAME, 3000);
  }

  // queryHost() yields 0.0.0.0 when nothing answered.
  if (found == IPAddress((uint32_t)0)) {
    if (!found.fromString(BESZEL_FALLBACK_IP)) {
      setError("bad fallback IP");
      s_hostValid = false;
      return false;
    }
    Serial.printf("[beszel] mDNS failed, using fallback %s\n", found.toString().c_str());
  } else {
    Serial.printf("[beszel] mDNS resolved %s.local -> %s\n",
                  BESZEL_MDNS_NAME, found.toString().c_str());
  }

  s_host = found;
  s_hostValid = true;
  snprintf(s_hostStr, sizeof(s_hostStr), "%s", found.toString().c_str());
  return true;
}

// --- helpers ---------------------------------------------------------------

static String baseUrl()
{
  String url = "http://";
  url += s_host.toString();
  if (BESZEL_PORT != 80) {
    url += ":";
    url += String(BESZEL_PORT);
  }
  return url;
}

static void configure(HTTPClient &http)
{
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setReuse(false);
}

// --- auth ------------------------------------------------------------------

bool beszelHaveToken()
{
  return s_token[0] != '\0';
}

void beszelForgetToken()
{
  s_token[0] = '\0';
}

bool beszelAuth()
{
  if (!s_hostValid) {
    setError("no host");
    return false;
  }

  WiFiClient client;
  HTTPClient http;
  configure(http);

  String url = baseUrl() + "/api/collections/" BESZEL_COLLECTION "/auth-with-password";
  if (!http.begin(client, url)) {
    setError("auth: begin failed");
    return false;
  }
  http.addHeader("Content-Type", "application/json");

  JsonDocument req;
  req["identity"] = BESZEL_IDENTITY;
  req["password"] = BESZEL_PASSWORD;
  String body;
  serializeJson(req, body);

  int code = http.POST(body);
  if (code != HTTP_CODE_OK) {
    setError("auth HTTP %d", code);
    Serial.printf("[beszel] auth failed: %d\n", code);
    http.end();
    return false;
  }

  // Only the token is of interest; the user record can be large.
  JsonDocument filter;
  filter["token"] = true;

  JsonDocument doc;
  DeserializationError err =
      deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();

  if (err) {
    setError("auth parse: %s", err.c_str());
    return false;
  }

  const char *token = doc["token"];
  if (!token || !*token) {
    setError("auth: no token");
    return false;
  }

  snprintf(s_token, sizeof(s_token), "%s", token);
  Serial.println("[beszel] authenticated");
  s_error[0] = '\0';
  return true;
}

// --- fetch -----------------------------------------------------------------

// Days since 1970-01-01 from a civil date (Howard Hinnant's algorithm). Used
// instead of mktime() because the incoming timestamp is UTC while the board's
// TZ is local, and timegm() is not dependable across cores.
static int32_t daysFromCivil(int32_t y, uint32_t m, uint32_t d)
{
  y -= m <= 2;
  const int32_t era = (y >= 0 ? y : y - 399) / 400;
  const uint32_t yoe = (uint32_t)(y - era * 400);
  const uint32_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int32_t)doe - 719468;
}

// "2026-09-29 11:30:21.749Z" -> UTC epoch seconds. 0 when unparseable.
static uint32_t parseIsoUtc(const char *iso)
{
  if (!iso) return 0;
  int Y, M, D, h, mi, sec;
  if (sscanf(iso, "%d-%d-%d %d:%d:%d", &Y, &M, &D, &h, &mi, &sec) != 6) {
    if (sscanf(iso, "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &mi, &sec) != 6) {
      return 0;
    }
  }
  int32_t days = daysFromCivil(Y, (uint32_t)M, (uint32_t)D);
  return (uint32_t)(days * 86400L + h * 3600L + mi * 60L + sec);
}

static void copyName(char *dst, size_t dstSize, const char *src)
{
  if (!src) {
    snprintf(dst, dstSize, "?");
    return;
  }
  snprintf(dst, dstSize, "%s", src);
}

// Performs one GET. Returns the system count, or a negative BESZEL_ERR_*.
// `httpStatus` receives the HTTP code so the caller can spot a 401.
static int fetchOnce(ServerStat *out, uint8_t maxCount, int *httpStatus)
{
  *httpStatus = 0;

  WiFiClient client;
  HTTPClient http;
  configure(http);

  String url = baseUrl() +
               "/api/collections/systems/records"
               "?perPage=" + String(BESZEL_MAX_SERVERS) +
               "&sort=name&fields=id,name,status,updated,info";

  if (!http.begin(client, url)) {
    setError("fetch: begin failed");
    return BESZEL_ERR_HTTP;
  }
  http.addHeader("Authorization", s_token);

  int code = http.GET();
  *httpStatus = code;

  if (code != HTTP_CODE_OK) {
    setError("HTTP %d", code);
    http.end();
    return BESZEL_ERR_HTTP;
  }

  // Without this filter the full `info` blob (CPU model strings, per-core
  // arrays, container lists) would dwarf the handful of numbers we draw.
  JsonDocument filter;
  JsonObject item = filter["items"].add<JsonObject>();
  item["id"]      = true;
  item["name"]    = true;
  item["status"]  = true;
  item["updated"] = true;
  JsonObject info = item["info"].to<JsonObject>();
  info["cpu"] = true;  // CPU percent
  info["mp"]  = true;  // memory percent
  info["dp"]  = true;  // disk percent
  info["bb"]  = true;  // bandwidth, bytes
  info["u"]   = true;  // uptime, seconds
  info["dt"]  = true;  // dashboard temperature

  JsonDocument doc;
  DeserializationError err =
      deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();

  if (err) {
    setError("parse: %s", err.c_str());
    return BESZEL_ERR_PARSE;
  }

  JsonArrayConst items = doc["items"].as<JsonArrayConst>();
  if (items.isNull()) {
    setError("no items");
    return BESZEL_ERR_PARSE;
  }

  uint8_t n = 0;
  for (JsonObjectConst rec : items) {
    if (n >= maxCount) break;

    ServerStat &s = out[n];

    // Hang on to the history we already hold for this slot; a server keeps its
    // chart across polls as long as the same system is still sitting here.
    char prevName[sizeof(s.name)];
    memcpy(prevName, s.name, sizeof(prevName));
    uint8_t prevHist = s.histCount;

    copyName(s.id,   sizeof(s.id),   rec["id"]);
    copyName(s.name, sizeof(s.name), rec["name"]);

    s.histCount = (strcmp(prevName, s.name) == 0) ? prevHist : 0;

    const char *status = rec["status"];
    snprintf(s.status, sizeof(s.status), "%s", status ? status : "unknown");
    s.up = status && strcmp(status, "up") == 0;
    s.updatedEpoch = parseIsoUtc(rec["updated"]);

    // A system that is not up reports nothing current. Drop its history rather
    // than leave an hour-old chart on screen looking live.
    if (!s.up) {
      s.histCount = 0;
    }

    JsonObjectConst inf = rec["info"].as<JsonObjectConst>();
    s.cpu            = inf["cpu"] | 0.0f;
    s.mem            = inf["mp"]  | 0.0f;
    s.disk           = inf["dp"]  | 0.0f;
    s.temp           = inf["dt"]  | 0.0f;
    s.uptimeSec      = inf["u"]   | (uint32_t)0;
    s.bandwidthBytes = inf["bb"]  | (uint64_t)0;

    n++;
  }

  s_error[0] = '\0';
  return n;
}

int beszelFetch(ServerStat *out, uint8_t maxCount)
{
  if (!s_hostValid) {
    setError("no host");
    return BESZEL_ERR_NO_HOST;
  }

  if (!beszelHaveToken() && !beszelAuth()) {
    return BESZEL_ERR_AUTH;
  }

  int status = 0;
  int result = fetchOnce(out, maxCount, &status);

  // A stale or revoked token: re-auth once, then try again exactly once more.
  if (status == HTTP_CODE_UNAUTHORIZED) {
    Serial.println("[beszel] token rejected, re-authenticating");
    beszelForgetToken();
    if (!beszelAuth()) {
      return BESZEL_ERR_AUTH;
    }
    result = fetchOnce(out, maxCount, &status);
  }

  return result;
}

// --- CPU history -----------------------------------------------------------

bool beszelFetchHistory(ServerStat &s)
{
  if (!s_hostValid || !beszelHaveToken()) {
    return false;
  }

  // Nothing to chart for a system that is not reporting.
  if (!s.up) {
    s.histCount = 0;
    return true;
  }

  WiFiClient client;
  HTTPClient http;
  configure(http);

  // PocketBase ids are [a-z0-9] so only the fixed operators need escaping:
  //   filter=system="<id>" && type="1m"
  String url = baseUrl() +
               "/api/collections/system_stats/records"
               "?perPage=" + String(CPU_HIST_LEN) +
               "&sort=-created&fields=stats.cpu"
               "&filter=system%3D%22" + String(s.id) + "%22%20%26%26%20type%3D%221m%22";

  if (!http.begin(client, url)) {
    return false;
  }
  http.addHeader("Authorization", s_token);

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    return false;
  }

  // Projecting down to stats.cpu shrinks an hour of samples from ~29 KB to
  // ~1.4 KB, which is the difference between this being practical and not.
  JsonDocument filter;
  filter["items"][0]["stats"]["cpu"] = true;

  JsonDocument doc;
  DeserializationError err =
      deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();

  if (err) {
    return false;
  }

  JsonArrayConst items = doc["items"].as<JsonArrayConst>();
  if (items.isNull()) {
    return false;
  }

  uint8_t tmp[CPU_HIST_LEN];
  uint8_t n = 0;
  for (JsonObjectConst rec : items) {
    if (n >= CPU_HIST_LEN) break;
    float v = rec["stats"]["cpu"] | 0.0f;
    if (v < 0)   v = 0;
    if (v > 100) v = 100;
    tmp[n++] = (uint8_t)(v + 0.5f);
  }

  // The API returns newest first; flip so the chart reads left to right.
  for (uint8_t i = 0; i < n; i++) {
    s.cpuHist[i] = tmp[n - 1 - i];
  }
  s.histCount = n;
  return true;
}
