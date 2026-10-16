# Changelog

## 1.0 (2026-10-16)

First release. Everything below was built and verified on one board and one WatchPAT ONE between 2026-10-15 and
2026-10-16 (see docs/devlog.md).

- Recorder: WatchPAT ONE protocol over the Nordic UART Service (session, acquisition, ACKs, END OF TEST, resume after a
  lost link), recordings on the card in the phone app's length-prefixed format, named by date and serial.
- Patient flow on the touch screen: new/same patient, name, date of birth, turn-on screen, checklist in the patient
  guide's words, recording screen, end-of-session prompt.
- Streaming analysis on the device: pulse, SpO2, desaturations and ODI, PAT events with pAHI/pRDI/severity, breathing,
  body position, per-minute trends.
- Night mode: analysis of the whole night to `report.json` (about 90 s for 6 h), phone page with charts served on the home
  network, QR code and summary on the screen, upload of the raw night to a Nidra site when configured.
- Settings in tabs: patient, Wi-Fi (on-screen network list and password keyboard), nights on the card, device (clock,
  touch calibration, reset data, demo mode).
- Demo mode replaying a recorded night from `/DEMO.DAT`, with a serial download of the file.
- Release cleanup: the unreachable in-place upload path and the generic PC server were removed (nights go through night
  mode and the Nidra API), the sketch compiles without warnings, the launcher library and the library configurations are
  bundled, `tools/flash.py` replaces the dual-slot flasher.
