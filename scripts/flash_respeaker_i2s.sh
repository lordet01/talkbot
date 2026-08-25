#!/usr/bin/env bash
# ReSpeaker Lite I2S 펌웨어 플래시
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FIRMWARE="${ROOT}/firmware/respeaker_lite_i2s_dfu_firmware_v1.0.9.bin"

if [[ ! -f "$FIRMWARE" ]]; then
  echo "Firmware not found: $FIRMWARE"
  echo "Download: curl -LO https://github.com/respeaker/ReSpeaker_Lite/raw/master/xmos_firmwares/respeaker_lite_i2s_dfu_firmware_v1.0.9.bin"
  exit 1
fi

if ! command -v dfu-util >/dev/null 2>&1; then
  echo "Install dfu-util: brew install dfu-util"
  exit 1
fi

echo "Checking DFU device..."
dfu-util -l

echo ""
echo "Flashing I2S firmware..."
dfu-util -R -e -a 1 -D "$FIRMWARE"

echo ""
echo "Done. Reconnect USB cable if needed."
echo "Note: In I2S mode ReSpeaker will NOT appear as a Mac audio input device."
