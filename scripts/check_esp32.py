#!/usr/bin/env python3
"""ESP32-S3 연결 검증 — esptool chip_id"""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

try:
    from dotenv import load_dotenv
except ImportError:
    load_dotenv = None  # type: ignore[misc, assignment]


def find_esp32_port() -> str | None:
    for pattern in ("cu.usbmodem*", "cu.wchusbserial*"):
        for path in sorted(Path("/dev").glob(pattern)):
            return str(path)
    return None


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    if load_dotenv:
        load_dotenv(root / ".env")

    port = os.environ.get("SERIAL_PORT") or find_esp32_port()
    if not port:
        print("ERROR: ESP32 serial port not found.")
        print("  - Connect XIAO ESP32S3 USB-C to Mac")
        print("  - Set SERIAL_PORT in .env (see .env.example)")
        print("  - Run: ls /dev/cu.usbmodem*")
        return 1

    print(f"Using port: {port}")
    cmd = [sys.executable, "-m", "esptool", "--port", port, "chip_id"]
    print(f"Running: {' '.join(cmd)}")
    result = subprocess.run(cmd, check=False)
    if result.returncode == 0:
        print("OK: ESP32 chip_id verified")
    return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
