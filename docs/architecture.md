# Architecture

## Hardware

| Part | Bus / pins | Notes |
|---|---|---|
| ILI9341 320x240 | HSPI via TFT_eSPI | `config/TFT_eSPI_User_Setup.h` |
| XPT2046 touch | bit-banged: CLK 25, MISO 39, MOSI 32, CS 33, IRQ 36 | both hardware SPI buses are taken; GPIO 39 picks up Wi-Fi noise (ESP32 errata), so a press needs 3 consistent samples |
| microSD | VSPI: SCK 18, MISO 19, MOSI 23, CS 5 | FAT, formatted by the firmware when blank (`f_mkfs`) |
| Flash 4 MB | `partitions.csv` | nvs 20 KB, otadata, one 3.75 MB app, spiffs 128 KB, coredump |
| RAM 320 KB, no PSRAM | | the limiting resource, see Memory |

## One sketch, three boots

`SleepCYD.ino` is one file in sections (search for `// ------`): pins, colours, display, touch, settings (NVS), UI helpers,
SD card, Bluetooth, the screens (ready / recording / report), patient name keyboard, the night's flow, the synthetic demo
night, the WatchPAT session driver, the technical UI, Nights, serial commands, night mode, Wi-Fi setup, setup/loop.

Bluetooth and Wi-Fi never run together: Bluetooth plus the LVGL screens leave about 45 KB of heap, and TLS alone needs
50 KB. So the firmware has three boot modes, chosen by flags in NVS at the top of `setup()`. In the two Wi-Fi modes the
Bluetooth controller memory is released (`esp_bt_controller_mem_release`), which leaves about 130 KB free.

| Boot | NVS flag | What runs |
|---|---|---|
| Recorder (default) | none | Bluetooth scan/connect, the patient flow, recording to the card, the screens |
| Night mode | `pend` = path of the recording | `report_pass()` (analysis to JSON), Wi-Fi, the Nidra upload if due (`psent` false), the HTTP server and the QR screen, then restart |
| Wi-Fi setup | `wifiset` | network list, password keyboard, connection test, NTP, save, restart; also the `g <url>` download |

A recording that ends with Wi-Fi configured is marked with `pend_set()` and the board restarts; the mark is cleared by
`nm_finish()` when the page is closed. A failed upload keeps the mark (`pskip` set, so the next recorder boot does not
loop) and Settings > Nights > Send retries it.

## Recording path

NimBLE runs in its own task. Its notify callback copies each packet into a `Rec` (`records.h`) and pushes it on a FreeRTOS
queue; `loop()` drains the queue, feeds the WatchPAT session driver (`wp_rx`), appends the packet to the open `.DAT` file
with a 4-byte length prefix, and feeds the analysis (`an_feed`). The session driver answers the protocol (ACKs,
SESSION_START/CONFIRM, START_ACQUISITION, END_OF_TEST), see `watchpat.h`.

The analysis (`analysis.h`) keeps 5 s rings of the 100 Hz waveforms, 6 s of chest, 10 s of motion, and computes once per
packet: pulse from PAT peaks (oximetry as fallback), SpO2 from the AC/DC ratio of the two oximetry channels at the pulse
frequency (trimmed median of 10, then EMA), breathing every 5 s from the chest band, motion level and body position from the
accelerometer, desaturations, PAT events, and per-minute trends of SpO2, pulse and position (10 h). Night mode runs the
same code over the file, one packet per simulated second.

## Screens

LVGL 9.3 with a 12-line (7.5 KB) partial render buffer from internal RAM. The recorder keeps four screens alive (welcome, turn-on,
ready, recording, plus the technical Status screen) and builds Settings, the name keyboard, the date screen, the report
and the dialogs on demand, deleting them on Back: every persistent widget costs heap that Bluetooth needs. The device
list of the Status screen is only rebuilt when that screen is visible.

Touch input goes through a four-cross calibration stored in NVS (run at first boot) (Settings > Device > Calibrate), with a swap flag for
boards whose controller is rotated. The serial command `P x,y` injects a tap for scripted tests.

Hibernate (Settings > Device) uses the same touch controller: after 30 s without a touch while the recorder is idle,
`hib_enter()` puts the panel to sleep, holds the backlight and amplifier pins low and high, and enters ESP32 deep sleep
with the pen interrupt (GPIO 36, PENIRQ, an RTC pin) as the only wake source. The wake is a normal boot:
`hib_wake_release()` frees the held pins first, and the launcher splash skips its chirp when the wake cause is that pin.

## Memory budget (measured)

| Situation | Free heap |
|---|---|
| Recorder after boot, Bluetooth on, screens built | 43 KB |
| Recorder with Settings open | 31 to 35 KB |
| Night mode after the Bluetooth memory is released | 133 KB, largest block 70 KB |
| Night mode serving the page | 70 KB |
| Wi-Fi setup | 132 KB |

Things that bit: a hidden device list rebuilt after every scan (16 KB), string-built JSON (now streamed to the card with
`printf`), Preferences writes outside an open handle (silently lost; `nv_put()` opens and closes per key).

## Time

The board has no clock. NTP runs whenever Wi-Fi is up (a few seconds at power-up when a network is configured, and in
night mode); the clock survives restarts but not power loss. Recording names need the date, so Start asks for it when it
is unknown. The time zone is a POSIX string in NVS (`tz`).

## NVS keys (namespace `sleep`)

`filter`, `last` (remembered sensor address), `sess`, `serial`, `ssid`, `wpass`, `api`, `tok`, `patient`, `dob`, `tz`,
`demo`, `hib`, `pend`, `pskip`, `psent`, `prec` (the site's name for the pending night), `wifiset`, `cx0 cx1 cy0 cy1 cswap`
(touch calibration). The Wi-Fi of a previous firmware on the board (`cfg` namespace, `ssid`/`pass`) is reused when
`ssid` is empty.

## The launcher

`lib/CydLauncher` is a header-only helper that embeds an ID string in the image (`CYD_FW_ID`), shows the boot splash,
and can switch between OTA slots when the partition table has two. SleepCYD's table has one slot, so only the splash
and the ID remain in use; the "Switch firmware" button appears only when a second valid image is found.
