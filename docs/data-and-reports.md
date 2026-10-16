# Data on the card and the report

## Files

| Path | Content |
|---|---|
| `/SLEEP/YYMMDD_<serial>_NNN.DAT` | one night: the WatchPAT packets as received. Local date of the start (time zone `tz`), the sensor's serial, NNN counts that day's recordings. `000000_...` when the date was unknown |
| `/SLEEP/<same name>_report.json` | the analysis of that night, written by night mode (and reused when the page is opened again) |
| `/SLEEP/<same name>_report.txt`, `.txt` | the on-device report text, and the session log with the packet counters, both written when the night ends |
| `/DEMO.DAT`, `/DEMO_report.json` | the demo night and its cached report; outside `/SLEEP`, so Nights does not list it and Reset data keeps it |

Older firmware named the nights `watchpat_sNNNN.dat`; those still show in Nights.

## Recording format

The `.DAT` file is a sequence of `u32 little-endian length` + `packet`, one per DATA_PACKET from the sensor, the same
layout the WatchPAT phone app writes. A packet is a 24-byte header plus payload (`watchpat.h`):

| Offset | Field |
|---|---|
| 0 | `0xBBBB` signature (big-endian) |
| 2 | opcode, big-endian (`0x0800` DATA_PACKET) |
| 4 | u64 timestamp (0) |
| 12 | u32 LE packet id |
| 16 | u16 LE total length |
| 18 | u16 LE opcode-dependent field |
| 20 | u16 reserved |
| 22 | u16 LE CRC-16 CCITT over the packet with this field zeroed |

A DATA_PACKET holds one second of every channel as records: `0xAAAA` sync, u8 id, u8 type, u16 LE length, u16 LE rate,
u32 LE flags, then the samples. Record ids: 1 and 2 oximetry (two wavelengths, 100 Hz), 3 PAT (100 Hz), 4 chest
(100 Hz), 5 the 1 Hz metric, 6 motion (5 Hz, three axes). Waveforms are delta-coded (byte deltas; nibble deltas for
some channels), decoded in `analysis.h`. About 570 bytes per second, 2.06 MB per hour, 16.5 MB for 8 hours.

Session commands (host to sensor): SESSION_START `0x0100` (mode 1, or 4 to resume after a lost link), SESSION_CONFIRM
`0x0200` from the sensor (serial at payload offset 54), START_ACQUISITION `0x0600`, STOP_ACQUISITION, TECH_STATUS
request; END_OF_TEST `0x0900` from the sensor carries a reason code. Everything is ACKed. The sensor requests the
MTU (247) itself, so the client must not.

## Analysis rules (`analysis.h`)

All values are computed once per packet (one second) from short windows, the same way live and in night mode.

- **Pulse**: peaks of the PAT waveform over 5 s (oximetry as fallback), threshold 1.5x the median with a 350 ms
  refractory period.
- **SpO2**: AC/DC ratio of the two oximetry channels at the pulse frequency over 4 s; the channel order is learned from
  which gives plausible ratios. Trimmed median of the last 10 raw values, then an EMA (0.15). `-1` when there is no
  finger signal.
- **Desaturation (oxygen drop)**: baseline = 90th percentile of the last 30 SpO2 values seen outside events; an event
  starts at a drop of 3 % or more, ends when the drop is under 1 %, and counts when it lasted 10 s or more. ODI = events
  per hour.
- **PAT event**: envelope = peak-to-peak of the last 3 s of PAT; baseline = 75th percentile of the last 300 envelopes seen
  outside events. The event starts when the envelope is at or below 70 % of the baseline and ends when it is back above
  80 % or after 120 s. An event of 10 to 120 s is an **apnea** when SpO2 fell 4 % or more during it (max minus min of the
  last duration + 5 s of values), a **hypopnea** at 3 %, a **RERA** when the pulse rose 6 bpm or more from the start of the
  event, otherwise a PAT-only event (not counted).
- **Central**: a desaturation with no PAT event starting within 60 s of it.
- **pAHI** = (apneas + hypopneas) / hours recorded; **pRDI** adds the RERAs; **severity**: normal < 5, mild < 15,
  moderate < 30, severe otherwise. Hours are the whole recording, there is no sleep staging.
- **Breathing**: zero crossings of the chest band over 6 s after a 0.5 s box-mean baseline is removed, every 5 s;
  shown only between 4 and 40 per minute.
- **Position**: the accelerometer axis closest to gravity: x+ upright, x- inverted, y+ left, y- right, z+ supine (on
  the back), z- prone. Seconds per position and the dominant position per minute.

On the demo night (6.1 h) the device reports pAHI 40.9 (239 apneas, 12 hypopneas, 2 RERA, 23 desaturations, 3 central)
where the PC analysis of the WatchPAT ONE BLE client project reports 39.0 (216 / 23 / 12 / 24 / 1): the pulse and SpO2
pipelines differ slightly, so events move between classes. Both are estimates, not the manufacturer's scoring.

## `report.json`

Written by `report_pass()` in night mode, served at `/report.json`, read by the phone page and by the QR summary.

| Field | Meaning |
|---|---|
| `device`, `version` | `"SleepCYD"`, firmware version |
| `patient_name`, `patient_dob` | from Settings (`"Demo"` and empty for the demo night) |
| `sensor_serial`, `file`, `bytes` | the WatchPAT serial, the recording's file name and size |
| `duration_s`, `packets` | seconds recorded (one packet per second) |
| `spo2.mean`, `spo2.min` | per cent, `null` without a finger signal |
| `pulse.mean`, `pulse.min`, `pulse.max` | bpm |
| `desaturations`, `odi3` | oxygen drops of 3 % for 10 s or more, and per hour |
| `breathing` | breaths per minute, last reading |
| `pahi`, `prdi`, `severity` | see the rules above |
| `apneas`, `hypopneas`, `reras`, `central` | event counts |
| `position_pct.supine/left/right/prone/upright` | per cent of the recording |
| `position_codes` | the six position labels, index = value in `per_minute.position` minus 1 |
| `per_minute.spo2`, `per_minute.pulse`, `per_minute.position` | one value per minute, 0 where nothing was measured |

The device's own report text (shown on the screen without Wi-Fi) contains the same headline numbers in words.

## The Nidra upload (`nidra.h`)

When the API URL contains `/webhook/nidra`, night mode speaks the Nidra ingest API: `start` (recording name, patient,
device, serial, time zone, start time), then `chunk` messages of 300 packets as base64 with a SHA-256 the server echoes
back (a chunk only counts when the hashes match), then `complete`. The recording is named
`CYD<serial>_<start date and time in UTC>`; the time comes from NTP. The token travels in the `X-Nidra-Token` header.
TLS runs without certificate verification. The site builds its own report; the device shows its link as the second QR
code. The recording name is kept in NVS (`prec`) so that a retry after a failure does not create a second night.

## PC report (`sleep_report.py`)

`python sleep_report.py night.dat [--start "YYYY-MM-DD HH:MM"] [--name Name] [--post URL --token T [--with-series]]`
replays a recording through `watchpat_analysis.py` of the WatchPAT ONE BLE client project (`WATCHPAT_DIR` environment
variable, default `C:\code\sleep\watchpat`) and writes `<file>_report.html` (self-contained, phone-friendly, light/dark)
and `<file>_report.json`. Its event rules are the ones ported into `analysis.h`.
