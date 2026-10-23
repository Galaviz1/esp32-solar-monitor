# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
Version numbers are pre-1.0 while the firmware is in staged bring-up: the minor
number tracks the build stage recorded in [`docs/stages.md`](docs/stages.md)
(`0.10.x` == Stage 10, the Wi-Fi uploader).

## [0.10.1] - 2026-10-23

### Added

- OLED main page shows the **PV panel current** on the power row, between PV
  power and load power (`PV <W> <A>  LOAD <W>`). Sourced from `pv_a_raw`
  (reg `0x0108`), which is still **UNVERIFIED** — sanity-check amps against
  watts ÷ volts under real sun.

## [0.10.0] - 2026-10-21

Baseline: the **Stage 10 uploader** firmware in `firmware/stage10_upload/`. An
ESP32-WROOM-32 reads a Renogy solar charge controller over Bluetooth LE (via the
BT-1 dongle, NimBLE stack) and a DHT11 for ambient air, renders status on a
128×32 SSD1306 OLED, buffers samples in a 144-slot queue, and POSTs them over TLS
to an ingest endpoint, each upload signed with HMAC-SHA256 and timestamped from
SNTP.

### Configuration

- Deployment values live in `firmware/stage10_upload/config.h` (tracked):
  `WIFI_SSID`, `BT1_MAC`, `INGEST_URL`, `DEVICE_ID`. They ship as
  **placeholders** here — set them for your own system. Secrets
  (`WIFI_PASSWORD`, `DEVICE_HMAC_KEY`) go in `firmware/stage10_upload/secrets.h`,
  which is git-ignored and never committed; copy it from
  [`firmware/common/secrets.example.h`](firmware/common/secrets.example.h).
- A Wi-Fi SSID is a location hint (public wardriving databases map SSIDs to
  approximate coordinates). If you fork this and commit a real SSID, keep the
  repository private, or move `WIFI_SSID` into `secrets.h` first.

[0.10.1]: https://github.com/Galaviz1/esp32-solar-monitor/releases/tag/v0.10.1
