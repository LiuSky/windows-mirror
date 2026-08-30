#!/bin/zsh
set -euo pipefail

ioreg -p IOService -n iPhone -r -t -l -w 0 \
  | rg '(bNumConfigurations|kUSBCurrentConfiguration|kUSBConfigurationCurrentOverride|bInterface(Class|SubClass|Protocol|Number)|bConfigurationValue|Valeria|Apple USB Multiplexor|NCM (Control|Data)|iOSScreenCaptureAssistant)'
