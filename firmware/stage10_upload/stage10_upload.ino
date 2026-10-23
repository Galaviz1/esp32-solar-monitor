// Stage 10 — Send measurements over Wi-Fi.
//
// This is the missing link between Stage 9 (BLE acquisition, samples buffered in
// RAM) and Stages 11–13 (the server already stores/plots whatever arrives). It
// keeps Stage 9's reconnect/backoff and FIFO buffer, and adds:
//
//   * Wi-Fi (STA) with auto-reconnect
//   * SNTP time, so every sample carries a real epoch stamp (`device_ts`)
//   * HTTPS POST to INGEST_URL, signed with HMAC-SHA256 (X-Device-Id,
//     X-Timestamp, X-Signature; see uploadBatch() and lab-portal's
//     netlify/lib/security.mjs)
//   * store-and-forward: samples that fail to upload stay queued and are
//     re-sent in the next batch, so a Wi-Fi/server outage loses nothing until
//     the buffer wraps
//   * connect-poll-release, instead of Stage 9's permanently held BLE link, so
//     the BT-1 stays usable by the Renogy phone app between readings
//   * an optional SSD1306 128x32 OLED showing SOC, battery, PV/load and link
//     health, so the rig can be checked without opening the dashboard
//   * an optional DHT11 for air temperature and humidity around the rig
//     (env_temp, env_hum), shown on the OLED and uploaded with each sample
//
// Prereq: Stages 3 (Wi-Fi) and 8 (verified decode) passed. Settings are split:
// config.h (tracked) carries SSID, BT1_MAC, INGEST_URL, DEVICE_ID, CONFIG_ID
// and the OLED options; secrets.h (git-ignored) carries WIFI_PASSWORD and
// DEVICE_HMAC_KEY only.
//
// ⚠️ Build note 1 — BLE + Wi-Fi + TLS does NOT fit the default 4 MB partition.
// Select "Huge APP (3MB No OTA/1MB SPIFFS)" (or "Minimal SPIFFS"). See
// firmware/README.md for the exact arduino-cli FQBN.
//
// ⚠️ Build note 2 — libraries:
//     arduino-cli lib install "NimBLE-Arduino"
//     arduino-cli lib install "Adafruit SSD1306"    # pulls GFX + BusIO
//     arduino-cli lib install "DHT sensor library"  # pulls Adafruit Unified Sensor
// Build without a display by setting OLED_ENABLED to 0 in config.h, without
// the DHT11 by setting ENV_ENABLED to 0.
//
// Why NimBLE, when Stages 4–9 used the stock Bluedroid stack: this is the stage
// where Wi-Fi and TLS join BLE, and measured on this board Bluedroid left only
// ~40 KB free heap with a 32.7 KB largest block. The mbedTLS handshake wants
// ~32 KB of buffers on top of that, so every upload failed with
// `tls=-32512 (SSL - Memory allocation failed)` while BLE itself worked fine.
// NimBLE is a drop-in-ish replacement that frees roughly 100 KB. The port is
// mechanical (callback classes, `subscribe()` instead of `registerForNotify()`,
// connect by address instead of by advertised-device pointer) and is confined
// to the BLE section below — the Modbus framing and decode are untouched, so
// the Stage 8 verification still stands.
//
// Provenance (docs/data-model.md): every field below is `measured` — read from
// the Wanderer via the BT-1 — except `config_id`, which is an `observation`
// (which physical test setup is running), and `rssi` / `bt_rssi`, which measure
// the ESP32's own Wi-Fi and BT-1 links, not the solar system. `env_temp` and
// `env_hum` are `measured` too, but by the DHT11 on the ESP32, not the Wanderer.

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <NimBLEDevice.h>
#include <time.h>
#include "mbedtls/md.h"
#include "config.h"    // tracked: SSID, BT-1 MAC, ingest URL, device + config id
#include "secrets.h"   // git-ignored: Wi-Fi password and device token ONLY

#if ENV_ENABLED
  #include <DHT.h>
#endif

#if OLED_ENABLED
  #include <Wire.h>
  #include <Adafruit_GFX.h>
  #include <Adafruit_SSD1306.h>
#endif

// ---- GATT identifiers (VERIFIED Stage 6, BT-TH-XXXXXXXX) ----
#define NOTIFY_SVC_UUID  "0000fff0-0000-1000-8000-00805f9b34fb"
#define NOTIFY_CHAR_UUID "0000fff1-0000-1000-8000-00805f9b34fb"
#define WRITE_SVC_UUID   "0000ffd0-0000-1000-8000-00805f9b34fb"
#define WRITE_CHAR_UUID  "0000ffd1-0000-1000-8000-00805f9b34fb"

// ---- Modbus request (VERIFIED Stage 7) ----
#define MB_DEVICE_ADDR   0xFF
#define MB_FUNC_READ     0x03
#define REG_START        0x0100
#define REG_COUNT        0x0010          // 16 regs: 0x0100..0x010F

// CONFIG_ID (which physical test setup is running) now lives in config.h,
// alongside the rest of the per-deployment settings.

// Hand the BT-1 back after each reading. The BT-1 accepts ONE central and stops
// advertising while connected, so holding the link permanently (as Stage 9 did)
// means the Renogy phone app can never connect. With this on, the link is up for
// only a few seconds per POLL_INTERVAL_MS — the BT-1 is free >99% of the time
// and the app works normally between polls. Cost: each reading pays a reconnect
// (~2-3 s by address, longer if a rescan is needed), and a poll that lands while
// the phone holds the BT-1 fails and retries at RETRY_INTERVAL_MS.
// Set to 0 only if you want the old always-connected behaviour.
#define RELEASE_BT1_BETWEEN_POLLS 1

// ---- cadence ----
// The dashboard advertises "~10 min"; MAX_HISTORY on the server is 1008 samples
// (= 7 days at this rate). Shorten for a bring-up run, then put it back.
#define POLL_INTERVAL_MS   600000UL      // 10 min
#define RETRY_INTERVAL_MS   60000UL      // after a failed poll, retry sooner
#define RESP_TIMEOUT_MS      3000UL
#define BACKOFF_MIN_MS       1000UL
#define BACKOFF_MAX_MS      30000UL
#define QUEUE_SIZE             144       // ~24 h of backlog at 10 min
#define MAX_BATCH               30       // samples per POST (bounds JSON size)
#define HTTP_TIMEOUT_MS      15000

// BT-1 RSSI between polls. A poll connects only every POLL_INTERVAL_MS, so on
// its own the BT figure would sit unchanged for 10 min after the board moves.
// Between polls, a short non-blocking scan picks the RSSI up from the BT-1's
// advertisements instead (it advertises whenever no phone holds it).
#define BT_RSSI_REFRESH_MS   30000UL     // how often to listen
#define BT_RSSI_SCAN_MS       3000UL     // how long; stops early once heard
#define BT_RSSI_STALE_MS     90000UL     // older than this shows as "--"

// ---- Wi-Fi transmit power ----
//
// The radio's transmit burst is the largest current spike this board draws, and
// it is what tripped the brownout detector on a bus-powered USB hub (see
// firmware/README.md). Lowering it shrinks that spike at the cost of uplink
// range — a direct stability-vs-range dial for when the supply is marginal.
//
// Note this only affects whether the ACCESS POINT hears us. The RSSI printed at
// boot is what WE hear from the AP and does not change with this setting. APs
// usually have better receivers and antennas than an ESP32, so the uplink
// normally has margin to give away.
//
//   WIFI_POWER_19_5dBm   core default, biggest current spike
//   WIFI_POWER_15dBm     ~4.5 dB down — noticeably gentler, still long-ranged
//   WIFI_POWER_13dBm     ~6.5 dB down — try this if brownouts persist
//   WIFI_POWER_8_5dBm    only for a strong AP nearby
//
// If Wi-Fi starts failing to associate after lowering this, raise it one step.
#define WIFI_TX_POWER    WIFI_POWER_15dBm

// ---- time ----
#define NTP_SERVER_1     "pool.ntp.org"
#define NTP_SERVER_2     "time.nist.gov"
#define NTP_WAIT_MS      15000UL
#define EPOCH_SANE       1700000000UL    // anything below this is "clock not set"

// Let's Encrypt trust anchor. solar.example.com serves a chain that climbs to
// ISRG Root X1, so this single root validates it. Pinning the root (rather than
// setInsecure()) is what keeps uploads from being sent to an impostor.
// Expires 2035-06-04.
static const char ISRG_ROOT_X1[] PROGMEM = R"CERT(
-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh
cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4
WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu
ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY
MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc
h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+
0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U
A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW
T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH
B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC
B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv
KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn
OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn
jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw
qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI
rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV
HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq
hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL
ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ
3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK
NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5
ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur
TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC
jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc
oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq
4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA
mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d
emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=
-----END CERTIFICATE-----
)CERT";

// ---- decoded sample -------------------------------------------------------
// Raw register words are kept as-read; scaling happens once, at JSON time, so a
// scaling correction never has to be applied retroactively to buffered data.
struct Sample {
  uint32_t t_uptime_ms;    // millis() at read; converted to epoch at upload
  uint8_t  soc;            // 0x0100  %
  uint16_t batt_raw;       // 0x0101  ×0.1 V   [verified Stage 8]
  int16_t  charge_raw;     // 0x0102  ×0.01 A  [verified Stage 8]
  uint16_t load_v_raw;     // 0x0104  ×0.1 V   [verified Stage 8]
  uint16_t load_a_raw;     // 0x0105  ×0.01 A  [verified Stage 8]
  uint16_t load_w_raw;     // 0x0106  ×1 W     [verified Stage 8]
  uint16_t pv_v_raw;       // 0x0107  ×0.1 V   [verified Stage 8]
  uint16_t pv_a_raw;       // 0x0108  ×0.01 A  [UNVERIFIED — see protocol-bt1.md]
  uint16_t pv_w_raw;       // 0x0109  ×1 W     [UNVERIFIED — see protocol-bt1.md]
  uint16_t temp_raw;       // 0x0103  packed °C: high byte controller, low byte
                           //         battery; bit 7 = sign  [Renogy map, display only]
  int8_t   rssi;           // ESP32 Wi-Fi RSSI at read time
  int8_t   bt_rssi;        // ESP32 <-> BT-1 RSSI from this poll's connection
  int16_t  env_t10;        // DHT11 air temperature ×0.1 °C; ENV_NONE = no reading
  int16_t  env_rh10;       // DHT11 relative humidity ×0.1 %;  ENV_NONE = no reading
};
static const int16_t ENV_NONE = INT16_MIN;

// Renogy packs each temperature as sign-magnitude: bit 7 = negative, bits 0-6 =
// whole °C. The Wanderer 10A has no battery sensor, so its low byte reads 0.
static int tempC(uint8_t b) { return (b & 0x80) ? -(int)(b & 0x7F) : (int)(b & 0x7F); }

// ---- FIFO queue (oldest first; drained from the front on a successful POST) --
static Sample   q[QUEUE_SIZE];
static size_t   qHead = 0, qCount = 0;
static uint32_t droppedSamples = 0, goodPolls = 0, failedPolls = 0;
static uint32_t uploadsOk = 0, uploadsFail = 0;

#if OLED_ENABLED
// Declared up here because uploadBatch() sets them and it appears above the
// display section further down.
static uint32_t lastUploadMs = 0;
static bool     lastUploadOK = false;
#endif

static void qPush(const Sample& s) {
  if (qCount == QUEUE_SIZE) {                 // full → drop the oldest unsent
    qHead = (qHead + 1) % QUEUE_SIZE;
    qCount--;
    droppedSamples++;
  }
  q[(qHead + qCount) % QUEUE_SIZE] = s;
  qCount++;
}
static Sample& qAt(size_t i)      { return q[(qHead + i) % QUEUE_SIZE]; }
static void    qPopFront(size_t n) {
  if (n > qCount) n = qCount;
  qHead = (qHead + n) % QUEUE_SIZE;
  qCount -= n;
}

// ---- air temperature + humidity (DHT11) -----------------------------------
//
// Read every ENV_READ_MS in the idle part of loop(), never during a BT-1
// transaction. The DHT11 answers in ~4 ms with interrupts off on this core
// only; Wi-Fi and BLE run on the other core. A sample carries the last good
// reading if it is fresh (ENV_STALE_MS), otherwise no env fields at all, so a
// missing or failing sensor can never hold up the solar data.
static float    envT = 0, envRH = 0;   // last good reading
static uint32_t envMs = 0;             // when it was taken; 0 = never
static uint32_t envFails = 0;

static bool envFresh() { return envMs && millis() - envMs < ENV_STALE_MS; }

#if ENV_ENABLED
static DHT dht(ENV_DHT_PIN, ENV_DHT_TYPE);

static void envInit() {
  dht.begin();
  Serial.printf("[env] DHT on GPIO%d, read every %lus\n", ENV_DHT_PIN, ENV_READ_MS / 1000);
}

static void envRead() {
  dht.read(true);                      // one transaction; the getters reuse it
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (isnan(t) || isnan(h) || t < -40 || t > 80 || h < 0 || h > 100) {
    // Log the first failure and then every 10th, not every 30 s forever.
    if (envFails++ % 10 == 0)
      Serial.printf("[env] no reading from the DHT on GPIO%d (%lu so far): wiring, "
                    "or 5V/3V3 mix-up\n", ENV_DHT_PIN, envFails);
    return;
  }
  if (!envMs || envFails) Serial.printf("[env] %.1f C  %.0f %%RH\n", t, h);
  envT = t; envRH = h; envMs = millis(); envFails = 0;
}
#else
static void envInit() {}
static void envRead() {}
#endif

// ---- BLE state (NimBLE — see "Why NimBLE" at the bottom of this file) ----
static const uint32_t SCAN_MS = 10000;
static NimBLEAddress                targetAddr;
static bool                         haveTarget = false;
static NimBLEClient*                client     = nullptr;
static NimBLERemoteCharacteristic*  writeChar  = nullptr;
static NimBLERemoteCharacteristic*  notifyChar = nullptr;
static volatile bool                connected  = false;

// BT-1 signal strength as the ESP32 hears it. Updated from any scan that hears
// the BT-1 (the 30 s refresh scans included) and from the live link on every
// connection. 0 means "no value"; bt1RssiMs is when it was last measured, so a
// value that stops being refreshed can be shown as stale instead of current.
// Written from NimBLE's host task (scan callback), hence volatile.
static volatile int8_t              bt1Rssi    = 0;
static volatile uint32_t            bt1RssiMs  = 0;
static volatile bool                rssiScan   = false;   // background refresh scan running

static int8_t bt1RssiFresh() {
  return (bt1Rssi && millis() - bt1RssiMs < BT_RSSI_STALE_MS) ? bt1Rssi : 0;
}

static volatile bool   haveResponse = false;
static uint8_t         resp[80];
static volatile size_t respLen = 0;

static uint16_t modbusCRC(const uint8_t* buf, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= buf[i];
    for (int b = 0; b < 8; b++)
      crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : (crc >> 1);
  }
  return crc;
}

static void notifyCB(NimBLERemoteCharacteristic*, uint8_t* d, size_t n, bool) {
  for (size_t i = 0; i < n && respLen < sizeof(resp); i++) resp[respLen++] = d[i];
  if (respLen >= 3 && respLen >= (size_t)(3 + resp[2] + 2)) haveResponse = true;
}

class ClientCB : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient*) override { connected = true; }
  void onDisconnect(NimBLEClient*, int reason) override {
    connected = false; notifyChar = nullptr; writeChar = nullptr;
    Serial.printf("[ble] disconnected (reason %d)\n", reason);
  }
};

static uint32_t advSeen = 0;             // advertisements per scan, for diagnosis

class FindCB : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* d) override {
    advSeen++;
    String addr = String(d->getAddress().toString().c_str());
    String name = d->haveName() ? String(d->getName().c_str()) : String("");
    bool haveMac = (strlen(BT1_MAC) > 0);
    // Match on MAC, never on name: with Wi-Fi associated the BT-1 is often seen
    // in a scan response that carries no name at all (confirmed on this unit).
    if ((haveMac && addr.equalsIgnoreCase(BT1_MAC)) ||
        (!haveMac && name.startsWith(BT1_NAME_PREFIX))) {
      targetAddr = d->getAddress();      // by value: the device object is the
      haveTarget = true;                 // scanner's, and dies with the results
      bt1Rssi    = (int8_t)d->getRSSI();
      bt1RssiMs  = millis();
      if (!rssiScan)                     // the 30 s refresh scans stay quiet
        Serial.printf("[ble] found BT-1 at RSSI %d dBm\n", bt1Rssi);
      NimBLEDevice::getScan()->stop();
    }
  }
};

static FindCB findCB;                    // one instance, registered once in setup

static bool scanForTarget() {
  Serial.print("[ble] scanning for BT-1... ");
  advSeen = 0;
  NimBLEScan* scan = NimBLEDevice::getScan();
  if (scan->isScanning()) scan->stop();  // a background RSSI scan may be running
  rssiScan = false;
  scan->getResults(SCAN_MS, false);      // blocking scan
  scan->clearResults();
  if (!haveTarget) {
    // Distinguish "radio is deaf" from "this one device wasn't heard". If other
    // advertisements are coming in, the cause is either scan duty cycle (the
    // BT-1 is weak, ~-79 dBm, and Wi-Fi steals radio time) or another central
    // holding it — the BT-1 stops advertising while connected. See
    // firmware/README.md -> "Not finding the BT-1".
    Serial.printf("no BT-1 (%lu other advertisements seen%s)\n", advSeen,
                  advSeen ? "; radio OK — weak advertiser, or another central "
                            "(phone app) holds it"
                          : "; radio saw nothing at all");
  }
  return haveTarget;
}

static bool acquireChars() {
  NimBLERemoteService* nsvc = client->getService(NimBLEUUID(NOTIFY_SVC_UUID));
  NimBLERemoteService* wsvc = client->getService(NimBLEUUID(WRITE_SVC_UUID));
  if (!nsvc || !wsvc) { Serial.println("[ble] service UUID not found"); return false; }
  notifyChar = nsvc->getCharacteristic(NimBLEUUID(NOTIFY_CHAR_UUID));
  writeChar  = wsvc->getCharacteristic(NimBLEUUID(WRITE_CHAR_UUID));
  if (!notifyChar || !writeChar) { Serial.println("[ble] char UUID not found"); return false; }
  if (notifyChar->canNotify() && !notifyChar->subscribe(true, notifyCB)) {
    Serial.println("[ble] subscribe failed"); return false;
  }
  return true;
}

static bool ensureConnected() {
  if (connected && notifyChar && writeChar) return true;
  if (!haveTarget && !scanForTarget()) return false;
  Serial.printf("[ble] connecting to %s...\n", targetAddr.toString().c_str());
  NimBLEScan* sc = NimBLEDevice::getScan();
  if (sc->isScanning()) sc->stop();      // never connect over a refresh scan
  rssiScan = false;
  if (!client->connect(targetAddr)) {
    Serial.println("[ble] connect failed");
    haveTarget = false;                  // re-scan; it may have moved or slept
    return false;
  }
  if (!acquireChars()) { client->disconnect(); return false; }
  // Read the live link. getRssi() returns 0 when the read fails; that used to
  // keep the old value silently AND print it as if fresh, which hid the
  // failure. Now a failure is logged, and the old value ages out (bt1RssiFresh).
  int r = client->getRssi();
  if (r != 0) {
    bt1Rssi = (int8_t)r; bt1RssiMs = millis();
    Serial.printf("[ble] connected + subscribed  RSSI=%d dBm\n", r);
  } else {
    Serial.printf("[ble] connected + subscribed  (RSSI read failed, rc=%d)\n",
                  client->getLastError());
  }
  return true;
}

// One Modbus poll → decode → queue. Returns true on a CRC-validated sample.
static bool pollOnce() {
  uint8_t req[8] = { MB_DEVICE_ADDR, MB_FUNC_READ,
                     (REG_START >> 8) & 0xFF, REG_START & 0xFF,
                     (REG_COUNT >> 8) & 0xFF, REG_COUNT & 0xFF, 0, 0 };
  uint16_t crc = modbusCRC(req, 6);
  req[6] = crc & 0xFF; req[7] = (crc >> 8) & 0xFF;

  haveResponse = false; respLen = 0;
  writeChar->writeValue(req, sizeof(req), writeChar->canWrite());

  unsigned long t0 = millis();
  while (!haveResponse && millis() - t0 < RESP_TIMEOUT_MS) delay(10);
  if (!haveResponse) { Serial.println("[poll] timeout"); return false; }

  size_t frameLen = 3 + resp[2];
  if (frameLen + 2 > respLen || frameLen + 2 > sizeof(resp)) {
    Serial.println("[poll] short frame"); return false;
  }
  uint16_t rxCrc = resp[frameLen] | (resp[frameLen + 1] << 8);
  if (resp[1] != MB_FUNC_READ || rxCrc != modbusCRC(resp, frameLen)) {
    Serial.println("[poll] bad frame/CRC"); return false;
  }
  // Did we actually get every register we decode below (through 0x0109)?
  if (resp[2] < 20) { Serial.println("[poll] byte count too small"); return false; }

  auto reg = [&](uint16_t addr) -> uint16_t {
    int i = addr - REG_START;
    return (resp[3 + i * 2] << 8) | resp[4 + i * 2];
  };

  Sample s;
  s.t_uptime_ms = millis();
  s.soc         = reg(0x0100) & 0xFF;
  s.batt_raw    = reg(0x0101);
  s.charge_raw  = (int16_t)reg(0x0102);
  s.load_v_raw  = reg(0x0104);
  s.load_a_raw  = reg(0x0105);
  s.load_w_raw  = reg(0x0106);
  s.pv_v_raw    = reg(0x0107);
  s.pv_a_raw    = reg(0x0108);
  s.pv_w_raw    = reg(0x0109);
  s.temp_raw    = reg(0x0103);
  s.rssi        = (WiFi.status() == WL_CONNECTED) ? (int8_t)WiFi.RSSI() : 0;
  s.bt_rssi     = bt1RssiFresh();    // read in ensureConnected(), moments ago
  bool env      = envFresh();
  s.env_t10     = env ? (int16_t)lroundf(envT * 10) : ENV_NONE;
  s.env_rh10    = env ? (int16_t)lroundf(envRH * 10) : ENV_NONE;
  qPush(s);
  oledNote(s);                       // screen shows the newest reading

  Serial.printf("[poll] batt=%.1fV soc=%u%% chg=%.2fA pv=%.1fV/%.2fA/%uW load=%uW "
                "ctrl=%dC batt_t=%dC (queued %u/%u)\n",
                s.batt_raw * 0.1, s.soc, s.charge_raw * 0.01,
                s.pv_v_raw * 0.1, s.pv_a_raw * 0.01, s.pv_w_raw, s.load_w_raw,
                tempC(s.temp_raw >> 8), tempC(s.temp_raw & 0xFF),
                (unsigned)qCount, QUEUE_SIZE);
  if (s.env_t10 != ENV_NONE)
    Serial.printf("[poll] air=%.1fC rh=%.0f%%\n", s.env_t10 * 0.1, s.env_rh10 * 0.1);
  return true;
}

// ---- Wi-Fi + time ---------------------------------------------------------

// Apply the configured transmit power.
//
// Must be called AFTER the station is associated. Setting it between
// WiFi.mode() and WiFi.begin() silently does nothing — the driver reports 19.5
// dBm regardless (observed on core 3.3.11). It also has to be re-applied after
// every reassociation, because auto-reconnect restores the default.
static void applyTxPower() {
  if (WiFi.getTxPower() == WIFI_TX_POWER) return;
  WiFi.setTxPower(WIFI_TX_POWER);
}

// Bring the STA up. Called ONCE, from setup().
static bool wifiStart(unsigned long timeoutMs) {
  Serial.printf("[wifi] connecting to '%s'", WIFI_SSID);
  WiFi.persistent(false);            // don't wear flash rewriting credentials
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) {
    delay(500); Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    applyTxPower();
    Serial.printf("[wifi] up  IP=%s  RSSI=%d dBm  txpower=%.1f dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI(),
                  (int)WiFi.getTxPower() / 4.0);
    return true;
  }
  Serial.println("[wifi] FAILED");
  return false;
}

// Wait for the link, WITHOUT reconfiguring the interface.
//
// This used to call WiFi.mode(WIFI_STA) + WiFi.begin() on every upload whenever
// the link happened to be down, which races the driver's own auto-reconnect.
// Kept as defensive hygiene — auto-reconnect already handles a dropped AP, and
// re-entering mode()/begin() underneath it is not something to do on a hot path.
//
// It is NOT the fix for the panics seen on 2026-09-17. Those were traced to the
// brownout detector firing at WiFi.begin() (`E BOD: Brownout detector was
// triggered`) on a bus-powered USB hub: the radio's first transmit sagged the
// supply and corrupted RAM, which surfaced as a different random exception each
// time — including one null-handler crash inside wifi_sta_receive() that looked
// convincingly like a netif race. Power first, then suspect software.
static bool wifiWait(unsigned long timeoutMs) {
  if (WiFi.status() == WL_CONNECTED) return true;
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) delay(250);
  bool up = (WiFi.status() == WL_CONNECTED);
  if (up) applyTxPower();            // auto-reconnect resets it to the default
  Serial.printf("[wifi] %s after wait (status %d)\n", up ? "up" : "still down",
                (int)WiFi.status());
  return up;
}

// Last resort if auto-reconnect has clearly given up. Kept off the hot path and
// rate-limited, because this is the call that can trigger the netif race above.
static void wifiKick() {
  static unsigned long lastKick = 0;
  if (millis() - lastKick < 300000UL) return;      // at most once every 5 min
  lastKick = millis();
  Serial.println("[wifi] auto-reconnect stalled; forcing a reassociation");
  WiFi.disconnect(false, false);
  delay(100);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);            // no mode() change here
}

static bool timeSynced() { return time(nullptr) > (time_t)EPOCH_SANE; }

// Local time for the OLED only. time() stays UTC epoch seconds no matter what
// TZ says, so uploads are unaffected. configTime() rewrites TZ to UTC, hence
// this runs after it as well as once at boot (a soft reset keeps the clock set,
// so configTime() may never run on that boot).
static void applyDisplayTz() { setenv("TZ", DISPLAY_TZ, 1); tzset(); }

static void syncTime() {
  if (timeSynced()) return;
  configTime(0, 0, NTP_SERVER_1, NTP_SERVER_2);   // UTC; the browser localizes
  applyDisplayTz();
  unsigned long t0 = millis();
  while (!timeSynced() && millis() - t0 < NTP_WAIT_MS) delay(250);
  if (timeSynced()) Serial.printf("[ntp] synced, epoch=%lu\n", (unsigned long)time(nullptr));
  else              Serial.println("[ntp] not synced (samples will carry server_ts only)");
}

// ---- upload ---------------------------------------------------------------

static void appendSampleJSON(String& out, const Sample& s, time_t epochNow, uint32_t msNow) {
  // Reconstruct the read time from how long ago it was taken. Guarded by
  // time_synced: without NTP we omit device_ts and let the server stamp it.
  char buf[96];
  out += '{';
  if (epochNow > (time_t)EPOCH_SANE) {
    uint32_t ageS = (msNow - s.t_uptime_ms) / 1000UL;   // wraps every ~49.7 days
    snprintf(buf, sizeof(buf), "\"device_ts\":%lu,\"time_synced\":true,",
             (unsigned long)(epochNow - (time_t)ageS));
  } else {
    snprintf(buf, sizeof(buf), "\"time_synced\":false,");
  }
  out += buf;
  snprintf(buf, sizeof(buf), "\"device_id\":\"%s\",\"config_id\":%d,", DEVICE_ID, CONFIG_ID);
  out += buf;
  snprintf(buf, sizeof(buf), "\"battery_v\":%.1f,\"soc\":%u,\"charge_a\":%.2f,",
           s.batt_raw * 0.1, s.soc, s.charge_raw * 0.01);
  out += buf;
  snprintf(buf, sizeof(buf), "\"pv_v\":%.1f,\"pv_a\":%.2f,\"pv_w\":%u,",
           s.pv_v_raw * 0.1, s.pv_a_raw * 0.01, s.pv_w_raw);
  out += buf;
  snprintf(buf, sizeof(buf), "\"load_v\":%.1f,\"load_a\":%.2f,\"load_w\":%u,\"rssi\":%d,\"bt_rssi\":%d,",
           s.load_v_raw * 0.1, s.load_a_raw * 0.01, s.load_w_raw, s.rssi, s.bt_rssi);
  out += buf;
  // Controller temperature (0x0103 high byte). Battery temp is omitted: the
  // Wanderer 10A has no sensor and always reports 0 there.
  snprintf(buf, sizeof(buf), "\"ctrl_temp\":%d", tempC(s.temp_raw >> 8));
  out += buf;
  // DHT11 air temperature + humidity, only when this sample has a fresh reading.
  if (s.env_t10 != ENV_NONE) {
    snprintf(buf, sizeof(buf), ",\"env_temp\":%.1f,\"env_hum\":%.0f",
             s.env_t10 * 0.1, s.env_rh10 * 0.1);
    out += buf;
  }
  out += '}';
}

// HMAC-SHA256 of msg with DEVICE_HMAC_KEY, as 64 lowercase hex characters.
// The key is used as its ASCII text, exactly as the server does.
static String hmacHex(const String& msg) {
  uint8_t mac[32];
  mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                  (const unsigned char*)DEVICE_HMAC_KEY, strlen(DEVICE_HMAC_KEY),
                  (const unsigned char*)msg.c_str(), msg.length(), mac);
  char hex[65];
  for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", mac[i]);
  return String(hex);
}

// POST the oldest samples as one batch. On HTTP 2xx, drop them from the queue.
// Returns true if anything was accepted.
//
// Every request is signed: X-Signature = HMAC-SHA256(key, "<ts>.<body>"). The
// server refuses a timestamp more than 5 min off its clock, or not newer than
// the last one it accepted, so the clock must be NTP-synced before uploading.
// Without NTP the batch simply stays queued until the next attempt.
static bool uploadBatch() {
  if (qCount == 0) return true;
  if (WiFi.status() != WL_CONNECTED && !wifiWait(15000)) { wifiKick(); return false; }
  syncTime();
  if (!timeSynced()) {
    Serial.println("[up] clock not synced yet: can't sign, keeping the batch queued");
    return false;
  }

  size_t n = qCount < MAX_BATCH ? qCount : MAX_BATCH;
  time_t   epochNow = time(nullptr);
  uint32_t msNow    = millis();

  String body;
  body.reserve(64 + n * 200);
  body += "{\"samples\":[";
  for (size_t i = 0; i < n; i++) {
    if (i) body += ',';
    appendSampleJSON(body, qAt(i), epochNow, msNow);
  }
  body += "]}";

  // Scoped so the TLS buffers (~40 KB heap alongside Bluedroid) are released
  // immediately after the request.
  int code = -1;
  {
    WiFiClientSecure tls;
    tls.setCACert(ISRG_ROOT_X1);
    tls.setTimeout(HTTP_TIMEOUT_MS / 1000);

    HTTPClient http;
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.setConnectTimeout(HTTP_TIMEOUT_MS);
    if (!http.begin(tls, INGEST_URL)) {
      Serial.println("[up] begin() failed (bad URL?)");
      uploadsFail++;
      return false;
    }
    http.addHeader("Content-Type", "application/json");
    char ts[16];
    snprintf(ts, sizeof(ts), "%lu", (unsigned long)time(nullptr));
    http.addHeader("X-Device-Id", DEVICE_ID);
    http.addHeader("X-Timestamp", ts);
    http.addHeader("X-Signature", hmacHex(String(ts) + "." + body));
    code = http.POST(body);
    if (code > 0) {
      String reply = http.getString();
      Serial.printf("[up] HTTP %d  %s\n", code, reply.substring(0, 120).c_str());
    } else {
      // -1 here is almost always the TLS handshake, not the network. Report the
      // mbedTLS reason and the heap: BLE + Wi-Fi + TLS is memory-tight, and an
      // allocation failure looks exactly like "connection refused".
      char err[128] = "";
      int tlsErr = tls.lastError(err, sizeof(err));
      Serial.printf("[up] transport error %d (%s)\n", code,
                    HTTPClient::errorToString(code).c_str());
      Serial.printf("[up] tls=%d (%s)  heap=%u maxAlloc=%u\n",
                    tlsErr, err[0] ? err : "no detail",
                    ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    }
    http.end();
  }

  if (code >= 200 && code < 300) {
    qPopFront(n);
    uploadsOk++;
#if OLED_ENABLED
    lastUploadMs = millis();
    lastUploadOK = true;
#endif
    Serial.printf("[up] %u sample(s) accepted, %u still queued\n",
                  (unsigned)n, (unsigned)qCount);
    return true;
  }
  // 401: signature refused - wrong DEVICE_HMAC_KEY, or the clock is off. The
  // server's reason is in the reply printed above; the data stays queued.
  if (code == 401) Serial.println("[up] 401: signature refused (check DEVICE_HMAC_KEY and NTP time)");
  // 400: the server's validation refused a field; see the reply above.
  if (code == 400) Serial.println("[up] 400: reading refused by server validation");
  uploadsFail++;
  return false;
}

// ---- OLED status display ---------------------------------------------------
//
// 128x32 shows two pages in turn (durations in config.h, OLED_PAGE_*_MS).
//
// Main page, three rows: one 16 px line and two 8 px lines. The two numbers you
// want at a glance go in the big row, the diagnostics underneath.
//
//   [▮▮▮ ]86%  12.9V    <- battery icon filled to SOC, then size 2 headline
//   PV 6W 0.3A LOAD 2W  <- size 1: PV power + PV current, then load power
//   WiFi -34 BT -79 4m  <- size 1: both RSSIs (dBm) + age of the last upload
//
// Info page:
//
//    ParadoxLab Solar   <- size 1, centred
//   39°C  ctrl temp     <- size 2 temperature (register 0x0103, high byte),
//                          label on one line; "batt 25°C" below it only if a
//                          battery sensor ever reports
//    2026-10-07  14:32  <- local date/time (DISPLAY_TZ), "clock not set" before NTP
//
// Environment page (only while the DHT11 has a fresh reading):
//
//      Environment      <- size 1, centred
//   24.5°C     45%      <- size 2: air temperature, relative humidity
//   air temp  humidity  <- size 1 labels
//
// The age counter is the useful one: if it climbs past ~11 min the logger has
// stopped and you can see that from across the room, without the dashboard.
// "--" in place of an RSSI means that link is down: Wi-Fi not associated, or
// the BT-1 has not been heard for BT_RSSI_STALE_MS. The BT figure refreshes
// every BT_RSSI_REFRESH_MS from the BT-1's advertisements, and on each poll's
// connection; the Wi-Fi figure is live.
#if OLED_ENABLED
static Adafruit_SSD1306 oled(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
static bool oledOK = false;

// Last values shown, kept separate from the queue so the display survives the
// queue being drained.
static float    dispBatt = 0, dispPvV = 0, dispPvA = 0;
static uint8_t  dispSoc = 0;
static uint16_t dispPvW = 0, dispLoadW = 0;
static int      dispCtrlT = 0, dispBattT = 0;   // °C, from register 0x0103
static bool     dispHave = false;
static bool     dispCharging = false;           // charge current > 0 on the last poll

static void oledInit() {
  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  // Probe first: Adafruit's begin() only fails when its buffer malloc fails. It
  // never checks that a panel answered, so on its own it logs "up" with nothing
  // attached (seen 2026-10-01: "up at 0x3C" while the bus had no pull-ups).
  Wire.beginTransmission(OLED_ADDR);
  if (Wire.endTransmission() != 0 || !oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.printf("[oled] not found at 0x%02X — continuing without it\n", OLED_ADDR);
    return;                       // never fatal: the logger matters, the screen doesn't
  }
  oledOK = true;
  oled.cp437(true);               // correct code page, so 0xF8 is the degree sign
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(16, 4);
  oled.print(F("ParadoxLab Solar"));
  oled.setCursor(16, 18);
  oled.print(F("starting..."));
  oled.display();
  Serial.printf("[oled] SSD1306 %dx%d up at 0x%02X\n", OLED_WIDTH, OLED_HEIGHT, OLED_ADDR);
}

// "WiFi -34 BT -79": 0 prints as "--". With two-digit RSSIs and up to a 4-digit
// age the row is at most 21 characters, one full 128 px line at size 1; a
// longer one (RSSI -100) just wraps off the bottom of the panel, harmlessly.
static void oledRssiPair() {
  int wifi = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;
  if (wifi) oled.printf("WiFi %d ", wifi); else oled.print(F("WiFi -- "));
  int bt = bt1RssiFresh();
  if (bt) oled.printf("BT %d", bt); else oled.print(F("BT --"));
}

// Print text centred on a size-1 row (6 px per character).
static void oledCentre(int y, const char* text) {
  int x = (OLED_WIDTH - (int)strlen(text) * 6) / 2;
  oled.setCursor(x < 0 ? 0 : x, y);
  oled.print(text);
}

// Battery icon, 16x12 at (x, y): outline, terminal nub, and a fill that
// follows the state of charge. While charging, the next 10 % step blinks.
static void oledBatteryIcon(int x, int y, uint8_t soc, bool charging) {
  oled.drawRect(x, y, 14, 12, SSD1306_WHITE);           // body
  oled.fillRect(x + 14, y + 3, 2, 6, SSD1306_WHITE);    // terminal
  int level = soc > 100 ? 100 : soc;
  int w = (level * 10 + 50) / 100;                      // 0..10 px inside
  if (charging && (millis() / 500) % 2 && w < 10) w++;  // blink the next step
  if (w > 0) oled.fillRect(x + 2, y + 2, w, 8, SSD1306_WHITE);
}

static void oledEnvPage() {
  oled.setTextSize(1);
  oledCentre(0, "Environment");

  oled.setTextSize(2);                         // y 9..24
  oled.setCursor(0, 9);
  oled.printf("%.1f", envT);
  oled.write(0xF8);                            // degree sign (cp437)
  oled.print('C');
  char rh[8];
  snprintf(rh, sizeof(rh), "%.0f%%", envRH);
  oled.setCursor(OLED_WIDTH - (int)strlen(rh) * 12, 9);   // right-aligned
  oled.print(rh);

  oled.setTextSize(1);
  oled.setCursor(0, 25);
  oled.print(F("air temp"));
  oled.setCursor(OLED_WIDTH - 8 * 6, 25);
  oled.print(F("humidity"));
}

static void oledInfoPage() {
  oled.setTextSize(1);
  oledCentre(0, "ParadoxLab Solar");

  oled.setTextSize(2);                         // temperature, y 9..24
  oled.setCursor(0, 9);
  oled.printf("%d", dispCtrlT);
  oled.write(0xF8);                            // degree sign (cp437)
  oled.print('C');

  oled.setTextSize(1);
  if (dispBattT != 0) {                        // 0 = no battery sensor fitted
    oled.setCursor(62, 9);
    oled.print(F("ctrl temp"));
    oled.setCursor(62, 17);
    oled.printf("batt %d", dispBattT);
    oled.write(0xF8);
    oled.print('C');
  } else {
    oled.setCursor(62, 13);                    // one line, centred on the number
    oled.print(F("ctrl temp"));
  }

  char when[24] = "clock not set";             // date row, y 25..31
  time_t now = time(nullptr);
  if (now > (time_t)EPOCH_SANE) {
    struct tm t;
    localtime_r(&now, &t);
    strftime(when, sizeof(when), "%Y-%m-%d  %H:%M", &t);
  }
  oledCentre(25, when);
}

// forceMain shows the readings page regardless of the page rotation; used right
// after a poll so a new reading appears at once, not after the upload.
static void oledDraw(bool forceMain = false) {
  if (!oledOK) return;
  oled.clearDisplay();

  // Page rotation: main, info, then environment while the DHT11 is reporting.
  uint32_t envPage = envFresh() ? OLED_PAGE_ENV_MS : 0;
  uint32_t cycle = OLED_PAGE_MAIN_MS + OLED_PAGE_INFO_MS + envPage;
  uint32_t phase = millis() % cycle;
  bool rotate = !forceMain && dispHave;
  bool info = rotate && phase >= OLED_PAGE_MAIN_MS && phase < OLED_PAGE_MAIN_MS + OLED_PAGE_INFO_MS;
  bool envP = rotate && envPage && phase >= OLED_PAGE_MAIN_MS + OLED_PAGE_INFO_MS;

  if (!dispHave) {
    oled.setTextSize(1);
    oled.setCursor(0, 0);
    oled.println(F("waiting for"));
    oled.println(F("first reading"));
    oledRssiPair();
  } else if (info) {
    oledInfoPage();
  } else if (envP) {
    oledEnvPage();
  } else {
    oled.setTextSize(2);                       // row 1: the headline, y 0..15
    oled.setCursor(0, 0);
    oledBatteryIcon(0, 2, dispSoc, dispCharging);
    oled.setCursor(19, 0);
    oled.printf("%u%%", dispSoc);
    char volts[8];                             // right-aligned, so "100%" still fits
    snprintf(volts, sizeof(volts), "%.1fV", dispBatt);
    oled.setCursor(OLED_WIDTH - (int)strlen(volts) * 12, 0);
    oled.print(volts);

    oled.setTextSize(1);                       // row 2: power, y 17..24
    oled.setCursor(0, 17);
    oled.printf("PV %uW %.1fA  LOAD %uW", dispPvW, dispPvA, dispLoadW);

    oled.setCursor(0, 25);                     // row 3: health, y 25..31
    uint32_t ageMin = lastUploadMs ? (millis() - lastUploadMs)/60000UL : 0;
    oledRssiPair();
    oled.printf(" %lum", (unsigned long)ageMin);
  }
  oled.display();
}
#else
static void oledInit() {}
static void oledDraw(bool = false) {}
#endif

// Capture a freshly decoded sample for the screen. Kept out of pollOnce() so
// the display can never affect acquisition.
static void oledNote(const Sample& s) {
#if OLED_ENABLED
  dispSoc   = s.soc;
  dispBatt  = s.batt_raw * 0.1f;
  dispPvV   = s.pv_v_raw * 0.1f;
  dispPvA   = s.pv_a_raw * 0.01f;    // UNVERIFIED register (0x0108); shown for glanceability
  dispPvW   = s.pv_w_raw;
  dispLoadW = s.load_w_raw;
  dispCtrlT = tempC(s.temp_raw >> 8);
  dispBattT = tempC(s.temp_raw & 0xFF);
  dispCharging = s.charge_raw > 0;
  dispHave  = true;
#else
  (void)s;
#endif
}

// ---- main -----------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println(F("\n=== Upload over Wi-Fi (Stage 10) ==="));
  Serial.printf("device_id=%s  config_id=%d  ingest=%s\n",
                DEVICE_ID, CONFIG_ID, INGEST_URL);

  oledInit();          // before the radios, so it can show bring-up progress
  envInit();
  applyDisplayTz();
  wifiStart(20000);
  syncTime();

  NimBLEDevice::init("");
  client = NimBLEDevice::createClient();
  client->setClientCallbacks(new ClientCB(), false);
  // Default is 30 s. Since we now reconnect once per poll, a BT-1 that is busy
  // (phone app connected) should fail fast and let the backoff handle it rather
  // than blocking the loop for half a minute.
  client->setConnectTimeout(10000);

  // Registered once — re-registering per scan (as Stage 9 did) leaks a callback
  // object on every retry. Near-100% duty cycle: the BT-1 is a weak advertiser
  // (~-75 to -85 dBm here) and the default window misses it.
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&findCB, false);
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(99);

  // The TLS handshake needs ~32 KB of contiguous heap; this line is the check
  // that it will fit. Under Bluedroid it read 64 KB free / 32.7 KB largest block
  // and the handshake failed with SSL_ALLOC_FAILED every time.
  Serial.printf("[mem] free heap after init: %u B (largest block %u B)\n",
                ESP.getFreeHeap(), ESP.getMaxAllocHeap());
}

void loop() {
  static unsigned long nextPollAt = 0;
  static unsigned long backoff    = BACKOFF_MIN_MS;
  static unsigned long lastStat   = 0;
  static unsigned long lastDrain  = 0;

  // Connect only when a reading is actually due. Stage 9 held the link open
  // permanently, which is fine when the ESP32 is the only central — but the
  // BT-1 serves one at a time and stops advertising while connected, so a
  // permanent link locks the Renogy phone app out around the clock for the sake
  // of a ~2 s transaction every 10 minutes. See RELEASE_BT1_BETWEEN_POLLS.
  if ((long)(millis() - nextPollAt) >= 0) {
    if (ensureConnected()) {
      backoff = BACKOFF_MIN_MS;                      // reset on healthy link
      if (pollOnce()) {
        goodPolls++;
        nextPollAt = millis() + POLL_INTERVAL_MS;
        // Show it now: the upload below blocks the loop for ~5 s (measured
        // 2026-10-07: reading at 10.45 s, upload done at 15.19 s), and the
        // periodic redraw can't run until it returns.
        oledDraw(true);
      } else {
        failedPolls++;
        nextPollAt = millis() + RETRY_INTERVAL_MS;
        if (failedPolls % 3 == 0) {                  // 3 strikes → cycle the link
          Serial.println("[ble] repeated poll failures, cycling link");
          client->disconnect();
        }
      }
      uploadBatch();                                 // this sample + any backlog

#if RELEASE_BT1_BETWEEN_POLLS
      if (client->isConnected()) {
        client->disconnect();
        Serial.println("[ble] released BT-1 (free for the Renogy app until next poll)");
      }
#endif
    } else {
      // Couldn't reach the BT-1 — back off, but don't let that stall uploads.
      bt1Rssi    = 0;                                // screen shows "BT --"
      nextPollAt = millis() + backoff;
      Serial.printf("[ble] retry in %lus\n", backoff / 1000);
      backoff = min(backoff * 2, BACKOFF_MAX_MS);    // exponential backoff
      uploadBatch();
    }
  }

  // Refresh the BT-1 RSSI from its advertisements while idle between polls.
  // Non-blocking, so the OLED keeps redrawing; the scan callback stops it as
  // soon as the BT-1 is heard. Silent if a phone holds the BT-1 (it does not
  // advertise then) — the value then ages out to "--" after BT_RSSI_STALE_MS.
  static unsigned long lastRssiScan = 0;
  NimBLEScan* bgScan = NimBLEDevice::getScan();
  if (rssiScan && !bgScan->isScanning()) rssiScan = false;
  if (!client->isConnected() && !bgScan->isScanning() &&
      millis() - lastRssiScan >= BT_RSSI_REFRESH_MS) {
    lastRssiScan = millis();
    bgScan->clearResults();
    rssiScan = true;
    bgScan->start(BT_RSSI_SCAN_MS, false, true);
  }

  // Drain a backlog left by an earlier Wi-Fi/server outage while we sit idle
  // between polls, so recovery doesn't have to wait for the next reading.
  if (qCount && millis() - lastDrain >= RETRY_INTERVAL_MS) {
    lastDrain = millis();
    uploadBatch();
  }

  // Air temperature + humidity, between BT-1 transactions only.
  static unsigned long lastEnv = 0;
  if (ENV_ENABLED && !client->isConnected() &&
      (!lastEnv || millis() - lastEnv >= ENV_READ_MS)) {
    lastEnv = millis();
    envRead();
  }

  static unsigned long lastDraw = 0;
  if (millis() - lastDraw >= OLED_REFRESH_MS) {      // RSSI + age tick live
    lastDraw = millis();
    oledDraw();
  }

  if (millis() - lastStat >= 60000) {                // periodic health line
    lastStat = millis();
    Serial.printf("[stat] polls ok=%lu fail=%lu | uploads ok=%lu fail=%lu | "
                  "queued=%u dropped=%lu | wifi=%s rssi=%d bt=%d | heap=%u\n",
                  goodPolls, failedPolls, uploadsOk, uploadsFail,
                  (unsigned)qCount, droppedSamples,
                  WiFi.status() == WL_CONNECTED ? "up" : "down",
                  WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0,
                  bt1RssiFresh(), ESP.getFreeHeap());
    if (ENV_ENABLED) {
      if (envFresh()) Serial.printf("[stat] air=%.1fC rh=%.0f%%\n", envT, envRH);
      else            Serial.println("[stat] air: no fresh DHT reading");
    }
  }

  delay(20);
}
