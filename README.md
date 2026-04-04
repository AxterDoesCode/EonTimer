<div align="center">
<img src="public/favicon.svg" width="128"/>

### EonTimer

A precision timer for Pokémon RNG manipulation — now on the [web](https://dasampharos.github.io/EonTimer/).

A port of the Pokémon RNG Timer originally written by
[ToastPlusOne](https://bitbucket.org/ToastPlusOne/eontimer/downloads/).

[![Deploy](https://github.com/DasAmpharos/EonTimer/actions/workflows/deploy.yml/badge.svg?branch=web-rewrite)](https://github.com/DasAmpharos/EonTimer/actions?query=workflow:deploy+branch:web-rewrite)

</div>

## Features

- **Gen 5** — Standard, C-Gear, Entralink, and Enhanced Entralink modes
- **Gen 4** — Delay and second calibration
- **Gen 3** — Standard and Variable Target modes
- **FR/LG** — FireRed/LeafGreen shiny starter RNG with NXBT macro output and ESP32 hardware controller support
- **Custom** — Multi-phase timers with configurable units
- **PWA** — Installable from your browser, works fully offline
- **Responsive** — Works on desktop, tablet, and mobile

## Getting Started

### Prerequisites

- [Node.js](https://nodejs.org/) 20+

### Install Dependencies

```sh
npm install
```

### Development

```sh
npm run dev
```

### Production Build

```sh
npm run build
```

The build output is in the `dist/` directory and can be served by any static file server.

### Preview Production Build

```sh
npm run preview
```

## FR/LG — FireRed/LeafGreen Shiny Starter

The **FR/LG** tab targets shiny starter RNG in FireRed and LeafGreen via Nintendo Switch
Online (NSO). It manages three timing phases — seed, continue screen, and frame — and
offers two ways to execute the button sequence:

### Option A: NXBT macro (software, easier setup)

The tab generates a ready-to-run [NXBT](https://github.com/Brikwerk/nxbt) macro. Copy
the **NXBT Macro (CLI)** output and paste it into a terminal on a Linux/macOS machine
with NXBT installed:

```sh
pip install nxbt
sudo nxbt macro -c "A 0.1s\n1s\n..."
```

> **Note:** NXBT uses Python + Bluetooth which can introduce timing jitter. For best
> results use the ESP32 hardware controller below.

### Option B: ESP32 hardware controller (recommended)

An **ESP32-WROOM-32** emulates a Nintendo Switch Pro Controller over Classic Bluetooth
HID and executes the macro with microsecond-level hardware-timer precision. Calibration
values from EonTimer are pushed to the ESP32 over WiFi/WebSocket — no reflashing needed.

#### Architecture

```
EonTimer (browser) ←── WebSocket/WiFi ──→ ESP32 ──── Classic BT HID ────→ Switch
```

#### Hardware setup

1. **Flash the firmware** — see [`firmware/esp32/README.md`](firmware/esp32/README.md)
   for full instructions. In short:
   ```sh
   # Edit WIFI_SSID / WIFI_PASS in firmware/esp32/esp32_macro_runner/main/main.c
   cd firmware/esp32/esp32_macro_runner
   idf.py build && idf.py -p /dev/ttyUSB0 flash monitor
   ```
   On first boot the serial monitor prints:
   ```
   IP: 192.168.1.42
   WebSocket: ws://192.168.1.42/ws
   ```

2. **Pair with the Switch** — go to **System Settings → Controllers and Sensors →
   Change Grip/Order** while the ESP32 LED is blinking. Select **Pro Controller**.
   The Switch remembers the pairing across power cycles.

#### In-game workflow

1. Save in front of the starter ball and open the Switch HOME menu.
2. Open EonTimer via `npm run dev` (**must be HTTP**, not HTTPS).
3. In the **FR/LG** tab, paste `ws://192.168.1.42/ws` and click **Connect**.
   Wait for status **BLE Connected**.
4. Set your seed timing and continue advances, then click **Trigger + Start EonTimer**
   (or press the physical boot button on the ESP32).
5. The ESP32 executes the full button sequence; EonTimer shows the live phase timers.
6. After the run, enter your actual seed hit / continue hit and click **Update**.
   The calibrated values are sent to the ESP32 automatically — no reflash needed.

#### Calibration

| Field | Meaning |
|-------|---------|
| Seed Timer (ms) | Target milliseconds for the initial seed |
| Seed Calibration | Correction applied to the seed phase (ms) |
| Continue Screen Advances | GBA frame advances on the continue screen |
| Continue Calibration | Correction applied to the continue phase (ms) |

After each attempt, fill in the **hit** values you observed and press **Update (F6)**
to auto-calculate and apply the corrections. Values are persisted in both the browser
(localStorage) and the ESP32 (NVS flash).

## Tech Stack

- **React 19** — UI framework
- **TypeScript** — Type-safe codebase
- **Vite** — Build tool and dev server
- **Zustand** — State management with localStorage persistence
- **Web Workers** — High-precision timer loop off the main thread
- **Web Audio API** — Synthesized action sounds
- **Workbox** — Service worker for offline PWA support

## Deployment

This project includes a GitHub Actions workflow (`.github/workflows/deploy.yml`) that builds and deploys to GitHub Pages on push to the `main` branch.

To enable: go to your repo **Settings → Pages → Source** and select **GitHub Actions**.

## License

EonTimer is released under the [MIT License](LICENSE.md).

## Credits

### FR/LG ESP32 firmware

Classic BT HID implementation based on
[UARTSwitchCon](https://github.com/nullstalgia/UARTSwitchCon) by nullstalgia (GPL-3.0).

### Icon Attribution

- **Concept:** [DasAmpharos](https://github.com/DasAmpharos)
- **Preliminary Renderings:** Provided by ChatGPT, OpenAI
- **Final Design:** [dartanian300](https://github.com/dartanian300)
