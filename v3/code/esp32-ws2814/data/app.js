function defaultStrip() {
    return {
        name: '',
        on: false,
        brightnessLevel: 10,
        currentMode: 0,
        useCustomColor: false,
        customRed: 255,
        customGreen: 255,
        customBlue: 255,
        customWhite: 0
    };
}

let deviceState = {
    deviceName: '',
    wifiNetwork: '',
    macAddress: '',
    signalStrength: 0,
    uptime: 0,
    temperature: 0,
    humidity: 0,
    sensorAvailable: false,
    backlight: true,
    power: { available: false, voltage: 0, current: 0, currentMa: 0, power: 0 },
    strips: [defaultStrip(), defaultStrip()]
};

// Which strip the controls act on: '0', '1', or 'all'
let selectedStrip = '0';

// User interaction flags
let userInteracting = false;
let colorPickerOpen = false;
let lastInteractionTime = 0;

const PRESET_HEX = {
    0: '#fffaf0', 1: '#ff0000', 2: '#00ff00', 3: '#0000ff', 4: '#ffff00',
    5: '#ff00ff', 6: '#00ffff', 7: '#ffa500', 8: '#ffffff'
};
const PRESET_RGB = {
    0: [255, 250, 240], 1: [255, 0, 0], 2: [0, 255, 0], 3: [0, 0, 255], 4: [255, 255, 0],
    5: [255, 0, 255], 6: [0, 255, 255], 7: [255, 165, 0], 8: [255, 255, 255], 9: [128, 128, 128]
};

// W channel for each preset (only pure-white / bright-white drive it)
const PRESET_WHITE = { 0: 255, 8: 255 };

// The strip whose state the UI reflects (strip 0 when "Both" is selected).
function repIndex() {
    return selectedStrip === 'all' ? 0 : parseInt(selectedStrip);
}
function currentStrip() {
    return deviceState.strips[repIndex()] || defaultStrip();
}
// Strips a command applies to.
function targetIndices() {
    return selectedStrip === 'all' ? [0, 1] : [parseInt(selectedStrip)];
}

document.addEventListener('DOMContentLoaded', function() {
    updateStatusBubble('loading');
    setupStripSelector();
    setupEventListeners();
    updateTargetBadge();

    fetchStatus();
    setInterval(fetchStatus, 2000);
});

function setupStripSelector() {
    document.querySelectorAll('.strip-tab').forEach(tab => {
        tab.addEventListener('click', function() {
            selectedStrip = this.dataset.strip;
            document.querySelectorAll('.strip-tab').forEach(t => t.classList.remove('active'));
            this.classList.add('active');
            updateTargetBadge();

            // Reflect the newly-selected strip's state right away
            userInteracting = false;
            colorPickerOpen = false;
            updateUIControls();
            updateColorControls();
        });
    });
}

function updateTargetBadge() {
    const badge = document.getElementById('targetBadge');
    if (!badge) return;
    if (selectedStrip === 'all') { badge.textContent = 'Both'; return; }
    const s = deviceState.strips[parseInt(selectedStrip)] || {};
    badge.textContent = s.name || (selectedStrip === '0' ? 'Strip 1' : 'Strip 2');
}

// Reflect each strip's configured name on its selector tab
function updateStripLabels() {
    document.querySelectorAll('.strip-tab').forEach(tab => {
        const ds = tab.dataset.strip;
        if (ds === '0' || ds === '1') {
            const s = deviceState.strips[parseInt(ds)] || {};
            if (s.name) tab.textContent = s.name;
        }
    });
}

function setupEventListeners() {
    const brightnessSlider = document.getElementById('brightnessSlider');
    const colorMode = document.getElementById('colorMode');
    const colorPicker = document.getElementById('colorPicker');
    const whiteSlider = document.getElementById('whiteSlider');
    const backlightToggle = document.getElementById('backlightToggle');
    const powerButton = document.getElementById('powerButton');

    brightnessSlider.addEventListener('input', function() {
        userInteracting = true;
        lastInteractionTime = Date.now();
        updateBrightnessDisplay(this.value);
    });

    brightnessSlider.addEventListener('change', function() {
        setBrightness(this.value);
        setTimeout(() => { userInteracting = false; }, 3000);
    });

    colorMode.addEventListener('change', function() {
        if (this.value === 'custom') {
            userInteracting = true;
            lastInteractionTime = Date.now();
            return;
        }
        userInteracting = true;
        lastInteractionTime = Date.now();
        setColorMode(this.value);
        setTimeout(() => { userInteracting = false; }, 3000);
    });

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
        applyCustomColor();
    });

    colorPicker.addEventListener('change', function() {
        applyCustomColor();
    });

    colorPicker.addEventListener('blur', function() {
        setTimeout(() => {
            colorPickerOpen = false;
            userInteracting = false;
        }, 15000);
    });

    whiteSlider.addEventListener('input', function() {
        userInteracting = true;
        lastInteractionTime = Date.now();
        document.getElementById('whiteValue').textContent = this.value;
    });

    whiteSlider.addEventListener('change', function() {
        applyCustomColor();
        setTimeout(() => { userInteracting = false; }, 3000);
    });

    backlightToggle.addEventListener('change', function() {
        userInteracting = true;
        lastInteractionTime = Date.now();
        const on = this.checked ? 1 : 0;
        fetch(`/backlight?on=${on}`)
            .then(response => response.text())
            .then(() => { deviceState.backlight = !!on; })
            .catch(error => { console.error('Error setting backlight:', error); updateStatusBubble('error'); });
        setTimeout(() => { userInteracting = false; }, 3000);
    });

    powerButton.addEventListener('click', function() {
        userInteracting = true;
        lastInteractionTime = Date.now();
        togglePower();
        setTimeout(() => { userInteracting = false; }, 3000);
    });
}

function updateBrightnessDisplay(value) {
    document.getElementById('brightnessValue').textContent = (value * 10) + '%';
}

function setBrightness(value) {
    fetch(`/brightness?strip=${selectedStrip}&level=${value}`)
        .then(response => response.text())
        .then(() => {
            targetIndices().forEach(i => { deviceState.strips[i].brightnessLevel = parseInt(value); });
            updateBrightnessDisplay(value);
        })
        .catch(error => {
            console.error('Error setting brightness:', error);
            updateStatusBubble('error');
        });
}

function setColorMode(mode) {
    fetch(`/mode?strip=${selectedStrip}&mode=${mode}`)
        .then(response => response.text())
        .then(() => {
            targetIndices().forEach(i => {
                deviceState.strips[i].currentMode = parseInt(mode);
                deviceState.strips[i].useCustomColor = false;
            });
            if (mode !== '9' && PRESET_HEX[mode]) {
                document.getElementById('colorPicker').value = PRESET_HEX[mode];
                updateRGBDisplay(PRESET_HEX[mode]);
            }
        })
        .catch(error => {
            console.error('Error setting color mode:', error);
            updateStatusBubble('error');
        });
}

// Custom color = the RGB picker + the White slider. Changing either sends all four.
function applyCustomColor() {
    const colorValue = document.getElementById('colorPicker').value;
    const hex = colorValue.replace('#', '');
    const r = parseInt(hex.substr(0, 2), 16);
    const g = parseInt(hex.substr(2, 2), 16);
    const b = parseInt(hex.substr(4, 2), 16);
    const w = parseInt(document.getElementById('whiteSlider').value) || 0;

    fetch(`/color?strip=${selectedStrip}&r=${r}&g=${g}&b=${b}&w=${w}`)
        .then(response => response.text())
        .then(() => {
            targetIndices().forEach(i => {
                const s = deviceState.strips[i];
                s.useCustomColor = true;
                s.customRed = r;
                s.customGreen = g;
                s.customBlue = b;
                s.customWhite = w;
            });
            document.getElementById('colorMode').value = 'custom';
            updateRGBDisplay(colorValue);
            document.getElementById('whiteValue').textContent = w;
        })
        .catch(error => {
            console.error('Error setting custom color:', error);
            updateStatusBubble('error');
        });
}

function togglePower() {
    fetch(`/toggle?strip=${selectedStrip}`)
        .then(response => response.text())
        .then(() => {
            if (selectedStrip === 'all') {
                const allOn = deviceState.strips.every(s => s.on);
                deviceState.strips.forEach(s => { s.on = !allOn; });
            } else {
                const i = repIndex();
                deviceState.strips[i].on = !deviceState.strips[i].on;
            }
            // Refresh ALL controls, not just the button — turning a strip on must
            // immediately enable its color/brightness controls (don't wait for the poll).
            updateUIControls();
            updateColorControls();
        })
        .catch(error => {
            console.error('Error toggling power:', error);
            updateStatusBubble('error');
        });
}

function fetchStatus() {
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
    deviceState.deviceName = data.deviceName;
    deviceState.wifiNetwork = data.wifiSSID;
    deviceState.macAddress = data.macAddress;
    deviceState.signalStrength = data.signalStrength;
    deviceState.uptime = data.uptime;
    deviceState.temperature = data.temperature;
    deviceState.humidity = data.humidity;
    deviceState.sensorAvailable = data.sensorAvailable;
    if (typeof data.backlight === 'boolean') deviceState.backlight = data.backlight;
    if (data.power) deviceState.power = data.power;
    if (Array.isArray(data.strips) && data.strips.length) deviceState.strips = data.strips;
}

function updateUI() {
    updateDeviceTitle();
    updateStripLabels();
    updateTargetBadge();
    updateDeviceInfo();
    updateSensorInfo();
    updatePowerInfo();
    updateBacklightControl();
    updateUIControls();
    updateColorControls();
}

function updateDeviceTitle() {
    document.getElementById('deviceTitle').textContent = deviceState.deviceName || 'ESP32 LED Controller V3';
}

function updateDeviceInfo() {
    document.getElementById('networkName').textContent = deviceState.wifiNetwork || 'Unknown';
    document.getElementById('signalStrength').textContent = deviceState.signalStrength + ' dBm';
    document.getElementById('macAddress').textContent = deviceState.macAddress || 'Unknown';

    const hours = Math.floor(deviceState.uptime / 3600);
    const minutes = Math.floor((deviceState.uptime % 3600) / 60);
    const seconds = deviceState.uptime % 60;

    const hoursStr = hours.toString().padStart(2, '0');
    const minutesStr = minutes.toString().padStart(2, '0');
    const secondsStr = seconds.toString().padStart(2, '0');

    document.getElementById('uptime').textContent = `${hoursStr}h ${minutesStr}m ${secondsStr}s`;
}

function updateSensorInfo() {
    const tempElement = document.getElementById('temperature');
    const humidityElement = document.getElementById('humidity');

    if (deviceState.sensorAvailable) {
        tempElement.textContent = deviceState.temperature.toFixed(1);
        humidityElement.textContent = deviceState.humidity.toFixed(1);
    } else {
        tempElement.textContent = '--';
        humidityElement.textContent = '--';
    }
}

function updatePowerInfo() {
    const p = deviceState.power || {};
    const vEl = document.getElementById('powerVoltage');
    const cEl = document.getElementById('powerCurrent');
    const cUnit = document.getElementById('powerCurrentUnit');
    const wEl = document.getElementById('powerWatts');
    const wUnit = document.getElementById('powerWattsUnit');
    if (!vEl || !cEl || !cUnit || !wEl || !wUnit) return;  // power panel not in DOM (stale HTML) — don't throw

    if (!p.available) {
        vEl.textContent = '--';
        cEl.textContent = '--'; cUnit.textContent = 'A';
        wEl.textContent = '--'; wUnit.textContent = 'W';
        return;
    }

    vEl.textContent = Number(p.voltage).toFixed(2);

    // Current: auto-range mA / A
    const amps = Number(p.current);
    if (amps < 1) {
        cEl.textContent = Math.round(amps * 1000);
        cUnit.textContent = 'mA';
    } else {
        cEl.textContent = amps.toFixed(2);
        cUnit.textContent = 'A';
    }

    // Power: auto-range mW / W
    const watts = Number(p.power);
    if (watts < 1) {
        wEl.textContent = Math.round(watts * 1000);
        wUnit.textContent = 'mW';
    } else {
        wEl.textContent = watts.toFixed(1);
        wUnit.textContent = 'W';
    }
}

function updateBacklightControl() {
    const t = document.getElementById('backlightToggle');
    if (!t) return;
    if (!userInteracting) t.checked = !!deviceState.backlight;
}

function updateUIControls() {
    updatePowerButton();
    updateBrightnessControls();
}

function updatePowerButton() {
    const powerButton = document.getElementById('powerButton');
    if (currentStrip().on) {
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
    const s = currentStrip();

    if (!userInteracting) {
        brightnessSlider.value = s.brightnessLevel;
        brightnessValue.textContent = (s.brightnessLevel * 10) + '%';
    }

    brightnessSlider.disabled = !s.on;
}

function updateColorControls() {
    const colorMode = document.getElementById('colorMode');
    const colorPicker = document.getElementById('colorPicker');
    const whiteSlider = document.getElementById('whiteSlider');
    const s = currentStrip();

    colorMode.disabled = !s.on;
    colorPicker.disabled = !s.on;
    whiteSlider.disabled = !s.on;

    if (!userInteracting && !colorPickerOpen) {
        if (s.useCustomColor) {
            colorMode.value = 'custom';
            const customHex = `#${s.customRed.toString(16).padStart(2, '0')}${s.customGreen.toString(16).padStart(2, '0')}${s.customBlue.toString(16).padStart(2, '0')}`;
            colorPicker.value = customHex;
            updateRGBDisplay(customHex);
        } else {
            colorMode.value = s.currentMode.toString();
            if (PRESET_HEX[s.currentMode]) {
                colorPicker.value = PRESET_HEX[s.currentMode];
                updateRGBDisplay(PRESET_HEX[s.currentMode]);
            }
        }
        const wVal = s.useCustomColor ? s.customWhite : (PRESET_WHITE[s.currentMode] || 0);
        whiteSlider.value = wVal;
        document.getElementById('whiteValue').textContent = wVal;
    }

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
    const s = currentStrip();
    if (!s.on) {
        document.getElementById('rgbR').textContent = '---';
        document.getElementById('rgbG').textContent = '---';
        document.getElementById('rgbB').textContent = '---';
        document.getElementById('rgbHex').textContent = '---';
    } else if (s.useCustomColor) {
        document.getElementById('rgbR').textContent = s.customRed;
        document.getElementById('rgbG').textContent = s.customGreen;
        document.getElementById('rgbB').textContent = s.customBlue;
        const hex = `#${s.customRed.toString(16).padStart(2, '0')}${s.customGreen.toString(16).padStart(2, '0')}${s.customBlue.toString(16).padStart(2, '0')}`;
        document.getElementById('rgbHex').textContent = hex.toUpperCase();
    } else {
        const rgb = PRESET_RGB[s.currentMode] || [0, 0, 0];
        document.getElementById('rgbR').textContent = rgb[0];
        document.getElementById('rgbG').textContent = rgb[1];
        document.getElementById('rgbB').textContent = rgb[2];
        const hex = `#${rgb[0].toString(16).padStart(2, '0')}${rgb[1].toString(16).padStart(2, '0')}${rgb[2].toString(16).padStart(2, '0')}`;
        document.getElementById('rgbHex').textContent = hex.toUpperCase();
    }
}

function updateStatusBubble(status) {
    const statusBubble = document.getElementById('statusBubble');
    statusBubble.classList.remove('connected', 'error', 'loading');
    if (status === 'connected') {
        statusBubble.classList.add('connected');
    } else if (status === 'error') {
        statusBubble.classList.add('error');
    } else if (status === 'loading') {
        statusBubble.classList.add('loading');
    }
}
