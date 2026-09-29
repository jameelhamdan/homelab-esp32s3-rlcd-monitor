# homelab-esp32s3-rlcd-monitor

An always-on [Beszel](https://beszel.dev) dashboard for an ESP32-S3 driving a 4.2"
300x400 reflective monochrome LCD (ST7305). It joins WiFi, authenticates against
Beszel's PocketBase API, and renders every monitored system as a status dot, a
one-hour CPU chart, and memory/disk bars.

```
+----------------------------+
| BESZEL            14:32 .ıl|
+----------------------------+
| * NERV        43'C up 55d 2h|
| CPU  _/\__/\___/\_      1% |
|  5%                        |
| MEM ###...........     21% |
| DSK #..............      7% |
+----------------------------+
| * PC          28'C up 3h 55m|
| CPU _/\_/\__/\__/\_    10% |
| 18%                        |
| MEM #######........    46% |
| DSK ###########....    72% |
+----------------------------+
| o backup-vm     down 2h 14m |
| o staging             paused|
+----------------------------+
| 2/4 up               3s ago |
+----------------------------+
```

## Hardware

- ESP32-S3 (2.4 GHz WiFi only -- see gotchas)
- 4.2" 300x400 reflective LCD, ST7305 controller, SPI

| Signal | GPIO |
|--------|------|
| SCK    | 11   |
| MOSI   | 12   |
| DC     | 5    |
| CS     | 40   |
| RST    | 41   |

Change these at the top of `sketch_servermonitor.ino` if your board differs.

## Features

- **CPU history chart** per system, 60 one-minute samples. The vertical axis
  auto-scales to the window's peak with a 5% floor, because on a fixed 0-100
  axis an idle server is a flat line that tells you nothing. The scale ceiling
  is printed under the `CPU` label so the scaling is never a surprise.
- **Adaptive layout.** Up to 5 systems get the full treatment; beyond that it
  falls back to one compact line each (up to 12).
- **Honest offline handling.** A system that is down, paused or pending shows no
  bars, no chart and no percentages -- Beszel still holds its last values, and
  drawing them would present a dead server as a healthy idle one. The real
  status is shown with a duration: `down 2h 14m`, `paused`, `pending`.
- **Stale data survives an outage.** If Beszel becomes unreachable the last good
  readings stay on screen and the footer says how old they are, rather than the
  panel blanking or freezing. Backoff climbs 2s -> 60s.
- **Redundant refreshes are skipped.** A signature over everything drawn gates
  the redraw, so a reflective panel is not refreshed 12 times a minute for
  pixel-identical content.

## Setup

1. **Libraries** -- install via Arduino Library Manager:
   - [U8g2](https://github.com/olikraus/u8g2) (tested 2.36.19)
   - [ArduinoJson](https://arduinojson.org) **v7** (v6 will not compile)

2. **Config** -- copy the template and fill it in. `config.h` is gitignored
   because it holds credentials:

   ```sh
   cp config.example.h config.h
   ```

3. **Beszel account.** Create a dedicated read-only user, then add it to each
   system you want displayed. This is the step most likely to leave you staring
   at an empty dashboard -- see gotchas.

4. **Build and flash** for your ESP32-S3 board. Serial monitor at 115200:

   ```
   [boot] beszel monitor
   [wifi] connected, ip 192.168.1.42
   [beszel] mDNS resolved monitoring.local -> 192.168.1.10
   [beszel] authenticated
   [beszel] 3 systems
   ```

## Gotchas

**The ESP32-S3 radio is 2.4 GHz only.** It has no 5 GHz support at all. Pointing
`WIFI_SSID` at a 5 GHz SSID leaves the board scanning forever.

**A Beszel user sees only the systems it is assigned to.** Authentication
succeeding tells you nothing about visibility -- a valid account with no
assignments returns `totalItems: 0`, identical to an empty Beszel. Add the
account to each system's `users` relation in the PocketBase admin UI at
`http://<beszel-host>/_/`. Changing the account's *role* does not help; the
relation is what governs visibility.

**`.local` needs mDNS.** A plain `WiFiClient` cannot resolve it, so the firmware
resolves via ESPmDNS and caches the result, falling back to
`BESZEL_FALLBACK_IP`. Set that fallback to a real address -- on many networks it
is what actually carries the connection.

## API notes

Beszel is built on PocketBase, so this is plain REST.

Each record in the `systems` collection carries an `info` field holding the
latest snapshot already, so one request covers every system:

```
GET /api/collections/systems/records?fields=id,name,status,updated,info
```

| key   | meaning          | key  | meaning               |
|-------|------------------|------|-----------------------|
| `cpu` | CPU %            | `u`  | uptime, seconds       |
| `mp`  | memory %         | `dt` | dashboard temperature |
| `dp`  | disk %           | `bb` | bandwidth, bytes      |

History comes from `system_stats`. Projecting down to a single field matters a
lot here -- an hour of samples is **1.4 KB** with the projection and **29 KB**
without:

```
GET /api/collections/system_stats/records
    ?perPage=60&sort=-created&fields=stats.cpu
    &filter=system="<id>" && type="1m"
```

Agents push once per minute, so polling faster buys latency, not resolution.
The firmware polls the live numbers every 5s and refreshes charts every 60s.

## License

MIT
