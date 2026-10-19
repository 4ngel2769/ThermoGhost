#pragma once

// Local status page served at GET / (both over the Tollan tunnel and on the
// LAN). Kept in its own header so main.cpp stays about behaviour, not markup.
//
// Routes:
//   GET /      -> this page (if the client accepts HTML)
//   GET /api   -> JSON reading (also the default for curl / pollers)
//   GET /json  -> alias of /api
//
// The page polls /api every 3 s and updates the DOM — no framework, ~1 kB.

#include <pgmspace.h>

static const char PAGE_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Sensor</title>
<link href="https://fonts.googleapis.com/css2?family=Press+Start+2P&display=swap" rel="stylesheet">
<style>
* { box-sizing: border-box; margin: 0; padding: 0; border-radius: 0 !important; }
body {
  background: #d7d7d7; color: #111;
  font-family: 'Press Start 2P', monospace;
  min-height: 100vh; display: flex; align-items: center; justify-content: center;
  padding: 16px; image-rendering: pixelated;
}
.card { background: #fff; border: 4px solid #111; box-shadow: 8px 8px 0 #111; width: 320px; max-width: 92vw; }
.header { background: #111; color: #fff; display: flex; justify-content: space-between; align-items: center; font-size: 10px; padding: 12px; line-height: 1.4; }
.status { color: #7cfc00; font-size: 8px; }
.temp { font-size: 36px; padding: 20px 16px 8px; line-height: 1; }
.unit { font-size: 14px; }
.sub { font-size: 9px; padding: 0 16px 16px; line-height: 1.6; }
.meta { border-top: 4px solid #111; display: grid; grid-template-columns: 1fr 1fr; font-size: 8px; line-height: 1.6; }
.meta div { padding: 10px; }
.meta div:nth-child(odd) { border-right: 4px solid #111; }
.meta div:nth-child(n+3) { border-top: 4px solid #111; }
.meta a { color: #111; }
</style>
</head>
<body>
<div class="card">
  <div class="header"><span id="room">ROOM</span><span class="status" id="status">&#9632; ONLINE</span></div>
  <div class="temp"><span id="temp">--</span><span class="unit">&deg;C</span></div>
  <div class="sub" id="sub">Loading...</div>
  <div class="meta">
    <div>IP<br><span id="ip">-</span></div>
    <div>WIFI<br><span id="wifi">-</span></div>
    <div>UP<br><span id="up">-</span></div>
    <div>API<br><a href="/api">/api</a></div>
  </div>
</div>
<script>
async function refresh() {
  const el = (id) => document.getElementById(id);
  try {
    const d = await (await fetch('/api')).json();
    el('room').textContent = d.room || '';
    if (d.error) {
      el('temp').textContent = 'ERR';
      el('sub').textContent = d.error;
      el('status').textContent = '\u25A0 FAIL';
      el('status').style.color = '#ff5555';
    } else {
      el('temp').textContent = d.tempC.toFixed(1);
      el('sub').textContent = (d.humidity != null) ? ('HUM ' + d.humidity.toFixed(1) + '%') : 'SENSOR OK';
      el('status').textContent = '\u25A0 ONLINE';
      el('status').style.color = '#7cfc00';
    }
    el('ip').textContent = d.ip || '-';
    el('wifi').textContent = (d.rssi ?? '-') + ' dBm';
    const s = Math.floor((d.tsMs || 0) / 1000);
    el('up').textContent = Math.floor(s / 3600) + 'h ' + Math.floor((s % 3600) / 60) + 'm ' + (s % 60) + 's';
  } catch (e) {
    el('status').textContent = '\u25A0 OFFLINE';
    el('status').style.color = '#ff5555';
  }
}
refresh();
setInterval(refresh, 3000);
</script>
</body>
</html>
)HTML";
