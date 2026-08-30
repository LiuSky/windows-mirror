# Project acceptance rules

## Non-negotiable product requirement

- The Windows result must be continuous, real-time iPhone screen mirroring over USB.
- The Windows computer is the display base: it must continuously receive and decode the Valeria/CoreMedia audio-video stream.
- Screenshot polling, DVT screenshot loops, PNG/JPEG refresh, HTTP image refresh, or repeated still-frame capture are not screen mirroring and must never be presented as a solution, fallback, milestone, or successful test.
- A valid transport must expose the Valeria USB media interface (`FF/2A/FF`) and continuously consume its bulk stream. Expected media packets include `FEED` video and `EAT!` audio.
- Keep Apple device pairing/usbmux functionality working. Do not install or depend on libusb0, libusb-win32, or a libusb filter driver.
- Prefer Apple-signed Windows components and Microsoft inbox `winusb.sys`. A custom kernel driver (`.sys`) is out of scope.
- If a prototype cannot demonstrate continuous media packets, report it as an incomplete diagnostic only; do not label it a mirror.

## Verification gate

A Windows implementation is complete only when a physical iOS 18 device demonstrates all of the following:

1. Valeria USB configuration/interface is present.
2. Bulk IN/OUT endpoints open without libusb0 or a filter driver.
3. The QuickTime/Valeria handshake completes.
4. `FEED` video packets arrive continuously and decode into changing frames.
5. The preview remains live for at least five minutes and reports measured FPS.
