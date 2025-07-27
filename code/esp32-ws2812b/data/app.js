let deviceState = {
    isPoweredOn: false,
    brightnessLevel: 10,
    currentMode: 0,
    customRed: 255,
    customGreen: 255,
    customBlue: 255,
    useCustomColor: false,
    deviceName: '',
    wifiNetwork: '',
    macAddress: '',
    signalStrength: 0,
    uptime: 0
};

// User interaction flags
let userInteracting = false;
let colorPickerOpen = false;
let lastInteractionTime = 0;

document.addEventListener('DOMContentLoaded', function() {
    // Get elements
    const brightnessSlider = document.getElementById('brightnessSlider');
    const brightnessValue = document.getElementById('brightnessValue');
    const colorMode = document.getElementById('colorMode');
    const colorPicker = document.getElementById('colorPicker');
    const powerButton = document.getElementById('powerButton');
    const statusBubble = document.getElementById('statusBubble');

    // Initialize status bubble
    updateStatusBubble('loading');

    // Initialize event listeners
    setupEventListeners();

    // Start periodic updates
    fetchStatus();
    setInterval(fetchStatus, 2000);
});

function setupEventListeners() {
    const brightnessSlider = document.getElementById('brightnessSlider');
    const colorMode = document.getElementById('colorMode');
    const colorPicker = document.getElementById('colorPicker');
    const powerButton = document.getElementById('powerButton');

    // Brightness control
    brightnessSlider.addEventListener('input', function() {
        userInteracting = true;
        lastInteractionTime = Date.now();
        updateBrightnessDisplay(this.value);
    });

    brightnessSlider.addEventListener('change', function() {
        setBrightness(this.value);
        setTimeout(() => {
            userInteracting = false;
        }, 3000);
    });

    // Color mode control
    colorMode.addEventListener('change', function() {
        if (this.value === 'custom') {
            // Don't make API call for custom option, just enable color picker
            userInteracting = true;
            lastInteractionTime = Date.now();
            return;
        }

        userInteracting = true;
        lastInteractionTime = Date.now();
        setColorMode(this.value);
        setTimeout(() => {
            userInteracting = false;
        }, 3000);
    });

    // Color picker control with enhanced interaction detection
    colorPicker.addEventListener('focus', function() {
        colorPickerOpen = true;
        userInteracting = true;
        lastInteractionTime = Date.now();
    });

    colorPicker.addEventListener('click', function() {
        colorPickerOpen = true;
        userInteracting = true;
        lastInteractionTime = Date.now();
    });

    colorPicker.addEventListener('input', function() {
        colorPickerOpen = true;
        userInteracting = true;
        lastInteractionTime = Date.now();
        handleCustomColorChange(this.value);
    });

    colorPicker.addEventListener('change', function() {
        handleCustomColorChange(this.value);
    });

    colorPicker.addEventListener('blur', function() {
        setTimeout(() => {
            colorPickerOpen = false;
            userInteracting = false;
        }, 15000); // Longer timeout for color picker
    });

    // Power control
    powerButton.addEventListener('click', function() {
        userInteracting = true;
        lastInteractionTime = Date.now();
        togglePower();
        setTimeout(() => {
            userInteracting = false;
        }, 3000);
    });
}

function updateBrightnessDisplay(value) {
    document.getElementById('brightnessValue').textContent = (value * 10) + '%';
}

function setBrightness(value) {
    fetch(`/brightness?level=${value}`)
        .then(response => response.text())
        .then(data => {
            console.log('Brightness set to:', value);
            deviceState.brightnessLevel = parseInt(value);
            updateBrightnessDisplay(value);
        })
        .catch(error => {
            console.error('Error setting brightness:', error);
            updateStatusBubble('error');
        });
}

function setColorMode(mode) {
    fetch(`/mode?mode=${mode}`)
        .then(response => response.text())
        .then(data => {
            console.log('Color mode set to:', mode);
            deviceState.currentMode = parseInt(mode);
            deviceState.useCustomColor = false;

            // Update color picker to match preset color
            if (mode !== '8') { // Not effects mode
                const presetColors = {
                    0: '#ffffff', // White
                    1: '#ff0000', // Red
                    2: '#00ff00', // Green
                    3: '#0000ff', // Blue
                    4: '#ffff00', // Yellow
                    5: '#ff00ff', // Purple
                    6: '#00ffff', // Cyan
                    7: '#ffa500'  // Orange
                };

                if (presetColors[mode]) {
                    document.getElementById('colorPicker').value = presetColors[mode];
                    updateRGBDisplay(presetColors[mode]);
                }
            }
        })
        .catch(error => {
            console.error('Error setting color mode:', error);
            updateStatusBubble('error');
        });
}

function handleCustomColorChange(colorValue) {
    const hex = colorValue.replace('#', '');
    const r = parseInt(hex.substr(0, 2), 16);
    const g = parseInt(hex.substr(2, 2), 16);
    const b = parseInt(hex.substr(4, 2), 16);

    fetch(`/color?r=${r}&g=${g}&b=${b}`)
        .then(response => response.text())
        .then(data => {
            console.log('Custom color set to:', colorValue);
            deviceState.useCustomColor = true;
            deviceState.customRed = r;
            deviceState.customGreen = g;
            deviceState.customBlue = b;

            // Update color mode select to show custom selection
            document.getElementById('colorMode').value = 'custom';

            updateRGBDisplay(colorValue);
        })
        .catch(error => {
            console.error('Error setting custom color:', error);
            updateStatusBubble('error');
        });
}

function togglePower() {
    fetch('/toggle')
        .then(response => response.text())
        .then(data => {
            console.log('Power toggled');
            deviceState.isPoweredOn = !deviceState.isPoweredOn;
            updatePowerButton();
        })
        .catch(error => {
            console.error('Error toggling power:', error);
            updateStatusBubble('error');
        });
}

function fetchStatus() {
    // Skip status updates if user is actively interacting
    if (userInteracting || colorPickerOpen) {
        if (Date.now() - lastInteractionTime > (colorPickerOpen ? 15000 : 3000)) {
            userInteracting = false;
            colorPickerOpen = false;
        } else {
            return;
        }
    }

    fetch('/status')
        .then(response => response.json())
        .then(data => {
            updateDeviceState(data);
            updateUI();
            updateStatusBubble('connected');
        })
        .catch(error => {
            console.error('Error fetching status:', error);
            updateStatusBubble('error');
        });
}

function updateDeviceState(data) {
    deviceState = {
        isPoweredOn: data.isPoweredOn,
        brightnessLevel: data.brightnessLevel,
        currentMode: data.currentMode,
        customRed: data.customRed,
        customGreen: data.customGreen,
        customBlue: data.customBlue,
        useCustomColor: data.useCustomColor,
        deviceName: data.deviceName,
        wifiNetwork: data.wifiSSID,
        macAddress: data.macAddress,
        signalStrength: data.signalStrength,
        uptime: data.uptime
    };
}

function updateUI() {
    updateDeviceTitle();
    updateDeviceInfo();
    updateUIControls();
    updateColorControls();
}

function updateDeviceTitle() {
    const deviceTitle = document.getElementById('deviceTitle');
    deviceTitle.textContent = deviceState.deviceName || 'ESP32 LED Controller';
}

function updateDeviceInfo() {
    document.getElementById('networkName').textContent = deviceState.wifiNetwork || 'Unknown';
    document.getElementById('signalStrength').textContent = deviceState.signalStrength + ' dBm';
    document.getElementById('macAddress').textContent = deviceState.macAddress || 'Unknown';

    // Format uptime with consistent width
    const hours = Math.floor(deviceState.uptime / 3600);
    const minutes = Math.floor((deviceState.uptime % 3600) / 60);
    const seconds = deviceState.uptime % 60;

    // Pad numbers to ensure consistent width
    const hoursStr = hours.toString().padStart(2, '0');
    const minutesStr = minutes.toString().padStart(2, '0');
    const secondsStr = seconds.toString().padStart(2, '0');

    document.getElementById('uptime').textContent = `${hoursStr}h ${minutesStr}m ${secondsStr}s`;
}

function updateUIControls() {
    updatePowerButton();
    updateBrightnessControls();
}

function updatePowerButton() {
    const powerButton = document.getElementById('powerButton');
    if (deviceState.isPoweredOn) {
        powerButton.textContent = 'Turn OFF';
        powerButton.classList.add('power-off');
    } else {
        powerButton.textContent = 'Turn ON';
        powerButton.classList.remove('power-off');
    }
}

function updateBrightnessControls() {
    const brightnessSlider = document.getElementById('brightnessSlider');
    const brightnessValue = document.getElementById('brightnessValue');

    if (!userInteracting) {
        brightnessSlider.value = deviceState.brightnessLevel;
        brightnessValue.textContent = (deviceState.brightnessLevel * 10) + '%';
    }

    brightnessSlider.disabled = !deviceState.isPoweredOn;
}

function updateColorControls() {
    const colorMode = document.getElementById('colorMode');
    const colorPicker = document.getElementById('colorPicker');

    // Enable/disable controls based on power state
    colorMode.disabled = !deviceState.isPoweredOn;
    colorPicker.disabled = !deviceState.isPoweredOn;

    if (!userInteracting && !colorPickerOpen) {
        if (deviceState.useCustomColor) {
            colorMode.value = 'custom';
            const customHex = `#${deviceState.customRed.toString(16).padStart(2, '0')}${deviceState.customGreen.toString(16).padStart(2, '0')}${deviceState.customBlue.toString(16).padStart(2, '0')}`;
            colorPicker.value = customHex;
            updateRGBDisplay(customHex);
        } else {
            colorMode.value = deviceState.currentMode.toString();

            // Update color picker to match preset
            const presetColors = {
                0: '#ffffff', // White
                1: '#ff0000', // Red
                2: '#00ff00', // Green
                3: '#0000ff', // Blue
                4: '#ffff00', // Yellow
                5: '#ff00ff', // Purple
                6: '#00ffff', // Cyan
                7: '#ffa500'  // Orange
            };

            if (presetColors[deviceState.currentMode]) {
                colorPicker.value = presetColors[deviceState.currentMode];
                updateRGBDisplay(presetColors[deviceState.currentMode]);
            }
        }
    }

    // Update RGB display based on device state
    updateRGBDisplayFromState();
}

function updateRGBDisplay(hexColor) {
    const hex = hexColor.replace('#', '');
    const r = parseInt(hex.substr(0, 2), 16);
    const g = parseInt(hex.substr(2, 2), 16);
    const b = parseInt(hex.substr(4, 2), 16);

    document.getElementById('rgbR').textContent = r;
    document.getElementById('rgbG').textContent = g;
    document.getElementById('rgbB').textContent = b;
    document.getElementById('rgbHex').textContent = hexColor.toUpperCase();
}

function updateRGBDisplayFromState() {
    if (!deviceState.isPoweredOn) {
        document.getElementById('rgbR').textContent = '---';
        document.getElementById('rgbG').textContent = '---';
        document.getElementById('rgbB').textContent = '---';
        document.getElementById('rgbHex').textContent = '---';
    } else if (deviceState.useCustomColor) {
        document.getElementById('rgbR').textContent = deviceState.customRed;
        document.getElementById('rgbG').textContent = deviceState.customGreen;
        document.getElementById('rgbB').textContent = deviceState.customBlue;

        const hex = `#${deviceState.customRed.toString(16).padStart(2, '0')}${deviceState.customGreen.toString(16).padStart(2, '0')}${deviceState.customBlue.toString(16).padStart(2, '0')}`;
        document.getElementById('rgbHex').textContent = hex.toUpperCase();
    } else {
        // Show preset color RGB values
        const presetRGB = {
            0: [255, 255, 255], // White
            1: [255, 0, 0],     // Red
            2: [0, 255, 0],     // Green
            3: [0, 0, 255],     // Blue
            4: [255, 255, 0],   // Yellow
            5: [255, 0, 255],   // Purple
            6: [0, 255, 255],   // Cyan
            7: [255, 165, 0],   // Orange
            8: [128, 128, 128]  // Effects (grey placeholder)
        };

        const rgb = presetRGB[deviceState.currentMode] || [0, 0, 0];
        document.getElementById('rgbR').textContent = rgb[0];
        document.getElementById('rgbG').textContent = rgb[1];
        document.getElementById('rgbB').textContent = rgb[2];

        const hex = `#${rgb[0].toString(16).padStart(2, '0')}${rgb[1].toString(16).padStart(2, '0')}${rgb[2].toString(16).padStart(2, '0')}`;
        document.getElementById('rgbHex').textContent = hex.toUpperCase();
    }
}

function updateStatusBubble(status) {
    const statusBubble = document.getElementById('statusBubble');

    // Remove all status classes
    statusBubble.classList.remove('connected', 'error', 'loading');

    // Add the appropriate class
    if (status === 'connected') {
        statusBubble.classList.add('connected');
    } else if (status === 'error') {
        statusBubble.classList.add('error');
    } else if (status === 'loading') {
        statusBubble.classList.add('loading');
    }
}
