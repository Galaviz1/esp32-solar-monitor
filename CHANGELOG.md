# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
Version numbers are pre-1.0 while the firmware is in staged bring-up: the minor
number tracks the build stage recorded in [`docs/stages.md`](docs/stages.md)
(`0.10.x` == Stage 10, the Wi-Fi uploader).

## [Unreleased]

### Added

- OLED main page shows the **PV panel current** on the power row, between PV
  power and load power (`PV <W> <A>  LOAD <W>`). Sourced from `pv_a_raw`
  (reg `0x0108`), which is still **UNVERIFIED** — sanity-check amps against
  watts ÷ volts under real sun.

## [0.10.0] - 2026-10-21

First tagged release. The baseline is the **Stage 10 uploader** firmware in
`firmware/stage10_upload/`: an ESP32-WROOM-32 reads a Renogy solar charge
controller over Bluetooth LE (via the BT-1 dongle, NimBLE stack) and a DHT11 for
ambient air, renders status on a 128×32 SSD1306 OLED, buffers samples in a
144-slot queue, and POSTs them over TLS to the lab portal
(`https://solar.example.com/api/ingest`), each upload signed with HMAC-SHA256
and timestamped from SNTP. This release cuts a version baseline and records the
production network migration below.

### Changed

- **Wi-Fi network migrated from `MyWiFi-2.4G-EXT` to `MyWiFi-2.4G-EXT`.** The 2.4 GHz
  SSID the ESP32 associates with at boot is defined by `WIFI_SSID` in
  `firmware/stage10_upload/config.h` (tracked, non-secret). The matching WPA2
  passphrase was rotated in the git-ignored `firmware/stage10_upload/secrets.h`
  and is **not** part of this or any commit. No firmware logic changed — this is
  a deployment/configuration change only. (`47e9efe`)

### Verified

- On-hardware acceptance after the migration: association at **RSSI −39 dBm**
  (up from −62 dBm on the previous SSID), SNTP sync, a Renogy BT-1 BLE poll, and
  ingest accepted by the lab portal with **HTTP 200**
  (`{"ok":true,"accepted":1,...}`). Build: `arduino-cli` with
  `esp32:esp32:esp32:PartitionScheme=huge_app`, flashed over
  `/dev/cu.usbserial-0001`.

### Security

- `config.h` is intentionally tracked and must remain so **only while this
  repository is private**. An SSID is a location hint (public wardriving
  databases map SSIDs to approximate coordinates). If the repository is ever made
  public, move `WIFI_SSID` back into `secrets.h` before doing so — see the header
  comment in `config.h`. `secrets.h` (`WIFI_PASSWORD`, `DEVICE_HMAC_KEY`) remains
  git-ignored and is never committed.

