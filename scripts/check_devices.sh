#!/usr/bin/env bash
# USB / DFU / 시리얼 포트 연결 상태 점검
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

echo "=== Serial ports (/dev/cu.*) ==="
ls /dev/cu.* 2>/dev/null || echo "(none)"

echo ""
echo "=== ESP32 candidate ports ==="
ls /dev/cu.usbmodem* /dev/cu.wchusbserial* 2>/dev/null || echo "(none — XIAO ESP32S3 USB 연결 필요)"

echo ""
echo "=== DFU devices (ReSpeaker Lite) ==="
if command -v dfu-util >/dev/null 2>&1; then
  dfu-util -l 2>&1 || true
else
  echo "dfu-util not installed (brew install dfu-util)"
fi

echo ""
echo "=== USB devices (ioreg) ==="
ioreg -p IOUSB -l -w 0 2>/dev/null \
  | grep -E '"USB Product Name"|"idVendor"|"idProduct"|"USB Vendor Name"' \
  || echo "(none)"

echo ""
echo "=== Summary ==="
if ioreg -p IOUSB -l -w 0 2>/dev/null | grep -q "ReSpeaker Lite"; then
  echo "[OK] ReSpeaker Lite USB detected"
else
  echo "[--] ReSpeaker Lite not detected"
fi

if ls /dev/cu.usbmodem* >/dev/null 2>&1; then
  echo "[OK] ESP32 serial port detected"
else
  echo "[--] ESP32 serial port not detected — connect XIAO ESP32S3 USB-C"
fi

if command -v dfu-util >/dev/null 2>&1 && dfu-util -l 2>&1 | grep -q "2886:0019"; then
  echo "[INFO] ReSpeaker in DFU mode — run scripts/flash_respeaker_i2s.sh if I2S firmware needed"
fi
