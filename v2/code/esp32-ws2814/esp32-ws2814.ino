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
#include "config.h"  // Copy config.h.example → config.h and fill in your values

// WiFi credentials (defined in config.h)
const char* ssid     = WIFI_SSID;
const char* password = WIFI_PASSWORD;

// LED Strip settings (WS2814 24V RGBW on IO40)
// NUM_LEDS defined in config.h
#define LED_STRIP_PIN 40

// Status LEDs settings (3x SK6805-EC20 on IO41)
#define STATUS_LED_PIN 41
#define NUM_STATUS_LEDS 3
#define STATUS_LED_TYPE WS2812
#define STATUS_COLOR_ORDER GRB

// Status LED indices
#define LED_POWER 0    // LED1: Power indicator (always on when powered)
#define LED_WIFI 1     // LED2: WiFi status
#define LED_STRIP 2    // LED3: LED strip on/off status

// I2C pins for AHT20 temperature sensor
#define I2C_SDA 2
#define I2C_SCL 1

// Button pins
#define BTN_MINUS 4  // Brightness decrease
#define BTN_PLUS 5   // Brightness increase
#define BTN_MODE 6   // Mode change
#define BTN_POWER 7  // Power toggle

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
const unsigned long tempReadInterval = 5000; // Read every 5 seconds

// WiFi connection state — written from the WiFi event task, read from loop
volatile bool wifiConnected   = false;
volatile bool isReconnecting  = false;
volatile int  reconnectAttempts = 0;
const int MAX_RECONNECT_ATTEMPTS = 10;  // Restart device after 10 consecutive failures

// LED strip (RGBW) using Adafruit NeoPixel
Adafruit_NeoPixel strip(NUM_LEDS, LED_STRIP_PIN, NEO_WRGB + NEO_KHZ800);

// Status LEDs (RGB) using FastLED
CRGB statusLeds[NUM_STATUS_LEDS];

// State variables
bool isPoweredOn = false;         // Strip starts OFF — turn on via Alexa or buttons
int brightnessLevel = 10;         // Full brightness when turned on
int currentMode = 0;              // Warm white
unsigned long lastButtonPressTime = 0;
const unsigned long debounceDelay = 200;

// Custom color variables (RGBW)
uint8_t customRed = 255;
uint8_t customGreen = 255;
uint8_t customBlue = 255;
uint8_t customWhite = 0;
bool useCustomColor = false;

// Power button long press variables
bool powerButtonPressed = false;
unsigned long powerButtonPressStartTime = 0;
const unsigned long powerButtonLongPressDelay = 500;

// Color definitions for RGBW LEDs (R, G, B, W)
const int MAX_MODES = 10;
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

// Effect variables
unsigned long lastEffectTime = 0;
const int effectSpeed = 50;
int effectStep = 0;

// Status LED brightness (dimmed for status indication)
const uint8_t STATUS_LED_BRIGHTNESS = 10;  // ~4% brightness

void setup() {
  Serial.begin(115200);
  Serial.println("ESP32-S3 LED Controller V2 - WS2814 RGBW & SK6805 Status LEDs");

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

  // Initialize I2C for AHT20
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

  // Initialize LED strip — all off
  strip.begin();
  strip.show();

  // Power LED on, others off while connecting
  fill_solid(statusLeds, NUM_STATUS_LEDS, CRGB::Black);
  statusLeds[LED_POWER] = CRGB(0, STATUS_LED_BRIGHTNESS, 0);
  FastLED.show();

  // Initialize buttons
  pinMode(BTN_MINUS, INPUT_PULLUP);
  pinMode(BTN_PLUS,  INPUT_PULLUP);
  pinMode(BTN_MODE,  INPUT_PULLUP);
  pinMode(BTN_POWER, INPUT_PULLUP);

  // Connect to WiFi
  connectToWiFi();

  if (wifiConnected) {
    Serial.println("Setting up Alexa...");
    fauxmo.createServer(true);
    fauxmo.setPort(80);
    fauxmo.enable(true);
    fauxmo.addDevice(DEVICE_NAME);
    Serial.printf("Fauxmo device '%s' added\r\n", DEVICE_NAME);

    fauxmo.onSetState([](unsigned char device_id, const char * device_name, bool state, unsigned char value) {
      Serial.printf("[ALEXA] Device #%d (%s) state: %s\r\n", device_id, device_name, state ? "ON" : "OFF");
      if (strcmp(device_name, DEVICE_NAME) == 0) {
        if (state) {
          isPoweredOn = true;
          currentMode = 0;
          brightnessLevel = 10;
          updateLEDStrip();
        } else {
          isPoweredOn = false;
          setStripColor(0, 0, 0, 0);
          strip.show();
        }
        updateStatusLEDs();
      }
    });

    Serial.println("Alexa integration setup complete");
    setupWebServer();
    setupOTA();
  }

  // Show initial state — strip stays OFF (isPoweredOn = false)
  updateStatusLEDs();
  updateLEDStrip();
}

void loop() {
  unsigned long now = millis();

  checkButtons();

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

  if (isPoweredOn && now - lastEffectTime > effectSpeed) {
    updateLEDStripEffects();
    lastEffectTime = now;
  }

  if (ahtAvailable && now - lastTempRead > tempReadInterval) {
    readTemperature();
    lastTempRead = now;
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

  // Show blue on WiFi LED and strip during connection
  statusLeds[LED_POWER] = CRGB(0, STATUS_LED_BRIGHTNESS, 0);
  statusLeds[LED_WIFI]  = CRGB(0, 0, STATUS_LED_BRIGHTNESS);
  statusLeds[LED_STRIP] = CRGB::Black;
  FastLED.show();

  setStripColor(0, 0, 255, 0);
  strip.setBrightness(255);
  strip.show();

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
      setStripColor(255, 255, 0, 0);
      strip.show();
      delay(200);

      fill_solid(statusLeds, NUM_STATUS_LEDS, CRGB::Black);
      FastLED.show();
      setStripColor(0, 0, 0, 0);
      strip.show();
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

void setStripColor(uint8_t r, uint8_t g, uint8_t b, uint8_t w) {
  for (int i = 0; i < NUM_LEDS; i++) {
    strip.setPixelColor(i, strip.Color(r, g, b, w));
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

void setupWebServer() {
  server.on("/",          HTTP_GET, handleRoot);
  server.on("/control",   HTTP_GET, handleControl);
  server.on("/toggle",    HTTP_GET, handleToggle);
  server.on("/brightness",HTTP_GET, handleBrightness);
  server.on("/mode",      HTTP_GET, handleMode);
  server.on("/status",    HTTP_GET, handleStatus);
  server.on("/color",     HTTP_GET, handleCustomColor);

  server.on("/style.css", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (LittleFS.exists("/style.css")) {
      request->send(LittleFS, "/style.css", "text/css");
    } else {
      request->send(404, "text/plain", "CSS file not found");
    }
  });

  server.on("/app.js", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (LittleFS.exists("/app.js")) {
      request->send(LittleFS, "/app.js", "application/javascript");
    } else {
      request->send(404, "text/plain", "JavaScript file not found");
    }
  });

  server.begin();
  Serial.printf("HTTP server started on port %d\r\n", WEB_SERVER_PORT);
}

void handleRoot(AsyncWebServerRequest *request) {
  if (LittleFS.exists("/index.html")) {
    request->send(LittleFS, "/index.html", "text/html");
  } else {
    request->send(404, "text/plain", "File not found. Please upload the data folder to LittleFS.");
  }
}

void handleControl(AsyncWebServerRequest *request) {
  String html = "<html><body><h1>WS2814 RGBW LED Control</h1>";
  html += "<p>Power: <a href='/toggle'>Toggle</a></p>";
  html += "<p>Brightness: ";
  for (int i = 1; i <= 10; i++) {
    html += "<a href='/brightness?level=" + String(i) + "'>" + String(i*10) + "%</a> ";
  }
  html += "</p><p>Mode: ";
  for (int i = 0; i < MAX_MODES; i++) {
    html += "<a href='/mode?mode=" + String(i) + "'>" + String(i) + "</a> ";
  }
  html += "</p></body></html>";
  request->send(200, "text/html", html);
}

void handleToggle(AsyncWebServerRequest *request) {
  isPoweredOn = !isPoweredOn;
  updateStatusLEDs();
  updateLEDStrip();
  request->send(200, "text/plain", "OK");
}

void handleBrightness(AsyncWebServerRequest *request) {
  if (request->hasArg("level")) {
    int level = request->arg("level").toInt();
    if (level >= 1 && level <= 10) {
      brightnessLevel = level;
      updateLEDStrip();
    }
  }
  request->send(200, "text/plain", "OK");
}

void handleMode(AsyncWebServerRequest *request) {
  if (request->hasArg("mode")) {
    int mode = request->arg("mode").toInt();
    if (mode >= 0 && mode < MAX_MODES) {
      currentMode = mode;
      useCustomColor = false;
      updateLEDStrip();
    }
  }
  request->send(200, "text/plain", "OK");
}

void handleStatus(AsyncWebServerRequest *request) {
  String status = "{";
  status += "\"isPoweredOn\":"     + String(isPoweredOn ? "true" : "false") + ",";
  status += "\"brightnessLevel\":" + String(brightnessLevel) + ",";
  status += "\"currentMode\":"     + String(currentMode) + ",";
  status += "\"deviceName\":\""    + String(DEVICE_NAME) + "\",";
  status += "\"macAddress\":\""    + WiFi.macAddress() + "\",";
  status += "\"wifiSSID\":\""      + String(ssid) + "\",";
  status += "\"signalStrength\":"  + String(WiFi.RSSI()) + ",";
  status += "\"uptime\":"          + String(millis() / 1000) + ",";
  status += "\"useCustomColor\":"  + String(useCustomColor ? "true" : "false") + ",";
  status += "\"customRed\":"       + String(customRed) + ",";
  status += "\"customGreen\":"     + String(customGreen) + ",";
  status += "\"customBlue\":"      + String(customBlue) + ",";
  status += "\"customWhite\":"     + String(customWhite) + ",";
  status += "\"temperature\":"     + String(currentTemperature, 1) + ",";
  status += "\"humidity\":"        + String(currentHumidity, 1) + ",";
  status += "\"sensorAvailable\":" + String(ahtAvailable ? "true" : "false");
  status += "}";
  request->send(200, "application/json", status);
}

void handleCustomColor(AsyncWebServerRequest *request) {
  if (request->hasArg("r") && request->hasArg("g") && request->hasArg("b")) {
    int r = request->arg("r").toInt();
    int g = request->arg("g").toInt();
    int b = request->arg("b").toInt();
    int w = request->hasArg("w") ? request->arg("w").toInt() : 0;
    if (r >= 0 && r <= 255 && g >= 0 && g <= 255 && b >= 0 && b <= 255 && w >= 0 && w <= 255) {
      customRed   = r;
      customGreen = g;
      customBlue  = b;
      customWhite = w;
      useCustomColor = true;
      updateLEDStrip();
    }
  }
  request->send(200, "text/plain", "OK");
}

// ── Buttons ──────────────────────────────────────────────────────────────────

void checkButtons() {
  unsigned long now = millis();

  // Power button — long press to toggle
  if (digitalRead(BTN_POWER) == LOW) {
    if (!powerButtonPressed) {
      powerButtonPressed = true;
      powerButtonPressStartTime = now;
    } else if (now - powerButtonPressStartTime >= powerButtonLongPressDelay) {
      isPoweredOn = !isPoweredOn;
      Serial.printf("Power: %s\r\n", isPoweredOn ? "ON" : "OFF");
      powerButtonPressed = false;
      lastButtonPressTime = now;
      updateStatusLEDs();
      updateLEDStrip();
    }
  } else {
    powerButtonPressed = false;
  }

  if (now - lastButtonPressTime < debounceDelay) return;

  if (digitalRead(BTN_MINUS) == LOW) {
    brightnessLevel = max(1, brightnessLevel - 1);
    Serial.printf("Brightness: %d%%\r\n", brightnessLevel * 10);
    lastButtonPressTime = now;
    updateLEDStrip();
  }

  if (digitalRead(BTN_PLUS) == LOW) {
    brightnessLevel = min(10, brightnessLevel + 1);
    Serial.printf("Brightness: %d%%\r\n", brightnessLevel * 10);
    lastButtonPressTime = now;
    updateLEDStrip();
  }

  if (digitalRead(BTN_MODE) == LOW) {
    currentMode = (currentMode + 1) % MAX_MODES;
    useCustomColor = false;
    Serial.printf("Mode: %d\r\n", currentMode);
    lastButtonPressTime = now;
    updateLEDStrip();
  }
}

// ── LED updates ──────────────────────────────────────────────────────────────

void updateStatusLEDs() {
  // LED1 — Power: always green
  statusLeds[LED_POWER] = CRGB(0, STATUS_LED_BRIGHTNESS, 0);

  // LED2 — WiFi: green=connected, yellow blink=reconnecting, red=failed
  bool actuallyConnected = (WiFi.status() == WL_CONNECTED);
  if (actuallyConnected) {
    statusLeds[LED_WIFI] = CRGB(0, STATUS_LED_BRIGHTNESS, 0);
  } else if (isReconnecting) {
    bool blink = (millis() % 1000) < 500;
    statusLeds[LED_WIFI] = blink
      ? CRGB(STATUS_LED_BRIGHTNESS, STATUS_LED_BRIGHTNESS, 0)  // yellow
      : CRGB::Black;
  } else {
    statusLeds[LED_WIFI] = CRGB(STATUS_LED_BRIGHTNESS, 0, 0);  // red
  }

  // LED3 — Strip: green=on, red=off
  statusLeds[LED_STRIP] = isPoweredOn
    ? CRGB(0, STATUS_LED_BRIGHTNESS, 0)
    : CRGB(STATUS_LED_BRIGHTNESS, 0, 0);

  FastLED.show();
}

void updateLEDStrip() {
  uint8_t brightness = map(brightnessLevel, 1, 10, 25, 255);
  strip.setBrightness(brightness);

  if (!isPoweredOn) {
    setStripColor(0, 0, 0, 0);
    strip.show();
    return;
  }

  if (useCustomColor) {
    setStripColor(customRed, customGreen, customBlue, customWhite);
    Serial.printf("Strip: Custom R=%d G=%d B=%d W=%d Bri=%d%%\r\n",
      customRed, customGreen, customBlue, customWhite, brightnessLevel * 10);
  } else if (currentMode < MAX_MODES - 1) {
    setStripColor(colorModes[currentMode][0], colorModes[currentMode][1],
                  colorModes[currentMode][2], colorModes[currentMode][3]);
    Serial.printf("Strip: Mode=%d Bri=%d%%\r\n", currentMode, brightnessLevel * 10);
  }

  strip.show();
}

void updateLEDStripEffects() {
  if (!isPoweredOn || useCustomColor) return;
  if (currentMode < MAX_MODES - 1) return;  // Only mode 9

  static uint8_t effectSubMode = 0;
  static unsigned long lastEffectChange = 0;
  static uint8_t hue = 0;
  static uint8_t breathHue = 0;

  if (millis() - lastEffectChange > 10000) {
    effectSubMode = (effectSubMode + 1) % 3;
    lastEffectChange = millis();
    Serial.printf("Effect changed to: %d\r\n", effectSubMode);
  }

  switch (effectSubMode) {
    case 0: rainbowEffect();       break;
    case 1: hue += 3; chaseEffect(hue);    break;
    case 2: breathHue++; breatheEffect(breathHue); break;
  }

  strip.show();
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

void breatheEffect(uint8_t hue) {
  float pulse = (exp(sin(millis() / 2000.0 * PI)) - 0.36787944) * 108.0;
  uint8_t brightness = (uint8_t)pulse;
  uint8_t r, g, b;
  hsvToRgb(hue, 255, 255, &r, &g, &b);
  setStripColor((r * brightness) / 255, (g * brightness) / 255, (b * brightness) / 255, 0);
}

void chaseEffect(uint8_t hue) {
  setStripColor(0, 0, 0, 0);
  effectStep = (effectStep + 1) % NUM_LEDS;
  uint8_t r, g, b;
  hsvToRgb(hue, 255, 255, &r, &g, &b);
  strip.setPixelColor(effectStep, strip.Color(r, g, b, 0));
  uint8_t tailLength = min(3, NUM_LEDS / 4);
  for (int i = 1; i <= tailLength; i++) {
    int pos  = (effectStep - i + NUM_LEDS) % NUM_LEDS;
    uint8_t fade = 255 - (i * 255 / (tailLength + 1));
    strip.setPixelColor(pos, strip.Color((r * fade) / 255, (g * fade) / 255, (b * fade) / 255, 0));
  }
}

void rainbowEffect() {
  effectStep = (effectStep + 1) % 256;
  for (int i = 0; i < NUM_LEDS; i++) {
    uint8_t pixelHue = (effectStep + (i * 256 / NUM_LEDS)) & 255;
    uint8_t r, g, b;
    hsvToRgb(pixelHue, 255, 255, &r, &g, &b);
    strip.setPixelColor(i, strip.Color(r, g, b, 0));
  }
}
