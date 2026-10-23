# Firmware — ESP32 (Arduino IDE, C/C++)

Primary development path: **ESP32 + Arduino IDE + C/C++**, using the ESP32 BLE
APIs to investigate the Renogy BT-1. Do **not** assume Raspberry-Pi Python BLE
libraries port here.

## Stage 2 — Install ESP32 board support

1. **Arduino IDE** → *Settings* → **Additional boards manager URLs**, add:
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
2. *Tools* → *Board* → **Boards Manager** → search **esp32** (by Espressif) →
   Install.
3. Select board **"ESP32 Dev Module"** (adjust after Stage 1 confirms the exact
   variant).
4. Recommended settings:
   - Upload Speed: **921600** (drop to 115200 if uploads fail)
   - Flash Size: **4MB (32Mb)**
   - Partition Scheme: **Default 4MB with spiffs** for now; switch to
     **"Minimal SPIFFS (1.9MB APP)"** or **"Huge APP"** later when BLE + Wi-Fi +
     TLS are combined (they need more app flash).
5. Serial Monitor baud: **115200**.

> **Acceptance (Stage 2):** a blank sketch compiles and uploads, and the Serial
> Monitor shows the ESP32 boot log.

## Configuration and secrets

Stage 10 splits these in two, because they are not the same thing:

| File | In git? | Holds |
|---|---|---|
| `stage10_upload/config.h` | **yes** | SSID, BT-1 MAC, ingest URL, `DEVICE_ID`, `CONFIG_ID` |
| `stage10_upload/secrets.h` | **no** | `WIFI_PASSWORD` and `DEVICE_TOKEN` — nothing else |

Nothing in `config.h` is a secret: an SSID is broadcast in the clear, the BT-1
MAC is already published in `../docs/hardware.md`, and the ingest URL is a public
endpoint. Tracking it means **the repo describes the whole deployment** — which
network, which controller, which test configuration — instead of most of it, and
it leaves `secrets.h` holding exactly two credentials, which is easy to reason
about.

To build Stage 10 on a fresh clone you therefore only supply the two credentials:

```bash
cd firmware/stage10_upload
cp ../common/secrets.example.h secrets.h
# delete everything except WIFI_PASSWORD and DEVICE_TOKEN, then fill those in
```

Earlier stages (3, 6, 7, 9) predate the split and still read everything from
their own `secrets.h`. Because Arduino compiles per-sketch-folder, each sketch
needs its own copy.

## Sketches by stage

| Folder | Stage | Purpose |
|---|---|---|
| `stage1_identify/` | 1 | Print chip model, revision, flash size, BT/BLE features |
| `stage3_wifi_test/` | 3 | Connect to Wi-Fi, print IP + RSSI |
| `stage4_ble_scan/` | 4 | Scan BLE, flag likely `BT-TH-` Renogy BT-1 |
| `stage6_gatt_enum/` | 6 | Connect to BT-1, enumerate services/characteristics |
| `stage7_modbus_read/` | 7–8 | Modbus read (CRC-checked) + decode + validation gate |
| `stage9_periodic/` | 9 | Persistent link, reconnect/backoff, RAM ring buffer |
| `stage10_upload/` | 10 | Stage 9 + Wi-Fi + NTP + HTTPS POST to the lab portal |
| `tools/wifi_survey/` | — | Site survey: which 2.4 GHz APs the board can hear, and how strongly |
| `tools/i2c_scan/` | — | Which I²C devices answer, and at what address (blank-OLED triage) |
| `tools/oled_test_pico/` | — | Tests the OLED panel on a Raspberry Pi Pico, away from the ESP32 |

> **Stage 5** ("identify the BT-1") needs no dedicated sketch: run the Stage 4
> scan, spot the `BT-TH-` device, and record its MAC into `stage10_upload/config.h`
> (`BT1_MAC`) and `../docs/protocol-bt1.md`.

`tools/` holds diagnostics rather than staged milestones. `wifi_survey` needs no
credentials at all — it scans and never associates — and answers the question
that matters when siting the board: what the *board* hears, not what a laptop
standing next to it hears. It only ever shows 2.4 GHz networks, since the
original ESP32 has no 5 GHz radio, which is also the quickest way to discover
that an extender you were counting on is 5 GHz-only.

Later stages (10–13) are added only as each prior stage's acceptance criterion is
met — see [`../docs/stages.md`](../docs/stages.md).

### Order of operations for the BT-1 sketches

1. **Stage 6** — run `stage6_gatt_enum`. Copy the real WRITE/NOTIFY service +
   characteristic UUIDs into both `../docs/protocol-bt1.md` and the `#define`s at
   the top of `stage7_modbus_read.ino`.
2. **Stage 7/8** — run `stage7_modbus_read`. If no reply, flip
   `MB_DEVICE_ADDR` from `0xFF` to `0x01`. **Do not** trust decoded values until
   battery voltage matches a multimeter (Stage 8 gate).

## Stage 10 — NimBLE and the partition scheme are not optional

Stage 10 is where Wi-Fi and TLS join BLE, and the two footprint problems the
"BLE library note" below anticipated both came due at once.

### 1. RAM — this is why Stage 10 is on NimBLE

Measured on this board, with Wi-Fi associated and a BLE link to the BT-1 up:

| BLE stack | Free heap | Largest block | mbedTLS handshake |
|---|---|---|---|
| Bluedroid (stock, Stages 4–9) | 40 KB | 32.7 KB | ✗ `tls=-32512 (SSL - Memory allocation failed)` |
| **NimBLE 2.x** | **118 KB** | **111 KB** | ✓ HTTP 200 |

The handshake wants ~32 KB of buffers on top of whatever else is live. Under
Bluedroid, BLE worked perfectly and *every* upload failed — a failure that
surfaces as `transport error -1 (connection refused)` and looks like a network
or certificate problem until you print the mbedTLS reason. Install the library:

```bash
arduino-cli lib install "NimBLE-Arduino"      # 2.5.1 at time of writing
```

Stages 4–9 deliberately stay on Bluedroid; only `stage10_upload` is ported. The
port is confined to the BLE section of the sketch — Modbus framing and register
decoding are byte-for-byte unchanged, so the Stage 8 verification still holds.

### 2. Flash — partition scheme

| Partition scheme | App space | `stage10_upload` (NimBLE) |
|---|---|---|
| Default 4MB with spiffs | 1.25 MB | ⚠️ 1.28 MB — fits with ~25 KB to spare |
| **Huge APP (3MB No OTA/1MB SPIFFS)** | 3.00 MB | ✓ 1.28 MB (40 %) |
| Minimal SPIFFS (1.9MB APP with OTA) | 1.90 MB | ✓ also fits |

Use **Huge APP** — the default leaves no headroom (the Bluedroid build was
1.78 MB and overflowed it outright). In the Arduino IDE: *Tools → Partition
Scheme → "Huge APP"*. Headless:

```bash
arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=huge_app stage10_upload
arduino-cli upload  --fqbn esp32:esp32:esp32:PartitionScheme=huge_app \
                    -p /dev/cu.usbserial-0001 stage10_upload
arduino-cli monitor -p /dev/cu.usbserial-0001 -c baudrate=115200
```

If you forget the flag the link fails with *"region `iram0_0_seg' overflowed"* or
a "text section exceeds available space" error — that is this, not a code bug.

### What to expect in the serial log

Actual first run (2026-09-17):

```
=== Upload over Wi-Fi (Stage 10) ===
[wifi] up  IP=192.168.1.50  RSSI=-62 dBm
[ntp] synced, epoch=1789699684
[mem] free heap after init: 117700 B (largest block 110580 B)
[ble] scanning for BT-1... [ble] found BT-1 at RSSI -79 dBm
[ble] connected + subscribed
[poll] batt=12.8V soc=84% chg=0.00A pv=0.0V/0.00A/0W load=2W (queued 1/144)
[up] HTTP 200  {"ok":true,"accepted":1,"stored":2}
```

Reading the failure modes:

| Log line | Meaning |
|---|---|
| `[up] 401` | `DEVICE_TOKEN` in `secrets.h` ≠ the Netlify env var |
| `[up] tls=-32512 (SSL - Memory allocation failed)` | not enough heap — you're on Bluedroid, not NimBLE |
| `no BT-1 (N other advertisements seen...)` | the radio works; this specific device wasn't heard — see "not finding the BT-1" below |
| `no BT-1 (0 other advertisements...)` | the radio saw nothing at all — a real BLE fault |
| `[mem] free heap ... (largest block < 35000)` | the next upload will fail; investigate before it does |

Samples are never dropped on an upload failure — they stay queued (144 slots,
≈24 h at the 10-minute cadence) and go out in the next batch.

### Random panics = check power before you debug code

Symptoms seen on 2026-09-17 after moving the board onto a bus-powered USB hub:

```
[wifi] connecting to 'MyWiFi-2.4G'Guru Meditation Error: Core 0 panic'ed (IllegalInstruction)
[wifi] connecting to 'MyWiFi-2.4G'Guru Meditation Error: Core 0 panic'ed (InstructionFetchError)
[wifi] connecting to 'MyWiFi-2.4G'Guru Meditation Error: Core 0 panic'ed (LoadProhibited)
[wifi] connecting to 'MyWiFi-2.4G'E BOD: Brownout detector was triggered
```

**A different exception type each time, always at the same moment, is not a code
bug — it is the supply.** `WiFi.begin()` is where the radio first transmits and
current spikes; a sagging rail corrupts RAM and whatever executes next dies in a
new way. One of those corrupted runs produced a null-handler crash deep inside
`wifi_sta_receive()` whose backtrace read exactly like a genuine netif race, and
cost an hour chasing a bug that was not there.

Rules that follow:

- Power the board from a **direct port on the machine, a powered hub, or a 5 V
  wall charger**. A bus-powered hub advertising `Current Available: 500 mA,
  Extra Operating Current: 0 mA` is not enough headroom for Wi-Fi + BLE.
- `rst:0xc (SW_CPU_RESET)` means a panic, `E BOD:` means brownout — but a brownout
  that corrupts memory *first* shows up as a panic, so the absence of `E BOD:`
  does not clear the supply.
- Weak Wi-Fi makes it worse: the ESP32 transmits harder to reach a distant AP,
  so the same hub that coped at -55 dBm can brown out at -78 dBm.
- Check the budget on macOS with:
  `system_profiler SPUSBDataType | grep -iE "hub|current|CP2102"`

#### Before blaming power: is anything wired to the flash pins?

Seen 2026-10-05 on the 38-pin USB-C board: an OLED whose GND and VCC wires sat
on **SD0 and CLK** (the two pins beside the USB) produced the identical
signature — `TG0WDT_SYS_RESET` boot loops, garbage `load:` lengths, `csum err`,
and resets the moment Wi-Fi started — even on a supply that is fine once the
wiring is right. On that board the pins nearest the USB are the SPI flash
(SD0–SD3, CMD, CLK) and 5V. **Check that nothing is on them before chasing the
supply.** Pinout: `docs/hardware.md`.

#### The boot loop: `TG0WDT_SYS_RESET` is under-voltage, NOT corrupted flash

```
rst:0x7 (TG0WDT_SYS_RESET),boot:0x13 (SPI_FAST_FLASH_BOOT)
   ... 123 times in 40 seconds, never reaching setup()
```

The bootloader hangs while mapping the app image out of flash and Timer Group 0's
watchdog resets the chip, forever. It looks exactly like a corrupted image.

**It is not.** Proven on 2026-09-18:

- `erase-flash` + reflash, every block verified on write → boot-looping again
  within *minutes*.
- In the middle of the loop, `esptool flash-id` read the chip perfectly
  (`ESP32-D0WD-V3 revision v3.1`, MAC intact). In esptool mode the radios are
  off and the current draw is minimal.
- Moving the board to a **5 V wall charger fixed it with no reflash at all** —
  it booted the existing image and resumed logging immediately.

So the flash contents were fine the whole time; the rail was too low for the
bootloader to *read* them reliably. The chip is flawless at low current and dies
as soon as the radios come up.

> An earlier commit here claimed this state meant a corrupted image needing
> erase-and-reflash. That was wrong, and it cost two unnecessary erase cycles —
> each of which writes flash on a supply that can sag mid-write, which is the one
> thing that could turn a power problem into real damage. **On a `TG0WDT` boot
> loop, fix the supply first and only reflash if good power doesn't fix it.**

### Siting: the two links pull in opposite directions

Measured at two positions in the same room:

| Position | Wi-Fi (AP) | BLE (BT-1) | Result |
|---|---|---|---|
| Near the router | −54 to −64 dBm | −79 dBm | stable for >1 h |
| Moved toward the controller | −71 to −78 dBm | −66 to −70 dBm | brownouts on a hub |

Wi-Fi had margin to spare at −79 dBm BLE; the BT-1 is the link that was short.
But gaining BLE margin by losing Wi-Fi margin raises transmit power and current
draw, which is what tipped the hub over. Site for the BT-1, then give the board
a supply that can afford the Wi-Fi.

### Not finding the BT-1

Two causes, in the order they've actually bitten:

1. **Scan duty cycle too low while Wi-Fi is associated.** The BT-1 is a weak
   advertiser here (−75 to −85 dBm), and with Wi-Fi up the radio time-shares.
   At the stock scan window, Stage 10 missed it on ten consecutive 10-second
   scans; with `setInterval(100)` / `setWindow(99)` it is found on the first
   scan, every time. Stages 4–9 never hit this because Wi-Fi wasn't running.
2. **Another central holds it.** The BT-1 accepts one connection at a time and
   stops advertising while connected, so a phone running the Renogy app locks
   the ESP32 out (and vice versa). Force-quit the app — backgrounding it is not
   enough, iOS keeps BLE links alive.

> **Attribution note (2026-09-17).** The original Stage 10 failure was first
> written up here as cause 2. That was not established. The diagnostic used to
> "rule out" coexistence scanned with Wi-Fi off and then on, but used the high
> duty cycle in *both* passes — so it never re-tested the combination that
> actually failed (stock duty + Wi-Fi associated). The phone was later confirmed
> not to have been connected at all, which leaves cause 1 as the explanation.
> To settle it properly: build with the `setInterval`/`setWindow` lines in
> `setup()` commented out, leave Wi-Fi up, and see whether the BT-1 goes missing
> again. Until someone runs that, cause 1 is the leading explanation, not a
> verified one.

## BLE library note

- **Stage 4 scan** uses the stock `BLEDevice`/`BLEScan` from the ESP32 core — no
  extra install, easiest for a first scan.
- **Stages 5–7 stay on stock BLE** — deliberately. Introducing a new BLE library
  at the exact moment of first BT-1 connection would add a second variable to
  debug. One change at a time.
- **NimBLE migration happened at Stage 10** (not Stage 9 as originally planned) —
  the moment Wi-Fi + TLS were linked in and Bluedroid's RAM footprint actually
  bit. It was not optional: see the table above. The API changes were mechanical,
  as expected — `NimBLEScanCallbacks::onResult(const NimBLEAdvertisedDevice*)`
  replaces `BLEAdvertisedDeviceCallbacks::onResult(BLEAdvertisedDevice)`,
  `subscribe()` replaces `registerForNotify()`, `onDisconnect()` takes a reason
  code, and you connect by `NimBLEAddress` rather than holding a pointer to a
  scan result that the scanner owns and frees.
