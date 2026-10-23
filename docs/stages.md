# 13-Stage Roadmap

Build strictly in order. **Do not start server/cloud stages (10+) until BLE read
(Stage 8) is proven.** Each stage has an acceptance criterion — meet it before
advancing.

| # | Stage | Status | Acceptance criterion |
|---|-------|--------|----------------------|
| 1 | Identify exact ESP32 variant | ✅ **done** | ESP32-D0WD-V3 rev3, 4 MB — via esptool; in `hardware.md` |
| 2 | Install ESP32 toolchain | ✅ **done** | arduino-cli + core 3.3.11; stage1 sketch runs (SDK v5.5.5) |
| 3 | Verify Wi-Fi connectivity | ✅ **done** | Joined MyWiFi-2.4G, IP 192.168.1.50, RSSI ~-67 dBm, stable |
| 4 | BLE scan | ✅ **done** | Found 29 BLE devices (Govee, ELK-BLEDOB, etc.); radio works |
| 5 | Identify the Renogy BT-1 | ✅ **done** | BT-TH-XXXXXXXX, MAC a1:b2:c3:d4:e5:f6, RSSI ~-85 dBm |
| 6 | Determine BT-1 services & characteristics | ✅ **done** | WRITE ffd0/ffd1, NOTIFY fff0/fff1 (matches defaults) |
| 7 | Determine protocol & packet structure | ✅ **done** | addr 0xFF, func 0x03; CRC-valid reply decoded |
| 8 | Read Wanderer measurements | ✅ **done** | 7 fields matched the Renogy app exactly (12.9 V, 88%, …) |
| 9 | Periodic data acquisition | ✅ **done** | `stage9_periodic` — polling + reconnect/backoff + ring buffer |
| 10 | Send measurements over Wi-Fi | ✅ **done** | `HTTP 200 {"ok":true,"accepted":1}` — device → server, TLS + token |
| 11 | Store historical data | ✅ **done** | Real samples persisted, appended, exported as CSV |
| 12 | Web dashboard | ✅ **done** | Metrics + chart + table render from live data |
| 13 | Publish at `solar.example.com/solar` | ✅ **done** | 24.1 h unattended, 146 samples, 100 % capture |

### How Stage 10 was reached (2026-09-17)

Stages 11–13 had been built **ahead of Stage 10** — a departure from the
build-in-order rule, and it hid the real gap. The dashboard was correct but
empty because nothing ever posted: `/api/data` returned
`{"latest":null,"history":[]}` and Stage 9 only buffered samples in RAM.

Three defects had to be cleared before the first sample landed:

1. **No uploader existed.** `stage10_upload` adds Wi-Fi + SNTP + HTTPS POST with
   store-and-forward on top of Stage 9's acquisition loop.
2. **Bluedroid starved mbedTLS.** BLE worked and every upload failed with
   `SSL - Memory allocation failed` (40 KB free heap, 32.7 KB largest block).
   Migrating Stage 10 to NimBLE freed ~78 KB and the handshake succeeded. This
   is the migration `firmware/README.md` had flagged as coming at Stage 9.
3. **The server clobbered its own history.** Netlify Blobs reads are eventually
   consistent by default, so an append could read a stale empty history and
   write it back. Fixed in `lab-portal` with `consistency: "strong"`.

### Stage 13 acceptance met — 2026-09-19

One successful upload was never the criterion; a sustained unattended run was.
Measured over the 24.1 h after the power supply was fixed:

| | |
|---|---|
| Samples | 146 |
| Capture | 100 % (145 intervals, 144 expected) |
| Off-nominal intervals | 1 — a 122 s gap, caused by a deliberate reflash |
| Wi-Fi | −26 to −56 dBm on the `MyWiFi-2.4G-EXT` repeater |
| Battery observed | 12.6–14.6 V, SOC 73–100 % |

That covers a full charge/discharge cycle: bulk charging through the morning,
absorption into the afternoon, PV falling to zero at 19:19, and overnight
discharge to 73 % before sunrise.

**All 13 stages are now complete.** What took the longest was not the firmware
but the power supply — see `firmware/README.md`, "random panics = check power
before you debug code".

### Open question: why Stage 10 first failed to find the BT-1

The first Stage 10 run never found the BT-1 across ten consecutive scans. Two
variables changed before the run that worked — the scan duty cycle was raised to
~100%, and time passed — so the cause was never cleanly isolated. It was
initially written up as the Renogy phone app holding the connection; the phone
was afterwards confirmed **not** connected, so that explanation is out. The
remaining candidate is Wi-Fi/BLE coexistence at the stock scan window.

This is exactly the kind of "it works now, move on" that the design philosophy
warns about. The A/B that settles it: comment out `setInterval`/`setWindow` in
`setup()`, keep Wi-Fi associated, and see whether the BT-1 goes missing again.
Until then it is a hypothesis, recorded as one.

### Known-good baseline

First real end-to-end sample: `battery_v 12.8, soc 84 %, load_w 2, pv_v 0.0`
(night, no PV input), `device_ts` NTP-synced, `config_id 1`.

## Gate between Stage 8 and Stage 9+

This is the hard checkpoint from the design philosophy. **Reliable BLE reads must
be demonstrated** (Stage 8 acceptance met, ideally sustained over hours) before
any cloud/web effort begins. If BLE is flaky, fix it at the ESP32/BT-1 layer
first — do not paper over it with server-side logic.

## Notes per stage

- **Stage 2** — board manager URL and settings are in `firmware/README.md`.
- **Stage 4/5** — BT-1 usually advertises as `BT-TH-...`. Record its MAC; you'll
  pin the connection to that MAC in later stages (put it in `secrets.h`).
- **Stage 6** — enumerate *all* services/characteristics; don't assume the
  community UUIDs match your unit.
- **Stage 7–8** — validate decoding against physical measurements before trusting
  any register (see `protocol-bt1.md` → "Verification protocol").
- **Stage 9** — add reconnect/backoff and a bounded RAM ring buffer.
- **Stage 10** — HTTPS POST (chosen over MQTT; see `architecture.md`). The sketch
  pins the Let's Encrypt root rather than calling `setInsecure()`, so the device
  token can't be handed to an impostor. **Requires the "Huge APP" partition
  scheme** — BLE + Wi-Fi + TLS overflows the default one (`firmware/README.md`).
- **Stages 11–13** — live in the separate `lab-portal` repo (Netlify Functions +
  Blobs), not in `server/`, which stays empty. Endpoints: `POST /api/ingest`,
  `GET /api/data`, `GET /api/export.csv`.
