// Standalone OLED test on a Raspberry Pi Pico — is the PANEL good?
//
// Why a second board: the ESP32 in this project runs Wi-Fi and BLE and has a
// long history of browning out on marginal USB supplies, so "the display does
// not light up" there has two possible causes tangled together. A plain Pico
// has no radio, draws a fraction of the current, and shares none of that
// history. If the panel works here it is good, and the fault is on the ESP32
// side; if it fails here too, the panel or its soldering is at fault.
//
// WIRING (Pico physical pin numbers, counting from the USB end):
//
//   OLED VCC -> pin 36   3V3(OUT)
//   OLED GND -> pin 38   GND
//   OLED SDA -> pin  6   GP4
//   OLED SCL -> pin  7   GP5
//
// Board: rp2040:rp2040:rpipico. Hold BOOTSEL while plugging in, then:
//   arduino-cli compile --fqbn rp2040:rp2040:rpipico tools/oled_test_pico
//   arduino-cli upload  --fqbn rp2040:rp2040:rpipico tools/oled_test_pico
//
// The test is deliberately loud: it fills the screen, inverts it, then draws
// text. A panel with a cracked ribbon or a partly-soldered header often shows
// a fill but garbles text, so a plain "Hello" alone can mislead.

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define SDA_PIN 4          // GP4 = physical pin 6
#define SCL_PIN 5          // GP5 = physical pin 7
#define OLED_W  128
#define OLED_H  32

static Adafruit_SSD1306 oled(OLED_W, OLED_H, &Wire, -1);
static uint8_t addr = 0;

static uint8_t findPanel() {
  for (uint8_t a = 0x3C; a <= 0x3D; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) return a;
  }
  return 0;
}

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 4000) { }   // USB CDC needs a moment
  delay(300);

  Serial.println(F("\n=== OLED panel test (Pico) ==="));
  Serial.printf("SDA=GP%d (pin 6)   SCL=GP%d (pin 7)\n", SDA_PIN, SCL_PIN);
  Serial.println(F("VCC -> pin 36 (3V3 OUT), GND -> pin 38\n"));

  Wire.setSDA(SDA_PIN);
  Wire.setSCL(SCL_PIN);
  Wire.begin();
  Wire.setClock(100000);

  addr = findPanel();
  if (!addr) {
    Serial.println(F("NO PANEL on the bus at 0x3C or 0x3D."));
    Serial.println(F("On a Pico with no radio this is almost certainly the"));
    Serial.println(F("panel or its wiring, NOT a power problem:"));
    Serial.println(F("  1. is the 4-pin header SOLDERED to the module?"));
    Serial.println(F("  2. continuity, each wire end to end"));
    Serial.println(F("  3. 3.3 V across the panel's own VCC/GND pins"));
    return;
  }

  Serial.printf("panel answered at 0x%02X\n", addr);
  if (!oled.begin(SSD1306_SWITCHCAPVCC, addr)) {
    Serial.println(F("...but SSD1306 begin() FAILED — it answers on the bus"));
    Serial.println(F("but will not initialise. Suspect a faulty controller."));
    return;
  }
  Serial.println(F("SSD1306 initialised. Running the visual test."));
  Serial.println(F("Watch the panel: all-on, inverted, then text."));
}

void loop() {
  if (!addr) { delay(2000); return; }
  static uint32_t n = 0;

  // 1. every pixel on — proves the glass and the ribbon
  oled.clearDisplay();
  oled.fillRect(0, 0, OLED_W, OLED_H, SSD1306_WHITE);
  oled.display();
  delay(900);

  // 2. inverted — proves addressing, not just a stuck-on panel
  oled.invertDisplay(true);
  delay(700);
  oled.invertDisplay(false);

  // 3. text and a border — proves the full drawing path
  oled.clearDisplay();
  oled.drawRect(0, 0, OLED_W, OLED_H, SSD1306_WHITE);
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(2);
  oled.setCursor(6, 3);
  oled.print(F("PANEL OK"));
  oled.setTextSize(1);
  oled.setCursor(6, 21);
  oled.printf("0x%02X  test %lu", addr, (unsigned long)++n);
  oled.display();

  Serial.printf("  pass %lu drawn\n", (unsigned long)n);
  delay(1800);
}
