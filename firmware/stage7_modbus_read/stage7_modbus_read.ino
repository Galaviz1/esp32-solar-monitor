// Stage 7 — Determine the BT-1 protocol / packet structure, and take the first
// Modbus read (begins Stage 8).
//
// This sketch:
//   1. scans for + connects to the BT-1 (BT1_MAC or name prefix),
//   2. subscribes to the NOTIFY characteristic,
//   3. writes a Modbus "Read Holding Registers" (func 0x03) request with a
//      CRC-16/MODBUS trailer,
//   4. reassembles the notification(s), validates CRC, and decodes registers.
//
// ⚠️ UNVERIFIED DEFAULTS. The UUIDs, device address, and register map below are
// community-reported (see docs/protocol-bt1.md). BEFORE trusting them:
//   - set WRITE_*/NOTIFY_* UUIDs from YOUR Stage 6 output,
//   - if you get no reply, try MB_DEVICE_ADDR = 0x01 instead of 0xFF,
//   - VALIDATE: decoded battery voltage MUST match a multimeter reading.
//     Only then is decoding proven (Stage 8 acceptance).
//
// Library: stock ESP32 BLE (NimBLE migration deferred to Stage 9).

#include <BLEDevice.h>
#include "secrets.h"

// ---- GATT identifiers (UPDATE from Stage 6 findings) ----
#define NOTIFY_SVC_UUID  "0000fff0-0000-1000-8000-00805f9b34fb"
#define NOTIFY_CHAR_UUID "0000fff1-0000-1000-8000-00805f9b34fb"
#define WRITE_SVC_UUID   "0000ffd0-0000-1000-8000-00805f9b34fb"
#define WRITE_CHAR_UUID  "0000ffd1-0000-1000-8000-00805f9b34fb"

// ---- Modbus request (UNVERIFIED — see notes above) ----
#define MB_DEVICE_ADDR   0xFF      // try 0x01 if 0xFF yields no response
#define MB_FUNC_READ     0x03
#define REG_START        0x0100    // block start
#define REG_COUNT        0x0008    // 8 regs: 0x0100..0x0107

static const uint32_t SCAN_SECONDS = 12;

// ---- BLE globals ----
static BLEAdvertisedDevice* target = nullptr;
static BLERemoteCharacteristic* writeChar  = nullptr;
static BLERemoteCharacteristic* notifyChar = nullptr;

// ---- notification reassembly buffer ----
static volatile bool     haveResponse = false;
static uint8_t           resp[64];
static volatile size_t   respLen = 0;

// CRC-16/MODBUS (poly 0xA001, init 0xFFFF). Appended low byte first.
static uint16_t modbusCRC(const uint8_t* buf, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= buf[i];
    for (int b = 0; b < 8; b++)
      crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : (crc >> 1);
  }
  return crc;
}

class FindCB : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice d) override {
    String addr = String(d.getAddress().toString().c_str());
    String name = d.haveName() ? String(d.getName().c_str()) : String("");
    bool haveMac = (strlen(BT1_MAC) > 0);
    if ((haveMac && addr.equalsIgnoreCase(BT1_MAC)) ||
        (!haveMac && name.startsWith(BT1_NAME_PREFIX))) {
      target = new BLEAdvertisedDevice(d);
      BLEDevice::getScan()->stop();
    }
  }
};

// Notification callback: append bytes, mark complete once the full Modbus
// response (3 header + byte_count + 2 CRC) has arrived (may span >1 notify).
static void notifyCB(BLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
  for (size_t i = 0; i < len && respLen < sizeof(resp); i++)
    resp[respLen++] = data[i];
  if (respLen >= 3) {
    size_t expected = 3 + resp[2] + 2;      // addr,func,count + data + crc
    if (respLen >= expected) haveResponse = true;
  }
}

static void printHex(const uint8_t* b, size_t n) {
  for (size_t i = 0; i < n; i++) Serial.printf("%02X ", b[i]);
  Serial.println();
}

void doRead() {
  BLEClient* client = BLEDevice::createClient();
  Serial.printf("Connecting to %s ...\n", target->getAddress().toString().c_str());
  if (!client->connect(target)) { Serial.println("  CONNECT FAILED"); return; }
  Serial.println("  connected.");

  BLERemoteService* nsvc = client->getService(BLEUUID(NOTIFY_SVC_UUID));
  BLERemoteService* wsvc = client->getService(BLEUUID(WRITE_SVC_UUID));
  if (!nsvc || !wsvc) {
    Serial.println("  Service not found — UUIDs likely wrong. Fix from Stage 6.");
    client->disconnect(); return;
  }
  notifyChar = nsvc->getCharacteristic(BLEUUID(NOTIFY_CHAR_UUID));
  writeChar  = wsvc->getCharacteristic(BLEUUID(WRITE_CHAR_UUID));
  if (!notifyChar || !writeChar) {
    Serial.println("  Characteristic not found — fix UUIDs from Stage 6.");
    client->disconnect(); return;
  }
  if (notifyChar->canNotify()) notifyChar->registerForNotify(notifyCB);

  // Build request: addr,func,reg_hi,reg_lo,cnt_hi,cnt_lo,crc_lo,crc_hi
  uint8_t req[8];
  req[0] = MB_DEVICE_ADDR;
  req[1] = MB_FUNC_READ;
  req[2] = (REG_START >> 8) & 0xFF; req[3] = REG_START & 0xFF;
  req[4] = (REG_COUNT >> 8) & 0xFF; req[5] = REG_COUNT & 0xFF;
  uint16_t crc = modbusCRC(req, 6);
  req[6] = crc & 0xFF; req[7] = (crc >> 8) & 0xFF;   // low byte first

  Serial.print("TX: "); printHex(req, sizeof(req));
  haveResponse = false; respLen = 0;
  writeChar->writeValue(req, sizeof(req), writeChar->canWrite());  // response if supported

  unsigned long start = millis();
  while (!haveResponse && millis() - start < 3000) delay(10);

  if (!haveResponse) {
    Serial.println("  No/again incomplete response within 3 s.");
    Serial.println("  Try MB_DEVICE_ADDR=0x01, or re-check Stage 6 UUIDs.");
    client->disconnect(); return;
  }

  Serial.print("RX: "); printHex(resp, respLen);

  // Validate framing + CRC
  size_t frameLen = 3 + resp[2];
  uint16_t rxCrc = resp[frameLen] | (resp[frameLen + 1] << 8);
  uint16_t calc  = modbusCRC(resp, frameLen);
  if (resp[1] != MB_FUNC_READ) {
    Serial.printf("  Unexpected func 0x%02X (exception?). Check request.\n", resp[1]);
    client->disconnect(); return;
  }
  if (rxCrc != calc) {
    Serial.printf("  CRC MISMATCH rx=%04X calc=%04X\n", rxCrc, calc);
    client->disconnect(); return;
  }
  Serial.println("  CRC OK.");

  // Decode registers (big-endian words) starting at REG_START
  uint8_t nReg = resp[2] / 2;
  for (uint8_t i = 0; i < nReg; i++) {
    uint16_t addr = REG_START + i;
    uint16_t raw  = (resp[3 + i * 2] << 8) | resp[4 + i * 2];
    Serial.printf("  reg 0x%04X = %5u (0x%04X)", addr, raw, raw);
    if (addr == 0x0101)                                   // battery voltage ×0.1
      Serial.printf("   <- battery voltage? %.1f V  (VALIDATE vs multimeter)", raw * 0.1);
    if (addr == 0x0107)                                   // PV voltage ×0.1
      Serial.printf("   <- PV voltage? %.1f V", raw * 0.1);
    Serial.println();
  }

  Serial.println("  >>> Stage 8 gate: confirm battery voltage matches a meter <<<");
  client->disconnect();
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println(F("\n=== BT-1 Modbus Read (Stage 7/8) ==="));

  BLEDevice::init("");
  BLEScan* scan = BLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(new FindCB());
  scan->setActiveScan(true);
  Serial.printf("Scanning up to %u s...\n", SCAN_SECONDS);
  scan->start(SCAN_SECONDS, false);

  if (!target) { Serial.println("BT-1 not found — set BT1_MAC, check power/range."); return; }
  doRead();
  Serial.println(F("=== end ==="));
}

void loop() {}
