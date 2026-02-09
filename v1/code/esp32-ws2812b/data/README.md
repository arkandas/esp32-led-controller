# ESP32 LED Controller - LittleFS Setup

This folder contains the web interface files that need to be uploaded to the ESP32's LittleFS filesystem.

## Files

- `index.html` - Main web interface HTML structure
- `style.css` - Stylesheet for the web interface
- `app.js` - JavaScript for real-time status updates and controls
- `README.md` - This file

## How to Upload to ESP32

### Method 1: Using Arduino IDE with ESP32 LittleFS Data Upload Tool

1. **Install the ESP32 LittleFS Data Upload Tool:**
   - Download from: https://github.com/lorol/arduino-esp32littlefs-plugin
   - Follow the installation instructions for your Arduino IDE version

2. **Upload the data folder:**
   - Place this entire `data` folder in the same directory as your `.ino` file
   - In Arduino IDE, go to `Tools` → `ESP32 LittleFS Data Upload`
   - Wait for the upload to complete

### Method 2: Using PlatformIO

1. **Configure platformio.ini:**
   ```ini
   [env:esp32dev]
   platform = espressif32
   board = esp32dev
   framework = arduino
   board_build.filesystem = littlefs
   ```

2. **Upload filesystem:**
   - Place the `data` folder in your project root
   - Run: `pio run --target uploadfs`

### Method 3: Using ESP32 Web File Manager

1. **Enable file manager in code** (if implemented)
2. **Access web interface** at `http://[ESP32_IP]/files`
3. **Upload files manually** through the web interface

## Features

The web interface provides:

- **Real-time status updates** - No placeholder data, always shows current device state
- **Automatic connection detection** - Disables controls when disconnected
- **Immediate feedback** - Status updates immediately after changes
- **Responsive design** - Works on desktop and mobile devices
- **Error handling** - Clear error messages for connection issues

## Technical Details

- **File system:** LittleFS (Little File System)
- **Update frequency:** Every 2 seconds
- **Fallback:** 404 error if files not found
- **MIME type:** Properly served as `text/html`

## Status Endpoint

The web interface fetches data from `/status` endpoint which returns:

```json
{
  "isPoweredOn": true,
  "brightnessLevel": 8,
  "currentMode": 0
}
```

## Notes

- The web interface will show "Connecting to device..." until the first successful status fetch
- Controls are disabled until a successful connection is established
- All data is fetched live from the ESP32 - no static placeholders are ever shown
- The interface automatically handles reconnection if the ESP32 restarts 
