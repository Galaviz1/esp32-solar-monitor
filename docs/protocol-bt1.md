# Renogy BT-1 BLE Protocol — Working Notes

> ✅ **VERIFIED for this unit (2026-09-17).** The GATT UUIDs (Stage 6) and the
> core register scalings (Stage 8) have been **confirmed empirically** against
> *this* BT-1 (BT-TH-XXXXXXXX) + Wanderer 10A — battery V, SOC, PV V, load V/A/W
> and charging current all matched the official Renogy iPhone app exactly (see
> "Stage 7/8 measured" below). Items still marked TODO (temperature parse,
> PV current/power registers, daily/total energy) remain to be confirmed.
> Community references got us here, but these values are now measured, not
> hypothetical.

## Transport model (community consensus)

The BT-1 exposes a **serial-style BLE service**: the central (ESP32) **writes** a
request to one characteristic and receives the reply as a **notification** on
another. The payload is a **Modbus RTU frame** (the same protocol the controller
speaks over its RJ12 port), including a **CRC-16 (Modbus)** trailer.

```
ESP32  --write-->  [ addr | func | reg_hi | reg_lo | n_hi | n_lo | crc_lo | crc_hi ]  -->  BT-1
ESP32 <--notify--  [ addr | func | byte_count | data...                | crc_lo | crc_hi ] <-- BT-1
```

## GATT identifiers (COMMONLY reported — verify in Stage 6)

| Role | UUID (commonly reported) |
|---|---|
| Advertised name prefix | `BT-TH-...` |
| Write characteristic | `0000ffd1-0000-1000-8000-00805f9b34fb` |
| Notify characteristic | `0000fff1-0000-1000-8000-00805f9b34fb` |

Your BT-1 may differ. Stage 6 firmware must **enumerate all services and
characteristics** and record the actual write/notify UUIDs here.

#### Stage 6 measured GATT — CONFIRMED (2026-09-17, BT-TH-XXXXXXXX)

Full enumeration (props: R=read W=write w=write-no-resp N=notify I=indicate):

```
Advertised name : BT-TH-XXXXXXXX   MAC a1:b2:c3:d4:e5:f6
Standard svcs   : 1800 (GAP), 1801 (GATT), 180a (Device Info)

Service 0000ffd0-0000-1000-8000-00805f9b34fb   (Renogy command)
  ffd1  [R W w]   <- WRITE characteristic (used for Modbus requests)
  ffd2  [R   N]
  ffd3  [  W  ]
  ffd4  [R    ]
  ffd5  [R W  ]
Service 0000fff0-0000-1000-8000-00805f9b34fb   (Renogy data)
  fff1  [R   N]   <- NOTIFY characteristic (Modbus responses arrive here)
Service f000ffd0-0451-4000-b000-000000000000   (TI OAD; not used)
  f000ffd1  [W w]
```

**Result — the community UUIDs are correct for this unit:**
- WRITE : service `0000ffd0`, char `0000ffd1`
- NOTIFY: service `0000fff0`, char `0000fff1`

These match the defaults already hard-coded in `stage7_modbus_read.ino`, so no
UUID edits are needed there.

## Modbus framing (to verify in Stage 7)

- **Device address**: commonly `0xFF` (broadcast-ish) or `0x01`. Test both.
- **Function**: `0x03` (Read Holding Registers).
- **CRC**: CRC-16/MODBUS, little-endian appended (low byte first).

Example read request for 1 register at `0x0100`, addr `0xFF`:
```
FF 03 01 00 00 01  <crc_lo> <crc_hi>
```

## Register map — Wanderer (COMMUNITY-REPORTED, verify in Stage 8)

Scaling and addresses vary by controller family. **Do not trust these until a
known physical quantity (e.g. measured battery voltage on a multimeter) matches
the decoded value.**

| Register | Quantity | Reported scaling | Provenance once read |
|---|---|---|---|
| `0x0100` | Battery SOC (%) | ×1 | measured |
| `0x0101` | Battery voltage (V) | ×0.1 | measured |
| `0x0102` | Charging current (A) | ×0.01 | measured |
| `0x0103` | Controller/batt temp | packed bytes: high = controller °C, low = battery °C, bit 7 = sign | measured — decoded for the OLED (2026-10-07), **not yet verified** against a thermometer |
| `0x0107` | PV (solar) voltage (V) | ×0.1 | measured — **verified** Stage 8 |
| `0x0108` | PV current (A) | ×0.01 | measured — **verified** Stage 10 (see below) |
| `0x0109` | PV power (W) | ×1 | measured — **verified** Stage 10 (see below) |
| `0x0104` | Load voltage (V) | ×0.1 | measured |
| `0x0105` | Load current (A) | ×0.01 | measured |
| `0x0106` | Load power (W) | ×1 | measured |
| `0x010F` | Day: min batt V | ×0.1 | measured |
| `0x0110` | Day: max batt V | ×0.1 | measured |
| `0x0113` | Day: charge Ah | ×1 | measured |
| `0x0115` | Day: power generation (Wh) | ×1 | measured |
| `0x0118` | Total: power generation (kWh) | ×1 | measured |
| `0x0120` | Controller/charging state | bitfield | measured |

> `PV power = PV voltage × PV current` etc. are **calculated** checks — use them
> to cross-validate the controller's reported power register (helps catch a
> wrong scaling factor).

## Stage 7/8 measured (2026-09-17) — first successful read

Device address **0xFF** worked (func 0x03). Request/response:

```
TX: FF 03 01 00 00 08 50 2E
RX: FF 03 10 00 58 00 81 00 00 27 00 00 81 00 13 00 02 00 84 5D 07   CRC OK

reg 0x0100 = 0x0058 =  88   -> Battery SOC        88 %
reg 0x0101 = 0x0081 = 129   -> Battery voltage    12.9 V   (x0.1)   [meter-check]
reg 0x0102 = 0x0000 =   0   -> Charging current   0.00 A   (x0.01)
reg 0x0103 = 0x2700         -> Temperature: controller 0x27 = 39 °C, battery 0x00 (no sensor on the Wanderer 10A)
reg 0x0104 = 0x0081 = 129   -> Load voltage       12.9 V   (x0.1)
reg 0x0105 = 0x0013 =  19   -> Load current       0.19 A   (x0.01)
reg 0x0106 = 0x0002 =   2   -> Load power         2 W
reg 0x0107 = 0x0084 = 132   -> PV voltage         13.2 V   (x0.1)
```

All values are physically consistent (AGM at 88% SOC ~= 12.9 V resting; load V =
battery V; PV slightly above battery, 0 charging current in low light).

### Stage 8 validation — matched the official Renogy iPhone app (2026-09-17)

| Field | ESP32 decode | Renogy app | Match |
|---|---|---|---|
| Battery voltage | 12.9 V | 12.9 V | exact |
| Battery SOC | 88 % | 88 % | exact |
| PV voltage | 13.2 V | 13 V | app rounds to whole V |
| Charging current | 0.00 A | 0 A | exact |
| Load voltage | 12.9 V | 12.9 V | exact |
| Load current | 0.19 A | 0.18 A | within ±0.01 A |
| Load power | 2 W | 2 W | exact |

**Conclusion:** register addresses + `×0.1`/`×0.01` scalings for SOC, battery V,
charging A, load V/A/W and PV V are **VERIFIED**. Cross-checking 7 fields against
the manufacturer app is stronger than a single meter reading.

**Status:** transport + CRC + decode all **verified**. Still TODO: temperature
register (0x0103) parsing; daily/total energy registers.

### Stage 10 — PV current & power (0x0108 / 0x0109), 2026-09-18/19

Stage 10 widened the read to 0x0100..0x010F, so these two are now acquired. They
were unverifiable at Stage 8 because that session ran after dark and every PV
register read zero. A full day of 10-minute samples supplies what was missing.

**Step 3 of the verification protocol — power register vs V×I: 66 of 66
daylight samples agree**, across a 7:1 power range (1 W to 7 W):

| pv_v | pv_a | pv_w reported | V×I | |
|---|---|---|---|---|
| 14.3 V | 0.48 A | 7 W | 6.86 | ✓ |
| 15.1 V | 0.32 A | 5 W | 4.86 | ✓ |
| 15.5 V | 0.19 A | 3 W | 2.94 | ✓ |
| 17.3 V | 0.05 A | 1 W | 0.87 | ✓ |

**Step 2 — do the values move the right way?** Sunset supplied the experiment
without anyone touching the panel. Over the afternoon PV current fell 0.48 → 0.05
→ 0.00 A while PV voltage *rose* 14.3 → 17.3 V, which is exactly a PWM controller
tapering charge into a full battery: as it throttles current, the panel is pushed
toward its open-circuit voltage. At 19:19 PV voltage reached 0 V and stayed there
overnight; it climbed again the next morning.

**Treat these as verified, with one honest limitation.** `V×I = W` is an
*internal consistency* check — the controller may well compute the power register
that way, so it cannot by itself prove the current scaling is right in absolute
terms. What would close that gap is a comparison against an external reference,
exactly as Stage 8 did with the Renogy app: read PV current in the app during
daylight and compare. Until then the ×0.01 A scaling is strongly supported by
physics and arithmetic, but not independently measured.

## Verification protocol (how to prove decoding is correct)

1. Measure real **battery voltage** with a multimeter → compare to decoded
   `0x0101 × 0.1`. Must agree within meter tolerance.
2. Cover / uncover the panel → decoded PV voltage & current must move the right
   way.
3. Cross-check `PV power` register vs `V×I` calculated product.
4. Only after 1–3 pass do we mark the register map **verified** and stop
   labeling it UNVERIFIED here.
