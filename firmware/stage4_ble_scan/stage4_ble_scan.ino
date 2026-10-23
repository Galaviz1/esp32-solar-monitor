// Stage 4 — BLE scan (and flag a likely Renogy BT-1).
// Uses the stock ESP32 BLE library (no extra install needed).
//
// Acceptance (Stage 4): lists nearby BLE devices with address + RSSI.
// Stage 5: confirm the BT-1 appears (name usually starts "BT-TH-"); record its
// MAC into firmware/common/secrets.h (BT1_MAC) and docs/protocol-bt1.md.

#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

static const uint32_t SCAN_SECONDS = 10;
static const char* BT1_PREFIX = "BT-TH-";  // common Renogy BT-1 name prefix

BLEScan* pScan = nullptr;

class ScanCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice dev) override {
    String name = dev.haveName() ? String(dev.getName().c_str()) : String("");
    bool likelyBT1 = name.startsWith(BT1_PREFIX) || name.startsWith("BT-1");

    Serial.printf("  %s  RSSI=%4d  name=\"%s\"%s\n",
                  dev.getAddress().toString().c_str(),
                  dev.getRSSI(),
                  name.c_str(),
                  likelyBT1 ? "   <-- possible Renogy BT-1" : "");
  }
};

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println(F("\n=== BLE Scan (Stage 4) ==="));

  BLEDevice::init("");                 // scanner needs no local name
  pScan = BLEDevice::getScan();
  pScan->setAdvertisedDeviceCallbacks(new ScanCallbacks());
  pScan->setActiveScan(true);          // request scan responses (names)
  pScan->setInterval(100);
  pScan->setWindow(99);
}

void loop() {
  Serial.printf("--- scanning for %u s ---\n", SCAN_SECONDS);
  // esp32 Arduino core 3.x: start() returns BLEScanResults* (pointer).
  BLEScanResults* results = pScan->start(SCAN_SECONDS, false);
  Serial.printf("--- done: %d device(s) found ---\n\n", results->getCount());
  pScan->clearResults();               // free heap between scans
  delay(2000);
}
