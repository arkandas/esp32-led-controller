#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <Adafruit_NeoPixel.h>
#include <fauxmoESP.h>
#include <AsyncTCP.h>
#include <LittleFS.h>

// WiFi credentials
const char* ssid = "<YOUR_WIFI_SSID>";
const char* password = "<YOUR_WIFI_PASSWORD>";

// Device settings
#define DEVICE_NAME "<YOUR_ALEXA_DEVICE_NAME>"
#define WIFI_HOSTNAME "<YOUR_DEVICE_HOSTNAME>"
#define WEB_SERVER_PORT <YOUR_WEB_SERVER_PORT>

// LED settings
#define NUM_LEDS    <YOUR_LED_COUNT>
#define LED_PIN 2

// Common Anode RGB LED for status
// Define the pins
#define RED_PIN 17
#define GREEN_PIN 16
#define BLUE_PIN 18

// Button pins
#define BTN_MINUS 4 // Brightness decrease
#define BTN_PLUS 5  // Brightness increase
#define BTN_MODE 6  // Mode change
#define BTN_POWER 7 // Power toggle

// PWM properties
#define FREQ 5000    // PWM frequency in Hz
#define RESOLUTION 8 // 8-bit resolution (0-255)

// Create NeoPixel strip object with SK6812 RGBW configuration
Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRBW + NEO_KHZ800);

// Web server on defined port (moved from 80 to avoid conflict with fauxmo)
AsyncWebServer server(WEB_SERVER_PORT);

// Alexa handler
fauxmoESP fauxmo;

// WiFi connection status
bool wifiConnected = false;
int wifiErrorCode = 0;  // 0 = no error, 1 = connection failed, 2 = SSID not found, 3 = timeout

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
uint8_t customWhite = 255;
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

// Color definitions for SK6812 RGBW LEDs
const int MAX_MODES = 10;
struct RGBWColor {
  uint8_t r, g, b, w;
};

RGBWColor colorModes[MAX_MODES] = {
  {255, 255, 255, 255}, // 0: Bright White (R+G+B+W for maximum brightness)
  {255, 0, 0, 0},       // 1: Red
  {0, 255, 0, 0},       // 2: Green
  {0, 0, 255, 0},       // 3: Blue
  {255, 255, 0, 0},     // 4: Yellow
  {255, 0, 255, 0},     // 5: Purple/Magenta
  {0, 255, 255, 0},     // 6: Cyan
  {255, 165, 0, 0},     // 7: Orange
  {0, 0, 0, 255},       // 8: Pure White (white channel only)
  {0, 0, 0, 0}          // 9: Effects mode (placeholder - not actually used)
};

// Effect variables
unsigned long lastEffectTime = 0;
const int effectSpeed = 50;
int effectStep = 0;

void setup() {
  // Initialize Serial for debugging
  Serial.begin(115200);
  Serial.println("ESP32 LED Controller - SK6812 RGBW & Status LED");

  // Validate configuration
  if (String(ssid) == "<YOUR_WIFI_SSID>" || String(password) == "<YOUR_WIFI_PASSWORD>") {
    Serial.println("ERROR: Please configure your WiFi credentials in the code!");
    Serial.println("Update the ssid and password variables with your actual WiFi details.");
    // Flash red to indicate configuration error
    while(true) {
      ledcWrite(RED_PIN, 0);    // Red on
      ledcWrite(GREEN_PIN, 255); // Green off  
      ledcWrite(BLUE_PIN, 255);  // Blue off
      delay(500);
      ledcWrite(RED_PIN, 255);   // Red off
      delay(500);
    }
  }

  // Initialize LittleFS
  if (!LittleFS.begin(true)) {
    Serial.println("An error occurred while mounting LittleFS");
    return;
  }
  Serial.println("LittleFS initialized successfully");

  // Initialize NeoPixel strip
  strip.begin();
  strip.setBrightness(255);  // Start at full brightness
  strip.clear();
  strip.show();
  Serial.println("SK6812 RGBW strip initialized");

  // Configure common anode LED PWM properties
  ledcAttach(RED_PIN, FREQ, RESOLUTION);
  ledcAttach(GREEN_PIN, FREQ, RESOLUTION);
  ledcAttach(BLUE_PIN, FREQ, RESOLUTION);

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
    fauxmo.addDevice(DEVICE_NAME);
    fauxmo.setPort(80);
    fauxmo.enable(true);
    Serial.printf("Fauxmo device '%s' added and enabled\r\n", DEVICE_NAME);

    // Set the callback
    fauxmo.onSetState([](unsigned char device_id, const char * device_name, bool state, unsigned char value) {
        Serial.printf("[ALEXA] Device #%d (%s) state: %s\r\n", device_id, device_name, state ? "ON" : "OFF");

        if (strcmp(device_name, DEVICE_NAME) == 0) {
            if (state) {
                // Turn on lights with bright white color at max brightness
                isPoweredOn = true;
                currentMode = 0;  // Bright white mode
                brightnessLevel = 10;  // Max brightness
                strip.setBrightness(255);
                setAllPixels(colorModes[0].r, colorModes[0].g, colorModes[0].b, colorModes[0].w);
                strip.show();
            } else {
                // Turn off lights
                isPoweredOn = false;
                strip.clear();
                strip.show();
            }
            updateStatusLED();
        }
    });

    Serial.println("Alexa integration setup complete");

    // Then set up web server
    setupWebServer();
  }

  // Show initial state
  updateStatusLED();
  updateSK6812();
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

    // Update status LED based on power state
    updateStatusLED();

    // Update SK6812 effects if needed
    unsigned long currentMillis = millis();
    if (isPoweredOn && currentMillis - lastEffectTime > effectSpeed) {
      updateSK6812Effects();
      lastEffectTime = currentMillis;
    }
  }

  // Small delay to reduce CPU usage
  delay(10);
}

void connectToWiFi() {
  Serial.print("Connecting to WiFi: ");
  Serial.println(ssid);

  // Set custom hostname (this is what appears on your router)
  WiFi.setHostname(WIFI_HOSTNAME);
  Serial.printf("WiFi hostname set to: %s\r\n", WIFI_HOSTNAME);

  // Set status LED to blue during connection attempt
  ledcWrite(GREEN_PIN, 255); // Green off
  ledcWrite(RED_PIN, 255);  // Red off
  ledcWrite(BLUE_PIN, 0);    // Blue on

  // Set SK6812 strip to blue during connection attempt
  setAllPixels(0, 0, 255, 0);
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

    // Flash yellow LED five times to indicate success
    for (int i = 0; i < 5; i++) {
      // Yellow (red + green on, blue off)
      ledcWrite(GREEN_PIN, 0);     // Green on
      ledcWrite(RED_PIN, 0);       // Red on
      ledcWrite(BLUE_PIN, 255);    // Blue off

      // Flash SK6812 strip yellow
      setAllPixels(255, 255, 0, 0);
      strip.show();
      delay(200);

      // All off
      ledcWrite(GREEN_PIN, 255);   // Green off
      ledcWrite(RED_PIN, 255);     // Red off
      ledcWrite(BLUE_PIN, 255);    // Blue off

      // Turn off SK6812 strip
      strip.clear();
      strip.show();
      delay(200);
    }
  } else {
    Serial.println("");
    Serial.println("Failed to connect to WiFi");
    wifiConnected = false;

    // Set SK6812 strip to red to indicate failure
    setAllPixels(255, 0, 0, 0);
    strip.show();

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
  html += "<h1>SK6812 RGBW LED Control</h1>";
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
  updateStatusLED();
  updateSK6812();
  request->send(200, "text/plain", "OK");
}

void handleBrightness(AsyncWebServerRequest *request) {
  if (request->hasArg("level")) {
    int level = request->arg("level").toInt();
    if (level >= 1 && level <= 10) {
      brightnessLevel = level;
      int neoPixelBrightness = map(brightnessLevel, 1, 10, 25, 255);
      strip.setBrightness(neoPixelBrightness);
      updateSK6812();
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
      updateSK6812();
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
  status += "\"customWhite\":" + String(customWhite);
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
      updateSK6812();
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
      updateStatusLED();
      updateSK6812();
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
    int neoPixelBrightness = map(brightnessLevel, 1, 10, 25, 255);
    strip.setBrightness(neoPixelBrightness);
    updateSK6812();
  }

  // Check plus button (increase brightness)
  if (digitalRead(BTN_PLUS) == LOW) {
    brightnessLevel = min(10, brightnessLevel + 1);
    Serial.print("Brightness: ");
    Serial.println(brightnessLevel * 10);
    lastButtonPressTime = currentMillis;
    int neoPixelBrightness = map(brightnessLevel, 1, 10, 25, 255);
    strip.setBrightness(neoPixelBrightness);
    updateSK6812();
  }

  // Check mode button (change color)
  if (digitalRead(BTN_MODE) == LOW) {
    currentMode = (currentMode + 1) % MAX_MODES;
    useCustomColor = false;  // Disable custom color when changing modes via button
    Serial.print("Mode: ");
    Serial.println(currentMode);
    lastButtonPressTime = currentMillis;
    updateSK6812();
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

    // Yellow flash for all error types
    if (errorLedState) {
      // Yellow (red + green on, blue off)
      ledcWrite(GREEN_PIN, 0);     // Green on
      ledcWrite(RED_PIN, 0);       // Red on
      ledcWrite(BLUE_PIN, 255);    // Blue off
    } else {
      // LED off between blinks
      ledcWrite(GREEN_PIN, 255);   // Green off
      ledcWrite(RED_PIN, 255);     // Red off
      ledcWrite(BLUE_PIN, 255);    // Blue off
    }

    // After 60 blinks (about 30 seconds), try to reconnect
    if (errorBlinkCount >= 60) {
      errorBlinkCount = 0;
      connectToWiFi();
    }
  }
}

void updateStatusLED() {
  if (isPoweredOn) {
    // Green at 30% intensity when ON
    ledcWrite(GREEN_PIN, 180);   // Green at 30% (0 = full on, 255 = off, ~180 = 30%)
    ledcWrite(RED_PIN, 255);     // Red off
    ledcWrite(BLUE_PIN, 255);    // Blue off
  } else {
    // Red at 30% intensity when OFF
    ledcWrite(GREEN_PIN, 255);   // Green off
    ledcWrite(RED_PIN, 180);     // Red at 30% (0 = full on, 255 = off, ~180 = 30%)
    ledcWrite(BLUE_PIN, 255);    // Blue off
  }
}

void setAllPixels(uint8_t r, uint8_t g, uint8_t b, uint8_t w) {
  for (int i = 0; i < NUM_LEDS; i++) {
    strip.setPixelColor(i, strip.Color(r, g, b, w));
  }
}

void updateSK6812() {
  if (!isPoweredOn) {
    // Turn off all SK6812 LEDs
    strip.clear();
    strip.show();
    return;
  }

  // Apply color based on mode or custom color
  if (useCustomColor) {
    // Use custom RGBW color
    setAllPixels(customRed, customGreen, customBlue, customWhite);
  } else {
    // Use predefined color mode
    RGBWColor color = colorModes[currentMode];
    setAllPixels(color.r, color.g, color.b, color.w);
  }

  // Show the changes
  strip.show();

  // Debug output
  if (useCustomColor) {
    Serial.print("SK6812 Update - Custom Color: R=");
    Serial.print(customRed);
    Serial.print(", G=");
    Serial.print(customGreen);
    Serial.print(", B=");
    Serial.print(customBlue);
    Serial.print(", W=");
    Serial.print(customWhite);
  } else {
    Serial.print("SK6812 Update - Mode: ");
    Serial.print(currentMode);
  }
  Serial.print(", Brightness: ");
  Serial.println(brightnessLevel * 10);
}

void updateSK6812Effects() {
  // Only process effects if power is on
  if (!isPoweredOn) return;

  // Don't override custom colors with effects
  if (useCustomColor) return;

  // Define variables used in multiple cases outside the switch
  int neoPixelBrightness = map(brightnessLevel, 1, 10, 25, 255);

  // Special effects only for mode 9, otherwise just fixed colors
  if (currentMode < 9) {
    // For modes 0-8: just show solid colors, no special effects
    strip.setBrightness(neoPixelBrightness);
    RGBWColor color = colorModes[currentMode];
    setAllPixels(color.r, color.g, color.b, color.w);
  } else {
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
        hue += 3; // Change color slightly each update
        chaseEffect(hue);
        break;

      case 2:
        // Breathing effect with changing colors
        breathHue++; // Change color slightly each update
        breatheEffect(breathHue);
        break;
    }
  }

  strip.show();
}

// Effect implementations
void breatheEffect(uint8_t hue) {
  // Simple sine wave breathing effect
  float pulse = (exp(sin(millis()/2000.0*PI)) - 0.36787944) * 108.0;
  uint8_t brightness = pulse;

  // Use ColorHSV for smooth color transitions
  uint32_t color = strip.ColorHSV(hue * 256, 255, brightness);

  for (int i = 0; i < NUM_LEDS; i++) {
    strip.setPixelColor(i, color);
  }
}

void chaseEffect(uint8_t hue) {
  // Moving dot chase effect
  strip.clear();
  effectStep = (effectStep + 1) % NUM_LEDS;

  // Use ColorHSV for smooth color transitions
  uint32_t color = strip.ColorHSV(hue * 256, 255, 255);

  strip.setPixelColor(effectStep, color);
  // Optional: add a tail
  uint8_t tailLength = min(3, NUM_LEDS/4);
  for (int i = 1; i <= tailLength; i++) {
    int pos = (effectStep - i + NUM_LEDS) % NUM_LEDS;
    uint32_t tailColor = strip.ColorHSV(hue * 256, 255, 255 - (i * 255 / (tailLength + 1)));
    strip.setPixelColor(pos, tailColor);
  }

  // Use the global brightness setting
  int neoPixelBrightness = map(brightnessLevel, 1, 10, 25, 255);
  strip.setBrightness(neoPixelBrightness);
}

void rainbowEffect() {
  // Moving rainbow effect
  for (int i = 0; i < NUM_LEDS; i++) {
    uint32_t color = strip.ColorHSV(((i * 256 / NUM_LEDS) + effectStep) % 65536, 255, 255);
    strip.setPixelColor(i, color);
  }
  effectStep = (effectStep + 1) % 256;

  // Use the global brightness setting
  int neoPixelBrightness = map(brightnessLevel, 1, 10, 25, 255);
  strip.setBrightness(neoPixelBrightness);
}
