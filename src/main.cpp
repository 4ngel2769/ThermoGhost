// ThermoGhost — ESP32 room temperature sensor served over a Tollan tunnel.
//
// Data flow:
//   poller --HTTPS--> Tollan relay --mTLS WSS (tollan.v1)--> this board
//
// The board dials OUT to the relay and holds the connection, so it needs no
// inbound ports and works behind NAT/CGNAT. Each tunneled HTTP request is
// answered here with a JSON reading (or a small status page for browsers).
//
// Provisioning (one-time per board):
//   1. Register the board in the Tollan console, build an ESP32 device bundle.
//   2. Drop the bundle's tollan_config.h (relay host/port + certs) into src/.
//   3. Copy secrets.h.example -> secrets.h with your WiFi credentials.
//   Both files are gitignored and never committed.
//
// Room identity: ROOM_ID names the room this sensor lives in (masterbr, br1,
// living, kitchen, ...). It is echoed in every response so the poller can
// verify the right sensor answered.

#include <TollanAgent.h>
#include <WiFi.h>

#include "web_page.h"

using namespace tollan;

// ---------------------------------------------------------------------------
// Board configuration.
// Override any of these per-env in platformio.ini without editing source:
//   build_flags = -DSENSOR_PIN=13 -DROOM_ID='"my_bedroom"'
// ---------------------------------------------------------------------------
#ifndef ROOM_ID
#define ROOM_ID "my_bedroom"  // room descriptor — change per sensor
#endif

#define SENSOR_DHT22 1
#define SENSOR_DHT11 2
#define SENSOR_DS18B20 3
#ifndef SENSOR_TYPE
#define SENSOR_TYPE SENSOR_DS18B20  // SENSOR_DHT22 | SENSOR_DHT11 | SENSOR_DS18B20
#endif
#ifndef SENSOR_PIN
// GPIO 13 by default: it is a plain GPIO, not a strapping pin. GPIO 12
// (MTDI) selects the flash voltage at boot, so a DS18B20's pull-up can stop
// the board from booting on a 3.3 V flash module — use 12 only if you know
// your board is safe for it.
#define SENSOR_PIN 13  // GPIO the sensor data line is on
#endif

// Secrets (gitignored): defines WIFI_SSID / WIFI_PASS.
#if defined(__has_include) && __has_include("secrets.h")
#include "secrets.h"
#else
#warning "secrets.h not found — building with placeholder WiFi credentials."
#define WIFI_SSID "your-wifi"
#define WIFI_PASS "your-wifi-password"
#endif

// Tollan bundle (gitignored): TOLLAN_RELAY_HOST / PORT / CA_CERT /
// DEVICE_CERT / DEVICE_KEY. A missing file warns loudly instead of silently
// shipping placeholder certs.
#if defined(__has_include) && __has_include("tollan_config.h")
#include "tollan_config.h"
#else
#warning "tollan_config.h not found — building with PLACEHOLDER certs."
#warning "Copy the tollan_config.h from your Tollan device bundle into src/."
static const char* TOLLAN_RELAY_HOST = "RELAY_PUBLIC_SNI_FROM_YOUR_BUNDLE";
static const uint16_t TOLLAN_RELAY_PORT = 443;
static const char* CA_CERT = R"PEM(
-----BEGIN CERTIFICATE-----
...use the console's tollan_config.h, or paste ca.crt here...
-----END CERTIFICATE-----
)PEM";
static const char* DEVICE_CERT = R"PEM(
-----BEGIN CERTIFICATE-----
...paste device.crt...
-----END CERTIFICATE-----
)PEM";
static const char* DEVICE_KEY = R"PEM(
-----BEGIN EC PRIVATE KEY-----
...paste device.key...
-----END EC PRIVATE KEY-----
)PEM";
#endif

// Sensor driver (selected by SENSOR_TYPE above).
#if SENSOR_TYPE == SENSOR_DS18B20
#include <DallasTemperature.h>
#include <OneWire.h>
#else
#include <DHT.h>
#endif

namespace {

// ---------------------------------------------------------------------------
// Bundle generations.
//
// Tollan has shipped two ESP32 bundle formats and both are still in the wild:
//
//   * enrollment (current, library 0.3.0+): the bundle carries a one-time
//     token; on first boot the board makes its own key and enrolls.
//       -> TOLLAN_ENROLL_URL / TOLLAN_ENROLL_TOKEN / TOLLAN_ENROLL_CA
//   * legacy (older bundles): the bundle shipped a client cert and private
//     key directly.
//       -> DEVICE_CERT / DEVICE_KEY
//
// A bundle also carries TOLLAN_RENEW_URL (certificate renewal) and the
// current one carries TOLLAN_DEVICE_ID / TOLLAN_TARGETS_URL (health
// check-in and live allowlist).
//
// The helpers below assign a config field only if the linked library
// actually has it — the assignment is selected by overload resolution, so
// the same sketch compiles against a library that predates the field, and
// reports at runtime which case it hit. "Silently not renewing" means a
// sensor that dies in 90 days; "silently not enrolling" means a board that
// never connects. Both are worth shouting about on the serial console.
// ---------------------------------------------------------------------------

// Generates a pair of overloads that assign an optional `const char*` field
// on TollanConfig, but only when the linked library has that field.
//
// The value is passed as an argument rather than referenced inside the
// decltype: a string literal in a template signature is not mangleable and
// GCC rejects it outright.
#define TOLLAN_OPTIONAL_FIELD(NAME)     \
  template <typename T>                 \
  auto set_##NAME(T& cfg, const char* v, int) \
      -> decltype(cfg.NAME = v, bool()) {    \
    cfg.NAME = v;                        \
    return true;                         \
  }                                       \
  template <typename T>                 \
  bool set_##NAME(T&, const char*, long) {  \
    return false;  /* library predates this field */ \
  }

TOLLAN_OPTIONAL_FIELD(enrollUrl)
TOLLAN_OPTIONAL_FIELD(enrollToken)
TOLLAN_OPTIONAL_FIELD(enrollCaCert)
TOLLAN_OPTIONAL_FIELD(renewUrl)
TOLLAN_OPTIONAL_FIELD(deviceId)
TOLLAN_OPTIONAL_FIELD(targetsUrl)

#undef TOLLAN_OPTIONAL_FIELD

// Wires the identity the bundle provides. Returns false when the board has
// no usable credentials, which is the one failure that stops it connecting
// at all.
bool applyCredentials(TollanConfig& cfg) {
#if defined(TOLLAN_ENROLL_URL) && defined(TOLLAN_ENROLL_TOKEN) && \
    defined(TOLLAN_ENROLL_CA)
  if (set_enrollUrl(cfg, TOLLAN_ENROLL_URL, 0) &&
      set_enrollToken(cfg, TOLLAN_ENROLL_TOKEN, 0) &&
      set_enrollCaCert(cfg, TOLLAN_ENROLL_CA, 0)) {
    Serial.println(
        "tollan: enrollment bundle — the board makes its own key on first boot");
    return true;
  }
  Serial.println(
      "tollan: bundle uses enrollment but this library has no enroll support "
      "(need TollanAgent 0.3.0+)");
  return false;
#elif defined(DEVICE_CERT) && defined(DEVICE_KEY)
  cfg.clientCert = DEVICE_CERT;
  cfg.clientKey = DEVICE_KEY;
  Serial.println("tollan: legacy bundle — using the bundled client cert + key");
  return true;
#else
  Serial.println(
      "tollan: no credentials found in tollan_config.h — copy the file from "
      "your Tollan bundle into src/");
  return false;
#endif
}

// Tuning knobs.
constexpr uint16_t kLocalPort = 80;          // LAN status page + API
constexpr uint32_t kWifiTimeoutMs = 20000;   // reboot if WiFi won't associate
constexpr uint32_t kSensorCacheMs = 2000;    // DHT22/DS18B20 need ~2 s between reads
// 4 KB fits a real browser GET (Chrome sends 700-1200 B + relay-added
// X-Forwarded-* headers) plus room for a second DATA frame when the relay
// chunks a large request. 512 B (the old value) dropped every browser hit
// with "oversize request — dropping". Cost: 8 × 4 KB = 32 KB static DRAM.
constexpr size_t kReqBufSize = 4096;         // per-channel HTTP head accumulator
constexpr size_t kJsonBufSize = 256;         // room + readings + ip/rssi
// Browsers open ~6 parallel connections for one page load (HTML + favicon +
// /api poll). 4 slots meant the 5th/6th got "busy, dropping".
constexpr uint8_t kMaxTollanChannels = 8;    // concurrent tunneled requests we track

#if SENSOR_TYPE == SENSOR_DS18B20
OneWire oneWire(SENSOR_PIN);
DallasTemperature ds18b20(&oneWire);
#else
DHT dht(SENSOR_PIN, SENSOR_TYPE == SENSOR_DHT11 ? DHT11 : DHT22);
#endif

TollanAgent agent;
WiFiServer localServer(kLocalPort);

// ---------------------------------------------------------------------------
// Cached sensor reading. DS18B20 conversion takes ~750 ms and DHT sensors
// need ~2 s between reads, so we never sample on every HTTP request —
// serve the last reading if it is fresher than kSensorCacheMs.
// ---------------------------------------------------------------------------
struct SensorReading {
  bool ok = false;
  float tempC = NAN;
  float humidity = NAN;  // NAN for DS18B20 (temperature only)
};

SensorReading cachedReading;
uint32_t cachedAtMs = 0;

SensorReading sampleSensor() {
  SensorReading r;
#if SENSOR_TYPE == SENSOR_DS18B20
  ds18b20.requestTemperatures();
  float t = ds18b20.getTempCByIndex(0);
  // DallasTemperature returns DEVICE_DISCONNECTED_C (-127) when the bus has
  // no device — treat it as a read error, not a temperature.
  if (t != DEVICE_DISCONNECTED_C) {
    r.ok = true;
    r.tempC = t;
  }
#else
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (!isnan(t) && !isnan(h)) {
    r.ok = true;
    r.tempC = t;
    r.humidity = h;
  }
#endif
  return r;
}

const SensorReading& getReading() {
  uint32_t now = millis();
  if (cachedAtMs == 0 || now - cachedAtMs >= kSensorCacheMs) {
    cachedReading = sampleSensor();
    cachedAtMs = now;
  }
  return cachedReading;
}

// ---------------------------------------------------------------------------
// JSON body. Always HTTP 200 so the poller can parse it and inspect
// the "error" field: {"room","tempC","humidity?","error?","tsMs","ip","rssi"}.
// tsMs is board uptime (no wall clock on the board).
// ---------------------------------------------------------------------------
void buildJsonBody(char* buf, size_t cap) {
  const SensorReading& r = getReading();
  IPAddress ip = WiFi.localIP();
  int rssi = WiFi.RSSI();

  if (!r.ok) {
    snprintf(buf, cap,
             "{\"room\":\"%s\",\"error\":\"sensor_read_failed\","
             "\"tsMs\":%lu,\"ip\":\"%u.%u.%u.%u\",\"rssi\":%d}\n",
             ROOM_ID, (unsigned long)millis(), ip[0], ip[1], ip[2], ip[3],
             rssi);
  } else if (SENSOR_TYPE == SENSOR_DS18B20) {
    snprintf(buf, cap,
             "{\"room\":\"%s\",\"tempC\":%.1f,"
             "\"tsMs\":%lu,\"ip\":\"%u.%u.%u.%u\",\"rssi\":%d}\n",
             ROOM_ID, r.tempC, (unsigned long)millis(), ip[0], ip[1], ip[2],
             ip[3], rssi);
  } else {
    snprintf(buf, cap,
             "{\"room\":\"%s\",\"tempC\":%.1f,\"humidity\":%.1f,"
             "\"tsMs\":%lu,\"ip\":\"%u.%u.%u.%u\",\"rssi\":%d}\n",
             ROOM_ID, r.tempC, r.humidity, (unsigned long)millis(), ip[0],
             ip[1], ip[2], ip[3], rssi);
  }
}

// ---------------------------------------------------------------------------
// Minimal request routing.
//   GET /api, GET /json -> JSON (also the default: curl and the poller send
//                                "Accept: */*", never "text/html").
//   GET /              -> status page for browsers, JSON otherwise.
// Browsers advertise "Accept: text/html" and "Sec-Fetch-Dest: document";
// searching for those substrings is sufficient and avoids a full parser.
// ---------------------------------------------------------------------------
enum class Route { Favicon, Json, Html, MethodNotAllowed };

Route routeFor(const char* req) {
  if (strncmp(req, "GET ", 4) != 0) {
    return Route::MethodNotAllowed;
  }
  if (strstr(req, "GET /favicon.ico") != nullptr) {
    return Route::Favicon;
  }
  if (strstr(req, "GET /api") != nullptr ||
      strstr(req, "GET /json") != nullptr) {
    return Route::Json;
  }
  if (strstr(req, "text/html") != nullptr ||
      strstr(req, "Sec-Fetch-Dest: document") != nullptr) {
    return Route::Html;
  }
  return Route::Json;  // default: machines get JSON
}

bool headersComplete(const char* buf) {
  return strstr(buf, "\r\n\r\n") != nullptr;
}

// ---------------------------------------------------------------------------
// Response writers — one per transport, shared routing.
// ---------------------------------------------------------------------------
void serveTollan(uint32_t channel, const char* req) {
  char body[kJsonBufSize];

  switch (routeFor(req)) {
    case Route::Favicon: {
      static const char resp[] = "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n";
      agent.send(channel, reinterpret_cast<const uint8_t*>(resp), sizeof(resp) - 1);
      break;
    }
    case Route::Html: {
      size_t len = strlen(PAGE_HTML);
      char hdr[128];
      int hlen = snprintf(hdr, sizeof(hdr),
                          "HTTP/1.1 200 OK\r\n"
                          "Content-Type: text/html; charset=utf-8\r\n"
                          "Content-Length: %u\r\n"
                          "Connection: close\r\n\r\n",
                          (unsigned)len);
      agent.send(channel, reinterpret_cast<const uint8_t*>(hdr), (size_t)hlen);
      agent.send(channel, reinterpret_cast<const uint8_t*>(PAGE_HTML), len);
      break;
    }
    case Route::Json: {
      buildJsonBody(body, sizeof(body));
      size_t len = strlen(body);
      char hdr[160];
      int hlen = snprintf(hdr, sizeof(hdr),
                          "HTTP/1.1 200 OK\r\n"
                          "Content-Type: application/json\r\n"
                          "Access-Control-Allow-Origin: *\r\n"
                          "Content-Length: %u\r\n"
                          "Connection: close\r\n\r\n",
                          (unsigned)len);
      agent.send(channel, reinterpret_cast<const uint8_t*>(hdr), (size_t)hlen);
      agent.send(channel, reinterpret_cast<const uint8_t*>(body), len);
      break;
    }
    case Route::MethodNotAllowed: {
      static const char resp[] =
          "HTTP/1.1 405 Method Not Allowed\r\nAllow: GET\r\n"
          "Content-Length: 0\r\nConnection: close\r\n\r\n";
      agent.send(channel, reinterpret_cast<const uint8_t*>(resp), sizeof(resp) - 1);
      break;
    }
  }
  agent.closeChannel(channel);
}

void serveLocal(WiFiClient& client, const char* req) {
  char body[kJsonBufSize];

  switch (routeFor(req)) {
    case Route::Favicon:
      client.print("HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n");
      break;
    case Route::Html:
      client.print(
          "HTTP/1.1 200 OK\r\n"
          "Content-Type: text/html; charset=utf-8\r\n"
          "Connection: close\r\nContent-Length: ");
      client.print(strlen(PAGE_HTML));
      client.print("\r\n\r\n");
      client.write(reinterpret_cast<const uint8_t*>(PAGE_HTML), strlen(PAGE_HTML));
      break;
    case Route::Json:
      buildJsonBody(body, sizeof(body));
      client.print(
          "HTTP/1.1 200 OK\r\n"
          "Content-Type: application/json\r\n"
          "Access-Control-Allow-Origin: *\r\n"
          "Connection: close\r\nContent-Length: ");
      client.print(strlen(body));
      client.print("\r\n\r\n");
      client.write(reinterpret_cast<const uint8_t*>(body), strlen(body));
      break;
    case Route::MethodNotAllowed:
      client.print(
          "HTTP/1.1 405 Method Not Allowed\r\nAllow: GET\r\n"
          "Content-Length: 0\r\nConnection: close\r\n\r\n");
      break;
  }
}

// ---------------------------------------------------------------------------
// Per-channel request buffers for the tunnel. The relay can multiplex
// channels, so each OPEN gets its own slot; bytes accumulate in onData
// until the headers are complete, then we answer exactly once.
// ---------------------------------------------------------------------------
struct TollanChannel {
  bool inUse = false;
  uint32_t id = 0;
  size_t len = 0;
  char buf[kReqBufSize];
};

TollanChannel channels[kMaxTollanChannels];

TollanChannel* findChannel(uint32_t id) {
  for (auto& c : channels) {
    if (c.inUse && c.id == id) {
      return &c;
    }
  }
  return nullptr;
}

TollanChannel* allocChannel(uint32_t id) {
  // The relay should not reuse an in-use id, but retire the stale slot if it
  // does (mirrors fwdAlloc) rather than ending up with two slots for one id.
  if (TollanChannel* existing = findChannel(id)) {
    existing->len = 0;
    existing->buf[0] = '\0';
    return existing;
  }
  for (auto& c : channels) {
    if (!c.inUse) {
      c.inUse = true;
      c.id = id;
      c.len = 0;
      c.buf[0] = '\0';
      return &c;
    }
  }
  return nullptr;  // pool exhausted — caller drops the channel
}

void freeChannel(uint32_t id) {
  if (TollanChannel* c = findChannel(id)) {
    c->inUse = false;
    c->len = 0;
  }
}

void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("wifi");
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > kWifiTimeoutMs) {
      Serial.println("\nwifi: association timed out — rebooting");
      ESP.restart();
    }
    delay(300);
    Serial.print('.');
  }
  Serial.printf(" ok, ip=%s\n", WiFi.localIP().toString().c_str());
}

// Single non-blocking-ish LAN request: read until end-of-headers (or a short
// deadline), answer, hang up. Only one LAN client at a time is plenty here.
void handleLocalClient() {
  WiFiClient client = localServer.available();
  if (!client) {
    return;
  }
  // Static, not stack: kReqBufSize is 4 KB and the loop task stack is ~8 KB.
  static char req[kReqBufSize];
  size_t len = 0;
  bool truncated = false;
  uint32_t start = millis();
  while (client.connected() && millis() - start < 500) {
    while (client.available()) {
      if (len < sizeof(req) - 1) {
        req[len++] = (char)client.read();
      } else {
        // Request too large — drain and close to protect RAM.
        truncated = true;
        client.read();
      }
    }
    req[len] = '\0';
    if (headersComplete(req)) {
      break;
    }
    delay(1);
  }
  if (len > 0 && !truncated) {
    serveLocal(client, req);
  } else if (truncated) {
    client.print(
        "HTTP/1.1 431 Request Header Fields Too Large\r\n"
        "Content-Length: 0\r\nConnection: close\r\n\r\n");
  }
  client.stop();
}

void logStateChanges() {
  static TollanState last = TollanState::Disconnected;
  TollanState now = agent.state();
  if (now == last) {
    return;
  }
  last = now;
  Serial.printf("tollan state -> %s\n",
                now == TollanState::Connected      ? "connected"
                : now == TollanState::Terminal     ? "terminal (version mismatch)"
                                                   : "disconnected");
}

}  // namespace

void setup() {
  Serial.begin(115200);

#if SENSOR_TYPE == SENSOR_DS18B20
  ds18b20.begin();
#else
  dht.begin();
#endif

  connectWifi();

  localServer.begin();
  Serial.println("local web server started on port 80");

  // The agent SNTP-syncs the clock before its first TLS handshake
  // (TollanConfig.syncClock, on by default) — mTLS cert date checks need a
  // real clock and a fresh ESP32 boots near 1970.
  TollanConfig cfg;
  cfg.host = TOLLAN_RELAY_HOST;
  cfg.port = TOLLAN_RELAY_PORT;
  cfg.caCert = CA_CERT;

  // Identity. Enrollment bundles carry a one-time token and the board
  // generates its own key; legacy bundles carry a client cert + key.
  const bool haveIdentity = applyCredentials(cfg);

  // Certificate renewal. "Silently not renewing" means a sensor that dies
  // in 90 days, so always say out loud which of the three cases we hit.
#ifdef TOLLAN_RENEW_URL
  if (set_renewUrl(cfg, TOLLAN_RENEW_URL, 0)) {
    Serial.println("tollan: cert renewal enabled");
  } else {
    Serial.println(
        "tollan: library has no renewUrl (need TollanAgent 0.2.0+) — "
        "certs will NOT auto-renew");
  }
#else
  Serial.println(
      "tollan: no TOLLAN_RENEW_URL in bundle — certs will NOT auto-renew "
      "(90-day expiry)");
#endif

  // Health check-in: reports free memory, Wi-Fi signal and uptime to the
  // console, and pulls the live route allowlist. Current bundles only.
#if defined(TOLLAN_DEVICE_ID) && defined(TOLLAN_TARGETS_URL)
  if (set_deviceId(cfg, TOLLAN_DEVICE_ID, 0) &&
      set_targetsUrl(cfg, TOLLAN_TARGETS_URL, 0)) {
    Serial.println("tollan: health check-in enabled");
  }
#endif

  // Per-frame cap. NOTE: the library's own receive buffer is the real
  // ceiling — the precompiled 0.3.0 build stages frames in a fixed 1600 B
  // buffer, so 4096 cannot be honoured there and a request over 1600 B is
  // dropped with "oversize request". A browser GET is ~700-1200 B plus the
  // relay's forwarded headers and fits; set this lower to shrink RAM.
  cfg.maxFrameBytes = 4096;

  if (!haveIdentity) {
    Serial.println(
        "tollan: no usable identity — the tunnel will never come up. See the "
        "setup steps in the README.");
  }

  agent.onOpen([](uint32_t ch, const char*, size_t) {
    Serial.printf("open  channel %u\n", ch);
    if (allocChannel(ch) == nullptr) {
      Serial.printf("busy (%u in use), dropping channel %u\n",
                    kMaxTollanChannels, ch);
      agent.closeChannel(ch);
    }
  });
  agent.onData([](uint32_t ch, const uint8_t* data, size_t len) {
    TollanChannel* c = findChannel(ch);
    if (c == nullptr) {
      return;
    }
    if (c->len + len >= sizeof(c->buf)) {
      Serial.printf("oversize request on channel %u (%u staged + %u new, cap %u) — 431\n",
                    ch, (unsigned)c->len, (unsigned)len, (unsigned)sizeof(c->buf));
      static const char resp[] =
          "HTTP/1.1 431 Request Header Fields Too Large\r\n"
          "Content-Length: 0\r\nConnection: close\r\n\r\n";
      agent.send(ch, reinterpret_cast<const uint8_t*>(resp), sizeof(resp) - 1);
      agent.closeChannel(ch);
      freeChannel(ch);
      return;
    }
    memcpy(c->buf + c->len, data, len);
    c->len += len;
    c->buf[c->len] = '\0';
    if (headersComplete(c->buf)) {
      Serial.printf("data  channel %u (%u bytes) -> serving request\n", ch,
                    (unsigned)len);
      serveTollan(ch, c->buf);
      freeChannel(ch);
    }
  });
  agent.onClose([](uint32_t ch) {
    Serial.printf("close channel %u\n", ch);
    freeChannel(ch);
  });

  agent.begin(cfg);
  Serial.println("tollan agent started");
}

void loop() {
  agent.loop();
  handleLocalClient();
  logStateChanges();
  delay(1);
}
