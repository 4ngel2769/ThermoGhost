# `lib/` — the TollanAgent library goes here, but it is NOT in this repo

`TollanAgent` is proprietary. It ships **inside your own ESP32 device bundle**
in the [Tollan console](https://app.tollan.ie/) and is *"licensed for use with
the Tollan service only"*. It is therefore **gitignored** here — you are not
expected to publish it, and this repo does not.

## Where to get it

1. Open your device in the Tollan console.
2. Click **Download locally** (or **Download demo project** — that archive has
   the library already vendored in, along with your `tollan_config.h`).
3. Unzip it. The bundle contains a `TollanAgent/` folder with this layout:

   ```
   TollanAgent/
   ├─ src/TollanAgent.h, TollanAgent.cpp, TollanProtocol.*, TollanTargets.*
   ├─ examples/BasicTunnel/, examples/LanProxy/
   ├─ library.properties, library.json
   └─ LICENSE
   ```

4. Copy that `TollanAgent/` folder into this project's `lib/` directory:

   ```
   lib/
   ├─ README.md              <- this file (tracked)
   └─ TollanAgent/           <- from your bundle (gitignored)
   ```

   PlatformIO picks it up automatically from `lib/`. Nothing to wire up in
   `platformio.ini` — the library is compiled from source.

## Use the version from your bundle

Do **not** hand-copy a library from somewhere else, and do not mix versions.
Two things depend on the exact library you have:

- **Protocol compatibility.** A version mismatch puts the agent in
  `TollanState::Terminal` — it stops and will not reconnect. The library
  shipped with your bundle is the one that matches your relay.
- **The 4 KB pump buffer.** The firmware in this repo raises the WebSocket
  frame cap to 4096 bytes (`cfg.maxFrameBytes`, `TollanAgent.h`) so a real
  browser request fits in one frame. That is a requirement of the library
  *build*, not just of the sketch: `TollanAgent::pump()` has to stage a frame
  that large, otherwise a single oversized frame drops the whole tunnel and
  every page load fails with "oversize request — dropping".

  A library whose `pump()` buffer is smaller than `maxFrameBytes` will cap
  frames at the smaller value. Symptom: `curl /api` works, but a browser
  hitting `/` never gets a page.

  If that happens, check `TollanAgent.cpp` for the size of the buffer in
  `pump()` and make sure it is at least 4096.

## Version requirements

| Library | What you get |
|---|---|
| 0.1.x | Works, but **no certificate renewal** — your sensor dies when the 90-day cert expires. |
| 0.2.0+ | Adds `TollanConfig::renewUrl` and `TOLLAN_RENEW_URL`, so the board renews its own certificate ~30 days before expiry. **Use this.** |

Current bundles ship 0.2.0 or newer. The sketch in this repo compiles against
both: it sets `cfg.renewUrl` when the library supports it and prints a warning
when it does not.

## Troubleshooting

| Symptom | Cause |
|---|---|
| Build fails: `TollanAgent.h: No such file` | The library is not in `lib/`. Unzip your bundle. |
| `tollan state -> terminal (version mismatch)` | Library version does not match the relay. Re-download your bundle. |
| `tollan: library has no renewUrl` on serial | You have library 0.1.x. Re-download your bundle for 0.2.0+. |
| Browser page never loads, `curl /api` is fine | The library's `pump()` buffer is smaller than 4096 (see above). |
