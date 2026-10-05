#!/usr/bin/env bash
# Supertonic-3 multilingual TTS int8 (Korean via lang=ko). CPU, tens–low-hundreds of MB.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DEST="${ROOT}/ios/Models/TTS"
NAME="sherpa-onnx-supertonic-3-tts-int8-2026-05-11"
URL="https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/${NAME}.tar.bz2"
mkdir -p "${DEST}"
tmp="$(mktemp -d)"
trap 'rm -rf "${tmp}"' EXIT
curl -L --fail --retry 3 -C - -o "${tmp}/${NAME}.tar.bz2" "${URL}"
tar -xjf "${tmp}/${NAME}.tar.bz2" -C "${DEST}"
python3 - <<PY
import hashlib, json, os
from pathlib import Path
root = Path("${DEST}/${NAME}")
files = sorted(p for p in root.rglob("*") if p.is_file())
h = hashlib.sha256()
total = 0
for path in files:
    total += path.stat().st_size
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
manifest = {
    "engine": "tts",
    "runtime": "sherpa-onnx CPU int8",
    "package": "${NAME}",
    "lang": "ko",
    "sample_rate": 24000,
    "bytes": total,
    "sha256_tree": h.hexdigest(),
    "files": [p.name for p in files],
}
out = root / "MANIFEST.json"
out.write_text(json.dumps(manifest, indent=2) + "\n")
print(json.dumps(manifest, indent=2))
PY
echo "done ${DEST}/${NAME}"
