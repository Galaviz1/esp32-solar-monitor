#pragma once
// Deployment configuration for Stage 10 — TRACKED IN GIT ON PURPOSE.
//
// Everything here is non-secret and describes *this* deployment: an SSID is
// broadcast in the clear anyway, the BT-1 MAC is already published in
// docs/hardware.md, and the ingest URL is a public endpoint. Keeping it in git
// means the repo describes the whole deployment rather than most of it.
//
// The two actual credentials live in secrets.h, which is git-ignored.
//
// ⚠️ This reasoning assumes the repository stays PRIVATE (both were, as of
// 2026-09-18). A Wi-Fi SSID is not a password, but it is a location hint —
// public wardriving databases map SSIDs to approximate coordinates. If this
// repo is ever made public, move WIFI_SSID back into secrets.h first.

// --- Wi-Fi network (password is in secrets.h) ---
// Switched from MyWiFi-2.4G to the MyWiFi-2.4G-EXT repeater on 2026-09-18: measured
// -26 to -34 dBm at the board, against -60 to -78 dBm on the parent AP. The
// extra margin is what allows siting the board for the BT-1 instead of
// splitting the difference between the two links.
#define WIFI_SSID       "MyWiFi-2.4G-EXT"

// --- Renogy BT-1 target (Stage 5) ---
#define BT1_MAC         "a1:b2:c3:d4:e5:f6"
#define BT1_NAME_PREFIX "BT-TH-"

// --- Server ingest (token is in secrets.h) ---
#define DEVICE_ID       "esp32-solar-01"
#define INGEST_URL      "https://solar.example.com/api/ingest"

// --- OLED status display (optional) ---
// SSD1306 128x32, I2C, 3.3 V — wires straight to the ESP32 with no level
// shifter, both being 3.3 V parts. Wiring in docs/hardware.md.
//   VCC -> 3V3     SDA -> GPIO21
//   GND -> GND     SCL -> GPIO22
// Set OLED_ENABLED to 0 to build without it. With it on but no panel attached
// the firmware logs "[oled] not found" and carries on — the display must never
// be able to stop the logger.
#define OLED_ENABLED     1
#define OLED_WIDTH       128
#define OLED_HEIGHT      32
#define OLED_ADDR        0x3C   // 0x3D on some modules; the log prints which
#define OLED_SDA_PIN     21
#define OLED_SCL_PIN     22
#define OLED_REFRESH_MS  500    // redraw cadence; fast enough to switch pages on time

// Two pages, shown in turn: the readings, then "ParadoxLab Solar" with the
// controller temperature and the local date/time. Set INFO to 0 for one page.
#define OLED_PAGE_MAIN_MS 4000  // SOC / battery / PV / load / link health
#define OLED_PAGE_INFO_MS 2000  // brand, temperature, date

// Time zone for the date on the display ONLY (POSIX TZ string). Uploads keep
// UTC epoch seconds, which no time zone changes. America/Chihuahua has been
// UTC-6 all year, with no daylight saving, since 2022.
#define DISPLAY_TZ       "CST6"

// --- Air temperature + humidity (optional) ---
// DHT11 module (hiBCTR 10-pack, Amazon B0DTKB16MD): 3 pins S / + / -, with the
// pull-up resistor already on the module. Wiring in docs/hardware.md.
//   +  -> 3V3     (shared with the OLED's VCC: one 3V3 pin on this board)
//   -  -> GND     (row A, pin 7)
//   S  -> GPIO23  (row A, pin 2)
// ⚠️ Power it from 3V3, NOT 5V: the module pulls its data line up to its own
// supply, and 5 V on an ESP32 pin is outside its rating.
// GPIO23 is an ordinary pin: not a strapping, flash or console pin, so the
// sensor can never stop the board booting.
// Set ENV_ENABLED to 0 to build without it. With it on but no sensor attached,
// the firmware logs "[env] no reading" and uploads the solar data as before.
#define ENV_ENABLED       1
#define ENV_DHT_PIN       23
#define ENV_DHT_TYPE      DHT11  // DHT22 / AM2302 also work: change this line only
#define ENV_READ_MS       30000UL   // DHT11 allows one read per second; 30 s is plenty
#define ENV_STALE_MS      120000UL  // older than this is neither uploaded nor shown
#define OLED_PAGE_ENV_MS  2000      // third OLED page: air temperature + humidity

// --- Which physical test setup is running (docs/data-model.md) ---
// Stamped onto every sample as config_id, so Config 1 and Config 2 datasets
// stay separable during analysis. Change this when you change the rig.
#define CONFIG_ID       1
