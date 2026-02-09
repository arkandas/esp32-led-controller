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

// WiFi credentials
const char* ssid = "<YOUR_WIFI_SSID>";
const char* password = "<YOUR_WIFI_PASSWORD>";

// Device settings
#define DEVICE_NAME "<YOUR_ALEXA_DEVICE_NAME>"
#define WIFI_HOSTNAME "<YOUR_DEVICE_HOSTNAME>"
#define WEB_SERVER_PORT <YOUR_WEB_SERVER_PORT>

// LED Strip settings (WS2814 24V RGBW on IO40)
#define NUM_LEDS <YOUR_LED_COUNT>
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

// WiFi connection status
bool wifiConnected = false;
int wifiErrorCode = 0; // 0 = no error, 1 = connection failed, 2 = SSID not found, 3 = timeout

// LED strip (RGBW) using Adafruit NeoPixel
Adafruit_NeoPixel strip(NUM_LEDS, LED_STRIP_PIN, NEO_WRGB + NEO_KHZ800);

// Status LEDs (RGB) using FastLED
CRGB statusLeds[NUM_STATUS_LEDS];

// State variables
bool isPoweredOn = true;
int brightnessLevel = 10;    // Start at full brightness (10 = 100%)
int currentMode = 0;         // Start with white (index 0 in the colorModes array)
unsigned long lastButtonPressTime = 0;
const unsigned long debounceDelay = 200; // Debounce time in milliseconds

// Custom color variables (RGBW)
uint8_t customRed = 255;
uint8_t customGreen = 255;
uint8_t customBlue = 255;
uint8_t customWhite = 0;
bool useCustomColor = false;

// Power button long press variables
bool powerButtonPressed = false;
unsigned long powerButtonPressStartTime = 0;
const unsigned long powerButtonLongPressDelay = 500; // 0.5 seconds for long press

// Error display variables
unsigned long lastErrorBlinkTime = 0;
const int errorBlinkInterval = 500;  // ms
bool errorLedState = false;
int errorBlinkCount = 0;
int errorCycleCount = 0;

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
  // Initialize Serial for debugging
  Serial.begin(115200);
  Serial.println("ESP32-S3 LED Controller V2 - WS2814 RGBW & SK6805 Status LEDs");

  // Initialize status LEDs first for error indication
  FastLED.addLeds<STATUS_LED_TYPE, STATUS_LED_PIN, STATUS_COLOR_ORDER>(statusLeds, NUM_STATUS_LEDS);

  // Validate configuration
  if (String(ssid) == "<YOUR_WIFI_SSID>" || String(password) == "<YOUR_WIFI_PASSWORD>" ||
      String(DEVICE_NAME) == "<YOUR_ALEXA_DEVICE_NAME>" || String(WIFI_HOSTNAME) == "<YOUR_DEVICE_HOSTNAME>") {
    Serial.println("ERROR: Please configure your settings in the code!");
    Serial.println("Update ssid, password, DEVICE_NAME, WIFI_HOSTNAME, WEB_SERVER_PORT, and NUM_LEDS.");
    // Flash red on all status LEDs to indicate configuration error
    while(true) {
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
    // Read initial temperature
    readTemperature();
  } else {
    Serial.println("WARNING: Could not find AHT20 sensor!");
    ahtAvailable = false;
  }

  // Initialize LED strip (RGBW)
  strip.begin();
  strip.show(); // Initialize all pixels to 'off'

  // Set initial status LED state - Power LED on (green)
  fill_solid(statusLeds, NUM_STATUS_LEDS, CRGB::Black);
  statusLeds[LED_POWER] = CRGB(0, STATUS_LED_BRIGHTNESS, 0);  // Power LED green
  FastLED.show();

  // Initialize buttons with pullup resistors
  pinMode(BTN_MINUS, INPUT_PULLUP);
  pinMode(BTN_PLUS, INPUT_PULLUP);
  pinMode(BTN_MODE, INPUT_PULLUP);
  pinMode(BTN_POWER, INPUT_PULLUP);

  // Connect to WiFi
  connectToWiFi();

  if (wifiConnected) {
    // Set up Alexa first
    Serial.println("Setting up Alexa...");

    // Initialize fauxmo - it will handle Alexa discovery automatically
    fauxmo.createServer(true);  // Create internal TCP server for Alexa communication
    fauxmo.setPort(80);         // Port 80 required for gen3 Alexa devices
    fauxmo.enable(true);        // Enable after WiFi is connected
    fauxmo.addDevice(DEVICE_NAME);
    Serial.printf("Fauxmo device '%s' added and enabled\r\n", DEVICE_NAME);

    // Set the callback
    fauxmo.onSetState([](unsigned char device_id, const char * device_name, bool state, unsigned char value) {
        Serial.printf("[ALEXA] Device #%d (%s) state: %s\r\n", device_id, device_name, state ? "ON" : "OFF");

        if (strcmp(device_name, DEVICE_NAME) == 0) {
            if (state) {
                // Turn on lights with white color at max brightness
                isPoweredOn = true;
                currentMode = 0;  // Pure White mode
                brightnessLevel = 10;  // Max brightness
                updateLEDStrip();
            } else {
                // Turn off lights
                isPoweredOn = false;
                setStripColor(0, 0, 0, 0);
                strip.show();
            }
            updateStatusLEDs();
        }
    });

    Serial.println("Alexa integration setup complete");

    // Then set up web server
    setupWebServer();
  }

  // Show initial state
  updateStatusLEDs();
  updateLEDStrip();
}

void loop() {
  // Handle WiFi error codes with LED flashes if not connected
  if (!wifiConnected && wifiErrorCode > 0) {
    displayErrorCode();
  } else {
    // Process normal operation
    // Check all button states
    checkButtons();

    if (wifiConnected) {
      // Handle Alexa requests
      fauxmo.handle();

      // Debug message every 30 seconds
      static unsigned long lastMsg = 0;
      if (millis() - lastMsg > 30000) {
        lastMsg = millis();
        Serial.printf("Alexa device '%s' is ready. IP: %s\r\n", DEVICE_NAME, WiFi.localIP().toString().c_str());
        Serial.printf("Web interface available at: http://%s:%d\r\n", WiFi.localIP().toString().c_str(), WEB_SERVER_PORT);
      }
    }

    // Update status LEDs based on power state
    updateStatusLEDs();

    // Update LED strip effects if needed
    unsigned long currentMillis = millis();
    if (isPoweredOn && currentMillis - lastEffectTime > effectSpeed) {
      updateLEDStripEffects();
      lastEffectTime = currentMillis;
    }

    // Read temperature periodically
    if (ahtAvailable && currentMillis - lastTempRead > tempReadInterval) {
      readTemperature();
      lastTempRead = currentMillis;
    }
  }

  // Small delay to reduce CPU usage
  delay(10);
}

void readTemperature() {
  if (!ahtAvailable) return;

  sensors_event_t humidity, temp;
  if (aht.getEvent(&humidity, &temp)) {
    currentTemperature = temp.temperature;
    currentHumidity = humidity.relative_humidity;
    Serial.printf("Temperature: %.1f°C, Humidity: %.1f%%\r\n", currentTemperature, currentHumidity);
  }
}

// Helper function to set all strip LEDs to one RGBW color
void setStripColor(uint8_t r, uint8_t g, uint8_t b, uint8_t w) {
  for (int i = 0; i < NUM_LEDS; i++) {
    strip.setPixelColor(i, strip.Color(r, g, b, w));
  }
}

void connectToWiFi() {
  Serial.print("Connecting to WiFi: ");
  Serial.println(ssid);

  // Set custom hostname (this is what appears on your router)
  WiFi.setHostname(WIFI_HOSTNAME);
  Serial.printf("WiFi hostname set to: %s\r\n", WIFI_HOSTNAME);

  // Set status LEDs during connection attempt
  // Power LED stays green, WiFi LED blue (connecting), Strip LED off
  statusLeds[LED_POWER] = CRGB(0, STATUS_LED_BRIGHTNESS, 0);  // Power: Green
  statusLeds[LED_WIFI] = CRGB(0, 0, STATUS_LED_BRIGHTNESS);   // WiFi: Blue (connecting)
  statusLeds[LED_STRIP] = CRGB::Black;                        // Strip: Off
  FastLED.show();

  // Set LED strip to blue during connection attempt
  setStripColor(0, 0, 255, 0);
  strip.setBrightness(255);
  strip.show();

  // Connect to WiFi
  WiFi.begin(ssid, password);

  // Wait for connection with timeout
  int connectionAttempts = 0;
  while (WiFi.status() != WL_CONNECTED && connectionAttempts < 20) { // 10 second timeout
    delay(500);
    Serial.print(".");
    connectionAttempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("");
    Serial.println("WiFi connected successfully");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    wifiConnected = true;
    wifiErrorCode = 0;

    // Flash yellow on all status LEDs and strip five times to indicate success
    for (int i = 0; i < 5; i++) {
      // Yellow on all status LEDs
      fill_solid(statusLeds, NUM_STATUS_LEDS, CRGB(STATUS_LED_BRIGHTNESS, STATUS_LED_BRIGHTNESS, 0));
      FastLED.show();

      // Flash LED strip yellow (R+G, no W)
      setStripColor(255, 255, 0, 0);
      strip.show();
      delay(200);

      // All off
      fill_solid(statusLeds, NUM_STATUS_LEDS, CRGB::Black);
      FastLED.show();

      setStripColor(0, 0, 0, 0);
      strip.show();
      delay(200);
    }
  } else {
    Serial.println("");
    Serial.println("Failed to connect to WiFi");
    wifiConnected = false;

    // Set LED strip to red to indicate failure
    setStripColor(255, 0, 0, 0);
    strip.show();

    // Set WiFi status LED to red
    statusLeds[LED_WIFI] = CRGB(STATUS_LED_BRIGHTNESS, 0, 0);  // WiFi: Red (error)
    FastLED.show();

    if (WiFi.status() == WL_NO_SSID_AVAIL) {
      Serial.println("SSID not found");
      wifiErrorCode = 2;
    } else if (WiFi.status() == WL_CONNECT_FAILED) {
      Serial.println("Connection failed - check password");
      wifiErrorCode = 1;
    } else {
      Serial.println("Connection timeout");
      wifiErrorCode = 3;
    }
  }
}

void setupWebServer() {
  // Set up web server routes
  server.on("/", HTTP_GET, handleRoot);
  server.on("/control", HTTP_GET, handleControl);
  server.on("/toggle", HTTP_GET, handleToggle);
  server.on("/brightness", HTTP_GET, handleBrightness);
  server.on("/mode", HTTP_GET, handleMode);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/color", HTTP_GET, handleCustomColor);

  // Serve static files from LittleFS
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

  // Start server
  server.begin();
  Serial.printf("HTTP server started on port %d\r\n", WEB_SERVER_PORT);
  Serial.printf("Web interface: http://%s:%d\r\n", WiFi.localIP().toString().c_str(), WEB_SERVER_PORT);
}

void handleRoot(AsyncWebServerRequest *request) {
  // Serve the HTML file from LittleFS
  if (LittleFS.exists("/index.html")) {
    request->send(LittleFS, "/index.html", "text/html");
  } else {
    Serial.println("ERROR: index.html not found in LittleFS");
    request->send(404, "text/plain", "File not found. Please upload the data folder to LittleFS.");
  }
}

void handleControl(AsyncWebServerRequest *request) {
  // Simple control interface
  String html = "<html><body>";
  html += "<h1>WS2814 RGBW LED Control</h1>";
  html += "<p>Power: <a href='/toggle'>Toggle</a></p>";
  html += "<p>Brightness: ";
  for (int i = 1; i <= 10; i++) {
    html += "<a href='/brightness?level=" + String(i) + "'>" + String(i*10) + "%</a> ";
  }
  html += "</p>";
  html += "<p>Mode: ";
  for (int i = 0; i < MAX_MODES; i++) {
    html += "<a href='/mode?mode=" + String(i) + "'>" + String(i) + "</a> ";
  }
  html += "</p>";
  html += "</body></html>";
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
      useCustomColor = false;  // Disable custom color when selecting preset mode
      updateLEDStrip();
    }
  }
  request->send(200, "text/plain", "OK");
}

void handleStatus(AsyncWebServerRequest *request) {
  String status = "{";
  status += "\"isPoweredOn\":" + String(isPoweredOn ? "true" : "false") + ",";
  status += "\"brightnessLevel\":" + String(brightnessLevel) + ",";
  status += "\"currentMode\":" + String(currentMode) + ",";
  status += "\"deviceName\":\"" + String(DEVICE_NAME) + "\",";
  status += "\"macAddress\":\"" + WiFi.macAddress() + "\",";
  status += "\"wifiSSID\":\"" + String(ssid) + "\",";
  status += "\"signalStrength\":" + String(WiFi.RSSI()) + ",";
  status += "\"uptime\":" + String(millis() / 1000) + ",";
  status += "\"useCustomColor\":" + String(useCustomColor ? "true" : "false") + ",";
  status += "\"customRed\":" + String(customRed) + ",";
  status += "\"customGreen\":" + String(customGreen) + ",";
  status += "\"customBlue\":" + String(customBlue) + ",";
  status += "\"customWhite\":" + String(customWhite) + ",";
  status += "\"temperature\":" + String(currentTemperature, 1) + ",";
  status += "\"humidity\":" + String(currentHumidity, 1) + ",";
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

    // Validate RGBW values
    if (r >= 0 && r <= 255 && g >= 0 && g <= 255 && b >= 0 && b <= 255 && w >= 0 && w <= 255) {
      customRed = r;
      customGreen = g;
      customBlue = b;
      customWhite = w;
      useCustomColor = true;
      updateLEDStrip();
    }
  }
  request->send(200, "text/plain", "OK");
}

void checkButtons() {
  // Only process regular button presses after debounce delay
  unsigned long currentMillis = millis();

  // Handle power button with long press
  if (digitalRead(BTN_POWER) == LOW) {
    // If button was not previously pressed, record the time
    if (!powerButtonPressed) {
      powerButtonPressed = true;
      powerButtonPressStartTime = currentMillis;
    }
    // Check if the button has been pressed long enough
    else if (currentMillis - powerButtonPressStartTime >= powerButtonLongPressDelay) {
      // Long press detected - toggle power
      isPoweredOn = !isPoweredOn;
      Serial.print("Power: ");
      Serial.println(isPoweredOn ? "ON" : "OFF");

      // Reset state to prevent multiple toggles
      powerButtonPressed = false;
      lastButtonPressTime = currentMillis;
      updateStatusLEDs();
      updateLEDStrip();
    }
  } else {
    // Button released
    powerButtonPressed = false;
  }

  // Only process other buttons after debounce delay
  if (currentMillis - lastButtonPressTime < debounceDelay) {
    return;
  }

  // Check minus button (decrease brightness)
  if (digitalRead(BTN_MINUS) == LOW) {
    brightnessLevel = max(1, brightnessLevel - 1);
    Serial.print("Brightness: ");
    Serial.println(brightnessLevel * 10);
    lastButtonPressTime = currentMillis;
    updateLEDStrip();
  }

  // Check plus button (increase brightness)
  if (digitalRead(BTN_PLUS) == LOW) {
    brightnessLevel = min(10, brightnessLevel + 1);
    Serial.print("Brightness: ");
    Serial.println(brightnessLevel * 10);
    lastButtonPressTime = currentMillis;
    updateLEDStrip();
  }

  // Check mode button (change color)
  if (digitalRead(BTN_MODE) == LOW) {
    currentMode = (currentMode + 1) % MAX_MODES;
    useCustomColor = false;  // Disable custom color when changing modes via button
    Serial.print("Mode: ");
    Serial.println(currentMode);
    lastButtonPressTime = currentMillis;
    updateLEDStrip();
  }
}

void displayErrorCode() {
  unsigned long currentMillis = millis();

  // Check if it's time to toggle the LED
  if (currentMillis - lastErrorBlinkTime >= errorBlinkInterval) {
    lastErrorBlinkTime = currentMillis;

    // Toggle LED state
    errorLedState = !errorLedState;
    errorBlinkCount++;

    // Yellow flash for WiFi LED during error
    if (errorLedState) {
      // Power LED stays green
      statusLeds[LED_POWER] = CRGB(0, STATUS_LED_BRIGHTNESS, 0);
      // WiFi LED yellow (error indication)
      statusLeds[LED_WIFI] = CRGB(STATUS_LED_BRIGHTNESS, STATUS_LED_BRIGHTNESS, 0);
      // Strip status LED off
      statusLeds[LED_STRIP] = CRGB::Black;
    } else {
      // Power LED stays green
      statusLeds[LED_POWER] = CRGB(0, STATUS_LED_BRIGHTNESS, 0);
      // WiFi LED off
      statusLeds[LED_WIFI] = CRGB::Black;
      // Strip status LED off
      statusLeds[LED_STRIP] = CRGB::Black;
    }
    FastLED.show();

    // After 60 blinks (about 30 seconds), try to reconnect
    if (errorBlinkCount >= 60) {
      errorBlinkCount = 0;
      connectToWiFi();
    }
  }
}

void updateStatusLEDs() {
  // LED1 (Power): Always green when powered
  statusLeds[LED_POWER] = CRGB(0, STATUS_LED_BRIGHTNESS, 0);

  // LED2 (WiFi): Green if connected, Red if not
  if (wifiConnected) {
    statusLeds[LED_WIFI] = CRGB(0, STATUS_LED_BRIGHTNESS, 0);  // Green
  } else {
    statusLeds[LED_WIFI] = CRGB(STATUS_LED_BRIGHTNESS, 0, 0);  // Red
  }

  // LED3 (Strip): Green if strip is on, Red if off
  if (isPoweredOn) {
    statusLeds[LED_STRIP] = CRGB(0, STATUS_LED_BRIGHTNESS, 0);  // Green
  } else {
    statusLeds[LED_STRIP] = CRGB(STATUS_LED_BRIGHTNESS, 0, 0);  // Red
  }

  FastLED.show();
}

void updateLEDStrip() {
  // Calculate brightness (map 1-10 to 25-255)
  uint8_t brightness = map(brightnessLevel, 1, 10, 25, 255);
  strip.setBrightness(brightness);

  if (!isPoweredOn) {
    // Turn off all LEDs on strip
    setStripColor(0, 0, 0, 0);
    strip.show();
    return;
  }

  // Apply color based on mode or custom color
  if (useCustomColor) {
    // Use custom RGBW color
    setStripColor(customRed, customGreen, customBlue, customWhite);
  } else if (currentMode < MAX_MODES - 1) {
    // Use predefined color mode (not effects mode)
    setStripColor(colorModes[currentMode][0], colorModes[currentMode][1],
                  colorModes[currentMode][2], colorModes[currentMode][3]);
  }

  // Show the changes
  strip.show();

  // Debug output
  if (useCustomColor) {
    Serial.print("LED Strip Update - Custom Color: R=");
    Serial.print(customRed);
    Serial.print(", G=");
    Serial.print(customGreen);
    Serial.print(", B=");
    Serial.print(customBlue);
    Serial.print(", W=");
    Serial.print(customWhite);
  } else {
    Serial.print("LED Strip Update - Mode: ");
    Serial.print(currentMode);
  }
  Serial.print(", Brightness: ");
  Serial.println(brightnessLevel * 10);
}

void updateLEDStripEffects() {
  // Only process effects if power is on
  if (!isPoweredOn) return;

  // Don't override custom colors with effects
  if (useCustomColor) return;

  // Special effects only for mode 9 (effects mode)
  if (currentMode < MAX_MODES - 1) {
    // For modes 0-8: just show solid colors, already handled in updateLEDStrip()
    return;
  }

  // Mode 9: Cycle through effects
  static uint8_t effectSubMode = 0;
  static unsigned long lastEffectChange = 0;
  static uint8_t hue = 0;
  static uint8_t breathHue = 0;

  // Change effect every 10 seconds
  if (millis() - lastEffectChange > 10000) {
    effectSubMode = (effectSubMode + 1) % 3; // 3 different effects
    lastEffectChange = millis();
    Serial.print("Effect changed to: ");
    Serial.println(effectSubMode);
  }

  // Apply the current effect
  switch (effectSubMode) {
    case 0:
      // Rainbow effect
      rainbowEffect();
      break;

    case 1:
      // Chase effect with changing colors
      hue += 3;
      chaseEffect(hue);
      break;

    case 2:
      // Breathing effect with changing colors
      breathHue++;
      breatheEffect(breathHue);
      break;
  }

  strip.show();
}

// Convert HSV to RGB for effects
void hsvToRgb(uint8_t h, uint8_t s, uint8_t v, uint8_t* r, uint8_t* g, uint8_t* b) {
  if (s == 0) {
    *r = *g = *b = v;
    return;
  }

  uint8_t region = h / 43;
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
  // Simple sine wave breathing effect
  float pulse = (exp(sin(millis()/2000.0*PI)) - 0.36787944) * 108.0;
  uint8_t brightness = pulse;

  uint8_t r, g, b;
  hsvToRgb(hue, 255, 255, &r, &g, &b);

  // Apply breathing brightness
  r = (r * brightness) / 255;
  g = (g * brightness) / 255;
  b = (b * brightness) / 255;

  setStripColor(r, g, b, 0);
}

void chaseEffect(uint8_t hue) {
  // Moving dot chase effect
  setStripColor(0, 0, 0, 0);

  effectStep = (effectStep + 1) % NUM_LEDS;

  uint8_t r, g, b;
  hsvToRgb(hue, 255, 255, &r, &g, &b);

  strip.setPixelColor(effectStep, strip.Color(r, g, b, 0));

  // Add a tail
  uint8_t tailLength = min(3, NUM_LEDS/4);
  for (int i = 1; i <= tailLength; i++) {
    int pos = (effectStep - i + NUM_LEDS) % NUM_LEDS;
    uint8_t fade = 255 - (i * 255 / (tailLength + 1));
    uint8_t fr = (r * fade) / 255;
    uint8_t fg = (g * fade) / 255;
    uint8_t fb = (b * fade) / 255;
    strip.setPixelColor(pos, strip.Color(fr, fg, fb, 0));
  }
}

void rainbowEffect() {
  // Moving rainbow effect
  effectStep = (effectStep + 1) % 256;

  for (int i = 0; i < NUM_LEDS; i++) {
    uint8_t pixelHue = (effectStep + (i * 256 / NUM_LEDS)) & 255;
    uint8_t r, g, b;
    hsvToRgb(pixelHue, 255, 255, &r, &g, &b);
    strip.setPixelColor(i, strip.Color(r, g, b, 0));
  }
}
