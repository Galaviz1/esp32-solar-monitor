# Architecture

## Data flow

```
┌────────────────────┐
│ Renogy Wanderer 10A│  charge controller (measures PV, battery, load)
└─────────┬──────────┘
          │ RJ12 comms port (Modbus RTU over serial, internal to Renogy)
┌─────────┴──────────┐
│    Renogy BT-1     │  bridges the serial comms to BLE
└─────────┬──────────┘
          │ Bluetooth Low Energy (GATT: write request → notify response)
┌─────────┴──────────┐
│       ESP32        │  BLE central + Wi-Fi station = local IoT gateway
│  - poll BT-1       │  builds Modbus read frames, parses responses
│  - timestamp       │  (RTC via NTP over Wi-Fi)
│  - buffer + retry  │  local ring buffer if network is down
└─────────┬──────────┘
          │ Wi-Fi — OUTBOUND ONLY (HTTPS POST or MQTT/TLS publish)
┌─────────┴──────────┐
│   Ingest API /     │  on solar.example.com infrastructure
│   MQTT broker      │  authenticates the device, writes to store
└─────────┬──────────┘
          │
┌─────────┴──────────┐
│  Time-series store │  (e.g. SQLite/CSV → Postgres/InfluxDB later)
└─────────┬──────────┘
          │
┌─────────┴──────────┐
│  Dashboard (auth)  │  solar.example.com/solar
└────────────────────┘
```

## Security model

- **ESP32 makes only outbound connections.** No inbound ports, no listening
  sockets exposed to the Internet.
- **No router port-forwarding** to the ESP32. Ever.
- Transport is **TLS**: HTTPS POST to an ingest endpoint, or MQTT over TLS
  (port 8883) to a broker.
- The ESP32 authenticates to the server with a **device token / client cert**
  (stored in `firmware/common/secrets.h`, git-ignored).
- The **public dashboard is authentication-protected** (login or access token).
  The ingest path and the view path are separate concerns with separate creds.

## Transport choice — HTTPS vs MQTT (decide at Stage 10)

| | HTTPS POST | MQTT/TLS |
|---|---|---|
| Simplicity on ESP32 | Simple (`WiFiClientSecure` + POST) | Needs broker + client lib |
| Push cadence | Poll/interval | Efficient for frequent small msgs |
| Server infra | Any web endpoint | Requires a broker (e.g. Mosquitto) |
| Offline buffering | App-level retry | QoS + local queue |

**Default recommendation:** start with **HTTPS POST** at Stage 10 (fewer moving
parts, matches the existing `example.com` static/serverless hosting). Revisit
MQTT only if update frequency or fleet size justifies a broker.

## Laboratory web portal

`solar.example.com` is the lab portal. Planned sections:

| Path | Purpose |
|---|---|
| `/solar` | **this project** — solar monitoring dashboard |
| `/rf` | RF experiments |
| `/dpd` | digital predistortion |
| `/fpga` | FPGA work |
| `/instruments` | instrument control/status |
| `/data` | datasets |

Only `/solar` is in scope here. Hosting/DNS for `lab.` is a **later** decision
(Stage 10+) and deliberately not designed yet — per the incremental philosophy.

## Local resilience

- ESP32 keeps a small **ring buffer** of samples; if Wi-Fi or the API is down,
  samples are retried, not dropped (bounded by RAM).
- Time comes from **NTP** after Wi-Fi connects; each sample is stamped with an
  epoch time. If NTP is unavailable, samples carry an uptime-relative stamp and a
  `time_synced=false` flag so the server can reconcile.
