#pragma once

#include <Arduino.h>
#include <IPAddress.h>

// Upper bound on systems we will track. Beszel installs are small; 16 is plenty
// and keeps the ServerStat array off the heap.
#define BESZEL_MAX_SERVERS 16

// One CPU sample per minute, so this is a 60-minute window.
#define CPU_HIST_LEN 60

struct ServerStat {
  char     id[20];          // PocketBase record id, needed to query history
  char     name[24];
  bool     up;              // true only for status "up"
  char     status[10];      // raw Beszel status: up / down / paused / pending
  uint32_t updatedEpoch;    // record mtime, UTC; ~when a down system went down
  float    cpu;             // percent
  float    mem;             // percent
  float    disk;            // percent
  float    temp;            // degrees C, 0 when the agent reports none
  uint32_t uptimeSec;
  uint64_t bandwidthBytes;

  // CPU percent over the last hour, oldest first. Rounded to whole percent so
  // a full set of servers costs under a kilobyte.
  uint8_t  cpuHist[CPU_HIST_LEN];
  uint8_t  histCount;
};

// Negative return codes from beszelFetch().
enum {
  BESZEL_ERR_NO_HOST = -1,
  BESZEL_ERR_HTTP    = -2,
  BESZEL_ERR_AUTH    = -3,
  BESZEL_ERR_PARSE   = -4,
};

// Resolve BESZEL_MDNS_NAME via mDNS, falling back to BESZEL_FALLBACK_IP.
// Must be called after WiFi is up, and again after every reconnect.
bool beszelResolveHost();
void beszelForgetHost();
bool beszelHaveHost();
const char *beszelHostString();

// Exchange the configured credentials for a PocketBase token.
bool beszelAuth();
void beszelForgetToken();
bool beszelHaveToken();

// Fill `out` with up to `maxCount` systems. Returns the count written, or one
// of the negative BESZEL_ERR_* codes. Re-authenticates once on a 401.
int beszelFetch(ServerStat *out, uint8_t maxCount);

// Fill s.cpuHist / s.histCount from the system_stats collection. Returns false
// on any failure, leaving whatever history the struct already held.
bool beszelFetchHistory(ServerStat &s);

// Human-readable description of the most recent failure, for the UI footer.
const char *beszelLastError();
