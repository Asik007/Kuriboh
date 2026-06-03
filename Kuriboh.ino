/*
  Sony S‑Link (Control‑S) Web Controller for ESP8266
  Cleaned & power‑optimised version.

  Protocol summary:
    - Sync pulse: HIGH 2400 µs, LOW 600 µs (delimiter)
    - Bit 1:      HIGH 1200 µs, LOW 600 µs
    - Bit 0:      HIGH 600 µs,  LOW 600 µs
    - Inter‑frame gap: bus idle (LOW) for > 2 ms
*/

#include <ArduinoOTA.h>
#include <ESP8266mDNS.h>
#include <ESP8266WiFiMulti.h>
#include <ESP8266WebServer.h>
#include "secrets.h"          // contains WIFI_SSID, WIFI_PASSWORD

// ----- Pin definitions -----
#define OUTPUT_PIN  D2
#define INPUT_PIN   D1

// ----- Network objects -----
ESP8266WiFiMulti wifiMulti;
ESP8266WebServer server(80);

// ----- S‑Link timing constants (microseconds) -----
const unsigned long DELIMITER_US   =  600;
const unsigned long BIT_0_HIGH_US  =  600;
const unsigned long BIT_1_HIGH_US  = 1200;
const unsigned long SYNC_HIGH_US   = 2400;
const unsigned long IDLE_GAP_US    = 2200;

// High‑pulse thresholds after scaling by 10 µs units
const int THRESHOLD_BIT  =  90;   // 900 µs → everything above is bit‑1
const int THRESHOLD_SYNC = 180;   // 1800 µs → above this is a sync pulse

// ----- Circular buffer for high‑pulse widths (scaled) -----
static const uint8_t PULSE_BUFFER_SIZE = 100;
volatile uint8_t bufferReadPos  = 0;
volatile uint8_t bufferWritePos = 0;
volatile uint8_t pulseBuffer[PULSE_BUFFER_SIZE];

// ----- Shared flags / variables (ISR <-> main) -----
volatile unsigned long lastEdgeTime = 0;
volatile bool bufferOverflowDetected = false;

// Transmission guard: ISR ignores edges while we are sending
volatile bool txActive = false;

// ----- Message handling for the web client -----
String  receivedMessageQueue = "";
bool    newMessageAvailable  = false;

// ==================== Forward declarations ====================
String decodeSlinkMessage(uint8_t bytes[], uint8_t len);
String getSourceName(uint8_t code);
void   handleRoot();
void   handleSendCommand();
void   handleGetMessage();
void   busChange();
void   processSlinkInput();
bool   isBusIdle();
void   sendPulseDelimiter();
void   sendSyncPulse();
void   sendBit(int bit);
void   sendByte(int value);
void   idleAfterCommand();
bool   sendCommand(uint8_t command[], int length);

// ==================== Setup ====================
void setup() {
  Serial.begin(115200);
  Serial.println("\nSony S‑Link Web Controller starting...");

  ArduinoOTA.begin();

  WiFi.hostname("sony_slink");
  WiFi.mode(WIFI_STA);
  wifiMulti.addAP(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Connecting to WiFi");
  while (wifiMulti.run() != WL_CONNECTED) {
    delay(100);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("Connected! IP address: ");
  Serial.println(WiFi.localIP());

  // ===== Power‑saving: allow modem to sleep when idle =====
  WiFi.setSleepMode(WIFI_MODEM_SLEEP);

  server.on("/",        handleRoot);
  server.on("/send",    handleSendCommand);
  server.on("/message", handleGetMessage);
  server.begin();
  Serial.println("Web server started.");

  pinMode(OUTPUT_PIN, OUTPUT);
  digitalWrite(OUTPUT_PIN, LOW);
  pinMode(INPUT_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(INPUT_PIN), busChange, CHANGE);
}

// ==================== Main loop ====================
void loop() {
  if (wifiMulti.run() == WL_CONNECTED) {
    processSlinkInput();
    server.handleClient();
    ArduinoOTA.handle();
    // Short delay to let the Wi‑Fi modem sleep between busy periods
    delay(1);
  }
}

// -------------------- Web page --------------------
void handleRoot() {
  // Raw HTML / CSS / JS – identical to original except for the
  // duplicated CD button, which is now a correct DVD button.
  static const char html[] = R"rawliteral(
<!DOCTYPE html>
<!doctype html>
<!doctype html>
<html lang="en">
    <head>
        <meta charset="UTF-8" />
        <meta name="viewport" content="width=device-width, initial-scale=1.0" />
        <title>Sony AV Controller · S-Link</title>
        <style>
            :root {
                --bg: #09090b;
                --card: #121214;
                --border: #27272a;
                --primary: #a3c9ff;
                --text: #e5e2e1;
                --muted: #8a919f;
                --green: #22c55e;
                --red: #ef4444;
            }
            * {
                box-sizing: border-box;
                margin: 0;
                padding: 0;
                user-select: none;
                -webkit-user-select: none;
            }
            body {
                background-color: var(--bg);
                color: var(--text);
                font-family:
                    -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto,
                    sans-serif;
                display: flex;
                flex-direction: column;
                align-items: center;
                justify-content: flex-start;
                min-height: 100vh;
                padding: 16px;
            }
            .container {
                width: 100%;
                max-width: 520px;
                display: flex;
                flex-direction: column;
                gap: 18px;
            }
            /* VFD panel */
            .vfd-box {
                background-color: #030303;
                border: 1px solid var(--border);
                border-radius: 12px;
                padding: 16px 20px;
                display: flex;
                justify-content: space-between;
                align-items: center;
                box-shadow: inset 0 2px 8px rgba(0, 0, 0, 0.8);
            }
            .vfd-status {
                display: flex;
                flex-direction: column;
                gap: 6px;
            }
            .label {
                font-size: 10px;
                letter-spacing: 0.1em;
                color: var(--muted);
                text-transform: uppercase;
                font-weight: 600;
            }
            .vfd-val {
                font-family: "Courier New", Courier, monospace;
                font-size: 34px;
                font-weight: bold;
                color: var(--green);
                text-shadow: 0 0 12px rgba(34, 197, 148, 0.7);
                letter-spacing: 2px;
                transition: all 0.2s;
            }
            .vfd-val.standby {
                color: var(--red);
                text-shadow: 0 0 12px rgba(239, 68, 68, 0.7);
            }
            .power-btn {
                background-color: #1a1a1c;
                border: 1px solid var(--border);
                border-radius: 12px;
                width: 70px;
                height: 70px;
                display: flex;
                flex-direction: column;
                align-items: center;
                justify-content: center;
                gap: 6px;
                cursor: pointer;
                transition: all 0.15s ease;
            }
            .power-btn:active {
                transform: scale(0.95);
                background-color: #0d0d0e;
                box-shadow: inset 0 2px 5px rgba(0, 0, 0, 0.5);
            }
            .power-icon {
                width: 26px;
                height: 26px;
                transition: stroke 0.2s;
            }
            .power-btn.on .power-icon {
                stroke: var(--green);
                filter: drop-shadow(0 0 5px var(--green));
            }
            .power-btn.off .power-icon {
                stroke: var(--red);
                filter: drop-shadow(0 0 5px var(--red));
            }
            .power-sub {
                font-size: 10px;
                letter-spacing: 0.1em;
                font-weight: bold;
            }
            .power-btn.on .power-sub {
                color: var(--green);
            }
            .power-btn.off .power-sub {
                color: var(--red);
            }

            /* section headers */
            .section-header {
                border-bottom: 1px solid var(--border);
                padding-bottom: 6px;
                margin-bottom: 10px;
                display: flex;
                justify-content: space-between;
                align-items: flex-end;
            }
            .grid-3 {
                display: grid;
                grid-template-columns: repeat(3, 1fr);
                gap: 10px;
                width: 100%;
            }
            .grid-4 {
                display: grid;
                grid-template-columns: repeat(4, 1fr);
                gap: 10px;
            }
            .grid-2 {
                display: grid;
                grid-template-columns: repeat(2, 1fr);
                gap: 10px;
            }
            .input-btn {
                background-color: var(--card);
                border: 1px solid var(--border);
                color: var(--text);
                border-radius: 8px;
                padding: 12px 6px;
                font-size: 12px;
                font-family: monospace;
                font-weight: bold;
                cursor: pointer;
                text-align: center;
                transition: all 0.15s ease;
            }
            /*.input-btn:active {
                transform: scale(0.96);
            }*/
            .input-btn.active {
                border-color: var(--primary);
                color: var(--primary);
                box-shadow: 0 0 6px rgba(163, 201, 255, 0.3);
                text-shadow: 0 0 4px rgba(163, 201, 255, 0.5);
            }
            .dvd-btn {
                grid-column: span 3;
            }
            .vol {
                grid-row: span 2;
                height: 140px; /* Add this line */
            }
            .terminal {
                background-color: #030303;
                border: 1px solid var(--border);
                border-radius: 10px;
                padding: 12px;
                font-family: monospace;
                font-size: 11px;
                color: #22c55e;
                height: 140px;
                overflow-y: auto;
                display: flex;
                flex-direction: column;
                gap: 5px;
            }
            .terminal-header {
                color: var(--muted);
                font-size: 10px;
                font-weight: bold;
                margin-bottom: 4px;
            }
            .terminal-item {
                opacity: 0.95;
                word-break: break-all;
                font-family: "Courier New", monospace;
                border-left: 2px solid #22c55e;
                padding-left: 8px;
                margin-bottom: 2px;
            }
            .clear-log {
                background: none;
                border: 1px solid var(--border);
                color: var(--muted);
                border-radius: 20px;
                padding: 4px 10px;
                font-size: 10px;
                cursor: pointer;
                transition: 0.1s;
            }
            .clear-log:hover {
                background: #1f1f24;
                color: white;
            }
            hr {
                border-color: var(--border);
                margin: 4px 0;
            }
        </style>
    </head>
    <body>
        <div class="container">
            <!-- VFD status panel -->
            <div class="vfd-box">
                <div class="vfd-status">
                    <span class="label">System Power Status</span>
                    <span id="display-state" class="vfd-val">ACTIVE</span>
                </div>
                <div id="pwr-btn" class="power-btn on" onclick="togglePower()">
                    <svg
                        class="power-icon"
                        viewBox="0 0 24 24"
                        fill="none"
                        stroke="currentColor"
                        stroke-width="2"
                        stroke-linecap="round"
                        stroke-linejoin="round"
                    >
                        <path d="M18.36 6.64a9 9 0 1 1-12.73 0"></path>
                        <line x1="12" y1="2" x2="12" y2="12"></line>
                    </svg>
                    <span id="pwr-status" class="power-sub">ON</span>
                </div>
            </div>

            <!-- Input selector (source) -->
            <div class="section-header">
                <span class="label">INPUT SELECTOR</span>
            </div>
            <div class="grid-3">
                <button
                    id="btn-dvd"
                    class="input-btn"
                    onclick="selectInput('dvd', 'C05019', 'DVD')"
                >
                    DVD (Turntable)
                </button>
                <button
                    id="btn-tape"
                    class="input-btn"
                    onclick="selectInput('tape', 'C05005', 'TAPE')"
                >
                    TAPE (PC)
                </button>
                <button
                    id="btn-video1"
                    class="input-btn active"
                    onclick="selectInput('video1', 'C05011', 'VIDEO 2')"
                >
                    VIDEO 2 (TV)
                </button>
                <button
                    id="btn-cd"
                    class="input-btn dvd-btn"
                    onclick="selectInput('cd', 'C05002', 'CD')"
                >
                    CD (Eq/Server)
                </button>
            </div>

            <!-- Volume & audio controls -->
            <div class="section-header">
                <span class="label">VOLUME / AUDIO</span>
            </div>
            <!-- Volume up and down should be larger buttons that are either side by side and span the entire width or large buttons vertical to each other. Mute and unmute can go below them-->
            <div class="grid-2">
                <button class="input-btn vol" onclick="sendVolCmd('C015')">
                    VOL -
                </button>
                <button class="input-btn vol" onclick="sendVolCmd('C014')">
                    VOL +
                </button>
                <button class="input-btn" onclick="sendVolCmd('C006')">
                    MUTE
                </button>
                <button class="input-btn" onclick="sendVolCmd('C007')">
                    UNMUTE
                </button>
            </div>

            <!-- Digital input type -->
            <div class="section-header">
                <span class="label">DIGITAL INPUT TYPE</span>
            </div>
            <div class="grid-3">
                <button class="input-btn" onclick="sendCustomCmd('C08301')">
                    OPTICAL
                </button>
                <button class="input-btn" onclick="sendCustomCmd('C08302')">
                    COAX
                </button>
                <button class="input-btn" onclick="sendCustomCmd('C08304')">
                    ANALOG
                </button>
            </div>

            <!-- Status queries -->
            <div class="section-header">
                <span class="label">STATUS QUERIES</span>
            </div>
            <div class="grid-4">
                <button class="input-btn" onclick="sendCustomCmd('C00F')">
                    SOURCE STATUS
                </button>
                <button class="input-btn" onclick="sendCustomCmd('C06A')">
                    DEVICE NAME
                </button>
                <button class="input-btn" onclick="sendCustomCmd('C043')">
                    INPUT TYPE
                </button>
                <button class="input-btn" onclick="sendCustomCmd('C00E')">
                    2ND AUDIO STATUS
                </button>
            </div>

            <div class="grid-2">
                <button class="input-btn" onclick="sendCustomCmd('C00C')">
                    5.1 INPUT ON
                </button>
                <button class="input-btn" onclick="sendCustomCmd('C00D')">
                    5.1 INPUT OFF
                </button>
            </div>

            <!-- Terminal / log -->
            <div class="section-header">
                <span class="label">// S-Link RESPONSE TERMINAL</span>
                <button id="clearLogBtn" class="clear-log">CLEAR LOG</button>
            </div>
            <div id="term" class="terminal">
                <div class="terminal-item">> SYSTEM READY</div>
                <div class="terminal-item">> POWER STATE: ON</div>
                <div class="terminal-item">> INPUT: VIDEO 1 (C05010)</div>
            </div>
        </div>

        <script>
            let powerOn = true;
            let selectedInput = "video1";
            let pollInterval = null;

            // Helper: send raw hex command (no spaces) to /send
            function sendRawHex(hexCmd, logPrefix = "TX") {
                if (!powerOn && hexCmd !== "C02E") {
                    logMessage("Power is OFF. Turn on first.");
                    return false;
                }
                fetch("/send?cmd=" + hexCmd)
                    .then((response) => response.text())
                    .then((txt) => {
                        logMessage(
                            `${logPrefix}: ${hexCmd.toUpperCase()} → ${txt}`,
                        );
                    })
                    .catch((err) => {
                        logMessage(`ERR: ${err.message} (command ${hexCmd})`);
                    });
                return true;
            }

            function logMessage(msg) {
                const term = document.getElementById("term");
                const item = document.createElement("div");
                item.className = "terminal-item";
                const timestamp = new Date().toLocaleTimeString();
                item.innerText = `[${timestamp}] ${msg}`;
                term.appendChild(item);
                term.scrollTop = term.scrollHeight;
                // keep last 200 messages to avoid overflow
                while (term.children.length > 220)
                    term.removeChild(term.firstChild);
            }

            // Power toggle
            function togglePower() {
                powerOn = !powerOn;
                const pwrBtn = document.getElementById("pwr-btn");
                const pwrStatus = document.getElementById("pwr-status");
                const displayState = document.getElementById("display-state");

                if (powerOn) {
                    pwrBtn.className = "power-btn on";
                    pwrStatus.innerText = "ON";
                    displayState.innerText = "ACTIVE";
                    displayState.classList.remove("standby");
                    sendRawHex("C02E", "POWER ON");
                } else {
                    pwrBtn.className = "power-btn off";
                    pwrStatus.innerText = "OFF";
                    displayState.innerText = "STANDBY";
                    displayState.classList.add("standby");
                    sendRawHex("C02F", "POWER OFF");
                }
            }

            // Input selection (with active class update)
            function selectInput(inputId, hexCode, label) {
                if (!powerOn) {
                    logMessage(
                        `Cannot switch to ${label}: amplifier is in STANDBY`,
                    );
                    return;
                }
                document
                    .querySelectorAll(".input-btn")
                    .forEach((btn) => btn.classList.remove("active"));
                const targetBtn = document.getElementById("btn-" + inputId);
                if (targetBtn) targetBtn.classList.add("active");
                selectedInput = inputId;
                sendRawHex(hexCode, `INPUT ${label}`);
            }

            // Volume / mute commands (same as custom, but explicit)
            function sendVolCmd(hexCode) {
                if (!powerOn) {
                    logMessage("Power is OFF - command ignored");
                    return;
                }
                sendRawHex(hexCode, "VOL/CTRL");
            }

            function sendCustomCmd(hexCode) {
                if (!powerOn) {
                    logMessage("Power is OFF - command ignored");
                    return;
                }
                sendRawHex(hexCode, "CMD");
            }

            // Poll /message for incoming S-Link replies (decoded by ESP)
            function fetchPendingMessages() {
                fetch("/message")
                    .then((res) => res.text())
                    .then((data) => {
                        if (data && data !== "No new response yet...") {
                            // split by newline, each line is a decoded S-Link message
                            const lines = data.split("\n");
                            for (let line of lines) {
                                if (line.trim() !== "") {
                                    logMessage(`RX: ${line}`);
                                    // Optional: update active input if status message contains source name
                                    updateUiFromReply(line);
                                }
                            }
                        }
                    })
                    .catch((err) => console.warn("Poll error:", err));
            }

            // try to keep UI in sync with status replies (e.g., "Status -> Audio: Video 1")
            function updateUiFromReply(reply) {
                if (!reply) return;
                const lowerReply = reply.toLowerCase();
                if (lowerReply.includes("audio:")) {
                    // extract source name like "video 1", "cd", "tuner", etc.
                    const match = reply.match(/audio:\s*([\w\s]+?)(?:\||$)/i);
                    if (match && match[1]) {
                        let src = match[1]
                            .trim()
                            .toLowerCase()
                            .replace(/\s+/g, "");
                        const mapping = {
                            tuner: "tuner",
                            cd: "cd",
                            md: "md",
                            tape: "tape",
                            video1: "video1",
                            video2: "video2",
                            dvd: "dvd",
                        };
                        let foundId = null;
                        if (src === "video1") foundId = "video1";
                        else if (src === "video2") foundId = "video2";
                        else if (src === "tuner") foundId = "tuner";
                        else if (src === "cd") foundId = "cd";
                        else if (src === "md") foundId = "md";
                        else if (src === "tape") foundId = "tape";
                        else if (src === "dvd") foundId = "dvd";

                        if (
                            foundId &&
                            document.getElementById("btn-" + foundId)
                        ) {
                            document
                                .querySelectorAll(".input-btn")
                                .forEach((btn) =>
                                    btn.classList.remove("active"),
                                );
                            document
                                .getElementById("btn-" + foundId)
                                .classList.add("active");
                            selectedInput = foundId;
                        }
                    }
                }
                // optional: if power status appears in reply, sync power UI
                if (
                    lowerReply.includes("power status: on") ||
                    lowerReply.includes("on sequence")
                ) {
                    if (!powerOn) {
                        // sync local power state
                        powerOn = true;
                        const pwrBtn = document.getElementById("pwr-btn");
                        const pwrStatus = document.getElementById("pwr-status");
                        const displayState =
                            document.getElementById("display-state");
                        pwrBtn.className = "power-btn on";
                        pwrStatus.innerText = "ON";
                        displayState.innerText = "ACTIVE";
                        displayState.classList.remove("standby");
                    }
                }
                if (
                    lowerReply.includes("standby") ||
                    lowerReply.includes("power off")
                ) {
                    if (powerOn) {
                        powerOn = false;
                        const pwrBtn = document.getElementById("pwr-btn");
                        const pwrStatus = document.getElementById("pwr-status");
                        const displayState =
                            document.getElementById("display-state");
                        pwrBtn.className = "power-btn off";
                        pwrStatus.innerText = "OFF";
                        displayState.innerText = "STANDBY";
                        displayState.classList.add("standby");
                    }
                }
            }

            // Clear terminal
            function clearTerminal() {
                const term = document.getElementById("term");
                term.innerHTML =
                    '<div class="terminal-item">> Log cleared.</div>';
            }

            // Start polling for device responses
            function startPolling() {
                if (pollInterval) clearInterval(pollInterval);
                pollInterval = setInterval(fetchPendingMessages, 550);
            }

            // attach clear button
            window.onload = () => {
                startPolling();
                const clearBtn = document.getElementById("clearLogBtn");
                if (clearBtn) clearBtn.onclick = () => clearTerminal();
                // initially sync power state UI (active)
                powerOn = true;
                document.getElementById("pwr-btn").className = "power-btn on";
                document.getElementById("pwr-status").innerText = "ON";
                document.getElementById("display-state").innerText = "ACTIVE";
            };
        </script>
    </body>
</html>

)rawliteral";
  server.send(200, "text/html", html);
}

// -------------------- Command handler --------------------
void handleSendCommand() {
  if (!server.hasArg("cmd")) {
    server.send(400, "text/plain", "Missing command");
    return;
  }
  String hexStr = server.arg("cmd");
  hexStr.toUpperCase();
  if (hexStr.length() % 2 != 0) {
    server.send(400, "text/plain", "Hex string must have even length");
    return;
  }
  int length = hexStr.length() / 2;
  uint8_t command[length];
  for (int i = 0; i < length; i++) {
    String hexByte = hexStr.substring(i * 2, i * 2 + 2);
    if (!isHexadecimalDigit(hexByte[0]) || !isHexadecimalDigit(hexByte[1])) {
      server.send(400, "text/plain", "Invalid hex character");
      return;
    }
    command[i] = strtol(hexByte.c_str(), nullptr, 16);
  }
  Serial.print("Sending S‑Link command: ");
  for (int i = 0; i < length; i++) {
    if (command[i] < 0x10) Serial.print("0");
    Serial.print(command[i], HEX);
    Serial.print(" ");
  }
  Serial.println();
  if (sendCommand(command, length)) {
    server.send(200, "text/plain", "Command sent");
  } else {
    server.send(503, "text/plain", "Bus busy or timeout");
  }
}

void handleGetMessage() {
  if (newMessageAvailable && receivedMessageQueue.length() > 0) {
    server.send(200, "text/plain", receivedMessageQueue);
    receivedMessageQueue = "";
    newMessageAvailable = false;
  } else {
    server.send(200, "text/plain", "No new response yet...");
  }
}

// ==================== S‑Link input (ISR + processing) ====================
IRAM_ATTR void busChange() {
  if (txActive) return;   // ignore edges during our own transmission

  unsigned long now = micros();
  static unsigned long riseTime = 0;
  int state = digitalRead(INPUT_PIN);

  if (state == HIGH) {
    riseTime = now;
  } else {
    unsigned long highWidth = now - riseTime;
    lastEdgeTime = now;

    if ((bufferWritePos + 1) % PULSE_BUFFER_SIZE == bufferReadPos) {
      bufferOverflowDetected = true;
      return;
    }
    uint8_t scaled = (highWidth / 10) > 255 ? 255 : (highWidth / 10);
    pulseBuffer[bufferWritePos] = scaled;
    bufferWritePos = (bufferWritePos + 1) % PULSE_BUFFER_SIZE;
  }
}

void processSlinkInput() {
  static uint8_t currentByte = 0;
  static uint8_t currentBit  = 0;
  static uint8_t msgBytes[32];
  static uint8_t msgLen      = 0;
  bool completeFrame = false;

  while (bufferReadPos != bufferWritePos) {
    uint8_t scaled = pulseBuffer[bufferReadPos];
    bufferReadPos = (bufferReadPos + 1) % PULSE_BUFFER_SIZE;
    unsigned int highWidth = scaled * 10;

    if (highWidth >= (unsigned int)(THRESHOLD_SYNC * 10)) {
      currentByte = 0;
      currentBit  = 0;
      msgLen      = 0;
    } else {
      currentBit++;
      if (highWidth >= (unsigned int)(THRESHOLD_BIT * 10))
        bitSet(currentByte, 8 - currentBit);
      else
        bitClear(currentByte, 8 - currentBit);

      if (currentBit == 8) {
        if (msgLen < sizeof(msgBytes))
          msgBytes[msgLen++] = currentByte;
        currentBit = 0;
      }
    }
    if (isBusIdle()) { completeFrame = true; break; }
  }
  if (bufferReadPos == bufferWritePos && isBusIdle())
    completeFrame = true;

  if (completeFrame && (msgLen > 0 || currentBit != 0)) {
    if (msgLen > 0) {
      String decoded = decodeSlinkMessage(msgBytes, msgLen);
      if (receivedMessageQueue.length() > 0) receivedMessageQueue += "\n";
      receivedMessageQueue += decoded;
      newMessageAvailable = true;
      Serial.print("S‑Link frame: ");
      Serial.println(decoded);
    }
    if (bufferOverflowDetected) {
      Serial.println("WARNING: Pulse buffer overflow!");
      bufferOverflowDetected = false;
    }
    if (currentBit != 0)
      Serial.printf("WARNING: %d stray bits received\n", currentBit);

    currentByte = 0;
    currentBit  = 0;
    msgLen      = 0;
  }
}

bool isBusIdle() {
  noInterrupts();
  bool idle = (micros() - lastEdgeTime) > IDLE_GAP_US;
  interrupts();
  return idle;
}

// ==================== S‑Link Decoder ====================
String getSourceName(uint8_t code) {
  switch (code) {
    case 0x00: return "Tuner";
    case 0x02: return "CD";
    case 0x04: return "MD";
    case 0x05: return "Tape";
    case 0x10: return "Video 1";
    case 0x11: return "Video 2";
    case 0x19: return "DVD";
    default:   return "Unknown (0x" + String(code, HEX) + ")";
  }
}

String decodeSlinkMessage(uint8_t bytes[], uint8_t len) {
  String output = "";
  for (uint8_t i = 0; i < len; i++) {
    if (bytes[i] < 0x10) output += "0";
    output += String(bytes[i], HEX) + " ";
  }
  output.toUpperCase();
  output += "-> ";
  uint8_t idx = 0;

  if (bytes[0] == 0xC8) {
    output += "[Receiver] ";
    idx = 1;
  }
  if (idx >= len) {
    output += "Device ID Handshake / Ping";
    return output;
  }

  uint8_t cmd = bytes[idx];
  if (cmd == 0x6A) {
    output += "Device Name: ";
    for (uint8_t i = idx + 1; i < len; i++) {
      output += (bytes[i] >= 32 && bytes[i] <= 126) ? (char)bytes[i] : '.';
    }
  } else if (cmd == 0x43 && (len - idx) >= 3) {
    uint8_t tt = bytes[idx + 1];
    output += "Input Connection: ";
    switch (tt) {
      case 0x01: output += "Optical"; break;
      case 0x02: output += "Coax";    break;
      case 0x04: output += "Analog";  break;
      default:   output += "Unknown Type (0x" + String(tt, HEX) + ")";
    }
  } else if (cmd == 0x61 && (len - idx) >= 7 &&
             bytes[idx+1]==0xC3 && bytes[idx+2]==0x87 &&
             bytes[idx+3]==0x0F && bytes[idx+4]==0x1F &&
             bytes[idx+5]==0x3F && bytes[idx+6]==0x7F) {
    output += "Power Status: ON Sequence";
  } else if (cmd == 0x70 && (len - idx) >= 4) {
    uint8_t audioSrc = bytes[idx + 1];
    uint8_t videoSrc = bytes[idx + 2];
    uint8_t flags    = bytes[idx + 3];
    output += "Status -> Audio: " + getSourceName(audioSrc);
    output += " | Video: " + getSourceName(videoSrc);
    output += " | Flags: [";
    bool first = true;
    if (flags & 0x10) { output += "5.1 Input"; first = false; }
    if (flags & 0x08) { if (!first) output += ", "; output += "Tape Loop"; first = false; }
    if (flags & 0x02) { if (!first) output += ", "; output += "Muted"; }
    output += "]";
  } else if (cmd == 0x71 && (len - idx) >= 2) {
    uint8_t src = bytes[idx + 1];
    output += "2nd Audio Zone Status -> Source: " + getSourceName(src);
  } else {
    output += "Unmapped S‑Link Frame";
  }
  return output;
}

// ==================== S‑Link Transmitter (power‑optimised) ====================
void sendPulseDelimiter() {
  digitalWrite(OUTPUT_PIN, LOW);
  delayMicroseconds(DELIMITER_US);
}
void sendSyncPulse() {
  digitalWrite(OUTPUT_PIN, HIGH);
  delayMicroseconds(SYNC_HIGH_US);
  sendPulseDelimiter();
}
void sendBit(int bit) {
  digitalWrite(OUTPUT_PIN, HIGH);
  delayMicroseconds(bit ? BIT_1_HIGH_US : BIT_0_HIGH_US);
  sendPulseDelimiter();
}
void sendByte(int value) {
  for (int i = 7; i >= 0; --i) sendBit(bitRead(value, i));
}
void idleAfterCommand() {
  delay(5);   // minimal gap – enough to satisfy the protocol
}

/*
  Sends a command on the S‑Link bus.
  No longer disables all interrupts – only the input‑capture ISR is
  temporarily ignored, so Wi‑Fi stays responsive.
*/
bool sendCommand(uint8_t command[], int length) {
  const unsigned long TIMEOUT_MS = 200;
  unsigned long start = millis();

  while (!isBusIdle()) {
    if (millis() - start > TIMEOUT_MS) {
      Serial.println("ERROR: Bus busy, timeout waiting to send command.");
      return false;
    }
    yield();
  }

  txActive = true;   // block busChange() from recording our own pulses
  sendSyncPulse();
  for (int i = 0; i < length; ++i) sendByte(command[i]);
  txActive = false;

  idleAfterCommand();
  return true;
}