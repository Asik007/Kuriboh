#include <ArduinoOTA.h>
#include <ESP8266mDNS.h>
#include <ESP8266WiFiMulti.h>
#include <ESP8266WebServer.h>
#include "secrets.h"   // <-- Your Wi-Fi credentials

#define OUTPUT_PIN D4
#define INPUT_PIN D3

ESP8266WiFiMulti wifiMulti;
ESP8266WebServer server(80);

// const byte OUTPUT_PIN = D2;
// const byte INPUT_PIN = D1;
const byte PULSE_BUFFER_SIZE = 100;

volatile unsigned long timeLowTransition = 0;
volatile byte bufferReadPosition = 0;
volatile byte bufferWritePosition = 0;
volatile byte pulseBuffer[PULSE_BUFFER_SIZE];

// For accumulating the received message to display on the web page
String lastReceivedMessage = "";
bool newMessageAvailable = false;
bool bufferOverflowDetected = false;

void setup()
{
  Serial.begin(115200);         // <-- Serial for debugging
  Serial.println("\nSony S-Link Web Controller starting...");

  ArduinoOTA.begin();

  WiFi.hostname("sony_slink");
  WiFi.mode(WIFI_STA);
  wifiMulti.addAP(WIFI_SSID, WIFI_PASSWORD);   // <-- From secrets.h

  // Wait for WiFi connection
  Serial.print("Connecting to WiFi");
  while (wifiMulti.run() != WL_CONNECTED) {
    delay(100);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("Connected! IP address: ");
  Serial.println(WiFi.localIP());

  // Setup web server routes
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

// --- Web Page Generation (unchanged) ---
void handleRoot() {
  String html = "<!DOCTYPE html><html><head><title>S-Link Amplifier Control</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>";
  html += "body { font-family: sans-serif; margin: 20px; }";
  html += "button { padding: 15px; margin: 5px; font-size: 1em; min-width: 80px; }";
  html += ".command-group { margin-bottom: 15px; }";
  html += "h3 { margin-bottom: 5px; }";
  html += "#responseBox { background: #f4f4f4; border: 1px solid #ccc; padding: 10px; margin-top: 20px; min-height: 50px; font-family: monospace; }";
  html += "</style></head><body>";
  // html += "<h1>Sony S-Link Amplifier Control</h1>";

  // Power Commands
  html += "<div class='command-group'><h3>Power</h3>";
  html += "<button onclick=\"sendCmd('C0 2E')\">Power On</button>";
  html += "<button onclick=\"sendCmd('C0 2F')\">Power Off</button>";
  html += "</div>";

  // Volume Commands
  html += "<div class='command-group'><h3>Volume</h3>";
  html += "<button onclick=\"sendCmd('C0 14')\">Vol +</button>";
  html += "<button onclick=\"sendCmd('C0 15')\">Vol -</button>";
  html += "<button onclick=\"sendCmd('C0 06')\">Mute</button>";
  html += "<button onclick=\"sendCmd('C0 07')\">Unmute</button>";
  html += "</div>";

  // Source Selection
  html += "<div class='command-group'><h3>Source</h3>";
  html += "<button onclick=\"sendCmd('C0 50 00')\">Tuner</button>";
  html += "<button onclick=\"sendCmd('C0 50 02')\">CD</button>";
  html += "<button onclick=\"sendCmd('C0 50 04')\">MD</button>";
  html += "<button onclick=\"sendCmd('C0 50 05')\">Tape</button>";
  // html += "<button onclick=\"sendCmd('C0 50 10')\">Video 1</button>";
  // html += "<button onclick=\"sendCmd('C0 50 11')\">Video 2</button>";
  // html += "<button onclick=\"sendCmd('C0 50 19')\">DVD</button>";
  html += "</div>";

  // Status Queries
  html += "<div class='command-group'><h3>Query Status</h3>";
  html += "<button onclick=\"sendCmd('C0 0F')\">Source Status</button>";
  html += "<button onclick=\"sendCmd('C0 6A')\">Device Name</button>";
  html += "</div>";

  // Response Area
  html += "<h3>Response:</h3>";
  html += "<div id='responseBox'>Waiting for command...</div>";

  html += "<script>";
  html += "function sendCmd(cmd) {";
  html += "  var xhr = new XMLHttpRequest();";
  html += "  xhr.open('GET', '/send?cmd=' + cmd.replace(/ /g, ''), true);";
  html += "  xhr.onload = function() {";
  html += "    if (xhr.status == 200) {";
  html += "      fetchMessage();";
  html += "    }";
  html += "  };";
  html += "  xhr.send();";
  html += "  document.getElementById('responseBox').innerHTML = 'Command sent: ' + cmd + '. Waiting for reply...';";
  html += "}";
  html += "function fetchMessage() {";
  html += "  var xhr = new XMLHttpRequest();";
  html += "  xhr.open('GET', '/message', true);";
  html += "  xhr.onload = function() {";
  html += "    if (xhr.status == 200) {";
  html += "      document.getElementById('responseBox').innerHTML = xhr.responseText;";
  html += "    }";
  html += "  };";
  html += "  xhr.send();";
  html += "}";
  html += "setInterval(fetchMessage, 1000);";
  html += "</script>";
  html += "</body></html>";

  server.send(200, "text/html", html);
}

// --- Command Handling from Web ---
void handleSendCommand() {
  if (!server.hasArg("cmd")) {
    server.send(400, "text/plain", "Missing command");
    return;
  }

  String hexString = server.arg("cmd");
  hexString.toUpperCase();

  Serial.print("Web request - raw command: "); // Debug
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

  // Print the bytes about to be sent
  Serial.print("Sending S-Link command: ");
  for (int i = 0; i < length; i++) {
    if (commandBytes[i] < 0x10) Serial.print("0");
    Serial.print(commandBytes[i], HEX);
    Serial.print(" ");
  }
  Serial.println();

  // Send the command over S-Link bus
  sendCommand(commandBytes, length);
  
  server.send(200, "text/plain", "Command sent");
}

// --- Provide Latest Message to Web Page ---
void handleGetMessage() {
  if (newMessageAvailable) {
    server.send(200, "text/plain", lastReceivedMessage);
    Serial.print("Web client fetched reply: ");  // Debug
    Serial.println(lastReceivedMessage);
    newMessageAvailable = false;
  } else {
    server.send(200, "text/plain", "No new response yet...");
  }
}

// --- S-Link Bus Handling (with added serial debugging) ---
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
  static String bytesReceivedHex;

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
      if (currentByte <= 0xF) {
        bytesReceivedHex += "0" + String(currentByte, HEX);
      } else {
        bytesReceivedHex += String(currentByte, HEX);
      }
      bytesReceivedHex += " ";
      currentBit = 0;
    }
  }

  completeMessageReceived |= isBusIdle();
  if (completeMessageReceived && (!bytesReceivedHex.isEmpty() || currentBit != 0)) {
    if (!bytesReceivedHex.isEmpty()) {
      lastReceivedMessage = bytesReceivedHex;
      newMessageAvailable = true;

      // Debug output to Serial
      Serial.print("S-Link reply: ");
      Serial.println(bytesReceivedHex);
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

    // Reset for next message
    bytesReceivedHex = String();
    currentByte = 0;
    currentBit = 0;
  }
}

bool isBusIdle()
{
  noInterrupts();
  bool isBusIdle = micros() - timeLowTransition > 1200 + 600 + 20000;
  interrupts();
  return isBusIdle;
}

// --- S-Link Sending Functions (unchanged) ---
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
  if (bit) {
    delayMicroseconds(1200);
  } else {
    delayMicroseconds(600);
  }
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

// --- Main Loop ---
void loop()
{
  if (wifiMulti.run() == WL_CONNECTED) {
    processSlinkInput();
    server.handleClient();
    ArduinoOTA.handle();
  }
}