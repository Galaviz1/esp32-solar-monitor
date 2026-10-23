// Site survey — which access points can the ESP32 hear FROM WHERE IT SITS?
//
// The board's own view is the only one that matters: a laptop standing next to
// it reads a different antenna at a different height. Used on 2026-09-18 to
// justify moving from MyWiFi-2.4G to the MyWiFi-2.4G-EXT repeater, which measured
// -26 to -34 dBm here against -60 to -78 dBm for the parent AP.
//
// Only 2.4 GHz networks appear at all — the original ESP32 has no 5 GHz radio —
// so whatever this prints is exactly the set that is actually usable. A 5 GHz
// extender is invisible here, which is the fastest way to find out it is 5 GHz.
//
// Needs no secrets.h and no config.h: it scans and never associates. Flash it,
// read the list, then flash stage10_upload back.
//
//   arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=huge_app \
//                       -u -p /dev/cu.usbserial-0001 tools/wifi_survey
//   arduino-cli monitor -p /dev/cu.usbserial-0001 -c baudrate=115200
#include <WiFi.h>
void setup() {
  Serial.begin(115200);
  delay(800);
  Serial.println(F("\n=== Wi-Fi site survey (ESP32 position) ==="));
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
}
void loop() {
  int n = WiFi.scanNetworks(false, true);   // sync, include hidden
  Serial.printf("\n--- %d networks, strongest first ---\n", n);
  int *idx = new int[n];
  for (int i = 0; i < n; i++) idx[i] = i;
  for (int i = 0; i < n - 1; i++)
    for (int j = i + 1; j < n; j++)
      if (WiFi.RSSI(idx[j]) > WiFi.RSSI(idx[i])) { int t = idx[i]; idx[i] = idx[j]; idx[j] = t; }
  for (int k = 0; k < n; k++) {
    int i = idx[k];
    const char* enc;
    switch (WiFi.encryptionType(i)) {
      case WIFI_AUTH_OPEN: enc = "OPEN"; break;
      case WIFI_AUTH_WEP:  enc = "WEP";  break;
      case WIFI_AUTH_WPA_PSK: enc = "WPA"; break;
      case WIFI_AUTH_WPA2_PSK: enc = "WPA2"; break;
      case WIFI_AUTH_WPA_WPA2_PSK: enc = "WPA/2"; break;
      case WIFI_AUTH_WPA3_PSK: enc = "WPA3"; break;
      default: enc = "?";
    }
    Serial.printf("  %4d dBm  ch%-3d %-7s %s\n",
                  WiFi.RSSI(i), WiFi.channel(i), enc, WiFi.SSID(i).c_str());
  }
  delete[] idx;
  WiFi.scanDelete();
  Serial.println("--- end of scan ---");
  delay(8000);
}
