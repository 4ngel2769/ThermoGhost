# `lib/` — the TollanAgent library goes here, but it is NOT in this repo

`TollanAgent` is proprietary. It ships **inside your own ESP32 device bundle**
in the [Tollan console](https://app.tollan.ie/) and is *"licensed only for use
with the Tollan service"*. It is therefore **gitignored** here — you are not
expected to publish it, and this repo does not.

## Where to get it

1. Open your device in the Tollan console.
2. Click **Download locally** (or **Download demo project** — that archive has
   the library already vendored in, along with your `tollan_config.h`).
3. Unzip it. The bundle contains:

   ```
   tollan_config.h     relay host/port, CA, enrollment token, renewal URL
   ca.crt              the relay CA as a raw PEM
   TollanAgent/        the library (see TollanAgent/LICENSE)
   ```

4. Copy the `TollanAgent/` folder into this project's `lib/`:

   ```
   lib/
   ├─ README.md          <- this file (tracked)
   └─ TollanAgent/       <- from your bundle (gitignored)
   ```

## It ships PRECOMPILED — one archive per chip

This is the part that bites. The library is not source you can compile; it is
a declaration-only header plus a **static archive per chip**:

```
TollanAgent/
├─ src/
│  ├─ TollanAgent.h          declarations only
│  ├─ TollanAgentDeps.cpp    a dependency shim
│  ├─ esp32/libTollanAgent.a
│  ├─ esp32s2/libTollanAgent.a
│  ├─ esp32s3/libTollanAgent.a
│  └─ esp32c3/libTollanAgent.a
```

So the linker has to be pointed at the right archive. `platformio.ini` in this
repo already does that per environment:

```ini
[env:esp32dev]
build_flags =
    -L lib/TollanAgent/src/esp32 -lTollanAgent
```

**If you see `undefined reference to tollan::TollanAgent::begin` (or `send`,
`closeChannel`, `onOpen`, …), the archive is not linked.** Check that
`lib/TollanAgent/` exists and that the `-L` path matches your chip:

| Board | `-L` path |
|---|---|
| classic ESP32 | `lib/TollanAgent/src/esp32` |
| ESP32-S2 | `lib/TollanAgent/src/esp32s2` |
| ESP32-S3 | `lib/TollanAgent/src/esp32s3` |
| ESP32-C3 | `lib/TollanAgent/src/esp32c3` |

The archive must be built for the **Arduino-ESP32 2.x** core. The library
also expects `TollanAgentDeps.cpp` to be compiled, which is why it lives
inside `src/`.

## Version requirements

| Library | What you get |
|---|---|
| 0.1.x | Source form. No enrollment, no renewal, no health check-in. **Superseded — do not use.** |
| 0.2.0+ | Adds `TollanConfig::renewUrl` and `TOLLAN_RENEW_URL`, so the board renews its own certificate ~30 days before the 90-day expiry. |
| 0.3.0+ | Adds first-boot **enrollment** (`enrollUrl` / `enrollToken` / `enrollCaCert`), the health check-in (`deviceId` / `targetsUrl`) and `agent.log()`. **Current bundles ship this.** |

The sketch in this repo compiles against all of them: it sets `cfg.enroll*`,
`cfg.renewUrl` and `cfg.deviceId` only when the linked library has those
fields, and prints which case it hit on the serial console at boot.

## Bundle format: two generations are still in the wild

Tollan moved ESP32 provisioning to **enrollment**. Current bundles carry a
one-time token instead of a client certificate:

| | Enrollment bundle (current) | Legacy bundle |
|---|---|---|
| Config provides | `TOLLAN_ENROLL_URL` / `_TOKEN` / `_CA` | `DEVICE_CERT` / `DEVICE_KEY` |
| Identity | board generates its own key on first boot | cert + key shipped in the bundle |
| Needs library | 0.3.0+ | any |

The sketch detects which one it has from `tollan_config.h` and wires the
right one. A bundle's `tollan_config.h` also holds a **one-time enrollment
token** — it is a secret until the board has enrolled. Keep it out of git and
off shared drives.

## The 1600-byte frame ceiling

The shipped library stages an inbound WebSocket frame in a fixed **1600-byte**
receive buffer. A request that does not fit is dropped and the tunnel is
reset with `oversize request — dropping`.

- `curl /api` sends a small request — fine.
- A browser `GET /` sends ~700–1200 B of headers plus the relay's forwarded
  headers — usually fine, but there is little margin.

`cfg.maxFrameBytes` can only *lower* this ceiling, never raise it: the
buffer is inside the precompiled archive. If you need more headroom, that is
an upstream library change, not a sketch change.

## Troubleshooting

| Symptom | Cause |
|---|---|
| Build fails: `TollanAgent.h: No such file` | The library is not in `lib/`. Unzip your bundle. |
| `undefined reference to tollan::TollanAgent::begin` | The precompiled archive is not linked — check the `-L` path for your chip in `platformio.ini`. |
| `'DEVICE_CERT' was not declared in this scope` | You have an enrollment bundle (no cert/key) but the sketch expects the legacy format — update the sketch, or use a 0.3.0+ library. |
| `tollan: bundle uses enrollment but this library has no enroll support` | Library is older than 0.3.0. Re-download your bundle for a newer library. |
| `tollan: no credentials found in tollan_config.h` | The bundle's `tollan_config.h` is not in `src/`. |
| `tollan: cert renewal enabled` absent | The bundle has no `TOLLAN_RENEW_URL`, or the library predates `renewUrl` (needs 0.2.0+). The board dies at the 90-day cert expiry. |
| `tollan state -> terminal (version mismatch)` | Library version does not match the relay. Re-download your bundle and use its library. |
| Browser page never loads, `curl /api` is fine | The request exceeded the 1600 B frame ceiling (see above). |
