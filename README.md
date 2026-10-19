# ThermoGhost

ESP32 room temperature sensor exposed through a secure Tollan tunnel.

```
poller ──HTTPS──▶ tollan relay ──(mTLS WSS, tollan.v1)──▶ ESP32
  "what's the temp in the living room?"              serves JSON reading
```

The board opens an outbound mutual-TLS WebSocket to the relay and holds it; no
inbound ports, no port-forwarding, works behind NAT/CGNAT.

## Parts

- `src/main.cpp` — firmware: TollanAgent handler mode + DHT22/DS18B20 reader.
  Set `ROOM_ID` per sensor (living, kitchen, office, ...). The room id is
  echoed in every JSON response.
- `src/web_page.h` — tiny status page (`GET /`, polls `GET /api`).
- `src/secrets.h.example` / `src/tollan_config.h.example` — copy to
  `src/secrets.h` / `src/tollan_config.h` (both gitignored, never commit).
- `lib/` — **the TollanAgent library is not shipped here.** It is proprietary
  ("licensed for use with the Tollan service only") and ships inside your own
  ESP32 device bundle. Unzip it into `lib/` — see [`lib/README.md`](lib/README.md).
- `LICENSE` — MIT for this repo's own code. It does not cover TollanAgent.

## One-time setup

1. **Tollan account/org** — sign up in the Tollan console. The free tier
   (1 device, 2 routes, 5 GB) is enough for this project.
2. **Register the device** — name e.g. `living-sensor`. **Choose bundle mode,
   not enrollment** — the ESP32 library does not support enrollment
   (on-device keygen is roadmap). Any enrollment token / `tollan-agent`
   command from the console is for the **Go agent** on a Linux box and cannot
   run on an ESP32.
3. **Build + download the bundle** (link is single-use, short expiry).
   - Copy `ca.crt`, `device.crt`, `device.key` into `src/tollan_config.h`
     (see `src/tollan_config.h.example`).
   - The bundle ships the relay host/port — paste them as given.
     The WebSocket path is the library default `TOLLAN_TUNNEL_PATH`
     (`/__tunnel`) — no override needed.
   - Copy `src/secrets.h.example` → `src/secrets.h` with your WiFi creds.
   - **Copy the bundle's `tollan_config.h` whole.** It also carries
     `TOLLAN_RENEW_URL`, without which your board stops working in 90 days
     (see [Certificate renewal](#certificate-renewal)).
4. **Install the TollanAgent library** — it is inside the bundle you just
   downloaded. Unzip it into `lib/` so this repo has `lib/TollanAgent/`.
   See [`lib/README.md`](lib/README.md) — it also explains why the library
   version matters and what breaks if you mix them up.
5. **Add a route** on the device. Pick the exposure mode deliberately, it
   decides what the relay can see — see [Route modes](#route-modes-and-what-the-relay-can-see).
   - Internal target: any placeholder (handler mode — the board answers
     directly).
6. **Access rules** (recommended): IP allowlist and/or basic auth — both
   are enforced at the edge in terminate mode.
7. **Build + flash** (pick your board env):
   ```bash
   pio run -e esp32s2mini -t upload   # ESP32-S2 Mini (LOLIN S2 Mini)
   pio run -e esp32dev -t upload      # classic ESP32 (WROOM-32 / DevKitC)
   pio device monitor -b 115200
   ```
   Watch for `tollan state -> connected`.
8. **Test**: `curl https://<your-route>/api` →
   `{"room":"living","tempC":22.5,"humidity":45.2,"tsMs":...}`

## Wiring

DS18B20 TO-92 (3-pin module) on **GPIO 13** — the default build:

```
   ESP32                     DS18B20 module
  ┌────────┐                ┌──────────────┐
  │  3V3   ├────────────────┤ VCC  (red)   │
  │  GND   ├────────────────┤ GND  (black) │
  │  IO13  ├────────────────┤ DQ   (yellow)│
  └────────┘                └──────────────┘
```

| DS18B20 | ESP32 | Note |
|---|---|---|
| VCC | 3V3 | |
| GND | GND | |
| DQ | GPIO 13 | 4.7 kΩ pull-up DQ → 3V3 |

**On the 4.7 kΩ pull-up:** it is required on a bare TO-92 sensor. Most
3-pin breakout modules (the `S`/`V`/`G` ones) already have it on board — check
before adding a second one. A module without a pull-up reports
`{"error":"sensor_read_failed"}` (DallasTemperature's -127 disconnected value
is treated as a read error, not a reading).

TO-92 pinout (flat side toward you, pins down): 1 = GND, 2 = DQ (data),
3 = VDD (3V3).

## Sensor

Compile-time switch in `src/main.cpp` (`#define SENSOR_TYPE ...`). Default
build uses **DS18B20 (TO-92)** on **GPIO 13**.

> **Why GPIO 13 and not 12?** GPIO 12 is a strapping pin (MTDI). It selects
> the flash voltage at boot: high = 1.8 V, low = 3.3 V. A DS18B20's 4.7 kΩ
> pull-up to 3V3 holds the line high at reset, so the chip may fail to boot
> or be unstable on a 3.3 V flash module. GPIO 13 has no such job.
> If you are following a build that used GPIO 12 and the board won't boot,
> move the data wire to **IO13**:
> `build_flags = -DSENSOR_PIN=12` in `platformio.ini`, or edit the `#define`.

| `SENSOR_TYPE` | Wiring | Notes |
|---|---|---|
| `SENSOR_DS18B20` (default) | DQ → GPIO 13, VCC 3V3, GND, **4.7kΩ pull-up DQ→3V3** | TO-92 air temp or waterproof probe; temp only |
| `SENSOR_DHT22` | data → GPIO 13, VCC 3V3, GND | temp + humidity |
| `SENSOR_DHT11` | data → GPIO 13 | temp + humidity, low precision |

**Customizing the pin / room / sensor without editing source:** all three are
`#ifndef`-guarded, so override per env in `platformio.ini`:

```ini
[env:esp32dev]
board = esp32dev
build_flags = -DSENSOR_PIN=13 -DROOM_ID='"my_bedroom"'
```

Safe pins if 13 is awkward: 18, 19, 21, 22, 23, 25, 26, 27, 32, 33 (all
broken out, non-strapping). Avoid 0, 12, 15 (strapping) and 6–11 (flash/PSRAM
on some modules).

## API

- `GET /api` (or `/json`, or `/` without `Accept: text/html`) →
  `{"room","tempC","humidity?","error?","tsMs","ip","rssi"}`, always HTTP 200.
- `GET /` with a browser → status page.
- `tsMs` is board uptime in ms (no wall clock on the board).

## Route modes, and what the relay can see

This is the one thing worth understanding before you publish a project like
this, because the answer changes with the mode you pick.

| | Passthrough | Terminate |
|---|---|---|
| Visitor's TLS ends at | your device | the relay edge |
| Certificate | yours, on the device | managed by the platform |
| Relay sees plaintext | never | **yes, for this route** |
| Basic auth at the edge | no — enforce in your app | yes |
| IP allowlist at the edge | yes (connection metadata) | yes |

**This board serves plain HTTP.** That means it needs **terminate** mode, and
in terminate mode the relay *does* see your reading in cleartext between the
visitor and the board. It is still end-to-end mTLS from the relay to the
board, and the relay never sees your device key, your WiFi password or your
cert — but it is not a zero-knowledge relay, and it is worth being precise
about that.

You get a genuinely zero-knowledge relay only with **passthrough**, which
needs the device to terminate TLS itself with its own certificate. That is a
bigger job than this sketch.

## Certificate renewal

Device certificates are valid for **90 days**.

Current bundles ship `TOLLAN_RENEW_URL` in `tollan_config.h`, and
TollanAgent **0.2.0+** uses it to renew the certificate on the board roughly
30 days before expiry — the board makes a new key on the chip and keeps the
renewed identity across reboots. The sketch in this repo sets `cfg.renewUrl`
when the library supports it, and prints which case you got on the serial
console at boot:

```
tollan: cert renewal enabled
tollan: library has no renewUrl (need TollanAgent 0.2.0+) — certs will NOT auto-renew
tollan: no TOLLAN_RENEW_URL in bundle — certs will NOT auto-renew (90-day expiry)
```

If you see either of the last two, the board will stop connecting when the
cert expires. Re-download your device bundle and flash the library it ships.

The board must be online at some point during its last 30 days. One that sits
off until after expiry cannot renew and needs a new bundle.

## TLS/clock gotchas (first bring-up)

- **TLS version:** most ESP32 cores negotiate **TLS 1.2**, and Tollan's relays
  accept it using forward-secret elliptic-curve suites only. If the handshake
  fails with a fatal alert — mbedTLS `-30592`, *"A fatal alert message was
  received from our peer"* — the cause is a **wrong relay host or port**, not
  the TLS version. Use `TOLLAN_RELAY_HOST` / `TOLLAN_RELAY_PORT` exactly as
  the bundle gives them to you (the bundle ships port **443**, which also
  gets through firewalls that block unusual outbound ports).
  - Note: `sdkconfig.defaults` **cannot** enable TLS 1.3 under PlatformIO's
    `framework = arduino` — the ESP-IDF/mbedTLS is precompiled in the core and
    ignores sdkconfig changes. A full ESP-IDF project (Arduino as a component)
    is the only build-side route.
- Fresh board boots near 1970 → cert date check fails (mbedTLS `-9984`). The
  agent SNTP-syncs automatically (`syncClock`), needs reachable NTP.
- `TollanState::Terminal` = protocol-version mismatch; the agent stops and
  won't reconnect. Use the library from your own bundle.

## Re-flashing for another room

Change `ROOM_ID`, register a second device in the console (each board has its
own mTLS identity), reuse this sketch, flash. Route per sensor.

## Hardening notes

- `device.key` never leaves your possession; keep it out of git (gitignored).
- The relay↔board hop is mTLS end-to-end. In terminate mode the visitor's TLS
  ends at the relay, so the relay sees that leg in plaintext — see
  [Route modes](#route-modes-and-what-the-relay-can-see).
- Put an IP allowlist or basic auth on the route. Both are enforced at the
  edge in terminate mode, and neither is enforced in passthrough mode.

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `{"error":"sensor_read_failed"}` | No sensor on the bus | Check wiring; 3-pin modules usually have the 4.7 kΩ pull-up, bare TO-92 sensors do not |
| Board will not boot | Sensor on GPIO 12 (strapping pin) holds the line high at reset | Move the data wire to GPIO 13 |
| `tollan state -> connected` never appears | Wrong relay host/port, or no route | Use the bundle's `TOLLAN_RELAY_HOST` / `TOLLAN_RELAY_PORT`; check the route is enabled |
| `tollan state -> terminal (version mismatch)` | Library version does not match the relay | Re-download your bundle and use its library |
| mbedTLS `-9984` | Clock not synced (board thinks it is 1970) | `syncClock` is on by default — make sure NTP is reachable |
| mbedTLS `-30592` | Wrong relay host/port | Use the bundle's values verbatim |
| `curl /api` works but the browser page never loads | Library's `pump()` buffer smaller than 4096 | See [`lib/README.md`](lib/README.md) |
| Tunnel works, then dies after ~90 days | No certificate renewal | See [Certificate renewal](#certificate-renewal) |
