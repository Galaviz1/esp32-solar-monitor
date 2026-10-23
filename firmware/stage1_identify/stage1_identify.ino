// Stage 1 — Identify the exact ESP32 variant.
// Flash this, open Serial Monitor @ 115200, and copy the output into
// docs/hardware.md ("Stage 1 measured output").
//
// The BT/BLE feature flags confirm whether this is the original ESP32
// (BR/EDR + BLE) vs an S3/C3 (BLE-only). See docs/hardware.md for the analysis.

#include <Arduino.h>
#include "esp_chip_info.h"

void setup() {
  Serial.begin(115200);
  delay(2000);  // let USB serial settle
  Serial.println();
  Serial.println(F("=== ESP32 Variant Identification (Stage 1) ==="));

  Serial.printf("Chip model      : %s\n", ESP.getChipModel());
  Serial.printf("Chip revision   : %d\n", ESP.getChipRevision());
  Serial.printf("CPU cores       : %d\n", ESP.getChipCores());
  Serial.printf("CPU freq (MHz)  : %u\n", getCpuFrequencyMhz());
  Serial.printf("Flash size (MB) : %.2f\n", ESP.getFlashChipSize() / (1024.0 * 1024.0));
  Serial.printf("Flash speed(MHz): %u\n", ESP.getFlashChipSpeed() / 1000000U);
  Serial.printf("SDK version     : %s\n", ESP.getSdkVersion());
  Serial.printf("PSRAM size (B)  : %u\n", ESP.getPsramSize());

  uint64_t mac = ESP.getEfuseMac();
  Serial.printf("eFuse MAC       : %04X%08X\n",
                (uint16_t)(mac >> 32), (uint32_t)mac);

  esp_chip_info_t info;
  esp_chip_info(&info);
  Serial.printf("Features        : WiFi%s%s%s\n",
                (info.features & CHIP_FEATURE_BT)  ? " + BT(BR/EDR)" : "",
                (info.features & CHIP_FEATURE_BLE) ? " + BLE"        : "",
                (info.features & CHIP_FEATURE_EMB_FLASH) ? " + embFlash" : "");
  Serial.printf("Silicon cores   : %d, revision %d\n", info.cores, info.revision);

  Serial.println(F("=== end ==="));
}

void loop() {
  // one-shot; nothing to do
}
