# Verse O'Clock

A beautiful, ultra‑low‑power Bible verse display that shows **real KJV verses** matching the current time (e.g., John 3:16 at 03:16). Features captive Wi‑Fi/timezone setup, NTP auto‑sync, deep sleep for months on battery, and full offline Bible storage. Perfect for makers, devs, and faith‑inspired IoT projects.

---

## Features

* **Time‑Matched Verses** – Displays a real KJV verse corresponding to the hour:minute (e.g., 04:13 → Philippians 4:13)
* **Famous Overrides** – Iconic verses at special times (John 3:16, Jeremiah 29:11, etc.)
* **Captive Portal Setup** – First boot opens a setup Wi‑Fi AP for SSID, password, timezone, and preferences
* **Accurate Timekeeping** – NTP sync on startup with periodic drift correction
* **e‑Paper Optimized** – 7.5" 800×480 B&W display, refreshes only when needed
* **Ultra‑Low Power** – Deep sleep between minute ticks (months of battery life)
* **Offline Bible** – Entire KJV stored locally using compressed binary files
* **Reset Mode** – Hold BOOT on power‑up to clear saved settings

---

## Hardware Requirements

* **MCU**: Seeed Studio XIAO ESP32‑C3
* **Display**: 7.5" e‑Paper (Waveshare / Seeed, 800×480, B&W)
* **Power**: USB‑C (dev) or 3.7 V LiPo (150 mAh+)

### Recommended All‑in‑One Panel (Used for This Project)

This project was developed and tested using the **Seeed Studio XIAO 7.5" ePaper Panel**, which integrates the XIAO ESP32‑C3 directly onto a 7.5" 800×480 black‑and‑white e‑paper display.

* **Product**: Seeed Studio XIAO 7.5" ePaper Panel
* **Resolution**: 800×480 (B&W)
* **Controller**: ESP32‑C3 (onboard)
* **Power**: USB‑C or LiPo
* **Mounting**: Designed as a single, clean unit (no wiring required)

🔗 Product page: [https://www.seeedstudio.com/XIAO-7-5-ePaper-Panel-p-6416.html](https://www.seeedstudio.com/XIAO-7-5-ePaper-Panel-p-6416.html)

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

## Software Prerequisites

### Arduino IDE

* Arduino IDE **v2.x** recommended

### ESP32 Board Support

1. **File → Preferences**
2. Add to *Additional Boards Manager URLs*:

   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
3. **Tools → Board → Boards Manager** → install **ESP32 by Espressif**

### Required Libraries

Install via **Library Manager**:

* `GxEPD2` (Jean‑Marc Zingg)
* `WiFiManager`
* `ArduinoJson`
* `HTTPClient` (ESP32 core)
* `LittleFS` (ESP32 core)

---

## Repository Layout

```
Bible-Epaper-Clock/
├─ devices/
│  └─ XIAO_7.5inch_ePaper/
│     ├─ XIAO_7.5inch_ePaper.ino
│     ├─ data/            # generated verse binaries (not committed)
│     ├─ partitions.csv
│     └─ platformio.ini (optional)
├─ helpers/
│  └─ build_verses_unishox.py
└─ README.md
```

---

## Building Verse Data (Required)

The Bible text is **not stored directly in the sketch**. It is generated and compressed ahead of time.

1. Ensure Python 3.10+ is installed
2. From the repo root:

   ```bash
   python helpers/build_verses_unishox.py
   ```
3. This generates:

   * `data/*.bin` (verse tables)
   * `summary.*` (build report)

> These files are intentionally `.gitignore`d and must be generated locally.

---

## Uploading Data to the Device (LittleFS)

Before flashing firmware, the binary verse data must be uploaded.

1. In Arduino IDE:

   * **Tools → Board** → `XIAO_ESP32C3`
   * **Tools → Partition Scheme** → `4MB with LittleFS`

2. Install **ESP32 Sketch Data Upload** plugin (if not already installed)

3. Upload data:

   ```
   Tools → ESP32 Sketch Data Upload
   ```

You should see `LittleFS OK` in the Serial Monitor.

---

## Flashing Firmware (Arduino IDE)

1. Open the device `.ino` file
2. Select:

   * **Board**: `XIAO_ESP32C3`
   * **Port**: your USB device
   * **CPU Frequency**: 160 MHz
   * **Flash Size**: 4MB
3. Upload (`Ctrl/Cmd + U`)
4. Open Serial Monitor @ **115200 baud**

---

## First Boot & Setup

1. Power on the device
2. A Wi‑Fi AP named **VerseOClock** appears
3. Connect and open `192.168.4.1`
4. Configure:

   * Wi‑Fi
   * Timezone
   * Units (°F / °C)
   * Clock format
5. Save → device reboots and syncs time

---

## Power Behavior

* Device wakes once per minute
* Screen updates only when content changes
* Wi‑Fi is disabled after sync
* Deep sleep between updates

---

## Customization

* **Famous Verses**: `verse_overrides[]`
* **Fonts/Layout**: `drawVerse()`
* **Weather Units**: Preferences page
* **Timezone List**: `tz_list[]`

---

## Troubleshooting

* **Black Screen** → verify partition scheme + data upload
* **LittleFS FAIL** → wrong flash layout selected
* **Wrong Time** → check timezone string
* **Wi‑Fi Issues** → hold BOOT on power‑up to reset

---

## License & Credits

* **License**: MIT
* **Bible Text**: King James Version (public domain)
* **Compression**: Unishox
* **Display Driver**: GxEPD2

---

## Contributing

Contributions are **very welcome** 🙌 — this project is intentionally structured to grow.

We would love to see **Verse O’Clock running on additional devices**, including:

* Other ESP32 variants (ESP32‑S3, ESP32‑C6, etc.)
* Different e‑Paper sizes or vendors
* Alternate display layouts or fonts
* Power optimizations and battery‑first improvements
* Additional Bible translations or verse selection strategies

### Adding a New Device

Each supported device lives in its own folder under `devices/`, with configuration driven by `devices.json`. This keeps hardware‑specific logic isolated and makes CI builds reproducible.

At a high level:

1. Copy an existing device folder as a starting point
2. Adjust pin mappings, display driver, and partitions as needed
3. Add a new entry to `devices.json`
4. Verify the build locally (Arduino IDE or CI)

If you’re unsure where to start, feel free to open an issue — we’re happy to help guide new contributors.

---

**Contributions welcome.**

This project is intentionally structured to support additional devices, OTA updates, and future Bible translations.