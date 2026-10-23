// OLED soak test on the ESP32 — does the panel talk, and does the board stay up?
//
// Answers two questions at once, with Wi-Fi and BLE never started:
//
//   1. Is the SSD1306 communicating?   -> serial says "[oled] up at 0x3C", the
//                                         panel shows a test pattern, then a
//                                         live counter, and every second the
//                                         bus is probed and errors are counted
//   2. Is the board stable on its own? -> uptime keeps climbing and the reset
//                                         reason printed at boot is POWERON
//
// Why no radios: the logger's resets happen at WiFi.begin(), where the radio's
// first transmit pulls the most current (firmware/README.md, "Random panics").
// If this sketch runs for minutes without a reset on the same cable and supply,
// the OLED and its wiring are fine and the supply can't carry the radio.
// If it resets here too, the problem is the cable, the supply or the board.
//
// Needs no secrets.h and no config.h. Flash at 115200 baud (faster uploads have
// corrupted on a marginal supply), read the log, then flash stage10_upload back:
//
//   arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=huge_app,UploadSpeed=115200 \
//                       -u -p /dev/cu.usbserial-0001 tools/oled_test_esp32
//   arduino-cli monitor -p /dev/cu.usbserial-0001 -c baudrate=115200

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <esp_system.h>

#define SDA_PIN 21
#define SCL_PIN 22
#define W       128
#define H       32

static Adafruit_SSD1306 oled(W, H, &Wire, -1);
static uint8_t  addr = 0;
static uint32_t probes = 0, probeErrors = 0, frames = 0;

static const char* resetName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "POWERON (normal: power applied)";
    case ESP_RST_SW:        return "SW (esp_restart)";
    case ESP_RST_PANIC:     return "PANIC (crash)";
    case ESP_RST_INT_WDT:   return "INT_WDT (watchdog)";
    case ESP_RST_TASK_WDT:  return "TASK_WDT (watchdog)";
    case ESP_RST_WDT:       return "WDT (watchdog)";
    case ESP_RST_BROWNOUT:  return "BROWNOUT (supply sagged)";
    case ESP_RST_EXT:       return "EXT (EN button / auto-reset)";
    default:                return "other";
  }
}

static bool ack(uint8_t a) {
  Wire.beginTransmission(a);
  return Wire.endTransmission() == 0;
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println(F("\n=== OLED soak test (no Wi-Fi, no BLE) ==="));
  Serial.printf("[boot] reset reason: %s\n", resetName(esp_reset_reason()));

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(100000);                       // forgiving of jumper wires
  if (ack(0x3C)) addr = 0x3C; else if (ack(0x3D)) addr = 0x3D;
  if (!addr) {
    Serial.println(F("[oled] no answer at 0x3C or 0x3D. Run tools/i2c_scan for wiring checks."));
    return;
  }
  if (!oled.begin(SSD1306_SWITCHCAPVCC, addr)) {
    Serial.printf("[oled] answers at 0x%02X but init failed (out of RAM?)\n", addr);
    addr = 0;
    return;
  }
  Serial.printf("[oled] up at 0x%02X\n", addr);

  // Pattern: full white, then a border and text. A dead row or column of pixels,
  // or a shifted image (SH1106 instead of SSD1306), is obvious on these.
  oled.clearDisplay(); oled.fillRect(0, 0, W, H, SSD1306_WHITE); oled.display();
  delay(800);
  oled.clearDisplay();
  oled.drawRect(0, 0, W, H, SSD1306_WHITE);
  oled.setTextColor(SSD1306_WHITE); oled.setTextSize(1);
  oled.setCursor(4, 4);  oled.print(F("OLED TEST"));
  oled.setCursor(4, 14); oled.printf("addr 0x%02X  I2C OK", addr);
  oled.display();
  delay(1500);
  Serial.println(F("[oled] test pattern drawn: you should have seen full white, then a box"));
}

void loop() {
  static uint32_t last = 0;
  if (millis() - last < 1000) { delay(10); return; }
  last = millis();
  uint32_t up = millis() / 1000;

  if (!addr) {                                 // no panel: still report uptime
    Serial.printf("[stat] uptime %lus, no panel\n", (unsigned long)up);
    return;
  }

  probes++;
  if (!ack(addr)) probeErrors++;

  oled.clearDisplay();
  oled.setTextSize(2); oled.setCursor(0, 0);
  oled.printf("%lus", (unsigned long)up);
  oled.setTextSize(1); oled.setCursor(0, 24);
  oled.printf("I2C err %lu/%lu", (unsigned long)probeErrors, (unsigned long)probes);
  oled.display();
  frames++;

  // A line every 5 s is enough to see the uptime keep climbing.
  if (up % 5 == 0)
    Serial.printf("[stat] uptime %lus  frames %lu  i2c errors %lu/%lu  heap %u\n",
                  (unsigned long)up, (unsigned long)frames,
                  (unsigned long)probeErrors, (unsigned long)probes, ESP.getFreeHeap());
}
