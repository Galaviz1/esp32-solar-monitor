// Stage 9 — Periodic data acquisition with reconnect/backoff + RAM ring buffer.
//
// Builds on Stage 7: keeps a persistent BLE link to the BT-1, polls at a fixed
// interval, survives disconnects (exponential backoff), and buffers decoded
// samples in a bounded ring buffer so nothing is lost between polls. This is the
// staging point for Stage 10 (upload over Wi-Fi) — samples accumulate here and,
// once Stage 10 exists, get drained to the server.
//
// Prereq: Stages 6–8 passed. Set the UUIDs below from YOUR Stage 6 output and
// confirm the register decoding matched a multimeter (Stage 8). BT1_MAC in
// secrets.h.
//
// ⚠️ Register scalings are still UNVERIFIED until Stage 8 signs off — see
// docs/protocol-bt1.md. Timestamps here are UPTIME seconds; real NTP epoch is
// added at Stage 10 when Wi-Fi comes online.
//
// Library: stock ESP32 BLE (NimBLE migration deferred to Stage 9→ port later).

#include <BLEDevice.h>
#include "secrets.h"

// ---- GATT identifiers (UPDATE from Stage 6) ----
#define NOTIFY_SVC_UUID  "0000fff0-0000-1000-8000-00805f9b34fb"
#define NOTIFY_CHAR_UUID "0000fff1-0000-1000-8000-00805f9b34fb"
#define WRITE_SVC_UUID   "0000ffd0-0000-1000-8000-00805f9b34fb"
#define WRITE_CHAR_UUID  "0000ffd1-0000-1000-8000-00805f9b34fb"

// ---- Modbus request (from Stage 7) ----
#define MB_DEVICE_ADDR   0xFF
#define MB_FUNC_READ     0x03
#define REG_START        0x0100
#define REG_COUNT        0x0010          // 16 regs: 0x0100..0x010F

// ---- acquisition config ----
#define POLL_INTERVAL_MS 5000UL
#define RESP_TIMEOUT_MS  3000UL
#define BACKOFF_MIN_MS   1000UL
#define BACKOFF_MAX_MS   30000UL
#define RING_SIZE        60              // ~5 min at 5 s cadence

// ---- decoded sample (raw register words; scale at analysis/upload) ----
struct Sample {
  uint32_t t_uptime_s;
  uint8_t  soc;            // 0x0100
  uint16_t batt_raw;       // 0x0101  (×0.1 V, unverified)
  uint16_t pv_v_raw;       // 0x0107  (×0.1 V)
  uint16_t pv_a_raw;       // 0x0108  (×0.01 A)
  uint16_t pv_w_raw;       // 0x0109  (×1 W)
  bool     valid;
};

static Sample   ring[RING_SIZE];
static size_t   ringHead = 0, ringCount = 0;
static uint32_t droppedSamples = 0, goodPolls = 0, failedPolls = 0;

static void ringPush(const Sample& s) {
  ring[ringHead] = s;
  ringHead = (ringHead + 1) % RING_SIZE;
  if (ringCount < RING_SIZE) ringCount++;
  else droppedSamples++;               // overwrote the oldest unsent sample
}

// ---- BLE state ----
static const uint32_t SCAN_SECONDS = 10;
static BLEAdvertisedDevice*      target     = nullptr;
static BLEClient*                client     = nullptr;
static BLERemoteCharacteristic*  writeChar  = nullptr;
static BLERemoteCharacteristic*  notifyChar = nullptr;
static volatile bool             connected  = false;

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

static void notifyCB(BLERemoteCharacteristic*, uint8_t* d, size_t n, bool) {
  for (size_t i = 0; i < n && respLen < sizeof(resp); i++) resp[respLen++] = d[i];
  if (respLen >= 3 && respLen >= (size_t)(3 + resp[2] + 2)) haveResponse = true;
}

class ClientCB : public BLEClientCallbacks {
  void onConnect(BLEClient*) override { connected = true; }
  void onDisconnect(BLEClient*) override {
    connected = false; notifyChar = nullptr; writeChar = nullptr;
    Serial.println("[ble] disconnected");
  }
};

class FindCB : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice d) override {
    String addr = String(d.getAddress().toString().c_str());
    String name = d.haveName() ? String(d.getName().c_str()) : String("");
    bool haveMac = (strlen(BT1_MAC) > 0);
    if ((haveMac && addr.equalsIgnoreCase(BT1_MAC)) ||
        (!haveMac && name.startsWith(BT1_NAME_PREFIX))) {
      if (target) { delete target; }
      target = new BLEAdvertisedDevice(d);
      BLEDevice::getScan()->stop();
    }
  }
};

static bool scanForTarget() {
  Serial.println("[ble] scanning for BT-1...");
  BLEScan* scan = BLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(new FindCB(), false);
  scan->setActiveScan(true);
  scan->start(SCAN_SECONDS, false);
  scan->clearResults();
  return target != nullptr;
}

// Fetch characteristics + subscribe. Returns false if UUIDs don't resolve.
static bool acquireChars() {
  BLERemoteService* nsvc = client->getService(BLEUUID(NOTIFY_SVC_UUID));
  BLERemoteService* wsvc = client->getService(BLEUUID(WRITE_SVC_UUID));
  if (!nsvc || !wsvc) { Serial.println("[ble] service UUID not found"); return false; }
  notifyChar = nsvc->getCharacteristic(BLEUUID(NOTIFY_CHAR_UUID));
  writeChar  = wsvc->getCharacteristic(BLEUUID(WRITE_CHAR_UUID));
  if (!notifyChar || !writeChar) { Serial.println("[ble] char UUID not found"); return false; }
  if (notifyChar->canNotify()) notifyChar->registerForNotify(notifyCB);
  return true;
}

static bool ensureConnected() {
  if (connected && notifyChar && writeChar) return true;
  if (!target && !scanForTarget()) return false;
  Serial.printf("[ble] connecting to %s...\n", target->getAddress().toString().c_str());
  if (!client->connect(target)) { Serial.println("[ble] connect failed"); return false; }
  if (!acquireChars()) { client->disconnect(); return false; }
  Serial.println("[ble] connected + subscribed");
  return true;
}

// One Modbus poll → decode → ring buffer. Returns true on a validated sample.
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
  uint16_t rxCrc = resp[frameLen] | (resp[frameLen + 1] << 8);
  if (resp[1] != MB_FUNC_READ || rxCrc != modbusCRC(resp, frameLen)) {
    Serial.println("[poll] bad frame/CRC"); return false;
  }

  auto reg = [&](uint16_t addr) -> uint16_t {
    int i = addr - REG_START;
    return (resp[3 + i * 2] << 8) | resp[4 + i * 2];
  };

  Sample s;
  s.t_uptime_s = millis() / 1000UL;
  s.soc      = reg(0x0100) & 0xFF;
  s.batt_raw = reg(0x0101);
  s.pv_v_raw = reg(0x0107);
  s.pv_a_raw = reg(0x0108);
  s.pv_w_raw = reg(0x0109);
  s.valid    = true;
  ringPush(s);

  Serial.printf("[poll] t=%us  batt=%.1fV  soc=%u%%  pv=%.1fV/%.2fA/%uW  (buf %u/%u)\n",
                s.t_uptime_s, s.batt_raw * 0.1, s.soc,
                s.pv_v_raw * 0.1, s.pv_a_raw * 0.01, s.pv_w_raw,
                (unsigned)ringCount, RING_SIZE);
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println(F("\n=== Periodic Acquisition (Stage 9) ==="));
  BLEDevice::init("");
  client = BLEDevice::createClient();
  client->setClientCallbacks(new ClientCB());
}

void loop() {
  static unsigned long lastPoll = 0;
  static unsigned long backoff  = BACKOFF_MIN_MS;
  static unsigned long nextTry  = 0;
  static unsigned long lastStat = 0;

  if (!ensureConnected()) {
    unsigned long now = millis();
    if (now >= nextTry) {
      nextTry = now + backoff;
      Serial.printf("[ble] retry in %lus\n", backoff / 1000);
      backoff = min(backoff * 2, BACKOFF_MAX_MS);   // exponential backoff
    }
    delay(50);
    return;
  }
  backoff = BACKOFF_MIN_MS;                          // reset on healthy link

  if (millis() - lastPoll >= POLL_INTERVAL_MS) {
    lastPoll = millis();
    if (pollOnce()) goodPolls++;
    else {
      failedPolls++;
      if (failedPolls % 3 == 0) {                    // 3 strikes → force reconnect
        Serial.println("[ble] repeated poll failures, cycling link");
        client->disconnect();
      }
    }
  }

  if (millis() - lastStat >= 30000) {                // periodic health line
    lastStat = millis();
    Serial.printf("[stat] good=%lu fail=%lu buffered=%u dropped=%lu\n",
                  goodPolls, failedPolls, (unsigned)ringCount, droppedSamples);
  }
}
