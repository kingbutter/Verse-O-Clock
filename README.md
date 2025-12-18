# Verse O’ Clock — XIAO ESP32‑C3 + 7.5" e‑Paper Bible Clock

[![Build](https://github.com/kingbutter/Verse-O-Clock/actions/workflows/build-release.yml/badge.svg)](https://github.com/kingbutter/Verse-O-Clock/actions/workflows/build-release.yml)
[![Latest Release](https://img.shields.io/github/v/release/kingbutter/Verse-O-Clock?label=release)](https://github.com/kingbutter/Verse-O-Clock/releases/latest)
[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)

A beautiful, ultra‑low‑power Bible verse display that shows **real KJV verses** matching the current time (e.g., John 3:16 at 03:16). Features captive Wi‑Fi/timezone setup, NTP auto‑sync, deep sleep for months on battery, and full offline Bible storage.

---

## Features

- **Time‑Matched Verses** – Displays a real KJV verse corresponding to the hour:minute (e.g., 04:13 → Philippians 4:13)
- **Famous Overrides** – Iconic verses at special times (John 3:16, Jeremiah 29:11, etc.)
- **Captive Portal Setup** – First boot opens a setup Wi‑Fi AP for SSID, password, timezone, and preferences
- **Accurate Timekeeping** – NTP sync on startup with periodic drift correction
- **e‑Paper Optimized** – 7.5" 800×480 B&W display, refreshes only when needed
- **Ultra‑Low Power** – Deep sleep between minute ticks (months of battery life)
- **Offline Bible** – Entire KJV stored locally using compressed binary files
- **Reset Mode** – Hold BOOT on power‑up to clear saved settings

---

## Hardware Requirements

- **MCU**: Seeed Studio XIAO ESP32‑C3
- **Display**: 7.5" e‑Paper (Waveshare / Seeed, 800×480, B&W)
- **Power**: USB‑C (dev) or 3.7 V LiPo (150 mAh+)

### Recommended All‑in‑One Panel (Used for This Project)

This project was developed and tested using the **Seeed Studio XIAO 7.5" ePaper Panel**, which integrates the XIAO ESP32‑C3 directly onto a 7.5" 800×480 black‑and‑white e‑paper display.

- **Product**: Seeed Studio XIAO 7.5" ePaper Panel  
- **Resolution**: 800×480 (B&W)  
- **Controller**: ESP32‑C3 (onboard)  
- **Power**: USB‑C or LiPo  
- **Mounting**: Designed as a single, clean unit (no wiring required)

Product page: https://www.seeedstudio.com/XIAO-7-5-ePaper-Panel-p-6416.html

> If you use this panel, **no external wiring or shields are required** — just flash the firmware and upload LittleFS data.

### Wiring (SPI)

| e‑Paper | XIAO GPIO |
| ------- | --------- |
| VCC     | 3.3V      |
| GND     | GND       |
| CS      | 2         |
| DC      | 1         |
| RST     | 3         |
| BUSY    | 0         |
| CLK     | 8         |
| DIN     | 6         |

> If using a Seeed e‑Paper shield or the integrated Seeed panel above, no wiring is required.

---

## Supported Devices

| Device                                                                                        | MCU      | Display           | Resolution | Status      | Notes                                                          |
| --------------------------------------------------------------------------------------------- | -------- | ----------------- | ---------- | ----------- | -------------------------------------------------------------- |
| [Seeed XIAO 7.5″ ePaper Panel](https://www.seeedstudio.com/XIAO-7-5-ePaper-Panel-p-6416.html) | ESP32‑C3 | 7.5″ ePaper (B&W) | 800×480    | ✅ Supported | All‑in‑one panel; no wiring required; primary reference device |

> 💡 **Want to add your device?** Check out the [Contributing](#contributing) section below — new devices are very welcome.

---

## Software Prerequisites

- Arduino IDE **v2.x** recommended
- ESP32 board package (Arduino Boards Manager URL):
  ```
  https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
  ```

### Required Libraries (Arduino Library Manager)

- `GxEPD2` (Jean‑Marc Zingg)
- `WiFiManager`
- `ArduinoJson`
- `HTTPClient` (ESP32 core)
- `LittleFS` (ESP32 core)

---

## Repository Layout

```
Verse-O-Clock/
├─ devices/
│  └─ xiao_esp32c3_7p5/
│     ├─ xiao_esp32c3_7p5.ino
│     ├─ data/                 # generated verse binaries (not committed)
│     ├─ partitions.csv
│     ├─ verseoclock_ota.h
│     └─ verseoclock_version.h
├─ helpers/
│  ├─ build_verses_unishox.py
│  └─ gen_ota_manifest.py
└─ README.md
```

---

## Building Verse Data (Required)

The Bible text is **not stored directly in the sketch**. It is generated and compressed ahead of time.

From the repo root:

```bash
python helpers/build_verses_unishox.py
```

This generates:

- `devices/<device>/data/*.bin` (verse tables)
- `devices/<device>/summary.*` (build report)

> These files are intentionally `.gitignore`d and must be generated locally.

---

## Uploading Data to the Device (LittleFS)

Before flashing firmware, the binary verse data must be uploaded.

1. In Arduino IDE:
   - **Tools → Board** → `XIAO_ESP32C3`
   - **Tools → Partition Scheme** → your **4MB + LittleFS** option (matches the device folder)

2. Install the **ESP32 Sketch Data Upload** plugin (if not already installed)

3. Upload data:
   - **Tools → ESP32 Sketch Data Upload**

You should see `LittleFS OK` in the Serial Monitor.

---

## Flashing Firmware (Arduino IDE)

1. Open: `devices/xiao_esp32c3_7p5/xiao_esp32c3_7p5.ino`
2. Select:
   - **Board**: `XIAO_ESP32C3`
   - **Port**: your USB device
   - **CPU Frequency**: 160 MHz
   - **Flash Size**: 4MB
3. Upload (`Ctrl/Cmd + U`)
4. Open Serial Monitor @ **115200 baud**

---

## First Boot & Setup

1. Power on the device
2. A Wi‑Fi AP named **VerseOClock** appears
3. Connect and open `192.168.4.1`
4. Configure:
   - Wi‑Fi
   - Timezone
   - Units (°F / °C)
   - Clock format
5. Save → device reboots and syncs time

---

## OTA Updates (HTTP / GitHub Releases)

Verse O’Clock supports **HTTP OTA firmware updates** by downloading release assets from GitHub (no API token required).

How it works:

- CI publishes a per‑device manifest: `releases/latest/download/<DEVICE_ID>_ota.json`
- The device fetches the manifest, compares versions, and streams `*_firmware.bin` into the OTA partition.

### Enabling OTA in firmware

1. Set your version in `devices/<device>/verseoclock_version.h`:

```cpp
#define DEVICE_ID  "xiao_esp32c3_7p5"
#define FW_VERSION "v25.12.0"
```

2. Build + flash like normal.

### Triggering an update

- While connected to Wi‑Fi, open the device config page and click **Check for firmware update**.
- If an update is available, the device will download it and reboot.

---

## Troubleshooting

- **Black Screen** → verify partition scheme + data upload
- **LittleFS FAIL** → wrong flash layout selected
- **Wrong Time** → check timezone string
- **Wi‑Fi Issues** → hold BOOT on power‑up to reset

---

## Contributing

Contributions are **very welcome** 🙌 — we’d love to see Verse O’Clock running on more devices.

- Other ESP32 variants (ESP32‑S3, ESP32‑C6, etc.)
- Different e‑Paper sizes/vendors
- Layout improvements, power optimizations, and battery-first changes
- Additional Bible translations or verse selection strategies

### Adding a New Device (high level)

1. Copy an existing device folder under `devices/`
2. Adjust pin mappings, display driver, and partitions as needed
3. Add a new entry to `devices.json`
4. Verify the build locally (Arduino IDE or CI)

If you’re unsure where to start, open an issue — we’re happy to help.

---

## License & Credits

- **License**: MIT
- **Bible Text**: King James Version (public domain)
- **Compression**: Unishox
- **Display Driver**: GxEPD2
