var gateway = `ws://${window.location.hostname}/ws`;
var websocket;

// AB
var currentPreset = 0;
const abData = {
    mutedSwitch: false,
    likesA: 0,
    likesB: 0,
};

// ABX
const abxData = {
    mutedSwitch: false,
    presetA: 0,
    presetB: 0,
    trials: 0,
    correct: 0,
    iteration: 1,
    presetX: 0,
};

// websocket
window.addEventListener('load', onLoad);

function onLoad(event) {
    initWebSocket();
}

function initWebSocket() {
    if (window.location.hostname === "127.0.0.1") {
        console.log('Detected localhost: Entering test mode');
        websocket = { send: wsSendMock, readyState: WebSocket.OPEN };
        onOpen();
        return;
    }
    console.log('Trying to open a WebSocket connection...');
    websocket = new WebSocket(gateway);
    websocket.onopen = onOpen;
    websocket.onclose = onClose;
    websocket.onmessage = onMessage;
}

function onOpen(event) {
    console.log('Connection opened');
    sendCommand('get_state', 0);
}

function onClose(event) {
    console.log('Connection closed');
    websocket = null;
    setTimeout(initWebSocket, 2000);
}

function onMessage(event) {
    console.log('Received: ', event.data);
    const state = JSON.parse(event.data);
    updateUI(state);
}

var wsSendMock = function (jsonString) {
    const data = JSON.parse(jsonString);
    const action = data.action;
    const value = data.value;

    var response;
    switch (action) {
        case 'start_test':
            response = {
                ab_test: {
                    preset_a: value.preset_a,
                    preset_b: value.preset_b,
                    is_running: true,
                    is_finished: false
                }
            };
            break;
        case 'stop_test':
            response = {
                ab_test: {
                    is_running: false,
                    is_finished: true
                }
            };
            break;
        case 'get_state':
            response = {
                amp_state: {
                    filter_name: 'test mode',
                    is_muted: false,
                    preset: 1,
                    eq_on: [false, true, true],
                    preset_source: [4, 1, 0],
                    current_source: 2,
                    volume_db: -42
                }
            }
            break;
        default:
            return;
    }
    onMessage({ data: JSON.stringify(response) });
}

function sendCommand(action, value) {
    const data = { action: action, value: value };
    const jsonString = JSON.stringify(data);
    if (websocket && websocket.readyState === WebSocket.OPEN) {
        websocket.send(jsonString);
        return;
    }
    console.log('Websocket not connected failed to send data ' + jsonString);
}

// Control
function setPreset(preset) {
    updatePresetButtons(preset);
    sendCommand('set_preset', preset);
}

function setVolume() {
    var volume = document.getElementById('volumeSlider').value;
    sendCommand('set_volume', parseInt(volume));
}

function updateVolumeLabel() {
    var volume = document.getElementById('volumeSlider').value;
    document.getElementById('volumeValue').innerHTML = parseInt(volume).toFixed(1) + ' dB';
}

function toggleMute() {
    var mute = document.getElementById('muteBtn').classList.toggle('active');
    sendCommand('set_mute', mute);
}

function toggleEq(preset) {
    var turnOnEq = document.getElementById('eqBtn' + preset).classList.toggle('active');
    sendCommand('set_eq_p' + preset, turnOnEq);
}

function setSource(preset) {
    var source = document.getElementById('source' + preset).value;
    sendCommand('set_source_p' + preset, parseInt(source));
}


// ABX tests

// p-value
// Function to calculate factorial
function factorial(n) {
    if (n < 0) return NaN;
    if (n === 0 || n === 1) return 1;
    let result = 1;
    for (let i = 2; i <= n; i++) {
        result *= i;
    }
    return result;
}

// Function to calculate binomial coefficient C(n, k)
function combinations(n, k) {
    if (k < 0 || k > n) {
        return 0;
    }
    if (k === 0 || k === n) {
        return 1;
    }
    if (k > n / 2) {
        k = n - k;
    }
    let res = 1;
    for (let i = 1; i <= k; i++) {
        res = res * (n - i + 1) / i;
    }
    return res;
}

// Function to calculate binomial probability mass function P(X=k)
function binomialPMF(n, k) {
    const p = 0.5; // Probability of success for ABX test
    const C_nk = combinations(n, k);
    return C_nk * Math.pow(p, k) * Math.pow(1 - p, n - k);
}

// Function to calculate the p-value for an ABX test
// This is the cumulative probability of getting k or more correct answers
function calculateABXPValue(totalTrials, correctAnswers) {
    if (totalTrials < 0 || correctAnswers < 0 || correctAnswers > totalTrials) {
        return NaN; // Invalid input
    }

    let pValue = 0;
    for (let i = correctAnswers; i <= totalTrials; i++) {
        pValue += binomialPMF(totalTrials, i);
    }
    return pValue;
}

function updateABXPresetButtons(enabledPresetBtnId) {
    ['abxPresetABtn', 'abxPresetBBtn', 'abxPresetXBtn'].forEach((id) => {
        document.getElementById(id).className = (enabledPresetBtnId === id) ? 'active' : '';
    });
}

function presetABX(presetBtnId) {
    updateABXPresetButtons(presetBtnId);
    if (abxData.mutedSwitch) {
        sendCommand('set_mute', true);
    }
    switch (presetBtnId) {
        case 'abxPresetABtn':
            setPreset(abxData.presetA);
            break;
        case 'abxPresetBBtn':
            setPreset(abxData.presetB);
            break;
        case 'abxPresetXBtn':
            setPreset(abxData.presetX);
            break;
    }
    if (abxData.mutedSwitch) {
        setTimeout(sendCommand, 1000, 'set_mute', false);
    }
}

function startAbxTest() {
    abxData.mutedSwitch = document.getElementById('abx_mute').checked;
    abxData.presetA = parseInt(document.getElementById('abxPresetA').value);
    abxData.presetB = parseInt(document.getElementById('abxPresetB').value);
    abxData.trials = parseInt(document.getElementById('abxTrials').value);
    abxData.correct = 0;
    abxData.iteration = 0;
    abxData.presetX = Math.random() < 0.5 ? abxData.presetA : abxData.presetB;

    // start with A
    presetABX('abxPresetABtn');
    switchView('abxActiveView');
}

function resetABXTest() {
    switchView('abxControlView');
}

function stopABXTest() {
    const pValue = calculateABXPValue(abxData.trials, abxData.correct);
    document.getElementById('abxResultPresetA').textContent = abxData.presetA;
    document.getElementById('abxResultPresetB').textContent = abxData.presetB;
    document.getElementById('abxResultTrials').textContent = abxData.iteration;
    document.getElementById('abxResultCorrect').textContent = abxData.correct;
    document.getElementById('abxResultPValue').textContent = pValue.toFixed(4);

    const interpretationDisplay = document.getElementById('interpretation');
    // Interpretation (common significance level alpha = 0.05)
    const alpha = 0.05;
    if (pValue <= alpha) {
        interpretationDisplay.textContent = `Since the p-value (${pValue.toFixed(4)}) is less than or equal to the common significance level of ${alpha}, we reject the null hypothesis. This suggests a statistically significant difference (unlikely to be due to chance).`;
        interpretationDisplay.classList.remove('abx-fail');
        interpretationDisplay.classList.add('abx-success');
    } else {
        interpretationDisplay.textContent = `Since the p-value (${pValue.toFixed(4)}) is greater than the common significance level of ${alpha}, we fail to reject the null hypothesis. This means there isn't enough evidence to conclude a statistically significant difference (results could be due to chance).`;
        interpretationDisplay.classList.remove('abx-success');
        interpretationDisplay.classList.add('abx-fail');
    }

    switchView('abxResultsView');
}

function getAbxPresetNumber(preset) {
    switch (preset) {
        case 'presetA':
            return abxData.presetA;
        case 'presetB':
            return abxData.presetB;
    }
}

function xIs(preset) {
    if (abxData.presetX === getAbxPresetNumber(preset)) {
        abxData.correct++;
    }
    abxData.iteration++;
    if (abxData.iteration >= abxData.trials) {
        stopABXTest();
    }
    // New X
    abxData.presetX = Math.random() < 0.5 ? abxData.presetA : abxData.presetB;
}

// AB tests
const timerEl = document.getElementById('timer');
var startTime;
var timerInterval; // Stores the interval ID for updating the stopwatch

function formatTime(timeInMs) {
    const totalSeconds = Math.floor(timeInMs / 1000); // Convert to total seconds
    const minutes = Math.floor(totalSeconds / 60);
    const seconds = totalSeconds % 60;

    // Pad with leading zeros
    const pad = (num, length = 2) => num.toString().padStart(length, '0');

    return `${pad(minutes)}:${pad(seconds)}`;
}

function updateTimer() {
    // Calculate current elapsed time: current time - start time + previously elapsed time
    const currentTime = Date.now();
    const elapsedTime = currentTime - startTime;
    timerEl.textContent = formatTime(elapsedTime);
}

function startTimer() {
    timerEl.textContent = '00:00';
    startTime = Date.now();
    timerInterval = setInterval(updateTimer, 1000);
}

function startABTest() {
    abData.mutedSwitch = document.getElementById('ab_mute').checked;
    abData.presetA = parseInt(document.getElementById('abPresetA').value);
    abData.presetB = parseInt(document.getElementById('abPresetB').value);
    abData.likesA = 0;
    abData.likesB = 0;

    const testConfig = {
        preset_a: abData.presetA,
        preset_b: abData.presetB,
        min_time: parseInt(document.getElementById('minTime').value),
        max_time: parseInt(document.getElementById('maxTime').value),
    };

    if (testConfig.min_time >= testConfig.max_time) {
        alert("Min. time must be less than max. time.");
        return;
    }
    switchView('abActiveView');
    sendCommand('start_test', testConfig);
}

function resetABTest() {
    switchView('abControlView');
}

function stopABTest() {
    sendCommand('stop_test', 0);

    clearInterval(timerInterval);

    document.getElementById('likesA').textContent = abData.likesA;
    document.getElementById('likesB').textContent = abData.likesB;
    switchView('abResultsView');
}

function stopABTestMode() {
    sendCommand('disable_test_mode', 0);
    switchView('mainControlView');
}

function logLike() {
    switch (currentPreset) {
        case abData.presetA:
            abData.likesA++;
            break;
        case abData.presetB:
            abData.likesB++;
            break;
        default:
            console.error('Trying to log like for unkown preset ' + currentPreset);
    }
}

// UI things
function sanitizePresetSelection(a, b) {
    const x = parseInt(document.getElementById(a).value);
    const y = parseInt(document.getElementById(b).value);
    if (x === y) {
        document.getElementById(b).value = [1, 2, 3].filter(value => value !== x)[0];
    }
}

function switchView(viewId) {
    document.getElementById('mainControlView').style.display = viewId === 'mainControlView' ? 'block' : 'none';

    document.getElementById('abControlView').style.display = viewId === 'abControlView' ? 'block' : 'none';
    document.getElementById('abActiveView').style.display = viewId === 'abActiveView' ? 'block' : 'none';
    document.getElementById('abResultsView').style.display = viewId === 'abResultsView' ? 'block' : 'none';

    document.getElementById('abxControlView').style.display = viewId === 'abxControlView' ? 'block' : 'none';
    document.getElementById('abxActiveView').style.display = viewId === 'abxActiveView' ? 'block' : 'none';
    document.getElementById('abxResultsView').style.display = viewId === 'abxResultsView' ? 'block' : 'none';

    if (viewId === 'abActiveView') {
        startTimer();
    }
}

function updatePresetButtons(enabledPreset) {
    ['preset1', 'preset2', 'preset3'].forEach((id, index) => {
        document.getElementById(id).className = (enabledPreset === (index + 1)) ? 'active' : '';
    });
}

function disableUI(disable) {
    const buttons = document.querySelectorAll('button');
    buttons.forEach(button => {
        button.disabled = disable;
    });
    const selects = document.querySelectorAll('select');
    selects.forEach(select => {
        select.disabled = disable;
    });
    const inputs = document.querySelectorAll('input');
    inputs.forEach(input => {
        input.disabled = disable;
    });

    document.body.classList.toggle('connected', !disable);
}

function getScanLabel(source) {
    switch (source) {
        case 1:
            return "Scan (XLR)"
        case 2:
            return "Scan (RCA)"
        case 4:
            return "Scan (SPDIF)"
        case 5:
            return "Scan (AES)"
        case 6:
            return "Scan (OPT)"
        case 7:
            return "Scan (EXT)"
    }
    return "Scan";
}

function updateUI(state) {
    if (state.amp_state) {
        const amp = state.amp_state;
        currentPreset = amp.preset ? amp.preset : 0;
        document.getElementById('filterName').innerHTML = amp.filter_name != "" ? amp.filter_name : "NOT CONNECTED";
        document.getElementById('muteBtn').className = amp.is_muted ? 'active' : '';
        updatePresetButtons(amp.preset);
        for (let i = 0; i < 3; i++) {
            const eqBtn = document.getElementById(`eqBtn${i + 1}`);
            eqBtn.classList.toggle('active', amp.eq_on[i]);
            const source = document.getElementById(`source${i + 1}`);
            source.value = amp.preset_source[i];
            // If current presets source is SCAN update auto dected source
            source[0].label = getScanLabel(amp.preset_source[i] == 0 && amp.preset == i + 1 ? amp.current_source : 0);
        }
        document.getElementById('volumeSlider').value = amp.volume_db;
        updateVolumeLabel();
    }
    if (state.ab_test) {
        const ab_state = state.ab_test;
        if (ab_state.preset_a) {
            document.getElementById('resultPresetA').textContent = ab_state.preset_a;
        }
        if (ab_state.preset_b) {
            document.getElementById('resultPresetB').textContent = ab_state.preset_b;
        }
        if (ab_state.is_running) {
            switchView('abActiveView');
        } else if (ab_state.is_finished) {
            switchView('abResultsView');
        } else {
            switchView('abControlView');
        }
    } else {
        switchView('mainControlView');
    }
    disableUI(currentPreset == 0);
}
