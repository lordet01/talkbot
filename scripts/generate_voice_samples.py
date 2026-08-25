#!/usr/bin/env python3
"""Generate yes/no clips as 16 kHz stereo 32-bit PCM for official I2S TX."""
from __future__ import annotations

import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "arduino" / "03_kws_tflite" / "yes_no_samples.h"
SAMPLE_RATE = 16000
VOLUME = 0.5  # half of the sine-test level


def run(cmd: list[str]) -> None:
    subprocess.run(cmd, check=True)


def wav_to_mono16(path: Path) -> bytes:
    data = path.read_bytes()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError(f"{path} is not a WAV file")

    offset = 12
    fmt_channels = fmt_rate = fmt_bits = None
    pcm = b""

    while offset + 8 <= len(data):
        chunk_id = data[offset : offset + 4]
        chunk_size = struct.unpack("<I", data[offset + 4 : offset + 8])[0]
        chunk_data = data[offset + 8 : offset + 8 + chunk_size]
        if chunk_id == b"fmt ":
            fmt_channels = struct.unpack("<H", chunk_data[2:4])[0]
            fmt_rate = struct.unpack("<I", chunk_data[4:8])[0]
            fmt_bits = struct.unpack("<H", chunk_data[14:16])[0]
        elif chunk_id == b"data":
            pcm = chunk_data
        offset += 8 + chunk_size
        if chunk_size % 2:
            offset += 1

    if not pcm or fmt_rate != SAMPLE_RATE or fmt_bits != 16 or fmt_channels != 1:
        raise ValueError(
            f"{path}: expected {SAMPLE_RATE} Hz mono 16-bit PCM, got "
            f"{fmt_rate} Hz ch={fmt_channels} bits={fmt_bits}"
        )
    return pcm


def trim_silence(samples: tuple[int, ...], thresh: int = 400) -> tuple[int, ...]:
    start = 0
    end = len(samples)
    while start < end and abs(samples[start]) < thresh:
        start += 1
    while end > start and abs(samples[end - 1]) < thresh:
        end -= 1
    pad = 240  # 15 ms
    start = max(0, start - pad)
    end = min(len(samples), end + pad)
    return samples[start:end]


def to_stereo32(mono16: bytes, volume: float) -> bytes:
    samples = trim_silence(struct.unpack("<" + "h" * (len(mono16) // 2), mono16))
    out = bytearray()
    for sample in samples:
        scaled = int(max(-32768, min(32767, sample * volume)))
        packed = struct.pack("<i", scaled << 16)
        out.extend(packed)  # left
        out.extend(packed)  # right
    return bytes(out)


def synthesize_word(word: str, wav_path: Path) -> None:
    with tempfile.TemporaryDirectory() as tmp:
        aiff = Path(tmp) / f"{word}.aiff"
        run(["say", "-v", "Samantha", "-r", "200", "-o", str(aiff), word])
        run(
            [
                "afconvert",
                str(aiff),
                str(wav_path),
                "-d",
                f"LEI16@{SAMPLE_RATE}",
                "-f",
                "WAVE",
            ]
        )


def to_c_array(name: str, data: bytes) -> str:
    lines = [f"const uint8_t {name}[] PROGMEM = {{"]
    row: list[str] = []
    for b in data:
        row.append(f"0x{b:02x}")
        if len(row) == 12:
            lines.append("  " + ", ".join(row) + ",")
            row = []
    if row:
        lines.append("  " + ", ".join(row))
    lines.append("};")
    lines.append(f"const size_t {name}_len = {len(data)};")
    return "\n".join(lines)


def main() -> int:
    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        yes_wav = tmp_path / "yes.wav"
        no_wav = tmp_path / "no.wav"
        synthesize_word("yes", yes_wav)
        synthesize_word("no", no_wav)
        yes_pcm = to_stereo32(wav_to_mono16(yes_wav), VOLUME)
        no_pcm = to_stereo32(wav_to_mono16(no_wav), VOLUME)

    header = "\n".join(
        [
            "#pragma once",
            "#include <Arduino.h>",
            "",
            "// 16 kHz stereo 32-bit PCM, volume 0.5, scripts/generate_voice_samples.py",
            to_c_array("yes_pcm", yes_pcm),
            "",
            to_c_array("no_pcm", no_pcm),
            "",
        ]
    )
    OUT.write_text(header)
    print(f"Wrote {OUT} ({len(yes_pcm)} + {len(no_pcm)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
