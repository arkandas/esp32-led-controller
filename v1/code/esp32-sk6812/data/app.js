let deviceState = {
    isPoweredOn: false,
    brightnessLevel: 10,
    currentMode: 0,
    customRed: 255,
    customGreen: 255,
    customBlue: 255,
    customWhite: 255,
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
    const whiteSlider = document.getElementById('whiteSlider');
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
    const whiteSlider = document.getElementById('whiteSlider');

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

    // Color picker control
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

    // White channel control
    whiteSlider.addEventListener('input', function() {
        userInteracting = true;
        lastInteractionTime = Date.now();
        deviceState.customWhite = parseInt(this.value);
        updateWhiteDisplay(this.value);
    });

    whiteSlider.addEventListener('change', function() {
        setWhiteValue(this.value);
        setTimeout(() => {
            userInteracting = false;
        }, 3000);
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

function updateWhiteDisplay(value) {
    document.getElementById('whiteValue').textContent = value;
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
            if (mode !== '9') { // Not effects mode
                const presetColors = {
                    0: '#ffffff', // Bright White
                    1: '#ff0000', // Red
                    2: '#00ff00', // Green
                    3: '#0000ff', // Blue
                    4: '#ffff00', // Yellow
                    5: '#ff00ff', // Purple
                    6: '#00ffff', // Cyan
                    7: '#ffa500', // Orange
                    8: '#ffffff'  // Pure White
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

    fetch(`/color?r=${r}&g=${g}&b=${b}&w=${deviceState.customWhite}`)
        .then(response => response.text())
        .then(data => {
            console.log('Custom color set to:', colorValue);
            deviceState.useCustomColor = true;
            deviceState.customRed = r;
            deviceState.customGreen = g;
            deviceState.customBlue = b;

            // Update color mode to show custom selection
            document.getElementById('colorMode').value = 'custom';

            updateRGBDisplay(colorValue);
        })
        .catch(error => {
            console.error('Error setting custom color:', error);
            updateStatusBubble('error');
        });
}

function setWhiteValue(value) {
    fetch(`/color?r=${deviceState.customRed}&g=${deviceState.customGreen}&b=${deviceState.customBlue}&w=${value}`)
        .then(response => response.text())
        .then(data => {
            console.log('White value set to:', value);
            deviceState.customWhite = parseInt(value);
            deviceState.useCustomColor = true;
            document.getElementById('colorMode').value = 'custom';
        })
        .catch(error => {
            console.error('Error setting white value:', error);
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
    // Don't fetch status if user is actively interacting
    if (userInteracting && Date.now() - lastInteractionTime < 2000) {
        return;
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
        customWhite: data.customWhite || 0,
        useCustomColor: data.useCustomColor,
        deviceName: data.deviceName,
        wifiNetwork: data.wifiSSID,
        macAddress: data.macAddress,
        signalStrength: data.signalStrength,
        uptime: data.uptime
    };
}

function updateUI() {
    if (userInteracting) return;

    updateDeviceTitle();
    updateDeviceInfo();
    updateUIControls();
    updatePowerButton();
    updateBrightnessControls();
    updateColorControls();
}

function updateDeviceTitle() {
    document.getElementById('deviceTitle').textContent = deviceState.deviceName || 'ESP32 LED Controller';
}

function updateDeviceInfo() {
    document.getElementById('networkName').textContent = deviceState.wifiNetwork || 'Unknown';
    document.getElementById('signalStrength').textContent = deviceState.signalStrength ? `${deviceState.signalStrength} dBm` : 'Unknown';
    document.getElementById('macAddress').textContent = deviceState.macAddress || 'Unknown';

    // Format uptime
    const uptime = deviceState.uptime || 0;
    const hours = Math.floor(uptime / 3600);
    const minutes = Math.floor((uptime % 3600) / 60);
    const seconds = uptime % 60;
    document.getElementById('uptime').textContent = `${hours}h ${minutes}m ${seconds}s`;
}

function updateUIControls() {
    // Update controls based on device state
    document.getElementById('brightnessSlider').value = deviceState.brightnessLevel;
    document.getElementById('colorMode').value = deviceState.useCustomColor ? 'custom' : deviceState.currentMode;
    document.getElementById('whiteSlider').value = deviceState.customWhite;
}

function updatePowerButton() {
    const powerButton = document.getElementById('powerButton');
    if (deviceState.isPoweredOn) {
        powerButton.textContent = 'Turn OFF';
        powerButton.className = 'power-off';
    } else {
        powerButton.textContent = 'Turn ON';
        powerButton.className = '';
    }
}

function updateBrightnessControls() {
    updateBrightnessDisplay(deviceState.brightnessLevel);
}

function updateColorControls() {
    // Update color picker based on RGB values
    const hex = rgbToHex(deviceState.customRed, deviceState.customGreen, deviceState.customBlue);
    document.getElementById('colorPicker').value = hex;
    updateRGBDisplay(hex);
    updateWhiteDisplay(deviceState.customWhite);
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

function rgbToHex(r, g, b) {
    return "#" + ((1 << 24) + (r << 16) + (g << 8) + b).toString(16).slice(1).toUpperCase();
}

function updateStatusBubble(status) {
    const statusBubble = document.getElementById('statusBubble');
    const statusText = document.getElementById('statusText');

    // Remove all status classes
    statusBubble.classList.remove('connected', 'error', 'loading');

    switch(status) {
        case 'connected':
            statusBubble.classList.add('connected');
            statusText.textContent = '●';
            statusBubble.title = 'Connected';
            break;
        case 'error':
            statusBubble.classList.add('error');
            statusText.textContent = '●';
            statusBubble.title = 'Error';
            break;
        case 'loading':
            statusBubble.classList.add('loading');
            statusText.textContent = '●';
            statusBubble.title = 'Loading...';
            break;
    }
}
