// Stage 6 — Connect to the Renogy BT-1 and enumerate its GATT.
//
// Prereq: copy firmware/common/secrets.example.h -> secrets.h into THIS folder,
// and (from Stage 4/5) set BT1_MAC to your BT-1's address. If BT1_MAC is left
// empty, this sketch falls back to matching the advertised name prefix.
//
// It scans (to capture the correct address *type*), connects, then prints every
// service and characteristic with its properties:
//   R = read, W = write(with response), w = write-no-response, N = notify, I = indicate
//
// GOAL: record the actual WRITE and NOTIFY characteristic UUIDs (and their
// service UUIDs) into docs/protocol-bt1.md "Stage 6 measured GATT". Do NOT trust
// the community UUIDs until you've confirmed them here.
//
// Library: stock ESP32 BLE (no extra install). NimBLE migration is deferred to
// Stage 9 per docs/stages.md — one new variable at a time.

#include <BLEDevice.h>
#include "secrets.h"

static const uint32_t SCAN_SECONDS = 12;
static BLEAdvertisedDevice* target = nullptr;

class FindCB : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice d) override {
    String addr = String(d.getAddress().toString().c_str());
    String name = d.haveName() ? String(d.getName().c_str()) : String("");
    bool haveMac   = (strlen(BT1_MAC) > 0);
    bool macMatch  = haveMac && addr.equalsIgnoreCase(BT1_MAC);
    bool nameMatch = name.startsWith(BT1_NAME_PREFIX);

    if (macMatch || (!haveMac && nameMatch)) {
      Serial.printf("Found BT-1: %s  name=\"%s\"\n", addr.c_str(), name.c_str());
      target = new BLEAdvertisedDevice(d);
      BLEDevice::getScan()->stop();
    }
  }
};

static void propsStr(BLERemoteCharacteristic* ch, char* out) {
  out[0] = ch->canRead()            ? 'R' : '-';
  out[1] = ch->canWrite()           ? 'W' : '-';
  out[2] = ch->canWriteNoResponse() ? 'w' : '-';
  out[3] = ch->canNotify()          ? 'N' : '-';
  out[4] = ch->canIndicate()        ? 'I' : '-';
  out[5] = '\0';
}

void enumerateGatt() {
  BLEClient* client = BLEDevice::createClient();
  Serial.printf("Connecting to %s ...\n", target->getAddress().toString().c_str());

  if (!client->connect(target)) {   // pass the advertised device (keeps addr type)
    Serial.println("  CONNECT FAILED. Re-run; move ESP32 closer; check BT1_MAC.");
    return;
  }
  Serial.println("  connected. Enumerating services/characteristics:\n");

  std::map<std::string, BLERemoteService*>* services = client->getServices();
  for (auto& s : *services) {
    BLERemoteService* svc = s.second;
    Serial.printf("Service  %s\n", svc->getUUID().toString().c_str());

    std::map<std::string, BLERemoteCharacteristic*>* chars = svc->getCharacteristics();
    for (auto& c : *chars) {
      BLERemoteCharacteristic* ch = c.second;
      char p[6]; propsStr(ch, p);
      Serial.printf("   Char  %s  [%s]\n", ch->getUUID().toString().c_str(), p);
    }
    Serial.println();
  }

  Serial.println("Enumeration complete. Record the WRITE [.,W or w] and");
  Serial.println("NOTIFY [.,N] characteristic + service UUIDs in protocol-bt1.md.");
  client->disconnect();
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println(F("\n=== BT-1 GATT Enumeration (Stage 6) ==="));

  BLEDevice::init("");
  BLEScan* scan = BLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(new FindCB());
  scan->setActiveScan(true);

  Serial.printf("Scanning up to %u s for the BT-1...\n", SCAN_SECONDS);
  scan->start(SCAN_SECONDS, false);

  if (target == nullptr) {
    Serial.println("BT-1 NOT found. Run Stage 4 scan, set BT1_MAC in secrets.h,");
    Serial.println("and confirm the BT-1 is powered and in range.");
    return;
  }
  enumerateGatt();
  Serial.println(F("=== end ==="));
}

void loop() {}
