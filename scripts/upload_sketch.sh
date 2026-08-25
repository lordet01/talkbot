#!/usr/bin/env bash
# Arduino 스케치 컴파일 후 업로드 (ESP32 USB 연결 필요)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FQBN="${FQBN:-esp32:esp32:XIAO_ESP32S3}"
SKETCH="${1:-}"

if [[ -z "$SKETCH" ]]; then
  echo "Usage: $0 <sketch_name>"
  echo "  e.g. $0 01_firmware_version"
  exit 1
fi

PORT="${SERIAL_PORT:-}"
if [[ -z "$PORT" ]] && [[ -f "$ROOT/.env" ]]; then
  # shellcheck disable=SC1090
  source "$ROOT/.env"
  PORT="${SERIAL_PORT:-}"
fi
if [[ -z "$PORT" ]]; then
  PORT="$(ls /dev/cu.usbmodem* 2>/dev/null | head -1 || true)"
fi
if [[ -z "$PORT" ]]; then
  echo "ERROR: No serial port. Connect ESP32 or set SERIAL_PORT in .env"
  exit 1
fi

echo "Compiling $SKETCH ..."
if [[ "$SKETCH" == "05_groq_voice_chat" ]]; then
  if [[ ! -f "$ROOT/arduino/05_groq_voice_chat/secrets.h" ]]; then
    echo "Generating secrets.h from .env ..."
    "$ROOT/scripts/sync_secrets.sh"
  fi
fi
arduino-cli compile --fqbn "$FQBN" \
  --board-options "CDCOnBoot=default,PSRAM=opi" \
  --build-property "build.psram=true" \
  "$ROOT/arduino/$SKETCH"

echo "Uploading $SKETCH to $PORT ..."
arduino-cli upload -p "$PORT" --fqbn "$FQBN" "$ROOT/arduino/$SKETCH"

echo "Done. Monitor: arduino-cli monitor -p $PORT -c baudrate=115200"
