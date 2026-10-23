# server/ — DEFERRED (do not build yet)

Per the project's core design philosophy, **no web/cloud infrastructure is built
until reliable BLE communication with the Renogy BT-1 is proven** (Stage 8, ideally
sustained through Stage 9).

This directory is an intentional placeholder. It stays empty until we reach:

- **Stage 10** — ingest API (default: HTTPS POST endpoint; auth via device token)
- **Stage 11** — historical storage (start simple: SQLite/CSV → time-series DB later)
- **Stage 12** — dashboard (live + historical)
- **Stage 13** — publish at `solar.example.com/solar` (authentication-protected)

Security constraints (from `../docs/architecture.md`):

- ESP32 connects **outbound only**; never exposed to the public Internet.
- **No** router port-forwarding to the ESP32.
- TLS everywhere; the public dashboard is auth-protected.

When we get here, decide HTTPS vs MQTT using the trade-off table in
`../docs/architecture.md`.
