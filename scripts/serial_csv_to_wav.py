#!/usr/bin/env python3
"""ESP32 Serial CSV 오디오 스트림 → WAV 파일 저장"""

from __future__ import annotations

import argparse
import os
import struct
import sys
import time
import wave
from pathlib import Path

try:
    import serial
except ImportError:
    print("Install pyserial: pip install pyserial")
    raise

try:
    from dotenv import load_dotenv
except ImportError:
    load_dotenv = None  # type: ignore[misc, assignment]


def find_port() -> str | None:
    for pattern in ("cu.usbmodem*", "cu.wchusbserial*"):
        for path in sorted(Path("/dev").glob(pattern)):
            return str(path)
    return None


def parse_csv_line(line: str) -> list[int]:
    parts = [p.strip() for p in line.split(",") if p.strip()]
    return [int(p) for p in parts]


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    if load_dotenv:
        load_dotenv(root / ".env")

    parser = argparse.ArgumentParser(description="Capture ESP32 CSV audio to WAV")
    parser.add_argument("--port", default=os.environ.get("SERIAL_PORT") or find_port())
    parser.add_argument("--baud", type=int, default=int(os.environ.get("BAUD_RATE", "115200")))
    parser.add_argument("--duration", type=float, default=3.0, help="Capture seconds")
    parser.add_argument("--rate", type=int, default=int(os.environ.get("SAMPLE_RATE", "16000")))
    parser.add_argument("--channels", type=int, default=int(os.environ.get("CHANNELS", "1")))
    parser.add_argument(
        "--output",
        default=str(root / "output" / "test_capture.wav"),
        help="Output WAV path",
    )
    args = parser.parse_args()

    if not args.port:
        print("ERROR: No serial port. Set SERIAL_PORT or connect ESP32.")
        return 1

    out_path = Path(args.output)
    out_path.parent.mkdir(parents=True, exist_ok=True)

    print(f"Port: {args.port} @ {args.baud}")
    print(f"Capturing {args.duration}s → {out_path}")

    samples: list[int] = []
    deadline = time.time() + args.duration

    with serial.Serial(args.port, args.baud, timeout=1) as ser:
        ser.dtr = False
        ser.rts = False
        time.sleep(0.5)
        ser.reset_input_buffer()
        while time.time() < deadline:
            raw = ser.readline()
            if not raw:
                continue
            try:
                line = raw.decode("utf-8", errors="ignore").strip()
            except UnicodeDecodeError:
                continue
            if not line or line.startswith("#"):
                continue
            try:
                values = parse_csv_line(line)
            except ValueError:
                continue
            if not values:
                continue
            # Use first channel if stereo CSV
            for i in range(0, len(values), max(1, len(values) // args.channels)):
                sample = values[i]
                # int32 → int16 clip
                clipped = max(-32768, min(32767, sample >> 16 if abs(sample) > 32767 else sample))
                samples.append(clipped)

    if not samples:
        print("ERROR: No audio samples received. Upload 02_i2s_csv_stream sketch first.")
        return 1

    with wave.open(str(out_path), "wb") as wf:
        wf.setnchannels(args.channels)
        wf.setsampwidth(2)
        wf.setframerate(args.rate)
        frames = struct.pack(f"<{len(samples)}h", *samples)
        wf.writeframes(frames)

    print(f"OK: Saved {len(samples)} samples → {out_path}")
    print(f"Play: afplay {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
