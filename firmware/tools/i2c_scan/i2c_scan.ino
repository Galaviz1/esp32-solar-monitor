// I²C bus scanner — is the display actually wired and talking?
//
// Answers, in order, the three questions a blank OLED raises:
//
//   1. is anything on the bus at all?      -> "found N device(s)"
//   2. at which address?                   -> 0x3C or 0x3D for an SSD1306
//   3. is it stable, or intermittent?      -> rescans every 3 s, so a loose
//                                             DuPont wire shows up as a device
//                                             that comes and goes
//
// A blank screen has several causes that look identical from the outside —
// miswired, wrong address, dead panel, or firmware never initialising it. This
// separates them before anyone edits code: if the scan finds 0x3C, the wiring
// and the panel are both fine and the problem is in software. If it finds
// nothing, no amount of firmware will help.
//
// Needs no secrets.h and no config.h. Flash it, read the serial log, then flash
// stage10_upload back:
//
//   arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=huge_app \
//                       -u -p /dev/cu.usbserial-0001 tools/i2c_scan
//   arduino-cli monitor -p /dev/cu.usbserial-0001 -c baudrate=115200

#include <Wire.h>

#define SDA_PIN 21
#define SCL_PIN 22

static void describe(uint8_t addr) {
  switch (addr) {
    case 0x3C:
    case 0x3D: Serial.print(F("  <- SSD1306 / SH1106 OLED")); break;
    case 0x27:
    case 0x3F: Serial.print(F("  <- PCF8574 LCD backpack")); break;
    case 0x68: Serial.print(F("  <- DS3231 RTC / MPU-6050")); break;
    case 0x76:
    case 0x77: Serial.print(F("  <- BMP/BME280")); break;
    case 0x40: Serial.print(F("  <- INA219 / SHT21")); break;
    default: break;
  }
}

// Electrical state of the two lines BEFORE any I²C traffic.
//
// An idle I²C bus is pulled high by resistors — on these modules they are on the
// panel itself. So the idle levels say something the address scan cannot:
//
//   HIGH / HIGH  bus is healthy. Pull-ups present, nothing shorting a line.
//                If the scan then finds nothing, the panel is powered but mute,
//                or the two data lines are on the wrong pins.
//   LOW  on SDA  that line is tied to GND — the classic symptom of wiring the
//   or   on SCL  panel by position when its order is SDA-SCL-VCC-GND.
//
// The ESP32's internal pull-ups are weak (~45 kΩ) but easily enough to read a
// floating line as HIGH, so a LOW here means something is actively holding it
// down, not merely that the panel is absent.
static void probeLines() {
  pinMode(SDA_PIN, INPUT_PULLUP);
  pinMode(SCL_PIN, INPUT_PULLUP);
  delay(5);
  int sda = digitalRead(SDA_PIN), scl = digitalRead(SCL_PIN);
  Serial.printf("[lines] SDA(GPIO%d)=%s  SCL(GPIO%d)=%s\n",
                SDA_PIN, sda ? "HIGH" : "LOW", SCL_PIN, scl ? "HIGH" : "LOW");
  if (sda && scl) {
    Serial.println(F("        both idle high -> no short."));

    // Is a POWERED panel actually attached to these two pins?
    //
    // Internal pull-ups make the lines read HIGH whether or not anything is
    // connected, so the test above cannot tell "panel present" from "nothing
    // there". Switching to an internal PULL-DOWN settles it:
    //
    //   the panel carries its own pull-ups to its VCC, typically 4.7 kΩ. Against
    //   the ESP32's ~45 kΩ pull-down that divider sits near 3.0 V -> still HIGH.
    //   With no panel (or an unpowered one) the pull-down wins -> LOW.
    //
    // So HIGH here means a powered panel really is on GPIO21/22; LOW means the
    // data lines reach nothing that has power.
    pinMode(SDA_PIN, INPUT_PULLDOWN);
    pinMode(SCL_PIN, INPUT_PULLDOWN);
    delay(10);
    int sdaX = digitalRead(SDA_PIN), sclX = digitalRead(SCL_PIN);
    Serial.printf("[pullup] with internal pull-DOWN: SDA=%s  SCL=%s\n",
                  sdaX ? "HIGH" : "LOW", sclX ? "HIGH" : "LOW");
    if (sdaX && sclX) {
      Serial.println(F("         -> external pull-ups detected: a POWERED panel IS"));
      Serial.println(F("            attached to GPIO21/22. Wiring is good; suspect the"));
      Serial.println(F("            panel's address or the panel itself."));
    } else {
      Serial.println(F("         -> NO external pull-ups. The data pins reach nothing"));
      Serial.println(F("            that has power. Check in this order:"));
      Serial.println(F("              a) is VCC on the panel really at 3.3 V?"));
      Serial.println(F("              b) is GND on the panel connected?"));
      Serial.println(F("              c) are SDA/SCL on GPIO21/22 and fully seated?"));
    }
    // leave them back on pull-up for the scan
    pinMode(SDA_PIN, INPUT_PULLUP);
    pinMode(SCL_PIN, INPUT_PULLUP);
  } else {
    Serial.println(F("        *** a line is held LOW — that is a wiring fault ***"));
    if (!sda) Serial.println(F("        SDA is shorted to GND or on the wrong pin"));
    if (!scl) Serial.println(F("        SCL is shorted to GND or on the wrong pin"));
  }
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println(F("\n=== I2C bus scan ==="));
  Serial.printf("SDA=GPIO%d  SCL=GPIO%d  (ESP32 core defaults)\n", SDA_PIN, SCL_PIN);
  Serial.println(F("An SSD1306 should answer at 0x3C, or 0x3D on some modules.\n"));
  probeLines();
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(100000);            // 100 kHz — forgiving of long jumper wires
}

// Last resort: find the panel on ANY usable pin pair.
//
// If the configured pins come up empty, the wires may simply be on different
// GPIOs. Rather than ask someone to re-examine the same two pins again, sweep
// every safe candidate pair and probe only the two SSD1306 addresses.
//
// Excluded and why: GPIO6-11 are the SPI flash (using them crashes the chip),
// GPIO34-39 are input-only so they cannot drive SDA, GPIO1/3 are the serial
// console this report comes out on, and GPIO0/2/12/15 are strapping pins.
static const uint8_t CAND[] = {4, 5, 13, 14, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33};

static bool sweepPins() {
  Serial.println(F("\n[sweep] configured pins found nothing — trying every safe pair"));
  bool hit = false;
  for (uint8_t i = 0; i < sizeof(CAND); i++) {
    for (uint8_t j = 0; j < sizeof(CAND); j++) {
      if (i == j) continue;
      uint8_t sda = CAND[i], scl = CAND[j];
      Wire.end();
      if (!Wire.begin(sda, scl)) continue;
      Wire.setClock(100000);
      for (uint8_t addr = 0x3C; addr <= 0x3D; addr++) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
          Serial.printf("[sweep] *** FOUND 0x%02X with SDA=GPIO%u  SCL=GPIO%u ***\n",
                        addr, sda, scl);
          hit = true;
        }
      }
    }
  }
  if (!hit) {
    Serial.println(F("[sweep] nothing on any pin pair."));
    Serial.println(F("[sweep] The panel is not electrically connected to this board."));
    Serial.println(F("[sweep] Most likely, in order:"));
    Serial.println(F("[sweep]   1. the panel's 4-pin header is NOT SOLDERED — many"));
    Serial.println(F("[sweep]      modules ship with a loose strip that only makes"));
    Serial.println(F("[sweep]      contact once soldered"));
    Serial.println(F("[sweep]   2. a broken jumper wire (check continuity end to end)"));
    Serial.println(F("[sweep]   3. no 3.3 V reaching the panel's VCC pin"));
  }
  Wire.end();
  Wire.begin(SDA_PIN, SCL_PIN);     // restore the configured bus
  Wire.setClock(100000);
  return hit;
}

void loop() {
  static uint32_t pass = 0;
  static bool swept = false;
  uint8_t found = 0;

  Serial.printf("--- pass %lu ---\n", (unsigned long)++pass);
  // 0x00-0x07 and 0x78-0x7F are reserved, so the usable range is 0x08..0x77.
  for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
    Wire.beginTransmission(addr);
    uint8_t err = Wire.endTransmission();
    if (err == 0) {
      found++;
      Serial.printf("  device at 0x%02X", addr);
      describe(addr);
      Serial.println();
    } else if (err == 4) {
      Serial.printf("  bus error at 0x%02X\n", addr);
    }
  }

  if (found == 0) {
    Serial.println(F("  no devices found."));
    if (!swept) {                     // advice and sweep once per boot, not per pass
      Serial.println(F("  Check, in this order:"));
      Serial.println(F("    1. VCC on the panel goes to 3V3 (NOT VIN, NOT a GPIO)"));
      Serial.println(F("    2. GND on the panel goes to GND"));
      Serial.println(F("    3. SDA -> GPIO21, SCL -> GPIO22 -- BY LABEL, not by"));
      Serial.println(F("       position: module pin order is not standardised"));
      Serial.println(F("    4. all four jumpers seated (the usual culprit)"));
      swept = true;
      sweepPins();
    }
  } else {
    Serial.printf("  found %u device(s)\n", found);
  }
  Serial.println();
  delay(3000);                      // rescan: an intermittent wire will flicker
}
