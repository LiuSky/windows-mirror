# Third-party notices

This proof of concept is a clean, bounds-checked C++ rework of protocol code
and observations from these MIT-licensed projects:

- `chotgpt/quicktime_video_hack_windows`: QuickTime/Valeria packet flow,
  CoreMedia sample-buffer layout, clocks, binary dictionary encoding, and the
  original Windows implementation. Its license is in
  `LICENSES/quicktime_video_hack_windows-MIT.txt`.
- `danielpaulus/quicktime_video_hack`: protocol documentation, packet fixtures,
  handshake construction, and binary dictionary details. Its license is in
  `LICENSES/quicktime_video_hack-MIT.txt`.

The old libusb-win32/libusb0 sources are deliberately not copied or linked.
The Windows transport in this directory uses Microsoft's inbox WinUSB API.
