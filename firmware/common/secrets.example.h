// Copy this file to secrets.h in the sketch folder and fill in real values.
// secrets.h is git-ignored — never commit real credentials.
//
// ⚠️ Stage 10 splits its settings in two:
//
//   config.h   TRACKED   SSID, BT-1 MAC, ingest URL, device id, config id —
//                        non-secret, so the repo describes the deployment
//   secrets.h  IGNORED   the Wi-Fi password and the device token, nothing else
//
// For Stage 10 you therefore only need the two credentials at the bottom; the
// rest already exists in firmware/stage10_upload/config.h. The earlier stages
// (3, 6, 7, 9) predate the split and still read everything from secrets.h, so
// the full set is listed here.
#pragma once

// --- Wi-Fi (Stage 3+) ---
#define WIFI_SSID       "your-ssid"        // Stage 10: lives in config.h
#define WIFI_PASSWORD   "your-password"

// --- Renogy BT-1 target (fill in after Stage 5 identifies it) ---
// MAC address of your BT-1, lower-case, colon-separated, e.g. "a1:b2:c3:d4:e5:f6"
#define BT1_MAC         ""                 // Stage 10: lives in config.h
// Advertised name prefix (commonly "BT-TH-")
#define BT1_NAME_PREFIX "BT-TH-"           // Stage 10: lives in config.h

// --- Server ingest (Stage 10+) ---
#define DEVICE_ID       "esp32-solar-01"   // Stage 10: lives in config.h
// Must be https:// — Stage 10 pins the Let's Encrypt root and will refuse a
// certificate it can't validate.
#define INGEST_URL      "https://solar.example.com/api/ingest"   // config.h
// Signs every upload (HMAC-SHA256). Must equal the DEVICE_HMAC_KEY environment
// variable on the Netlify project; generate one with `openssl rand -hex 32`.
// A mismatch shows up as HTTP 401 "signature mismatch" in the serial log.
// (Replaces the old DEVICE_TOKEN bearer token, 2026-10-11.)
#define DEVICE_HMAC_KEY ""
