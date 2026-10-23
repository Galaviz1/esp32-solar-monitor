// Stage 3 — Verify Wi-Fi connectivity.
// Prerequisite: copy firmware/common/secrets.example.h -> secrets.h, fill in
// WIFI_SSID / WIFI_PASSWORD, and place secrets.h in THIS sketch folder
// (Arduino compiles per-folder).
//
// Acceptance: prints an IP address and RSSI, and stays connected.

#include <WiFi.h>
#include "secrets.h"

void printStatus() {
  Serial.print("Status: ");
  switch (WiFi.status()) {
    case WL_CONNECTED:       Serial.println("CONNECTED");        break;
    case WL_NO_SSID_AVAIL:   Serial.println("NO_SSID_AVAIL");    break;
    case WL_CONNECT_FAILED:  Serial.println("CONNECT_FAILED");   break;
    case WL_IDLE_STATUS:     Serial.println("IDLE");             break;
    case WL_DISCONNECTED:    Serial.println("DISCONNECTED");     break;
    default:                 Serial.println(WiFi.status());      break;
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.printf("\nConnecting to SSID '%s'", WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Wi-Fi connected.");
    Serial.print("  IP address : "); Serial.println(WiFi.localIP());
    Serial.print("  Gateway    : "); Serial.println(WiFi.gatewayIP());
    Serial.print("  RSSI (dBm) : "); Serial.println(WiFi.RSSI());
    Serial.print("  MAC        : "); Serial.println(WiFi.macAddress());
  } else {
    Serial.println("Wi-Fi FAILED to connect within timeout.");
    printStatus();
  }
}

void loop() {
  // Report link health every 10 s so you can watch stability.
  static unsigned long last = 0;
  if (millis() - last > 10000) {
    last = millis();
    Serial.print("[health] "); printStatus();
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("         RSSI=%d dBm  IP=%s\n",
                    WiFi.RSSI(), WiFi.localIP().toString().c_str());
    }
  }
}
