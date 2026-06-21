#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <Adafruit_NeoPixel.h>
#include <FastLED.h>
#include <fauxmoESP.h>
#include <AsyncTCP.h>
#include <LittleFS.h>
#include <Wire.h>
#include <Adafruit_AHTX0.h>
#include <ArduinoOTA.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include "config.h"  // Copy config.h.example → config.h and fill in your values

// WiFi credentials (defined in config.h)
const char* ssid     = WIFI_SSID;
const char* password = WIFI_PASSWORD;

// ── LED strips (WS2814 24V RGBW) ───────────────────────────────────────────────
// Two independent strips: strip 0 on IO40, strip 1 on IO41 (both via the
// SN74AHCT125 5V level shifter). Lengths come from config.h (NUM_LEDS / NUM_LEDS_2).
#define NUM_STRIPS       2
#define LED_STRIP_PIN_1  40
#define LED_STRIP_PIN_2  41

// Status LEDs (4x SK6805-EC20 on IO42)
#define STATUS_LED_PIN 42
#define NUM_STATUS_LEDS 4
#define STATUS_LED_TYPE WS2812
#define STATUS_COLOR_ORDER GRB

// Status LED indices — data-chain order on IO42: WiFi → Temp → Strip 1 → Strip 2
// (LED5 → LED4 → LED2 → LED1 in the KiCad schematic). The two GREEN_PWR LEDs are
// discrete, hardwired power indicators — not driven from here.
#define LED_WIFI   0   // WiFi status
#define LED_TEMP   1   // temperature: green→red gradient, breathing
#define LED_STRIP1 2   // Strip 1: green = on, red = off
#define LED_STRIP2 3   // Strip 2: green = on, red = off

// I2C pins (shared by AHT20 and INA238)
#define I2C_SDA 2
#define I2C_SCL 1

// ST7735 LCD (SPI) — HS096T01H13, 0.96" 80×160 (LCSC C18198246)
// CLK/MOSI/DC are kept off the octal-PSRAM bus (GPIO35/36/37) so PSRAM stays usable.
#define LCD_CS   39
#define LCD_RST  38
#define LCD_DC   16
#define LCD_CLK  17
#define LCD_MOSI 18
#define LCD_BL   8

// INA238 power monitor
#define INA238_ADDR       0x40   // A0=GND, A1=GND
#define INA238_SHUNT_OHMS 0.020f // 20 mΩ shunt (HOJLR2512-3W-20MR)
#define INA238_REG_VSHUNT 0x04
#define INA238_REG_VBUS   0x05

// Button pins. The four control buttons act on the strip(s) chosen by the select
// button: Strip 1 / Strip 2 / Both.
#define BTN_MINUS  4  // Brightness decrease
#define BTN_PLUS   5  // Brightness increase
#define BTN_MODE   6  // Mode change
#define BTN_POWER  7  // Power toggle (long press)
#define BTN_SELECT 15 // Cycle button target: Strip 1 → Strip 2 → Both

// Web server on defined port (moved from 80 to avoid conflict with fauxmo)
AsyncWebServer server(WEB_SERVER_PORT);

// Alexa handler
fauxmoESP fauxmo;

// AHT20 temperature sensor
Adafruit_AHTX0 aht;
bool ahtAvailable = false;
float currentTemperature = 0.0;
float currentHumidity = 0.0;
unsigned long lastTempRead = 0;
const unsigned long tempReadInterval = 5000;

// INA238 power monitor
bool inaAvailable = false;
float inaVoltage = 0.0f;
float inaCurrent = 0.0f;
float inaPower   = 0.0f;
unsigned long lastInaRead = 0;
const unsigned long inaReadInterval = 250;  // 4 Hz

// ST7735 display
Adafruit_ST7735 tft = Adafruit_ST7735(&SPI, LCD_CS, LCD_DC, LCD_RST);
bool backlightOn = true;   // LCD backlight — toggled from the web

// WiFi connection state — written from the WiFi event task, read from loop
volatile bool wifiConnected   = false;
volatile bool isReconnecting  = false;
volatile int  reconnectAttempts = 0;
const int MAX_RECONNECT_ATTEMPTS = 10;  // Restart device after 10 consecutive failures

// ── Per-strip state ────────────────────────────────────────────────────────────
const int MAX_MODES = 10;

struct StripState {
  bool     on        = false;   // strip starts OFF — turn on via Alexa/buttons/web
  int      brightness = 10;     // 1..10
  int      mode      = 0;       // 0..MAX_MODES-1 (mode 9 = effects)
  bool     useCustom = false;
  uint8_t  r = 255, g = 255, b = 255, w = 0;  // custom RGBW
  // effect runtime (independent per strip)
  unsigned long lastEffect = 0;
  int      effectStep = 0;
  uint8_t  effectSub = 0;
  unsigned long lastEffectChange = 0;
  uint8_t  hue = 0, breathHue = 0;
};

StripState strips[NUM_STRIPS];

// Set by the web/Alexa callbacks (async task) to ask loop() to re-apply a strip's
// pixels. The LED libraries aren't thread-safe, so every show() happens in loop().
volatile bool stripDirty[NUM_STRIPS] = { false, false };

// LED strip hardware objects (RGBW) using Adafruit NeoPixel
Adafruit_NeoPixel pixels[NUM_STRIPS] = {
  Adafruit_NeoPixel(NUM_LEDS,   LED_STRIP_PIN_1, NEO_WRGB + NEO_KHZ800),
  Adafruit_NeoPixel(NUM_LEDS_2, LED_STRIP_PIN_2, NEO_WRGB + NEO_KHZ800)
};

// Per-strip name (config.h) — used for the Alexa device and the web selector label
const char* STRIP_NAMES[NUM_STRIPS] = { STRIP_NAME_1, STRIP_NAME_2 };

// Status LEDs (RGB) using FastLED
CRGB statusLeds[NUM_STATUS_LEDS];

// Shared button timing
unsigned long lastButtonPressTime = 0;
const unsigned long debounceDelay = 200;
bool powerButtonPressed = false;
unsigned long powerButtonPressStartTime = 0;
const unsigned long powerButtonLongPressDelay = 500;
// Which strip the on-device buttons target: 0 = Strip 1, 1 = Strip 2, NUM_STRIPS = Both.
// Cycled by BTN_SELECT. Defaults to Both.
int btnTarget = NUM_STRIPS;

// Color definitions for RGBW LEDs (R, G, B, W)
uint8_t colorModes[MAX_MODES][4] = {
  {0, 0, 0, 255},       // 0: Pure White (W channel only)
  {255, 0, 0, 0},       // 1: Red
  {0, 255, 0, 0},       // 2: Green
  {0, 0, 255, 0},       // 3: Blue
  {255, 255, 0, 0},     // 4: Yellow
  {255, 0, 255, 0},     // 5: Purple/Magenta
  {0, 255, 255, 0},     // 6: Cyan
  {255, 165, 0, 0},     // 7: Orange
  {255, 255, 255, 255}, // 8: Bright White (RGB + W)
  {0, 0, 0, 0}          // 9: Effects mode (placeholder)
};

const int effectSpeed = 50;

// Status LED brightness (dimmed for status indication)
const uint8_t STATUS_LED_BRIGHTNESS = 10;  // ~4% brightness

// ── INA238 ───────────────────────────────────────────────────────────────────

static int16_t ina238Reg(uint8_t reg) {
  Wire.beginTransmission(INA238_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return 0;
  Wire.requestFrom(INA238_ADDR, (uint8_t)2);
  if (Wire.available() < 2) return 0;
  return (int16_t)((Wire.read() << 8) | Wire.read());
}

void readINA238() {
  if (!inaAvailable) return;
  inaVoltage = ina238Reg(INA238_REG_VBUS)   * 3.125e-3f;  // LSB = 3.125 mV
  if (inaVoltage < 0.05f) inaVoltage = 0.0f;  // idle deadband — suppress sub-50mV offset
  float vShunt = ina238Reg(INA238_REG_VSHUNT) * 5e-6f;    // LSB = 5 µV
  inaCurrent = vShunt / INA238_SHUNT_OHMS;
  if (inaCurrent < 0.0f) inaCurrent = 0.0f;  // load is unidirectional; negatives are offset noise
  inaPower   = inaVoltage * inaCurrent;
}

// ── Display ──────────────────────────────────────────────────────────────────

// Layout: ST7735 0.96" at rotation 1 → 160 wide × 80 tall (landscape).
// Four metrics in a 2×2 grid, with a full-width IP line at the bottom.

// Color palette (RGB565)
#define C_BG      0x0000  // Black
#define C_VOLT    0xFFE0  // Yellow
#define C_AMP     0x07FF  // Cyan
#define C_WATT    0xFD20  // Orange
#define C_TEMP    0x867F  // Steel blue
#define C_IP      0x07E0  // Green
#define C_LABEL   0x8410  // Gray
#define C_LINE    0x2104  // Dim separator

#define GRID_ROW_H 28                    // height of each metric row
#define IP_Y       (2 * GRID_ROW_H + 2)  // top of the IP line (58)

static int gridColX(int col) { return col * (tft.width() / 2); }  // 0 or 80
static int gridColW()        { return tft.width() / 2; }          // 80

void displayDrawStatic() {
  tft.fillScreen(C_BG);
  int w = tft.width();

  // Grid separators
  tft.drawFastVLine(w / 2, 0, 2 * GRID_ROW_H, C_LINE);
  tft.drawFastHLine(0, GRID_ROW_H,     w, C_LINE);
  tft.drawFastHLine(0, 2 * GRID_ROW_H, w, C_LINE);

  // Labels (size 1)
  tft.setTextSize(1);
  tft.setTextColor(C_LABEL);
  tft.setCursor(gridColX(0) + 3, 2);              tft.print("VOLTAGE");
  tft.setCursor(gridColX(1) + 3, 2);              tft.print("CURRENT");
  tft.setCursor(gridColX(0) + 3, GRID_ROW_H + 2); tft.print("POWER");
  tft.setCursor(gridColX(1) + 3, GRID_ROW_H + 2); tft.print("TEMP");
}

// Draw a value (size 2) centered in one grid cell. Redraws only when the text
// changes and overwrites glyphs with an opaque background instead of clearing to
// black first, so steady/updating values don't flicker.
void displayCellValue(int col, int rowY, uint16_t color, const char* valStr, const char* unit) {
  static char last[4][16] = {{0}};
  int idx = (rowY >= GRID_ROW_H ? 2 : 0) + col;

  char text[16];
  snprintf(text, sizeof(text), "%s%s", valStr, unit);
  if (strcmp(text, last[idx]) == 0) return;  // unchanged → skip redraw entirely

  int x0   = gridColX(col);
  int w    = gridColW();
  int y    = rowY + 11;
  int newW = (int)strlen(text)      * 12;  // size 2: 12px/char
  int oldW = (int)strlen(last[idx]) * 12;

  tft.setTextSize(2);

  // Only clear when the new text is narrower than the old; a wider/equal redraw
  // with an opaque background fully covers the previous glyphs on its own.
  if (newW < oldW) {
    int clrX = x0 + (w - oldW) / 2;
    if (clrX < x0 + 1) clrX = x0 + 1;
    tft.fillRect(clrX, y, min(oldW, w - 1), 16, C_BG);
  }

  int x = x0 + (w - newW) / 2;
  if (x < x0 + 1) x = x0 + 1;
  tft.setCursor(x, y);
  tft.setTextColor(color, C_BG);
  tft.print(valStr);
  tft.setTextColor(C_LABEL, C_BG);
  tft.print(unit);

  strncpy(last[idx], text, sizeof(last[idx]) - 1);
  last[idx][sizeof(last[idx]) - 1] = '\0';
}

// Bottom line: WiFi IP address, sized to fill the band and centered.
// Only redraws when the text changes (no flicker).
void displayUpdateIP() {
  static String shown = "";
  String now;
  if (wifiConnected)      now = WiFi.localIP().toString();
  else if (isReconnecting) now = "connecting...";
  else                     now = "no wifi";
  if (now == shown) return;
  shown = now;

  int w     = tft.width();
  int bandH = tft.height() - IP_Y;
  tft.fillRect(0, IP_Y, w, bandH, C_BG);

  // Largest text size that fits the width (6px * size per char).
  int len  = now.length();
  int size = (len * 12 <= w) ? 2 : 1;
  int charW = 6 * size, charH = 8 * size;
  int x = (w - len * charW) / 2;  if (x < 0) x = 0;
  int y = IP_Y + (bandH - charH) / 2;

  tft.setTextSize(size);
  tft.setTextColor(wifiConnected ? C_IP : C_LABEL, C_BG);
  tft.setCursor(x, y);
  tft.print(now);
}

void updateDisplay() {
  char buf[12];

  snprintf(buf, sizeof(buf), inaAvailable ? "%.2f" : "--.-", inaVoltage);
  displayCellValue(0, 0, C_VOLT, buf, "V");

  // Current: auto-range — show mA below 1 A, switch to A above.
  if (inaAvailable) {
    if (inaCurrent < 1.0f) {
      snprintf(buf, sizeof(buf), "%.0f", inaCurrent * 1000.0f);
      displayCellValue(1, 0, C_AMP, buf, "mA");
    } else {
      snprintf(buf, sizeof(buf), "%.2f", inaCurrent);
      displayCellValue(1, 0, C_AMP, buf, "A");
    }
  } else {
    displayCellValue(1, 0, C_AMP, "-.--", "A");
  }

  snprintf(buf, sizeof(buf), inaAvailable ? "%.1f" : "--.-", inaPower);
  displayCellValue(0, GRID_ROW_H, C_WATT, buf, "W");

  if (ahtAvailable) {
    snprintf(buf, sizeof(buf), "%.1f", currentTemperature);
    displayCellValue(1, GRID_ROW_H, C_TEMP, buf, "\xf7""C");
  } else {
    displayCellValue(1, GRID_ROW_H, C_LABEL, "--", "\xf7""C");
  }

  displayUpdateIP();
}

// ── LED strip helpers ──────────────────────────────────────────────────────────

void setStripColor(int s, uint8_t r, uint8_t g, uint8_t b, uint8_t w) {
  uint16_t n = pixels[s].numPixels();
  for (uint16_t i = 0; i < n; i++) pixels[s].setPixelColor(i, pixels[s].Color(r, g, b, w));
}

// Push the logical state of one strip out to its pixels.
void applyStrip(int s) {
  StripState &S = strips[s];
  pixels[s].setBrightness(map(S.brightness, 1, 10, 25, 255));

  if (!S.on) {
    setStripColor(s, 0, 0, 0, 0);
    pixels[s].show();
    return;
  }

  if (S.useCustom) {
    setStripColor(s, S.r, S.g, S.b, S.w);
  } else if (S.mode < MAX_MODES - 1) {
    setStripColor(s, colorModes[S.mode][0], colorModes[S.mode][1],
                     colorModes[S.mode][2], colorModes[S.mode][3]);
  }
  // mode == MAX_MODES-1 (effects): left for updateStripEffects() to draw.
  pixels[s].show();
}

void applyAllStrips() {
  for (int s = 0; s < NUM_STRIPS; s++) applyStrip(s);
}

// Drive every strip to one solid color/brightness (used for boot/WiFi feedback).
void fillAllStrips(uint8_t r, uint8_t g, uint8_t b, uint8_t w, uint8_t brightness) {
  for (int s = 0; s < NUM_STRIPS; s++) {
    pixels[s].setBrightness(brightness);
    setStripColor(s, r, g, b, w);
    pixels[s].show();
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("ESP32-S3 LED Controller V3 - Dual WS2814 RGBW & SK6805 Status LEDs");

  // Initialize status LEDs first for error indication
  FastLED.addLeds<STATUS_LED_TYPE, STATUS_LED_PIN, STATUS_COLOR_ORDER>(statusLeds, NUM_STATUS_LEDS);

  // Validate configuration
  if (String(ssid) == "your_wifi_ssid" || String(password) == "your_wifi_password") {
    Serial.println("ERROR: Please configure config.h before flashing! Copy config.h.example to config.h and fill in your values.");
    while (true) {
      fill_solid(statusLeds, NUM_STATUS_LEDS, CRGB::Red);
      FastLED.show();
      delay(500);
      fill_solid(statusLeds, NUM_STATUS_LEDS, CRGB::Black);
      FastLED.show();
      delay(500);
    }
  }

  // Initialize LittleFS
  if (!LittleFS.begin(true)) {
    Serial.println("An error occurred while mounting LittleFS");
    return;
  }
  Serial.println("LittleFS initialized successfully");

  // Initialize ST7735 display
  SPI.begin(LCD_CLK, -1, LCD_MOSI, LCD_CS);
  tft.initR(INITR_MINI160x80);
  tft.setRotation(1);       // landscape: 160w × 80h (adjust 0-3 if upside-down)
  tft.setTextWrap(false);   // clip overflow instead of wrapping onto the next row
  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, backlightOn ? HIGH : LOW);   // backlight (GPIO21 → Q1)
  displayDrawStatic();
  Serial.println("ST7735 display initialized");

  // Initialize I2C for AHT20 + INA238
  Wire.begin(I2C_SDA, I2C_SCL);

  // Initialize AHT20 temperature sensor
  if (aht.begin()) {
    Serial.println("AHT20 temperature sensor initialized");
    ahtAvailable = true;
    readTemperature();
  } else {
    Serial.println("WARNING: Could not find AHT20 sensor!");
    ahtAvailable = false;
  }

  // Initialize INA238 power monitor
  Wire.beginTransmission(INA238_ADDR);
  inaAvailable = (Wire.endTransmission() == 0);
  if (inaAvailable) {
    Serial.println("INA238 power monitor initialized");
    readINA238();
  } else {
    Serial.println("WARNING: Could not find INA238 at 0x40!");
  }

  updateDisplay();

  // Initialize both LED strips — all off
  for (int s = 0; s < NUM_STRIPS; s++) {
    pixels[s].begin();
    pixels[s].show();
  }

  // All status LEDs off until WiFi/strip state is known (power is a discrete LED)
  fill_solid(statusLeds, NUM_STATUS_LEDS, CRGB::Black);
  FastLED.show();

  // Initialize buttons
  pinMode(BTN_MINUS, INPUT_PULLUP);
  pinMode(BTN_PLUS,  INPUT_PULLUP);
  pinMode(BTN_MODE,  INPUT_PULLUP);
  pinMode(BTN_POWER, INPUT_PULLUP);
  pinMode(BTN_SELECT, INPUT_PULLUP);

  // Connect to WiFi
  connectToWiFi();

  if (wifiConnected) {
    Serial.println("Setting up Alexa...");
    fauxmo.createServer(true);
    fauxmo.setPort(80);
    fauxmo.enable(true);

    // One Alexa device per strip; device_id matches the strip index.
    for (int s = 0; s < NUM_STRIPS; s++) {
      fauxmo.addDevice(STRIP_NAMES[s]);
      Serial.printf("Fauxmo device '%s' added (strip %d)\r\n", STRIP_NAMES[s], s);
    }

    fauxmo.onSetState([](unsigned char device_id, const char * device_name, bool state, unsigned char value) {
      if (device_id >= NUM_STRIPS) return;
      StripState &S = strips[device_id];
      S.on = state;
      if (state) S.brightness = max(1, (int)map(value, 0, 255, 1, 10));
      Serial.printf("[ALEXA] #%d (%s) -> %s (val %u)\r\n", device_id, device_name, state ? "ON" : "OFF", value);
      stripDirty[device_id] = true;
    });

    Serial.println("Alexa integration setup complete");
    setupWebServer();
    setupOTA();
  }

  // Show initial state — both strips stay OFF
  updateStatusLEDs();
  applyAllStrips();
}

void loop() {
  unsigned long now = millis();

  checkButtons();

  // Apply pending web/Alexa changes here. Those callbacks run in the async task,
  // and the LED libraries aren't thread-safe, so the show() must happen in loop().
  for (int s = 0; s < NUM_STRIPS; s++) {
    if (stripDirty[s]) { stripDirty[s] = false; applyStrip(s); }
  }

  if (wifiConnected) {
    ArduinoOTA.handle();
    fauxmo.handle();

    // Debug log every 30s
    static unsigned long lastMsg = 0;
    if (now - lastMsg > 30000) {
      lastMsg = now;
      Serial.printf("[Status] IP: %s  Heap: %u bytes  Uptime: %lus\r\n",
        WiFi.localIP().toString().c_str(), ESP.getFreeHeap(), now / 1000);
    }
  }

  updateStatusLEDs();

  // Run effects per strip on each strip's own cadence
  for (int s = 0; s < NUM_STRIPS; s++) {
    if (strips[s].on && now - strips[s].lastEffect > effectSpeed) {
      updateStripEffects(s);
      strips[s].lastEffect = now;
    }
  }

  if (ahtAvailable && now - lastTempRead > tempReadInterval) {
    readTemperature();
    lastTempRead = now;
  }

  if (inaAvailable && now - lastInaRead > inaReadInterval) {
    readINA238();
    lastInaRead = now;
  }

  // Refresh the screen on its own cadence (independent of sensor presence)
  // so live values and the IP line always update.
  static unsigned long lastDisplay = 0;
  if (now - lastDisplay > 250) {
    lastDisplay = now;
    updateDisplay();
  }

  delay(10);
}

// ── WiFi management ──────────────────────────────────────────────────────────

// Called automatically by the WiFi driver task — no polling needed
void onWifiEvent(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      wifiConnected    = true;
      isReconnecting   = false;
      reconnectAttempts = 0;
      Serial.printf("[WiFi] Connected! IP: %s\r\n", WiFi.localIP().toString().c_str());
      break;

    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      wifiConnected  = false;
      isReconnecting = true;
      reconnectAttempts++;
      Serial.printf("[WiFi] Disconnected (attempt %d/%d)\r\n",
                    reconnectAttempts, MAX_RECONNECT_ATTEMPTS);
      if (reconnectAttempts >= MAX_RECONNECT_ATTEMPTS) {
        Serial.println("[WiFi] Max reconnect attempts reached — restarting device");
        ESP.restart();
      }
      // setAutoReconnect(true) makes the driver retry automatically
      break;

    default:
      break;
  }
}

void connectToWiFi() {
  Serial.printf("Connecting to WiFi: %s\r\n", ssid);

  WiFi.onEvent(onWifiEvent);     // Register event handler before begin()
  WiFi.setHostname(WIFI_HOSTNAME);
  WiFi.setSleep(false);          // Disable power saving — prevents DHCP drops
  WiFi.setAutoReconnect(true);   // Driver retries automatically on disconnect
  WiFi.persistent(false);        // Don't write credentials to flash every boot

  // Blue WiFi LED while connecting; strip LEDs off
  statusLeds[LED_WIFI]   = CRGB(0, 0, STATUS_LED_BRIGHTNESS);
  statusLeds[LED_STRIP1] = CRGB::Black;
  statusLeds[LED_STRIP2] = CRGB::Black;
  FastLED.show();

  fillAllStrips(0, 0, 255, 0, 255);

  WiFi.begin(ssid, password);

  // Block here just for the initial boot connection (max 10s)
  int attempts = 0;
  while (!wifiConnected && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  Serial.println();

  if (wifiConnected) {
    // Flash yellow 5× on success
    for (int i = 0; i < 5; i++) {
      fill_solid(statusLeds, NUM_STATUS_LEDS, CRGB(STATUS_LED_BRIGHTNESS, STATUS_LED_BRIGHTNESS, 0));
      FastLED.show();
      fillAllStrips(255, 255, 0, 0, 255);
      delay(200);

      fill_solid(statusLeds, NUM_STATUS_LEDS, CRGB::Black);
      FastLED.show();
      fillAllStrips(0, 0, 0, 0, 255);
      delay(200);
    }
  } else {
    Serial.println("Initial WiFi connection failed — will keep retrying in background");
    isReconnecting = true;
  }
}

// ── Helpers ──────────────────────────────────────────────────────────────────

void readTemperature() {
  if (!ahtAvailable) return;
  sensors_event_t humidity, temp;
  if (aht.getEvent(&humidity, &temp)) {
    currentTemperature = temp.temperature;
    currentHumidity = humidity.relative_humidity;
    Serial.printf("Temperature: %.1f°C, Humidity: %.1f%%\r\n", currentTemperature, currentHumidity);
  }
}

// ── OTA ──────────────────────────────────────────────────────────────────────

void setupOTA() {
  ArduinoOTA.setHostname(WIFI_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);

  ArduinoOTA.onStart([]() {
    Serial.println("[OTA] Starting update...");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("[OTA] Done. Rebooting...");
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("[OTA] Error %u\r\n", error);
  });

  ArduinoOTA.begin();
  Serial.println("[OTA] Ready");
}

// ── Web server ───────────────────────────────────────────────────────────────
// All control endpoints accept an optional ?strip=N argument:
//   strip=0  → strip 1 (IO40)
//   strip=1  → strip 2 (IO41)
//   strip=all (or omitted) → both strips

// Fill out[] with the target strip indices; returns the count.
int stripTargets(AsyncWebServerRequest *request, int out[]) {
  String s = request->hasArg("strip") ? request->arg("strip") : String("all");
  if (s == "all" || s == "both") {
    for (int i = 0; i < NUM_STRIPS; i++) out[i] = i;
    return NUM_STRIPS;
  }
  int idx = s.toInt();
  if (idx < 0 || idx >= NUM_STRIPS) idx = 0;
  out[0] = idx;
  return 1;
}

void setupWebServer() {
  server.on("/",          HTTP_GET, handleRoot);
  server.on("/toggle",    HTTP_GET, handleToggle);
  server.on("/brightness",HTTP_GET, handleBrightness);
  server.on("/mode",      HTTP_GET, handleMode);
  server.on("/status",    HTTP_GET, handleStatus);
  server.on("/color",     HTTP_GET, handleCustomColor);
  server.on("/backlight", HTTP_GET, handleBacklight);

  server.on("/style.css", HTTP_GET, [](AsyncWebServerRequest *request) {
    sendStatic(request, "/style.css", "text/css");
  });

  server.on("/app.js", HTTP_GET, [](AsyncWebServerRequest *request) {
    sendStatic(request, "/app.js", "application/javascript");
  });

  server.begin();
  Serial.printf("HTTP server started on port %d\r\n", WEB_SERVER_PORT);
}

// Serve a static file from LittleFS with caching disabled. LittleFS files carry no
// changing mtime, so browsers otherwise cache index/css/js indefinitely and you keep
// seeing the old UI after re-uploading. no-store forces a fresh fetch every time.
void sendStatic(AsyncWebServerRequest *request, const char* path, const char* type) {
  if (!LittleFS.exists(path)) {
    request->send(404, "text/plain", "File not found — upload the data folder to LittleFS.");
    return;
  }
  AsyncWebServerResponse *response = request->beginResponse(LittleFS, path, type);
  response->addHeader("Cache-Control", "no-store");
  request->send(response);
}

void handleRoot(AsyncWebServerRequest *request) {
  sendStatic(request, "/index.html", "text/html");
}

void handleToggle(AsyncWebServerRequest *request) {
  int t[NUM_STRIPS];
  int n = stripTargets(request, t);
  if (n == 1) {
    strips[t[0]].on = !strips[t[0]].on;
    stripDirty[t[0]] = true;
  } else {
    // "both": if both are on, turn both off; otherwise turn both on.
    bool allOn = true;
    for (int i = 0; i < n; i++) allOn &= strips[t[i]].on;
    bool ns = !allOn;
    for (int i = 0; i < n; i++) { strips[t[i]].on = ns; stripDirty[t[i]] = true; }
  }
  request->send(200, "text/plain", "OK");
}

void handleBrightness(AsyncWebServerRequest *request) {
  if (request->hasArg("level")) {
    int level = request->arg("level").toInt();
    if (level >= 1 && level <= 10) {
      int t[NUM_STRIPS];
      int n = stripTargets(request, t);
      for (int i = 0; i < n; i++) { strips[t[i]].brightness = level; stripDirty[t[i]] = true; }
    }
  }
  request->send(200, "text/plain", "OK");
}

void handleMode(AsyncWebServerRequest *request) {
  if (request->hasArg("mode")) {
    int mode = request->arg("mode").toInt();
    if (mode >= 0 && mode < MAX_MODES) {
      int t[NUM_STRIPS];
      int n = stripTargets(request, t);
      for (int i = 0; i < n; i++) {
        strips[t[i]].mode = mode;
        strips[t[i]].useCustom = false;
        stripDirty[t[i]] = true;
      }
    }
  }
  request->send(200, "text/plain", "OK");
}

void handleCustomColor(AsyncWebServerRequest *request) {
  if (request->hasArg("r") && request->hasArg("g") && request->hasArg("b")) {
    int r = request->arg("r").toInt();
    int g = request->arg("g").toInt();
    int b = request->arg("b").toInt();
    int w = request->hasArg("w") ? request->arg("w").toInt() : 0;
    if (r >= 0 && r <= 255 && g >= 0 && g <= 255 && b >= 0 && b <= 255 && w >= 0 && w <= 255) {
      int t[NUM_STRIPS];
      int n = stripTargets(request, t);
      for (int i = 0; i < n; i++) {
        StripState &S = strips[t[i]];
        S.r = r; S.g = g; S.b = b; S.w = w;
        S.useCustom = true;
        stripDirty[t[i]] = true;
      }
    }
  }
  request->send(200, "text/plain", "OK");
}

// Screen backlight on/off (GPIO21). digitalWrite is atomic, so this is safe to do
// straight from the async web task — no deferral needed.
void handleBacklight(AsyncWebServerRequest *request) {
  if (request->hasArg("on")) {
    backlightOn = request->arg("on").toInt() != 0;
    digitalWrite(LCD_BL, backlightOn ? HIGH : LOW);
  }
  request->send(200, "text/plain", "OK");
}

void appendStripJson(String &j, int s) {
  StripState &S = strips[s];
  j += "{";
  j += "\"name\":\""         + String(STRIP_NAMES[s]) + "\",";
  j += "\"on\":"             + String(S.on ? "true" : "false") + ",";
  j += "\"brightnessLevel\":"+ String(S.brightness) + ",";
  j += "\"currentMode\":"    + String(S.mode) + ",";
  j += "\"useCustomColor\":" + String(S.useCustom ? "true" : "false") + ",";
  j += "\"customRed\":"      + String(S.r) + ",";
  j += "\"customGreen\":"    + String(S.g) + ",";
  j += "\"customBlue\":"     + String(S.b) + ",";
  j += "\"customWhite\":"    + String(S.w);
  j += "}";
}

void handleStatus(AsyncWebServerRequest *request) {
  String j = "{";
  j += "\"deviceName\":\""    + String(DEVICE_NAME) + "\",";
  j += "\"macAddress\":\""    + WiFi.macAddress() + "\",";
  j += "\"wifiSSID\":\""      + String(ssid) + "\",";
  j += "\"signalStrength\":"  + String(WiFi.RSSI()) + ",";
  j += "\"uptime\":"          + String(millis() / 1000) + ",";
  j += "\"temperature\":"     + String(currentTemperature, 1) + ",";
  j += "\"humidity\":"        + String(currentHumidity, 1) + ",";
  j += "\"sensorAvailable\":" + String(ahtAvailable ? "true" : "false") + ",";
  j += "\"backlight\":"       + String(backlightOn ? "true" : "false") + ",";

  j += "\"power\":{";
  j += "\"available\":" + String(inaAvailable ? "true" : "false") + ",";
  j += "\"voltage\":"   + String(inaVoltage, 2) + ",";
  j += "\"current\":"   + String(inaCurrent, 3) + ",";
  j += "\"currentMa\":" + String(inaCurrent * 1000.0f, 0) + ",";
  j += "\"power\":"     + String(inaPower, 2);
  j += "},";

  j += "\"strips\":[";
  for (int s = 0; s < NUM_STRIPS; s++) {
    appendStripJson(j, s);
    if (s < NUM_STRIPS - 1) j += ",";
  }
  j += "]}";

  request->send(200, "application/json", j);
}

// ── Buttons ──────────────────────────────────────────────────────────────────
// The four control buttons act on the strip(s) selected by BTN_SELECT:
// Strip 1, Strip 2, or Both.

// Fill out[] with the targeted strip indices and return the count.
int buttonTargets(int out[]) {
  if (btnTarget >= NUM_STRIPS) {                 // "Both"
    for (int i = 0; i < NUM_STRIPS; i++) out[i] = i;
    return NUM_STRIPS;
  }
  out[0] = btnTarget;                            // single strip
  return 1;
}

void checkButtons() {
  unsigned long now = millis();
  int t[NUM_STRIPS];

  // Select button — cycle the target on each press (edge-detected): S1 → S2 → Both
  static bool selectWasDown = false;
  bool selectDown = (digitalRead(BTN_SELECT) == LOW);
  if (selectDown && !selectWasDown) {
    btnTarget = (btnTarget + 1) % (NUM_STRIPS + 1);
    Serial.printf("Button target: %s\r\n",
      btnTarget >= NUM_STRIPS ? "Both" : (btnTarget == 0 ? "Strip 1" : "Strip 2"));
    lastButtonPressTime = now;
  }
  selectWasDown = selectDown;

  // Power button — long press toggles the targeted strip(s)
  if (digitalRead(BTN_POWER) == LOW) {
    if (!powerButtonPressed) {
      powerButtonPressed = true;
      powerButtonPressStartTime = now;
    } else if (now - powerButtonPressStartTime >= powerButtonLongPressDelay) {
      int n = buttonTargets(t);
      bool anyOn = false;
      for (int i = 0; i < n; i++) anyOn |= strips[t[i]].on;
      bool ns = !anyOn;  // if any target is on, turn them off; otherwise on
      for (int i = 0; i < n; i++) strips[t[i]].on = ns;
      Serial.printf("Power: %s\r\n", ns ? "ON" : "OFF");
      powerButtonPressed = false;
      lastButtonPressTime = now;
      updateStatusLEDs();
      applyAllStrips();
    }
  } else {
    powerButtonPressed = false;
  }

  if (now - lastButtonPressTime < debounceDelay) return;

  int n = buttonTargets(t);

  if (digitalRead(BTN_MINUS) == LOW) {
    for (int i = 0; i < n; i++) strips[t[i]].brightness = max(1, strips[t[i]].brightness - 1);
    Serial.printf("Brightness-: %d%%\r\n", strips[t[0]].brightness * 10);
    lastButtonPressTime = now;
    applyAllStrips();
  }

  if (digitalRead(BTN_PLUS) == LOW) {
    for (int i = 0; i < n; i++) strips[t[i]].brightness = min(10, strips[t[i]].brightness + 1);
    Serial.printf("Brightness+: %d%%\r\n", strips[t[0]].brightness * 10);
    lastButtonPressTime = now;
    applyAllStrips();
  }

  if (digitalRead(BTN_MODE) == LOW) {
    for (int i = 0; i < n; i++) {
      strips[t[i]].mode = (strips[t[i]].mode + 1) % MAX_MODES;
      strips[t[i]].useCustom = false;
    }
    Serial.printf("Mode: %d\r\n", strips[t[0]].mode);
    lastButtonPressTime = now;
    applyAllStrips();
  }
}

// ── LED updates ──────────────────────────────────────────────────────────────

// Temperature → smooth green(cool) … yellow … red(hot), with a gentle breathing
// pulse. Green at/below TEMP_MIN, red at/above TEMP_MAX (°C, from the AHT20).
CRGB tempToColor(float c) {
  const float TEMP_MIN = 22.0f, TEMP_MAX = 40.0f;
  float t = constrain((c - TEMP_MIN) / (TEMP_MAX - TEMP_MIN), 0.0f, 1.0f);

  // Breathing brightness: 0.35…1.0 of the dim status level, ~4 s period.
  float phase  = (millis() % 4000) / 4000.0f * TWO_PI;
  float breath = 0.35f + 0.65f * (0.5f + 0.5f * sinf(phase));
  uint8_t lvl  = (uint8_t)(STATUS_LED_BRIGHTNESS * breath);

  return CRGB((uint8_t)(t * lvl), (uint8_t)((1.0f - t) * lvl), 0);  // red rises, green falls
}

// Per-strip status color: green when on, red when off.
static CRGB stripStatusColor(int s) {
  return strips[s].on ? CRGB(0, STATUS_LED_BRIGHTNESS, 0)
                      : CRGB(STATUS_LED_BRIGHTNESS, 0, 0);
}

void updateStatusLEDs() {
  // WiFi: green=connected, yellow blink=reconnecting, red=failed
  if (WiFi.status() == WL_CONNECTED) {
    statusLeds[LED_WIFI] = CRGB(0, STATUS_LED_BRIGHTNESS, 0);
  } else if (isReconnecting) {
    bool blink = (millis() % 1000) < 500;
    statusLeds[LED_WIFI] = blink
      ? CRGB(STATUS_LED_BRIGHTNESS, STATUS_LED_BRIGHTNESS, 0)  // yellow
      : CRGB::Black;
  } else {
    statusLeds[LED_WIFI] = CRGB(STATUS_LED_BRIGHTNESS, 0, 0);  // red
  }

  // Temperature: green→red gradient with breathing (off if no sensor)
  statusLeds[LED_TEMP] = ahtAvailable ? tempToColor(currentTemperature) : CRGB::Black;

  // Strip 1 / Strip 2: green = on, red = off
  statusLeds[LED_STRIP1] = stripStatusColor(0);
  statusLeds[LED_STRIP2] = stripStatusColor(1);

  FastLED.show();
}

void updateStripEffects(int s) {
  StripState &S = strips[s];
  if (!S.on || S.useCustom) return;
  if (S.mode != MAX_MODES - 1) return;        // effects only on mode 9
  if (pixels[s].numPixels() == 0) return;

  if (millis() - S.lastEffectChange > 10000) {
    S.effectSub = (S.effectSub + 1) % 3;
    S.lastEffectChange = millis();
  }

  switch (S.effectSub) {
    case 0: rainbowEffect(s);                       break;
    case 1: S.hue += 3;       chaseEffect(s, S.hue); break;
    case 2: S.breathHue++;    breatheEffect(s, S.breathHue); break;
  }

  pixels[s].show();
}

// ── Effects ──────────────────────────────────────────────────────────────────

void hsvToRgb(uint8_t h, uint8_t s, uint8_t v, uint8_t* r, uint8_t* g, uint8_t* b) {
  if (s == 0) { *r = *g = *b = v; return; }
  uint8_t region    = h / 43;
  uint8_t remainder = (h - (region * 43)) * 6;
  uint8_t p = (v * (255 - s)) >> 8;
  uint8_t q = (v * (255 - ((s * remainder) >> 8))) >> 8;
  uint8_t t = (v * (255 - ((s * (255 - remainder)) >> 8))) >> 8;
  switch (region) {
    case 0:  *r = v; *g = t; *b = p; break;
    case 1:  *r = q; *g = v; *b = p; break;
    case 2:  *r = p; *g = v; *b = t; break;
    case 3:  *r = p; *g = q; *b = v; break;
    case 4:  *r = t; *g = p; *b = v; break;
    default: *r = v; *g = p; *b = q; break;
  }
}

void breatheEffect(int s, uint8_t hue) {
  float pulse = (exp(sin(millis() / 2000.0 * PI)) - 0.36787944) * 108.0;
  uint8_t brightness = (uint8_t)pulse;
  uint8_t r, g, b;
  hsvToRgb(hue, 255, 255, &r, &g, &b);
  setStripColor(s, (r * brightness) / 255, (g * brightness) / 255, (b * brightness) / 255, 0);
}

void chaseEffect(int s, uint8_t hue) {
  StripState &S = strips[s];
  uint16_t n = pixels[s].numPixels();
  setStripColor(s, 0, 0, 0, 0);
  S.effectStep = (S.effectStep + 1) % n;
  uint8_t r, g, b;
  hsvToRgb(hue, 255, 255, &r, &g, &b);
  pixels[s].setPixelColor(S.effectStep, pixels[s].Color(r, g, b, 0));
  uint8_t tailLength = min(3, (int)(n / 4));
  for (int i = 1; i <= tailLength; i++) {
    int pos  = (S.effectStep - i + n) % n;
    uint8_t fade = 255 - (i * 255 / (tailLength + 1));
    pixels[s].setPixelColor(pos, pixels[s].Color((r * fade) / 255, (g * fade) / 255, (b * fade) / 255, 0));
  }
}

void rainbowEffect(int s) {
  StripState &S = strips[s];
  uint16_t n = pixels[s].numPixels();
  S.effectStep = (S.effectStep + 1) % 256;
  for (uint16_t i = 0; i < n; i++) {
    uint8_t pixelHue = (S.effectStep + (i * 256 / n)) & 255;
    uint8_t r, g, b;
    hsvToRgb(pixelHue, 255, 255, &r, &g, &b);
    pixels[s].setPixelColor(i, pixels[s].Color(r, g, b, 0));
  }
}
