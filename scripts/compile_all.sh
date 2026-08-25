#!/usr/bin/env bash
# 모든 Arduino 테스트 스케치 컴파일
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FQBN="${FQBN:-esp32:esp32:XIAO_ESP32S3}"

for sketch in 01_firmware_version 02_i2s_csv_stream 03_kws_tflite 05_groq_voice_chat; do
  echo "=== Compiling $sketch ==="
  arduino-cli compile --fqbn "$FQBN" \
    --board-options "CDCOnBoot=default,PSRAM=opi" \
    --build-property "build.psram=true" \
    "$ROOT/arduino/$sketch"
done

echo "All sketches compiled OK."
