# ESP32 Macro Runner

Firmware for an **ESP32-WROOM-32** that emulates a Nintendo Switch Pro Controller over
Classic Bluetooth HID and executes the FireRed/LeafGreen shiny starter RNG macro with
hardware-timer precision. Timing values are received from EonTimer over a WiFi WebSocket,
so you never need to reflash after calibrating.

## Architecture

```
EonTimer (browser) ←── WebSocket/WiFi ──→ ESP32 ──── Classic BT HID ────→ Switch
```

## Prerequisites

- [ESP-IDF v5.x](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/)
- ESP32-WROOM-32 devboard (the classic ESP32, **not** ESP32-S2/S3/C3)
- Nintendo Switch on firmware ≥ 10.0 with the game installed

## Setup

### 1. Install ESP-IDF

Follow the official guide. Make sure `idf.py` is on your PATH and `$IDF_PATH` is set.

### 2. Edit WiFi credentials

Open `main/main.c` and edit the two lines near the top:

```c
#define WIFI_SSID "YOUR_SSID"
#define WIFI_PASS "YOUR_PASSWORD"
```

### 3. Build and flash

```bash
cd firmware/esp32/esp32_macro_runner
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor   # adjust port as needed
```

On first boot, the ESP32 generates and stores a random Nintendo-prefix Bluetooth MAC
address (D4:F0:57:xx:xx:xx) in NVS so it persists across power cycles.

### 4. Note the IP address

The serial monitor will print:

```
IP: 192.168.1.42
WebSocket: ws://192.168.1.42/ws
```

Copy the WebSocket URL.

### 5. Connect EonTimer

1. Open EonTimer in your browser via `npm run dev` (must be HTTP, not HTTPS)
2. Go to the **FR/LG** tab
3. Paste `ws://192.168.1.42/ws` into the URL field and click **Connect**
4. Status shows **Connected (BLE not paired)**

### 6. Pair the Switch

1. On the Switch, go to **System Settings → Controllers and Sensors → Change Grip/Order**
2. The ESP32 LED will blink while discoverable
3. Press L+R on any existing controller, then find **Pro Controller** in the list
4. After pairing, EonTimer status shows **BLE Connected**

> The pairing is stored on the Switch. On subsequent runs, the Switch reconnects
> automatically when the ESP32 is powered on.

## Usage

1. Set your seed timing and continue advances in EonTimer's FR/LG tab
2. In-game: save in front of the starter ball and open the HOME menu
3. Click **Trigger + Start EonTimer** (or press the physical boot button on the ESP32)
4. The ESP32 runs the full macro sequence; EonTimer shows the live phase timers
5. After the run, enter your actual seed hit / continue hit and click **Update** to
   calibrate. The new values are pushed to the ESP32 over WebSocket automatically.

## WebSocket Protocol

| Direction | Message |
|-----------|---------|
| EonTimer → ESP32 | `{"type":"config","seedMs":30441,"seedCalibration":0,"continueAdvances":1987,"continueCalibration":0}` |
| EonTimer → ESP32 | `{"type":"trigger"}` |
| ESP32 → EonTimer | `{"type":"status","state":"idle"}` |
| ESP32 → EonTimer | `{"type":"status","state":"running"}` |
| ESP32 → EonTimer | `{"type":"status","state":"ble_connected"}` |
| ESP32 → EonTimer | `{"type":"status","state":"ble_disconnected"}` |

## GPIO

| Pin | Function |
|-----|----------|
| GPIO 2 | LED (blinks while discoverable, solid when connected) |
| GPIO 0 | Physical trigger (boot button, active LOW) |

## Credits

Classic BT HID implementation based on
[UARTSwitchCon](https://github.com/nullstalgia/UARTSwitchCon) by nullstalgia (GPL-3.0).
