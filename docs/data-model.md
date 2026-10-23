# Data Model & Provenance

Every field carries a **provenance** tag so analysis never confuses a spec with a
measurement. This is a core project rule.

## Provenance categories

| Tag | Meaning | Example |
|---|---|---|
| `spec` | Manufacturer specification | Panel = 10 W nominal |
| `measured` | Read from the controller via BT-1 | Battery voltage = 12.9 V |
| `calculated` | Derived from measured values | PV power = V×I |
| `estimate` | Modeled/inferred, with assumptions | Remaining runtime |
| `observation` | Free-text experimental note | "cloud cover 14:20" |

> Nominal panel power (10 W) and battery capacity (7 Ah / 12 Ah) are **`spec`**.
> They must **never** be reported as measured experimental results.

## Target fields

Acquired **when exposed** by the BT-1 / Wanderer (confirm availability in
Stage 8). Units and scaling are verified per `protocol-bt1.md`.

| Field | Unit | Provenance | Notes |
|---|---|---|---|
| `pv_voltage` | V | measured | solar input |
| `pv_current` | A | measured | |
| `pv_power` | W | measured | cross-check vs `pv_voltage*pv_current` |
| `battery_voltage` | V | measured | primary decode-validation signal |
| `battery_current` | A | measured | sign = charge/discharge |
| `battery_soc` | % | measured | controller estimate; label as such |
| `load_voltage` | V | measured | if load output used |
| `load_current` | A | measured | |
| `load_power` | W | measured | |
| `energy_daily` | Wh | measured | resets daily |
| `energy_total` | kWh | measured | accumulated |
| `controller_status` | enum/bits | measured | charging state |
| `timestamp` | epoch s | measured | NTP; see `time_synced` |
| `time_synced` | bool | measured | false ⇒ uptime-relative stamp |
| `bt1_link` | enum | measured | connected / reconnecting / lost |
| `esp32_link` | enum | measured | wifi up / down; server ok/fail |
| `rssi` | dBm | measured | ESP32 ↔ Wi-Fi access point, as the ESP32 hears it. `0` = not associated |
| `bt_rssi` | dBm | measured | ESP32 ↔ BT-1, read on each poll's connection. `0` = BT-1 not reached. Sent from the 2026-10-01 firmware on |
| `ctrl_temp` | °C | measured | Wanderer controller temperature, register 0x0103 high byte. Decode per Renogy map, not yet checked against a thermometer. Sent from the 2026-10-07 firmware on |
| `env_temp` | °C | measured | Air temperature from the DHT11 on GPIO23 (±2 °C, 0–50 °C rated). Omitted when the sensor has no fresh reading. Sent from the 2026-10-14 firmware on |
| `env_hum` | % RH | measured | Relative humidity from the same DHT11 (±5 %, 20–90 % rated). Omitted with `env_temp` |
| `config_id` | 1 or 2 | observation | which test configuration |

> `battery_soc` is the **controller's own estimate**, not a coulomb-counted
> ground truth — record it as `measured` (we measured what the controller
> reports) but treat its *accuracy* skeptically in analysis.

## Sample record (JSON, proposed — finalize at Stage 10)

```json
{
  "device_id": "esp32-solar-01",
  "timestamp": 1700000000,
  "time_synced": true,
  "config_id": 1,
  "measured": {
    "pv_voltage": 18.4, "pv_current": 0.42, "pv_power": 7.7,
    "battery_voltage": 12.9, "battery_current": 0.55, "battery_soc": 86,
    "load_power": 0.0,
    "energy_daily": 12, "energy_total": 3.41,
    "controller_status": "mppt_charging"
  },
  "calculated": { "pv_power_vi": 7.73 },
  "links": { "bt1_link": "connected", "esp32_link": "server_ok" }
}
```

## Export for offline analysis

Retain samples in a form directly loadable by Python / MATLAB / Excel:

- **CSV** (one row per sample, provenance encoded in column naming or a companion
  schema) — universal, opens in Excel, `pandas.read_csv`, `readtable` in MATLAB.
- Optionally JSON Lines for richer structure.

The `config_id` column is essential for the Config-1 vs Config-2 comparison.
