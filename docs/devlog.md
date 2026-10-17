# SleepCYD build log, 2026-10-15

Record of the working session that created SleepCYD (Cheap Yellow Display firmware that records a WatchPAT ONE
home sleep test to a microSD card, shows a live dashboard and sends the night to a server). Written so the work can
be picked up later without the chat history.

## What exists now
- `SleepCYD/SleepCYD.ino` firmware, installed as the only firmware on the board (slot app0, the second slot is erased).
  `common/CydLauncher` is still compiled in; its menu and the "Switch to" button hide themselves when no second firmware exists.
- `SleepCYD/watchpat.h` WatchPAT ONE protocol over the Nordic UART Service, ported from the user's project
  `C:\code\sleep\watchpat` (Python + Android, reverse-engineered from the WatchPAT app 4.2.0).
- `SleepCYD/analysis.h` on-device analysis: waveform decoders, pulse, SpO2, breathing, body position, oxygen-drop events,
  session summary (same heuristics as `watchpat_analysis.py`, estimates only).
- `SleepCYD/sleep_report.py` tabbed HTML night report (Summary, Oxygen, Pulse & breathing, Apnea, Sleep & position, Events) + JSON,
  built on the PC with the user's analysis code; `--post` sends it to an API.
- `SleepCYD/server/sleep_server.py` the server the device uploads to: stores each night, builds the report, lists the nights at
  `http://<pc-ip>:8099/`. `start_server.bat` starts it (token `demo-token-123`).
- `tools/scripts/flash_dual.py` (per-slot flashing), `tools/ble/find_device.py` (PC scan for a serial number).

## Device workflow (patient)
Boot chirp -> scan -> "WatchPAT found, Serial 123456789" dialog (chirp; Connect, or Cancel for the manual list; auto-connect
after 30 s) -> Ready screen ("Start recording") -> recording screen: elapsed time, a green dot per sensor flashing on every
packet, SpO2 / pulse / breathing / position / movement, SpO2 trend, saved MB -> "End recording" (tap twice) -> report screen ->
automatic upload (summary JSON + the whole raw recording streamed from the card, with a percentage bar) -> "Congratulations! Data sent"
-> Close restarts the board. Settings: Format SD card, Calibrate touch, Forget pairing, Restart, Patient name (large letters-only
keyboard, 12 characters). Demo: tap the title 5 times. Serial commands are listed in README.md.

## Facts learned about the hardware and the device
- WatchPAT ONE (catalog AC2110800) advertises as `ITAMAR_075BCD15N` (the serial in hex) from a static random address. It initiates the MTU exchange
  itself (client must not), sends about one 560-byte packet per second with one second of all
  channels (oximetry A/B, PAT, chest at 100 Hz, 1 Hz metric, 5 Hz motion), and each packet must be ACKed.
  After STOP_ACQUISITION + a new mode-1 session it answered END_OF_TEST (reason 1), dropped the N from its name and slept; the
  first session after waking it always worked. One night = about 2.06 MB per hour, 16.5 MB for 8 hours.
- The touch panel on this board needs a firm press. The bit-banged XPT2046 driver must hold DIN low while reading the result
  (the first version kept it high and read constant garbage). Calibration ran: x 419..3624, y 504..3472, no axis swap.
- The SD card takes the hardware VSPI bus, so touch is bit-banged on 25/39/32/33. SD mount formats a blank card; `f_mkfs`
  formats on demand. The amplifier needs GPIO 4 low; the boot and found chirps use it.
- Memory is the constraint (no PSRAM). Static buffers were cut to 2 KB packets; the launcher scan buffer is temporary; the screens
  and the packet queue are freed before Wi-Fi so lwIP has room (uploads stalled at the first 1.3 KB segment when heap was low).
  Bluetooth and Wi-Fi are never on together: BLE is deinitialised before the upload and the board restarts afterwards.
  Verified upload: 16,735,598 bytes in 47 s, byte-exact. Free heap while recording was 17 KB before the last trims.
- The display firmware (fw/) still exists and builds; both firmwares embed `CYD_FW_ID()` for the launcher.

## Verified on the real device (2026-10-15)
Found -> connect -> ready -> start -> packets (all CRCs valid, ids consecutive) -> end -> report -> automatic upload -> server
paired summary and raw into one night and built the report. Recordings on the card: `/SLEEP/watchpat_sNNNN.dat` (+ `.txt`, `_report.txt`).

## Morning after (2026-10-15 13:00)
The first real night arrived: 06:08 to 12:15, 22,076 packets, 12.1 MB, report built (`/n/<date>_<patient>/`).
It was uploaded five times: a phantom touch on the Send button after each upload (Wi-Fi glitch on the GPIO 39 touch input,
pressure 134 at screen 82,193). Fixed with a 3-sample touch debounce, a one-shot upload per night and SHA-1 duplicate
detection on the server; duplicates removed. Build passes, not flashed yet (board on a charger, not on COM5).

## 2026-10-16: the device uploads to the public Nidra site (n8n)
The user wants friends to use the device without a PC. The n8n server (`C:\code\sleep\n8n`) already has the ingest API the
phone app uses (start / chunk / complete, token header, server-built reports, session page). `SleepCYD/nidra.h` makes the
device a client of that API: base64 chunks of 300 packets streamed from the card with SHA-256 confirmed by the server.
First attempt failed: TLS "Memory allocation failed" with 46 KB free after Wi-Fi. Solved with an upload mode at boot (night
marked in flash, restart without Bluetooth: 142 KB free, 70 KB largest block). Second bug: single-key Preferences writes
outside cfg_load/cfg_save were silently lost (the handle is only open there) -> `nv_put()` helpers.
Result: last night's 12.6 MB went up in 179 s as `CYD123456789_20261016_002449`, listed on
`the Nidra site report page` with "View report" and "WatchPAT-style" built on the server.

## 2026-10-16, later: night mode (report on the device, web page, QR code)
Built the user's five-step plan: streaming second analysis pass to JSON on the card; a night-mode boot without Bluetooth that
runs the pass, uploads (if due) and serves `/`, `/report.json`, `/data.dat` on the LAN; QR screen with countdown; Nights screen in
Settings. The binary outgrew the 1.9 MB slot (QR + canvas + 48 pt font): "No bootable app partitions" -> single 3.75 MB app
partition in SleepCYD/partitions.csv, table reflashed. First pass took 625 s for 6.1 h; box-mean as running sum, nth_element
median, one sinusoid by recurrence -> 81 s, identical numbers. Verified: page 5.7 KB, JSON 3.4 KB, data.dat byte-exact at 417 KB/s.
Recording screen simplified on request (time + Stop in the centre).

## 2026-10-16, evening: file names with the date, date screen
Recordings are now `YYMMDD_<WatchPAT serial>_NNN.DAT` (local time zone, serial `y`). The date comes from NTP during a few seconds
of Wi-Fi at power-up, followed by a restart because Wi-Fi left 34 KB less heap even when switched off (the clock survives a
software restart, not a power-up; guarded with an RTC_DATA_ATTR flag). Settings > "Date & time" sets the clock by hand with
up/down buttons and opens by itself when Start is pressed without a known date. Date and Nights screens are built on demand
(keeping them cost 15 KB). Heap at boot 55 KB again. Request logging on the web page; 5 min for the first scan.

## 2026-10-16, night: the patient flow and tabbed settings
The user described the flow a friend should see: New patient | Same patient; new = remove the card's data?, name, date of
birth; "Turn on the WatchPAT" (scanning starts there); a checklist with the WatchPAT ONE part names before Start; "Receiving
data"; an "End the session?" prompt; then the existing night mode (report, upload, QR). Done in one patch: welcome/wipe/turn-on
screens, date screen generalised to a date of birth (3 columns), checklist with lv_checkbox, End prompt (Stop, END OF TEST with
a 5 min auto-end, lost link with 10 min), finish_night(), settings as an lv_tabview (Patient, Wi-Fi, Nights, Device) built on
demand, a gear on every screen, and a Wi-Fi setup boot without Bluetooth (network list from WiFi.scanNetworks, lv_keyboard for
the password, test, save, NTP, restart). lv_conf: CHECKBOX and TABVIEW on. Verified with injected taps: new-patient path,
demo Stop prompt, same-patient path, Wi-Fi setup lists 9 networks with 132 KB free. Found and fixed on the way: in auto mode
the hidden device list on the Status screen was rebuilt after every scan (16 KB for 18 devices), which with settings open took
the heap to 14 KB.

## 2026-10-16, evening: the report page and pAHI
"The webserver is not reachable": the page only exists on the QR screen (5 min, 3 min after the last request); the board had been
closed and rebooted. Night mode now turns Wi-Fi modem sleep off while it serves (a dozing station answers phones late) and logs
ip/gateway/rssi; serial `k` runs the demo night. The user had typed 192.168.1.182 for 192.168.1.82.
"Can you get AHI and pAHI": ported the PAT event rules of watchpat_analysis.py into analysis.h (amplitude envelope against a
300-sample 75th-percentile baseline, 70 %/80 % hysteresis, apnea/hypopnea/RERA by SpO2 drop and pulse rise, central = desat with
no PAT event within 60 s). The numbers go into the report text, the JSON after a night, the night-mode JSON, the phone page (first
card, severity coloured) and the QR summary. Demo night: device pAHI 40.9 severe, pRDI 41.3, PC 39.0 / 40.9. Analysis 87 s.
Boot heap 42.9 KB (was 46.6): the baseline and SpO2 history live in the static Analysis struct.

## Open items
- A 6-hour run completed on the flashed build without a crash; the memory-trim build and the phantom-touch fix are still to be flashed.
- API URL and token are entered over serial only (`u`, `t`); Wi-Fi now has on-screen setup. The board reuses the Wi-Fi saved by fw/.
- The server is plain HTTP on the home network; needs a TLS proxy and a real endpoint before use outside.
- Gzip was measured (about 32 % smaller) and not implemented on the device (memory); could be done on the server side.
- The demo night sends a summary only. The report's apnea numbers from short or unworn-probe tests are noise.
