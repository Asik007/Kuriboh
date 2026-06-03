#include <ArduinoOTA.h>
#include <ESP8266mDNS.h>
#include <ESP8266WiFiMulti.h>
#include <ESP8266WebServer.h>
#include "secrets.h"   // <-- Your Wi-Fi credentials

#define OUTPUT_PIN D2
#define INPUT_PIN D1

ESP8266WiFiMulti wifiMulti;
ESP8266WebServer server(80);

const byte PULSE_BUFFER_SIZE = 100;

volatile unsigned long timeLowTransition = 0;
volatile byte bufferReadPosition = 0;
volatile byte bufferWritePosition = 0;
volatile byte pulseBuffer[PULSE_BUFFER_SIZE];

String lastReceivedMessage = "";
bool newMessageAvailable = false;
bool bufferOverflowDetected = false;

// Forward declarations
String decodeSlinkMessage(byte bytes[], byte len);
String getSourceName(byte code);
void handleRoot();
void handleSendCommand();
void handleGetMessage();
void busChange();
void processSlinkInput();
bool isBusIdle();
void sendPulseDelimiter();
void sendSyncPulse();
void sendBit(int bit);
void sendByte(int value);
void idleAfterCommand();
void sendCommand(byte command[], int commandLength);

void setup()
{
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

  server.on("/", handleRoot);
  server.on("/send", handleSendCommand);
  server.on("/message", handleGetMessage);
  server.begin();
  Serial.println("Web server started.");

  pinMode(OUTPUT_PIN, OUTPUT);
  digitalWrite(OUTPUT_PIN, LOW);
  pinMode(INPUT_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(INPUT_PIN), busChange, CHANGE);
}

// ---------------------------------------------------------------------------
// Web page
// ---------------------------------------------------------------------------
void handleRoot() {
  String html = "<!DOCTYPE html><html><head><title>S-Link Amplifier Control</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>";
  html += "body { font-family: sans-serif; margin: 20px; }";
  html += "button { padding: 15px; margin: 5px; font-size: 1em; min-width: 80px; cursor: pointer; }";
  html += ".command-group { margin-bottom: 15px; }";
  html += "h3 { margin-bottom: 5px; }";
  html += "#responseBox { background: #f4f4f4; border: 1px solid #ccc; padding: 10px; margin-top: 10px; min-height: 40px; font-family: monospace; }";

  // Log styles
  html += "#logContainer { margin-top: 20px; }";
  html += "#logHeader { display: flex; align-items: center; justify-content: space-between; margin-bottom: 6px; }";
  html += "#logBox { background: #f4f4f4; border: 1px solid #ccc; padding: 8px 10px; height: 200px; overflow-y: auto; font-family: monospace; font-size: 13px; }";
  html += ".log-entry { margin: 2px 0; line-height: 1.5; }";
  html += ".log-time { color: #888; font-size: 11px; margin-right: 6px; }";
  html += ".log-cmd   { color: #0055cc; }";
  html += ".log-reply { color: #007700; }";
  html += "#clearBtn  { padding: 5px 12px; font-size: 12px; cursor: pointer; }";
  html += "</style></head><body>";

  // Power
  html += "<div class='command-group'><h3>Power</h3>";
  html += "<button onclick=\"sendCmd('C0 2E')\">Power On</button>";
  html += "<button onclick=\"sendCmd('C0 2F')\">Power Off</button>";
  html += "</div>";

  // Volume buttons + slider
  html += "<div class='command-group'><h3>Volume</h3>";
  html += "<button onclick=\"sendCmd('C0 14')\">Vol +</button>";
  html += "<button onclick=\"sendCmd('C0 15')\">Vol -</button>";
  html += "<button onclick=\"sendCmd('C0 06')\">Mute</button>";
  html += "<button onclick=\"sendCmd('C0 07')\">Unmute</button>";
  html += "</div>";

  // Audio control
  html += "<div class='command-group'><h3>Audio</h3>";
  html += "<button onclick=\"sendCmd('C0 0C')\">5.1 Input On</button>";
  html += "<button onclick=\"sendCmd('C0 0D')\">5.1 Input Off</button>";
  html += "<button onclick=\"sendCmd('C0 0E')\">Status Request 2nd Audio</button>";
  html += "</div>";

  // Source
  html += "<div class='command-group'><h3>Source</h3>";
  html += "<button onclick=\"sendCmd('C0 50 00')\">Tuner</button>";
  html += "<button onclick=\"sendCmd('C0 50 02')\">CD</button>";
  html += "<button onclick=\"sendCmd('C0 50 04')\">MD</button>";
  html += "<button onclick=\"sendCmd('C0 50 05')\">Tape</button>";
  html += "<button onclick=\"sendCmd('C0 50 10')\">Video 1</button>";
  html += "<button onclick=\"sendCmd('C0 50 11')\">Video 2</button>";
  html += "<button onclick=\"sendCmd('C0 50 19')\">DVD</button>";
  html += "</div>";

  // Input selector
  html += "<div class='command-group'><h3>Inputs</h3>";
  html += "<button onclick=\"sendCmd('C0 83 01')\">Optical</button>";
  html += "<button onclick=\"sendCmd('C0 83 02')\">Coax</button>";
  html += "<button onclick=\"sendCmd('C0 83 04')\">Analog</button>";
  html += "</div>";

  // Status
  html += "<div class='command-group'><h3>Query Status</h3>";
  html += "<button onclick=\"sendCmd('C0 0F')\">Source Status</button>";
  html += "<button onclick=\"sendCmd('C0 6A')\">Device Name</button>";
  html += "</div>";

  // Response box
  html += "<h3>Response:</h3>";
  html += "<div id='responseBox'>Waiting for command...</div>";

  // Scrolling log
  html += "<div id='logContainer'>";
  html += "<div id='logHeader'><h3 style='margin:0'>Log</h3><button id='clearBtn' onclick='clearLog()'>Clear</button></div>";
  html += "<div id='logBox'><span class='log-placeholder' style='color:#aaa'>No activity yet.</span></div>";
  html += "</div>";

  // --- JavaScript ---
  html += "<script>";

  html += "function sendDirectVolume(val){";
  html += "  var hexVal = parseInt(val).toString(16).toUpperCase();";
  html += "  if(hexVal.length < 2) hexVal = '0' + hexVal;";
  html += "  sendCmd('C0 40 ' + hexVal);";
  html += "}";

  html += "function ts(){";
  html += "  var d = new Date();";
  html += "  return d.toTimeString().slice(0,8);";
  html += "}";

  html += "function appendLog(cls,text){";
  html += "  var box = document.getElementById('logBox');";
  html += "  if(box.querySelector('.log-placeholder')){box.innerHTML=''}";
  html += "  var d = document.createElement('div');";
  html += "  d.className = 'log-entry';";
  html += "  d.innerHTML = '<span class=\"log-time\">' + ts() + '</span><span class=\"' + cls + '\">' + text + '</span>';";
  html += "  box.appendChild(d);";
  html += "  box.scrollTop = box.scrollHeight;";
  html += "}";

  html += "function clearLog(){";
  html += "  document.getElementById('logBox').innerHTML = '<span class=\"log-placeholder\" style=\"color:#aaa\">Log cleared.</span>'";
  html += "}";

  html += "function sendCmd(cmd){";
  html += "  var xhr = new XMLHttpRequest();";
  html += "  xhr.open('GET','/send?cmd=' + cmd.replace(/ /g,''),true);";
  html += "  xhr.onload = function(){ if(xhr.status==200){ fetchMessage(); } };";
  html += "  xhr.send();";
  html += "  document.getElementById('responseBox').innerHTML = 'Command sent: ' + cmd + '. Waiting for reply...';";
  html += "  appendLog('log-cmd','\\u2192 TX: ' + cmd);";
  html += "}";

  html += "function fetchMessage(){";
  html += "  var xhr = new XMLHttpRequest();";
  html += "  xhr.open('GET','/message',true);";
  html += "  xhr.onload = function(){";
  html += "    if(xhr.status==200){";
  html += "      var msg = xhr.responseText;";
  html += "      document.getElementById('responseBox').innerHTML = msg;";
  html += "      if(msg.indexOf('No new response') === -1){";
  html += "        appendLog('log-reply','\\u2190 RX: ' + msg);";
  html += "      }";
  html += "    }";
  html += "  };";
  html += "  xhr.send();";
  html += "}";

  html += "setInterval(fetchMessage,1000);";

  html += "</script>";
  html += "</body></html>";

  server.send(200, "text/html", html);
}

// ---------------------------------------------------------------------------
// Command handler
// ---------------------------------------------------------------------------
void handleSendCommand() {
  if (!server.hasArg("cmd")) {
    server.send(400, "text/plain", "Missing command");
    return;
  }

  String hexString = server.arg("cmd");
  hexString.toUpperCase();

  Serial.print("Web request - raw command: ");
  Serial.println(hexString);

  if (hexString.length() % 2 != 0) {
    Serial.println("ERROR: Uneven hex length");
    server.send(400, "text/plain", "Hex string must have even length");
    return;
  }

  int length = hexString.length() / 2;
  byte commandBytes[length];

  for (int i = 0; i < length; i++) {
    String hexByte = hexString.substring(i * 2, i * 2 + 2);
    if (!isHexadecimalDigit(hexByte[0]) || !isHexadecimalDigit(hexByte[1])) {
      Serial.println("ERROR: Non-hex character");
      server.send(400, "text/plain", "Invalid hex character");
      return;
    }
    commandBytes[i] = strtol(hexByte.c_str(), NULL, 16);
  }

  Serial.print("Sending S-Link command: ");
  for (int i = 0; i < length; i++) {
    if (commandBytes[i] < 0x10) Serial.print("0");
    Serial.print(commandBytes[i], HEX);
    Serial.print(" ");
  }
  Serial.println();

  sendCommand(commandBytes, length);
  server.send(200, "text/plain", "Command sent");
}

// ---------------------------------------------------------------------------
// Message endpoint
// ---------------------------------------------------------------------------
void handleGetMessage() {
  if (newMessageAvailable) {
    server.send(200, "text/plain", lastReceivedMessage);
    Serial.print("Web client fetched reply: ");
    Serial.println(lastReceivedMessage);
    newMessageAvailable = false;
  } else {
    server.send(200, "text/plain", "No new response yet...");
  }
}

// ---------------------------------------------------------------------------
// S-Link bus input (ISR + processing)
// ---------------------------------------------------------------------------
IRAM_ATTR void busChange()
{
  unsigned long timeNow = micros();
  int busState = digitalRead(INPUT_PIN);
  if (busState == LOW) {
    timeLowTransition = timeNow;
    return;
  }
  int timeLow = timeNow - timeLowTransition;
  if ((bufferWritePosition + 1) % PULSE_BUFFER_SIZE == bufferReadPosition) {
    bufferOverflowDetected = true;
    return;
  }
  pulseBuffer[bufferWritePosition] = min(255, timeLow / 10);
  bufferWritePosition = (bufferWritePosition + 1) % PULSE_BUFFER_SIZE;
}

void processSlinkInput()
{
  static byte currentByte = 0;
  static byte currentBit = 0;
  static byte msgBytes[32]; // Temporary buffer array to hold parsed bytes
  static byte msgLen = 0;   // Track length of the active incoming packet

  bool completeMessageReceived = false;
  while (bufferReadPosition != bufferWritePosition) {
    int timeLow = pulseBuffer[bufferReadPosition] * 10;
    bufferReadPosition = (bufferReadPosition + 1) % PULSE_BUFFER_SIZE;

    if (timeLow > 2000) {
      completeMessageReceived = true;
      break;
    }

    currentBit += 1;
    if (timeLow > 900) {
      bitSet(currentByte, 8 - currentBit);
    } else {
      bitClear(currentByte, 8 - currentBit);
    }

    if (currentBit == 8) {
      if (msgLen < sizeof(msgBytes)) {
        msgBytes[msgLen++] = currentByte;
      }
      currentBit = 0;
    }
  }

  completeMessageReceived |= isBusIdle();
  if (completeMessageReceived && (msgLen > 0 || currentBit != 0)) {
    if (msgLen > 0) {
      // Decode the raw byte array into human-readable definitions
      lastReceivedMessage = decodeSlinkMessage(msgBytes, msgLen);
      newMessageAvailable = true;
      Serial.print("S-Link processed frame: ");
      Serial.println(lastReceivedMessage);
    }

    if (bufferOverflowDetected) {
      Serial.println("WARNING: Pulse buffer overflow detected!");
      bufferOverflowDetected = false;
    }

    if (currentBit != 0) {
      Serial.print("WARNING: ");
      Serial.print(currentBit);
      Serial.println(" stray bits received");
    }

    // Reset markers for next frame
    currentByte = 0;
    currentBit = 0;
    msgLen = 0;
  }
}

bool isBusIdle()
{
  noInterrupts();
  bool idle = micros() - timeLowTransition > 1200 + 600 + 20000;
  interrupts();
  return idle;
}

// ---------------------------------------------------------------------------
// S-Link Decoder Core Implementation
// ---------------------------------------------------------------------------
String getSourceName(byte code) {
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

String decodeSlinkMessage(byte bytes[], byte len) {
  // Construct raw Hex string representation first for easy comparison
  String output = "";
  for (byte i = 0; i < len; i++) {
    if (bytes[i] < 0x10) output += "0";
    output += String(bytes[i], HEX) + " ";
  }
  output.toUpperCase();
  output += "-> ";

  byte idx = 0;
  
  // Rule 1: Check for Receiver Source Prefix (C8)
  if (bytes[0] == 0xC8 || bytes[0] == 0xCB) {
    output += "[Receiver] ";
    idx = 1; // Advance the pointer past the device prefix
  }

  // Handle a lone C8 query/ping frame safely
  if (idx >= len) {
    output += "Device ID Handshake / Ping";
    return output;
  }

  byte cmd = bytes[idx];

  // Rule 2: Device Name (6A XXXXX...)
  if (cmd == 0x6A) {
    output += "Device Name: ";
    for (byte i = idx + 1; i < len; i++) {
      if (bytes[i] >= 32 && bytes[i] <= 126) { // Filter printable ASCII
        output += (char)bytes[i];
      } else {
        output += ".";
      }
    }
  }
  // Rule 3: Input Hardware Status Type (43 TT XX)
  else if (cmd == 0x43 && (len - idx) >= 3) {
    byte tt = bytes[idx + 1];
    output += "Input Connection: ";
    if (tt == 0x01)      output += "Optical";
    else if (tt == 0x02) output += "Coax";
    else if (tt == 0x04) output += "Analog";
    else                 output += "Unknown Type (" + String(tt, HEX) + ")";
  }
  // Rule 4: Dedicated Power Sequence status block Match
  else if (cmd == 0x61 && (len - idx) >= 7 &&
           bytes[idx+1] == 0xC3 && bytes[idx+2] == 0x87 &&
           bytes[idx+3] == 0x0F && bytes[idx+4] == 0x1F &&
           bytes[idx+5] == 0x3F && bytes[idx+6] == 0x7F) {
    output += "Power Status: ON Sequence";
  }
  // Rule 5: Standard Source Status Matrix (70 AA AV CC)
  else if (cmd == 0x70 && (len - idx) >= 4) {
    byte aa = bytes[idx + 1];
    byte av = bytes[idx + 2];
    byte cc = bytes[idx + 3];

    output += "Status -> Audio: " + getSourceName(aa);
    output += " | Video: " + getSourceName(av);

    // Decode explicit condition flag bits out of byte CC
    // Assuming standard 0-indexed bits: Bit 4 (0x10), Bit 3 (0x08), Bit 1 (0x02)
    output += " | Flags: [";
    bool subFlag = false;
    if (cc & 0x10) { output += "5.1 Input"; subFlag = true; }
    if (cc & 0x08) { if (subFlag) output += ", "; output += "Tape Loop"; subFlag = true; }
    if (cc & 0x02) { if (subFlag) output += ", "; output += "Muted"; }
    output += "]";
  }
  // Rule 6: Secondary Zone Status Matrix (71 AA XX XX...)
  else if (cmd == 0x71 && (len - idx) >= 2) {
    byte aa = bytes[idx + 1];
    output += "2nd Audio Zone Status -> Source: " + getSourceName(aa);
  }
  else if (cmd == 0x0E){
    output += "Error";
  }
  // Fallback for unmapped control frames
  else {
    output += "Unmapped S-Link Frame";
  }

  return output;
}

// ---------------------------------------------------------------------------
// S-Link sending
// ---------------------------------------------------------------------------
void sendPulseDelimiter()
{
  digitalWrite(OUTPUT_PIN, LOW);
  delayMicroseconds(600);
}

void sendSyncPulse()
{
  digitalWrite(OUTPUT_PIN, HIGH);
  delayMicroseconds(2400);
  sendPulseDelimiter();
}

void sendBit(int bit)
{
  digitalWrite(OUTPUT_PIN, HIGH);
  delayMicroseconds(bit ? 1200 : 600);
  sendPulseDelimiter();
}

void sendByte(int value)
{
  for (int i = 7; i >= 0; --i) {
    sendBit(bitRead(value, i));
  }
}

void idleAfterCommand()
{
  delay(20);
}

void sendCommand(byte command[], int commandLength)
{
  do {
    yield();
  } while (!isBusIdle());

  noInterrupts();
  sendSyncPulse();
  for (int i = 0; i < commandLength; ++i) {
    sendByte(command[i]);
  }
  interrupts();
  idleAfterCommand();
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------
void loop()
{
  if (wifiMulti.run() == WL_CONNECTED) {
    processSlinkInput();
    server.handleClient();
    ArduinoOTA.handle();
  }
}