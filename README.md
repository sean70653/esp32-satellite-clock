<h1 align = "center">🌟T-Display-S3-Long🌟</h1> 


![ESP32 Satellite Clock Display Demo](./images/demo.gif)

## Satellite Image Clock

This project turns the T-Display-S3-Long into a **live satellite cloud-map clock**. It periodically downloads satellite imagery from Taiwan's Central Weather Administration (CWA), animates the frames as a dynamic background, and overlays the current date & time.

### Features

- **WiFi auto-connect** — Scans available networks, connects using pre-configured credentials, and synchronizes time via NTP.
- **Animated satellite background** — Maintains 18 frames of satellite imagery (each 20 minutes apart, covering 6 hours), cycling at 2 frames per second (old → new). The most recent frame is offset by 20 minutes to avoid fetching images not yet available on the server.
- **On-device JPEG decode & crop** — Downloads the original 2750×2750 CWA JPEG, crops the center strip (2750×773), and scales it down to 640×180 to fit the rotated display. All done in streaming fashion with minimal memory overhead.
- **Date & time overlay** — Displays `YYYY Month DD, Weekday, HH:MM:SS` in the bottom-right corner using Montserrat 36 font, with a separate date line in Montserrat 14.
- **Smart scheduling** — Downloads new satellite images at **xx:01**, **xx:21**, and **xx:41** each hour, reusing previously downloaded frames by shifting the buffer. Only one new image is fetched per update.
- **Robust retry logic** — Initial download retries failed frames across multiple rounds with increasing delays. Periodic updates retry up to 5 times per new frame.
- **Debug frame timestamp** — Optionally displays the capture time of the current satellite frame in the top-right corner (Montserrat 8 font).

### How It Works

```
┌──────────────────────────────────────────────────────────────────────┐
│  CWA Server                                                         │
│  https://www.cwa.gov.tw/Data/satellite/LCC_TRGB_2750/               │
│  LCC_TRGB_2750-YYYY-MM-DD-HH-MM.jpg  (2750×2750, ~1–1.6 MB)       │
└──────────────────┬───────────────────────────────────────────────────┘
                   │ HTTPS download (FreeRTOS background task, Core 0)
                   ▼
┌──────────────────────────────────────────────────────────────────────┐
│  ESP32-S3  (PSRAM)                                                   │
│                                                                      │
│  1. Download JPEG into 2 MB buffer                                   │
│  2. Streaming decode with TJpgDec (ROM built-in)                     │
│  3. Crop center strip: y=988, h=773 from 2750×2750                  │
│  4. Nearest-neighbor scale: 2750×773 → 640×180                      │
│  5. Convert RGB888 → RGB565 (byte-swapped for LV_COLOR_16_SWAP)    │
│  6. Store in one of 18 frame slots (each 640×180×2 = 225 KB)        │
│                                                                      │
│  Total PSRAM for frames: 18 × 225 KB ≈ 3.96 MB                     │
│  JPEG buffer: 2 MB                                                   │
│  Total: ~6 MB of 8 MB PSRAM                                         │
└──────────────────┬───────────────────────────────────────────────────┘
                   │ LVGL timer (500 ms)
                   ▼
┌──────────────────────────────────────────────────────────────────────┐
│  180×640 Display (rotated 90°→ 640×180 logical)                     │
│  ┌────────────────────────────────────────────────────────────────┐  │
│  │                     Satellite cloud map                        │  │
│  │                     (animated background)                      │  │
│  │                                              [frame time] ──┐ │  │
│  │                                                             │ │  │
│  │                                    2025 July 7, Wed ──────┐ │ │  │
│  │                                            10:07:22 ──────┘ │ │  │
│  └────────────────────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────────────────────┘
```

### Configuration

All user-configurable settings are in the header files under `examples/satellite_img_clock/`.

#### WiFi & Time Settings (`pins_config.h`)

| Variable | Default | Description |
|---|---|---|
| `WIFI_SSID` | `"YOUR_SSID"` | Your WiFi network name |
| `WIFI_PASSWORD` | `"YOUR_WIFI_PASSWORD"` | Your WiFi password |
| `WIFI_CONNECT_WAIT_MAX` | `30000` (ms) | Max time to wait for WiFi connection |
| `NTP_SERVER1` | `"pool.ntp.org"` | Primary NTP server |
| `NTP_SERVER2` | `"time.nist.gov"` | Fallback NTP server |
| `CUSTOM_TIMEZONE` | `"CST-8"` | POSIX timezone string. `"CST-8"` = UTC+8 (Taiwan/China). See [timezone list](https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv) for other regions. |

#### Satellite Download Settings (`satellite.h`)

| Variable | Default | Description |
|---|---|---|
| `SAT_FRAME_COUNT` | `18` | Number of satellite frames to keep in memory. Each frame uses ~225 KB PSRAM. Max ~28 frames on 8 MB PSRAM. |
| `SAT_FRAME_W` | `640` | Output frame width in pixels (matches display logical width) |
| `SAT_FRAME_H` | `180` | Output frame height in pixels (matches display logical height) |
| `SAT_SRC_SIZE` | `2750` | Original CWA image dimension (2750×2750) |
| `SAT_CROP_H` | `773` | Height of the center crop from the original image, calculated as `2750 × (180/640)` |
| `SAT_CROP_Y_START` | `988` | Top pixel row of the crop region, calculated as `(2750 − 773) / 2` |
| `SAT_URL_BASE` | `"https://www.cwa.gov.tw/..."` | Base URL for CWA satellite imagery |
| `SAT_JPEG_BUF_SIZE` | `2097152` (2 MB) | JPEG download buffer size. Daytime images can reach ~1.6 MB. |

#### UI & Debug Settings (`satellite_img_clock_gui.h`)

| Variable | Default | Description |
|---|---|---|
| `SAT_SHOW_FRAME_TIME` | `1` | Set to `1` to show the capture time of the current satellite frame in the top-right corner (debug aid). Set to `0` to hide it. |

### Frame Update Schedule

- On boot, all 18 frames are downloaded at 20-minute intervals, starting from 20 minutes before the current time (to avoid fetching images not yet published by CWA). For example, at 09:00, the frames cover 08:40, 08:20, 08:00, ..., down to 03:00.
- After the initial download, updates happen at **xx:01**, **xx:21**, and **xx:41** each hour:
  - The oldest frame is discarded.
  - All existing frames shift by one slot.
  - Only the newest frame (e.g. xx:00, xx:20, or xx:40) is downloaded.
- This means each downloaded image is reused across multiple update cycles, minimizing network traffic.

### Memory Usage

| Resource | Usage |
|---|---|
| Frame buffers | 18 × 230,400 bytes = **~3.96 MB** (PSRAM) |
| JPEG download buffer | **2 MB** (PSRAM) |
| Total PSRAM | **~6 MB** of 8 MB |
| Flash | ~1.2 MB of 3 MB |
| SRAM | ~64 KB of 320 KB |

### FAQ

1. The board uses USB as the JTAG upload port. When printing serial port information on USB_CDC_ON_BOOT configuration needs to be turned on. If the port cannot be found when uploading the program or the USB has been used for other functions, the port does not appear. Please enter the upload mode manually.
   1. Connect the board via the USB cable
   2. Press and hold the BOOT button, While still pressing the BOOT button, press RST
   3. Release the RST
   4. Release the BOOT button
   5. Upload sketch

2. **No serial output?** — Make sure `platformio.ini` has `monitor_speed = 115200` and `build_flags` includes `-DARDUINO_USB_CDC_ON_BOOT=1`.

3. **Images show wrong colors?** — `LV_COLOR_16_SWAP` must be set to `1` in `lib/lv_conf.h`. The decode pipeline byte-swaps RGB565 to match.

4. **Background freezes but clock keeps running?** — Check serial log for download errors. Common causes: WiFi disconnect, CWA server temporary unavailability, or insufficient JPEG buffer size. The device will resume downloading on the next scheduled update.

## Product

| Product(PinMap)        | SOC        | Flash | PSRAM    | Resolution |
| ---------------------- | ---------- | ----- | -------- | ---------- |
| [T-Display-S3-Long][1] | ESP32-S3R8 | 16MB  | 8MB(OPI) | 180x640    |

| Current consumption    | Working current             | sleep current | sleep mode  |
| ---------------------- | --------------------------- | ------------- | ----------- |
| [T-Display-S3-Long][1] | (240MHz) WiFi On 90~350+ mA | About 1.1mA   | gpio wakeup |

[1]:https://www.lilygo.cc/products/t-display-s3-long

## PlatformIO Quick Start (Recommended)

1. Install [Visual Studio Code](https://code.visualstudio.com/) and [Python](https://www.python.org/)
2. Search for the `PlatformIO` plugin in the `VisualStudioCode` extension and install it.
3. After the installation is complete, you need to restart `VisualStudioCode`
4. After restarting `VisualStudioCode`, select `File` in the upper left corner of `VisualStudioCode` -> `Open Folder` -> select the `T-Display-S3-Long` directory
5. Wait for the installation of third-party dependent libraries to complete
6. Click on the `platformio.ini` file, and in the `platformio` column
7. Uncomment one of the lines `src_dir = xxxx` to make sure only one line works
8. Click the (✔) symbol in the lower left corner to compile
9. Connect the board to the computer USB
10. Click (→) to upload firmware
11. Click (plug symbol) to monitor serial output
12. If it cannot be written, or the USB device keeps flashing, please check the **FAQ** below

### Dependencies

**Do not upgrade the LVGL version, the lvgl software rotation has been forced to open.**

* [lvgl 8.3.0](https://github.com/lvgl/lvgl)
* [XPowersLib](https://github.com/lewisxhe/XPowersLib)
