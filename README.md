# Kuriboh

Kuriboh is an Arduino sketch for **ESP8266** boards that emulates Sony S-Link control for compatible Sony amplifiers.

It connects to Wi-Fi, hosts a small web UI, sends S-Link commands (power, volume, input select), and displays amplifier responses in real time.

## Features

- ESP8266-based Sony S-Link command transmitter/receiver
- Built-in web interface for common amplifier controls
- Live response polling from the amplifier (`/message`)
- OTA update support via `ArduinoOTA`

## Hardware

- ESP8266 board (NodeMCU/Wemos-style pin naming)
- Sony amplifier/device with S-Link support
- S-Link wiring connected to:
  - `OUTPUT_PIN`: `D4`
  - `INPUT_PIN`: `D3`

## Software Requirements

Arduino IDE (or compatible build environment) with ESP8266 core and libraries used by the sketch:

- `ESP8266WiFiMulti`
- `ESP8266WebServer`
- `ESP8266mDNS`
- `ArduinoOTA`

## Configuration

Edit `/tmp/workspace/Asik007/Kuriboh/secrets.h` with your Wi-Fi credentials:

```cpp
const char* WIFI_SSID = "YOUR NETWORK SSID";
const char* WIFI_PASSWORD = "YOUR NETWORK PASSWORD";
```

## Flashing and Running

1. Open `/tmp/workspace/Asik007/Kuriboh/test.ino` in Arduino IDE.
2. Select your ESP8266 board and correct serial port.
3. Compile and upload.
4. Open Serial Monitor at `115200` baud.
5. Wait for connection and note the printed local IP address.

The device hostname is set to `sony_slink`.

## Web Interface

Open `http://<device-ip>/` in your browser.

Available HTTP endpoints:

- `GET /` — control page
- `GET /send?cmd=<hexbytes>` — send raw S-Link command bytes (no spaces)
- `GET /message` — retrieve latest received response

Example:

- `GET /send?cmd=C02E` (Power On)

## Default Commands in UI

- Power: `C0 2E` (On), `C0 2F` (Off)
- Volume: `C0 14` (+), `C0 15` (-), `C0 06` (Mute), `C0 07` (Unmute)
- Source: `C0 50 00` (Tuner), `C0 50 02` (CD), `C0 50 04` (MD), `C0 50 05` (Tape)
- Status: `C0 0F` (Source Status), `C0 6A` (Device Name)

## Notes

- Keep `secrets.h` private and never commit real credentials.
- This repository currently contains a single sketch (`test.ino`) and does not define automated lint/build/test scripts.
