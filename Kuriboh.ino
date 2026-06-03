/*
  Sony S-Link (Control-S) Web Controller for ESP8266

  Protocol summary:
    - Sync pulse: HIGH 2400 µs, LOW 600 µs (delimiter)
    - Bit 1:      HIGH 1200 µs, LOW 600 µs
    - Bit 0:      HIGH 600 µs,  LOW 600 µs
    - Inter-frame gap: bus idle (LOW) for > 2 ms
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
const unsigned long DELIMITER_US   =  600;   // low time after any pulse
const unsigned long BIT_0_HIGH_US  =  600;
const unsigned long BIT_1_HIGH_US  = 1200;
const unsigned long SYNC_HIGH_US   = 2400;
const unsigned long IDLE_GAP_US    = 2200;   // minimum idle gap to declare end-of-frame

// High-pulse thresholds after scaling by 10 µs units
const int THRESHOLD_BIT      =  (900 / 10);   // 90  → everything above is bit-1
const int THRESHOLD_SYNC     = (1800 / 10);   // 180 → above this is a sync pulse

// ----- Circular buffer for high-pulse widths (scaled) -----
static const uint8_t PULSE_BUFFER_SIZE = 100;
volatile uint8_t bufferReadPos  = 0;
volatile uint8_t bufferWritePos = 0;
volatile uint8_t pulseBuffer[PULSE_BUFFER_SIZE];

// ----- Shared flags / variables (ISR <-> main) -----
volatile unsigned long lastEdgeTime = 0;          // micros() of last pin change
volatile bool bufferOverflowDetected = false;

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
bool   sendCommand(uint8_t command[], int length);   // returns true on success

// ==================== Setup ====================
void setup() {
  Serial.begin(115200);
  Serial.println("\nSony S-Link Web Controller starting...");

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
    processSlinkInput();       // decode any pulses
    server.handleClient();     // serve web requests
    ArduinoOTA.handle();       // OTA updates
  }
}

// -------------------- Web page (raw string literal) --------------------
void handleRoot() {
  // The entire HTML is stored as a raw string literal for clarity.
  // All JavaScript is embedded directly.
  static const char html[] = R"rawliteral(
<!DOCTYPE html>
<!doctype html>
<html lang="en"><!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>ESP8266 Amp Controller</title>
    <style>
        :root {
            --bg-body: #111111;
            --bg-panel: #1c1c1c;
            --bg-btn: #2a2a2a;
            --bg-btn-active: #333333;
            --text-primary: #dddddd;
            --text-secondary: #888888;
            --text-display: #6eb5f7;
            --accent-green: #4caf50;
            --accent-blue: #4fc3f7;
            --border-radius: 6px;
            --btn-padding: 16px;
        }

        body {
            background-color: var(--bg-body);
            color: var(--text-primary);
            font-family: 'Helvetica Neue', Helvetica, Arial, sans-serif;
            display: flex;
            justify-content: center;
            margin: 0;
            padding: 20px;
        }

        .container {
            width: 100%;
            max-width: 480px;
        }

        /* Common Panel Style */
        .panel {
            background-color: var(--bg-panel);
            border-radius: var(--border-radius);
            padding: 20px;
            margin-bottom: 20px;
            box-shadow: 0 2px 10px rgba(0,0,0,0.4);
        }

        /* Header Label */
        .label-header {
            text-align: center;
            color: var(--text-secondary);
            font-size: 0.85rem;
            letter-spacing: 1px;
            margin-bottom: 12px;
            text-transform: uppercase;
        }

        /* ----- STATUS PANEL ----- */
        .status-panel {
            display: flex;
            justify-content: space-between;
            align-items: center;
        }

        .status-display {
            font-size: 2.5rem;
            font-weight: 300;
            color: var(--text-display);
            font-family: 'Courier New', Courier, monospace;
            text-shadow: 0 0 8px rgba(79, 195, 247, 0.3);
        }

        .btn-power {
            background-color: var(--bg-btn);
            border: 1px solid #444;
            border-radius: var(--border-radius);
            width: 70px;
            height: 70px;
            display: flex;
            flex-direction: column;
            justify-content: center;
            align-items: center;
            cursor: pointer;
            transition: all 0.2s;
            color: #666;
        }

        .btn-power.on {
            border-color: var(--accent-green);
            color: var(--accent-green);
        }

        .btn-power svg {
            width: 28px;
            height: 28px;
            fill: currentColor;
        }

        .btn-power span {
            font-size: 0.7rem;
            margin-top: 2px;
            letter-spacing: 1px;
        }

        /* ----- VOLUME PANEL ----- */
        .vol-controls {
            display: flex;
            justify-content: center;
            align-items: center;
            gap: 15px;
            margin-bottom: 15px;
        }

        .btn-vol {
            background-color: var(--bg-btn);
            border: 1px solid #3a3a3a;
            border-radius: var(--border-radius);
            width: 55px;
            height: 55px;
            font-size: 1.8rem;
            display: flex;
            justify-content: center;
            align-items: center;
            cursor: pointer;
            color: var(--text-primary);
            transition: background 0.2s;
        }
        
        .btn-vol:active {
            background-color: #444;
        }

        .vol-display-box {
            text-align: center;
            min-width: 100px;
        }

        .vol-display {
            font-size: 2.6rem;
            color: var(--text-display);
            font-family: 'Courier New', Courier, monospace;
            text-shadow: 0 0 8px rgba(79, 195, 247, 0.3);
        }

        .vol-unit {
            color: var(--text-primary);
            font-size: 1.2rem;
            margin-left: 2px;
        }

        .btn-mute-container {
            display: flex;
            justify-content: center;
        }

        .btn-mute {
            background-color: var(--bg-btn);
            border: 1px solid #3a3a3a;
            border-radius: var(--border-radius);
            padding: 10px 30px;
            display: flex;
            align-items: center;
            gap: 10px;
            cursor: pointer;
            color: var(--text-primary);
            font-size: 0.9rem;
            letter-spacing: 1px;
            transition: background 0.2s;
        }

        /* ----- INPUT SELECTOR ----- */
        .input-divider {
            border-top: 1px solid #333;
            margin: 5px 0 25px 0;
            text-align: center;
        }
        .input-divider span {
            background-color: #111;
            padding: 0 12px;
            color: #555;
            font-size: 0.8rem;
            letter-spacing: 1px;
            position: relative;
            top: -10px;
        }

        .input-grid {
            display: grid;
            grid-template-columns: 1fr 1fr 1fr;
            gap: 10px;
            margin-bottom: 10px;
        }

        .btn-input {
            background-color: var(--bg-btn);
            border: none;
            border-radius: var(--border-radius);
            padding: 12px 0;
            color: var(--text-primary);
            font-size: 0.8rem;
            letter-spacing: 1px;
            cursor: pointer;
            transition: all 0.2s;
            text-align: center;
        }

        .btn-input:active {
            opacity: 0.7;
        }

        .btn-input.active {
            border-bottom: 2px solid var(--accent-blue);
            background-color: #2a2f36;
        }

        .btn-input.full-width {
            grid-column: 1 / -1;
        }

        /* ----- TERMINAL PANEL ----- */
        .terminal-box {
            background-color: #121212;
            border: 1px solid #2a2a2a;
            border-radius: var(--border-radius);
            padding: 15px;
            font-family: 'Courier New', Courier, monospace;
            font-size: 0.85rem;
            color: #9cecf0;
            min-height: 100px;
            overflow-wrap: break-word;
        }
        
        .terminal-line {
            margin-bottom: 4px;
        }
        .terminal-line.comment { color: #666; }
        .terminal-line.prompt { color: #fff; }
        .terminal-arrow { color: #888; }

        /* Responsive touch-ups */
        @media (max-width: 400px) {
            .status-display, .vol-display { font-size: 2rem; }
            .vol-unit { font-size: 1rem; }
            .btn-vol { width: 45px; height: 45px; font-size: 1.4rem; }
        }
    </style>
</head>
<body>

    <div class="container">

        <!-- STATUS PANEL -->
        <div class="panel status-panel" id="status-panel">
            <div>
                <div class="label-header">STATUS</div>
                <div class="status-display" id="status-vol-text">- 32.0 <span style="font-size:1.8rem; color:#ddd;">dB</span></div>
            </div>
            <div class="btn-power on" id="btn-power" onclick="togglePower()">
                <svg viewBox="0 0 24 24">
                    <path d="M13,3H11V13H13V3M19.1,4.9C18.6,4.4 18,4 17.4,3.8L15.9,5.3C18.1,6.5 19.7,8.9 19.7,11.6C19.7,15.9 16.2,19.5 11.9,19.5C7.6,19.5 4.1,15.9 4.1,11.6C4.1,8.9 5.7,6.5 7.9,5.3L6.5,3.8C5.8,4 5.2,4.4 4.7,4.9C2.9,6.7 1.9,9.1 1.9,11.6C1.9,16.9 6.5,21.5 11.9,21.5C17.3,21.5 21.9,16.9 21.9,11.6C21.9,9.1 20.9,6.7 19.1,4.9Z" />
                </svg>
                <span>ON</span>
            </div>
        </div>

        <!-- VOLUME PANEL -->
        <div class="panel">
            <div class="label-header">MASTER VOLUME</div>
            <div class="vol-controls">
                <div class="btn-vol" onclick="adjustVolume(-1)">-</div>
                <div class="vol-display-box">
                    <span class="vol-display" id="vol-display">-32.0</span>
                    <span class="vol-unit">dB</span>
                </div>
                <div class="btn-vol" onclick="adjustVolume(1)">+</div>
            </div>
            <div class="btn-mute-container">
                <button class="btn-mute" onclick="toggleMute()">
                    <svg width="20" height="20" viewBox="0 0 24 24" fill="currentColor">
                        <path d="M3,9V15H7L12,20V4L7,9H3M16.5,12C16.5,10.23 15.48,8.71 14,7.97V16.02C15.48,15.29 16.5,13.77 16.5,12Z" />
                        <path d="M19,12C19,9.5 17.76,7.29 15.9,5.9L14.5,7.3C15.9,8.4 16.9,10.1 16.9,12C16.9,13.9 15.9,15.6 14.5,16.7L15.9,18.1C17.76,16.71 19,14.5 19,12Z" style="opacity:0.3;"/>
                    </svg>
                    MUTE
                </button>
            </div>
        </div>

        <!-- INPUT SELECTOR -->
        <div class="panel">
            <div class="input-divider"><span>INPUT SELECTOR</span></div>
            <div class="input-grid">
                <button class="btn-input" onclick="setInput('TUNER')">TUNER</button>
                <button class="btn-input" onclick="setInput('CD')">CD</button>
                <button class="btn-input" onclick="setInput('MD')">MD</button>
                <button class="btn-input" onclick="setInput('TAPE')">TAPE</button>
                <button class="btn-input active" onclick="setInput('VIDEO 1')">VIDEO 1</button>
                <button class="btn-input" onclick="setInput('VIDEO 2')">VIDEO 2</button>
                <button class="btn-input full-width" onclick="setInput('DVD')">DVD</button>
            </div>
        </div>

        <!-- TERMINAL -->
        <div class="panel">
            <div class="terminal-box" id="terminal-box">
                <div class="terminal-line comment">// RX RESPONSE TERMINAL</div>
                <div class="terminal-line prompt"><span class="terminal-arrow">&gt;</span> SYSTEM READY</div>
                <div class="terminal-line prompt"><span class="terminal-arrow">&gt;</span> VOL : -32.0dB</div>
                <div class="terminal-line prompt"><span class="terminal-arrow">&gt;</span> INPUT: 10h (VIDEO 1)</div>
            </div>
        </div>

    </div>

    <!-- Javascript to handle logic / ESP8266 Communication -->
    <script>
        // Simulated ESP8266 endpoints - replace these with your actual IP/endpoints
        const ESP_BASE = ''; 

        function togglePower() {
            const btn = document.getElementById('btn-power');
            // Fetch request to ESP8266
            // fetch(ESP_BASE + '/power').then(...) 
            btn.classList.toggle('on');
            const status = btn.classList.contains('on') ? 'ON' : 'OFF';
            addTerminalLine(`> POWER: ${status}`);
        }

        function adjustVolume(delta) {
            const display = document.getElementById('vol-display');
            let current = parseFloat(display.innerText);
            let newVal = Math.round((current + delta) * 10) / 10; // step by 0.1

            // Limit checks if needed
            // fetch(ESP_BASE + `/volume?val=${newVal}`).then(...)

            display.innerText = newVal.toFixed(1);
            document.getElementById('status-vol-text').innerHTML = `${newVal.toFixed(1)} <span style="font-size:1.8rem; color:#ddd;">dB</span>`;
            addTerminalLine(`> VOL: ${newVal.toFixed(1)}dB`);
        }

        function toggleMute() {
            // fetch(ESP_BASE + '/mute').then(...)
            addTerminalLine('> MUTE TOGGLED');
        }

        function setInput(name) {
            // UI Update
            document.querySelectorAll('.btn-input').forEach(btn => btn.classList.remove('active'));
            // Find button by text content (simplistic matching)
            document.querySelectorAll('.btn-input').forEach(btn => {
                if(btn.innerText.trim() === name) {
                    btn.classList.add('active');
                }
            });

            // fetch(ESP_BASE + `/input?name=${encodeURIComponent(name)}`).then(...)
            addTerminalLine(`> INPUT: 10h (${name})`);
        }

        function addTerminalLine(text) {
            const term = document.getElementById('terminal-box');
            const line = document.createElement('div');
            line.className = 'terminal-line prompt';
            line.innerHTML = `<span class="terminal-arrow">&gt;</span> ${text}`;
            term.appendChild(line);
            term.scrollTop = term.scrollHeight;
        }
    </script>
</body>
</html>
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
            .input-btn:active {
                transform: scale(0.96);
            }
            .input-btn.active {
                border-color: var(--primary);
                color: var(--primary);
                box-shadow: 0 0 6px rgba(163, 201, 255, 0.3);
                text-shadow: 0 0 4px rgba(163, 201, 255, 0.5);
            }
            .dvd-btn {
                grid-column: span 3;
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
                font-family: 'Courier New', monospace;
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
                    <svg class="power-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
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
                // <button id="btn-tuner" class="input-btn" onclick="selectInput('tuner', 'C05000', 'TUNER')">TUNER</button>
                <button id="btn-dvd" class="input-btn dvd-btn" onclick="selectInput('dvd', 'C05019', 'DVD')">DVD</button>
                <button id="btn-cd" class="input-btn" onclick="selectInput('cd', 'C05002', 'CD')">CD</button>
                // <button id="btn-md" class="input-btn" onclick="selectInput('md', 'C05004', 'MD')">MD</button>
                <button id="btn-tape" class="input-btn" onclick="selectInput('tape', 'C05005', 'TAPE')">TAPE</button>
                <button id="btn-video1" class="input-btn active" onclick="selectInput('video1', 'C05010', 'VIDEO 1')">VIDEO 1</button>
                <button id="btn-video2" class="input-btn" onclick="selectInput('video2', 'C05011', 'VIDEO 2')">VIDEO 2</button>
                <button id="btn-cd" class="input-btn dvd-btn" onclick="selectInput('cd', 'C05002', 'CD')">CD</button>

            </div>

            <!-- Volume & audio controls -->
            <div class="section-header">
                <span class="label">VOLUME / AUDIO</span>
            </div>
            <div class="grid-4">
                <button class="input-btn" onclick="sendVolCmd('C014')">VOL +</button>
                <button class="input-btn" onclick="sendVolCmd('C015')">VOL -</button>
                <button class="input-btn" onclick="sendVolCmd('C006')">MUTE</button>
                <button class="input-btn" onclick="sendVolCmd('C007')">UNMUTE</button>
            </div>


            <!-- Digital input type -->
            <div class="section-header">
                <span class="label">DIGITAL INPUT TYPE</span>
            </div>
            <div class="grid-3">
                <button class="input-btn" onclick="sendCustomCmd('C08301')">OPTICAL</button>
                <button class="input-btn" onclick="sendCustomCmd('C08302')">COAX</button>
                <button class="input-btn" onclick="sendCustomCmd('C08304')">ANALOG</button>
            </div>

            <!-- Status queries -->
            <div class="section-header">
                <span class="label">STATUS QUERIES</span>
            </div>
            <div class="grid-3">
                <button class="input-btn" onclick="sendCustomCmd('C00F')">SOURCE STATUS</button>
                <button class="input-btn" onclick="sendCustomCmd('C06A')">DEVICE NAME</button>
                <button class="input-btn" onclick="sendCustomCmd('C043')">INPUT TYPE</button>
                <button class="input-btn" onclick="sendCustomCmd('C00E')">2ND AUDIO STATUS</button>
            </div>

            <div class="grid-2">
                <button class="input-btn" onclick="sendCustomCmd('C00C')">5.1 INPUT ON</button>
                <button class="input-btn" onclick="sendCustomCmd('C00D')">5.1 INPUT OFF</button>
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
                if (!powerOn && hexCmd !== 'C02E') {
                    logMessage("Power is OFF. Turn on first.");
                    return false;
                }
                fetch("/send?cmd=" + hexCmd)
                    .then(response => response.text())
                    .then(txt => {
                        logMessage(`${logPrefix}: ${hexCmd.toUpperCase()} → ${txt}`);
                    })
                    .catch(err => {
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
                while(term.children.length > 220) term.removeChild(term.firstChild);
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
                    logMessage(`Cannot switch to ${label}: amplifier is in STANDBY`);
                    return;
                }
                document.querySelectorAll(".input-btn").forEach(btn => btn.classList.remove("active"));
                const targetBtn = document.getElementById("btn-" + inputId);
                if (targetBtn) targetBtn.classList.add("active");
                selectedInput = inputId;
                sendRawHex(hexCode, `INPUT ${label}`);
            }

            // Volume / mute commands (same as custom, but explicit)
            function sendVolCmd(hexCode) {
                if (!powerOn) { logMessage("Power is OFF - command ignored"); return; }
                sendRawHex(hexCode, "VOL/CTRL");
            }

            function sendCustomCmd(hexCode) {
                if (!powerOn) { logMessage("Power is OFF - command ignored"); return; }
                sendRawHex(hexCode, "CMD");
            }

            // Poll /message for incoming S-Link replies (decoded by ESP)
            function fetchPendingMessages() {
                fetch("/message")
                    .then(res => res.text())
                    .then(data => {
                        if (data && data !== "No new response yet...") {
                            // split by newline, each line is a decoded S-Link message
                            const lines = data.split('\n');
                            for (let line of lines) {
                                if (line.trim() !== "") {
                                    logMessage(`RX: ${line}`);
                                    // Optional: update active input if status message contains source name
                                    updateUiFromReply(line);
                                }
                            }
                        }
                    })
                    .catch(err => console.warn("Poll error:", err));
            }

            // try to keep UI in sync with status replies (e.g., "Status -> Audio: Video 1")
            function updateUiFromReply(reply) {
                if (!reply) return;
                const lowerReply = reply.toLowerCase();
                if (lowerReply.includes("audio:")) {
                    // extract source name like "video 1", "cd", "tuner", etc.
                    const match = reply.match(/audio:\s*([\w\s]+?)(?:\||$)/i);
                    if (match && match[1]) {
                        let src = match[1].trim().toLowerCase().replace(/\s+/g, '');
                        const mapping = {
                            "tuner": "tuner", "cd": "cd", "md": "md", "tape": "tape",
                            "video1": "video1", "video2": "video2", "dvd": "dvd"
                        };
                        let foundId = null;
                        if (src === "video1") foundId = "video1";
                        else if (src === "video2") foundId = "video2";
                        else if (src === "tuner") foundId = "tuner";
                        else if (src === "cd") foundId = "cd";
                        else if (src === "md") foundId = "md";
                        else if (src === "tape") foundId = "tape";
                        else if (src === "dvd") foundId = "dvd";
                        
                        if (foundId && document.getElementById("btn-" + foundId)) {
                            document.querySelectorAll(".input-btn").forEach(btn => btn.classList.remove("active"));
                            document.getElementById("btn-" + foundId).classList.add("active");
                            selectedInput = foundId;
                        }
                    }
                }
                // optional: if power status appears in reply, sync power UI
                if (lowerReply.includes("power status: on") || (lowerReply.includes("on sequence"))) {
                    if (!powerOn) {
                        // sync local power state
                        powerOn = true;
                        const pwrBtn = document.getElementById("pwr-btn");
                        const pwrStatus = document.getElementById("pwr-status");
                        const displayState = document.getElementById("display-state");
                        pwrBtn.className = "power-btn on";
                        pwrStatus.innerText = "ON";
                        displayState.innerText = "ACTIVE";
                        displayState.classList.remove("standby");
                    }
                }
                if (lowerReply.includes("standby") || (lowerReply.includes("power off"))) {
                    if (powerOn) {
                        powerOn = false;
                        const pwrBtn = document.getElementById("pwr-btn");
                        const pwrStatus = document.getElementById("pwr-status");
                        const displayState = document.getElementById("display-state");
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
                term.innerHTML = '<div class="terminal-item">> Log cleared.</div>';
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

  Serial.print("Sending S-Link command: ");
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

// -------------------- Message endpoint --------------------
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
/*
  ISR: measures the HIGH pulse width on every falling edge.
  - On rising edge: store start time.
  - On falling edge: calculate high width, scale to 10 µs units, push into circular buffer.
*/
IRAM_ATTR void busChange() {
  unsigned long now = micros();
  static unsigned long riseTime = 0;   // safe: static inside ISR, only accessed here
  int state = digitalRead(INPUT_PIN);

  if (state == HIGH) {
    riseTime = now;
  } else { // falling edge
    unsigned long highWidth = now - riseTime;
    lastEdgeTime = now;                // for idle detection

    if ((bufferWritePos + 1) % PULSE_BUFFER_SIZE == bufferReadPos) {
      bufferOverflowDetected = true;
      return;
    }

    // Scale to 10 µs steps, clip at 255 (2.55 ms – far above any S‑Link pulse)
    uint8_t scaled = (highWidth / 10) > 255 ? 255 : (highWidth / 10);
    pulseBuffer[bufferWritePos] = scaled;
    bufferWritePos = (bufferWritePos + 1) % PULSE_BUFFER_SIZE;
  }
}

/*
  processSlinkInput() – called from main loop.
  Reads the pulse buffer and assembles a complete S‑Link message.
*/
void processSlinkInput() {
  static uint8_t currentByte = 0;
  static uint8_t currentBit  = 0;
  static uint8_t msgBytes[32];
  static uint8_t msgLen      = 0;

  bool completeFrame = false;

  while (bufferReadPos != bufferWritePos) {
    uint8_t scaled = pulseBuffer[bufferReadPos];
    bufferReadPos = (bufferReadPos + 1) % PULSE_BUFFER_SIZE;
    unsigned int highWidth = scaled * 10;   // back to µs

    // --- Decode pulse ---
    if (highWidth >= (unsigned int)(THRESHOLD_SYNC * 10)) {
      // Sync pulse → start of a new frame
      // If there was a previous unfinished frame, discard it gracefully
      currentByte = 0;
      currentBit  = 0;
      msgLen      = 0;
    } else {
      // Data bit
      currentBit++;
      if (highWidth >= (unsigned int)(THRESHOLD_BIT * 10)) {
        bitSet(currentByte, 8 - currentBit);   // bit = 1
      } else {
        bitClear(currentByte, 8 - currentBit); // bit = 0
      }

      if (currentBit == 8) {
        if (msgLen < sizeof(msgBytes)) {
          msgBytes[msgLen++] = currentByte;
        }
        currentBit = 0;
      }
    }

    // If the bus has been idle for longer than IDLE_GAP_US we consider the frame complete.
    // This check is performed inside the loop so we don't miss the end if the buffer empties.
    if (isBusIdle()) {
      completeFrame = true;
      break;
    }
  }

  // Also mark complete if buffer is empty but bus is idle (no more pulses coming)
  if (bufferReadPos == bufferWritePos && isBusIdle()) {
    completeFrame = true;
  }

  if (completeFrame && (msgLen > 0 || currentBit != 0)) {
    if (msgLen > 0) {
      String decoded = decodeSlinkMessage(msgBytes, msgLen);

      if (receivedMessageQueue.length() > 0) {
        receivedMessageQueue += "\n";
      }
      receivedMessageQueue += decoded;
      newMessageAvailable = true;

      Serial.print("S-Link frame: ");
      Serial.println(decoded);
    }

    if (bufferOverflowDetected) {
      Serial.println("WARNING: Pulse buffer overflow!");
      bufferOverflowDetected = false;
    }

    if (currentBit != 0) {
      Serial.printf("WARNING: %d stray bits received\n", currentBit);
    }

    // Reset assembly state
    currentByte = 0;
    currentBit  = 0;
    msgLen      = 0;
  }
}

/*
  Returns true if the bus has been idle (no edge) for longer than IDLE_GAP_US.
*/
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

  // A frame from the receiver starts with 0xC8
  if (bytes[0] == 0xC8) {
    output += "[Receiver] ";
    idx = 1;
  }

  if (idx >= len) {
    output += "Device ID Handshake / Ping";
    return output;
  }

  uint8_t cmd = bytes[idx];

  // --- Known message types ---
  if (cmd == 0x6A) {
    // Device name: 6A followed by ASCII characters
    output += "Device Name: ";
    for (uint8_t i = idx + 1; i < len; i++) {
      if (bytes[i] >= 32 && bytes[i] <= 126) {
        output += (char)bytes[i];
      } else {
        output += ".";
      }
    }
  }
  else if (cmd == 0x43 && (len - idx) >= 3) {
    // Input connection type: 43 TT XX
    uint8_t tt = bytes[idx + 1];
    output += "Input Connection: ";
    switch (tt) {
      case 0x01: output += "Optical"; break;
      case 0x02: output += "Coax";    break;
      case 0x04: output += "Analog";  break;
      default:   output += "Unknown Type (0x" + String(tt, HEX) + ")";
    }
  }
  else if (cmd == 0x61 && (len - idx) >= 7 &&
           bytes[idx+1] == 0xC3 && bytes[idx+2] == 0x87 &&
           bytes[idx+3] == 0x0F && bytes[idx+4] == 0x1F &&
           bytes[idx+5] == 0x3F && bytes[idx+6] == 0x7F) {
    // Specific "power on" sequence
    output += "Power Status: ON Sequence";
  }
  else if (cmd == 0x70 && (len - idx) >= 4) {
    // Main zone status: 70 AA AV CC
    uint8_t audioSrc = bytes[idx + 1];
    uint8_t videoSrc = bytes[idx + 2];
    uint8_t flags    = bytes[idx + 3];

    output += "Status -> Audio: " + getSourceName(audioSrc);
    output += " | Video: " + getSourceName(videoSrc);
    output += " | Flags: [";

    bool firstFlag = true;
    if (flags & 0x10) { output += "5.1 Input"; firstFlag = false; }
    if (flags & 0x08) { if (!firstFlag) output += ", "; output += "Tape Loop"; firstFlag = false; }
    if (flags & 0x02) { if (!firstFlag) output += ", "; output += "Muted"; }
    output += "]";
  }
  else if (cmd == 0x71 && (len - idx) >= 2) {
    // Second audio zone status: 71 AA ...
    uint8_t src = bytes[idx + 1];
    output += "2nd Audio Zone Status -> Source: " + getSourceName(src);
  }
  else {
    output += "Unmapped S-Link Frame";
  }

  return output;
}

// ==================== S‑Link Transmitter ====================
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
  for (int i = 7; i >= 0; --i) {
    sendBit(bitRead(value, i));
  }
}

void idleAfterCommand() {
  delay(20);   // small gap after transmission
}

/*
  Sends a command on the S‑Link bus.
  Returns true if the command was sent, false if the bus was busy for too long.
*/
bool sendCommand(uint8_t command[], int length) {
  const unsigned long TIMEOUT_MS = 200;
  unsigned long start = millis();

  // Wait for bus idle with timeout
  while (!isBusIdle()) {
    if (millis() - start > TIMEOUT_MS) {
      Serial.println("ERROR: Bus busy, timeout waiting to send command.");
      return false;
    }
    yield();   // keep WiFi stack alive
  }

  noInterrupts();
  sendSyncPulse();
  for (int i = 0; i < length; ++i) {
    sendByte(command[i]);
  }
  interrupts();
  idleAfterCommand();
  return true;
}