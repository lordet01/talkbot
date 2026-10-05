#!/usr/bin/env bash
# Download a ~3B Q4 GGUF that fits iPhone 15 Pro (A17 Pro, 8GB unified).
# Runtime target: llama.cpp Metal (or MLX later). Not Apple SystemLanguageModel.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DEST="${ROOT}/ios/Models/LLM"
# Apache-2.0 instruct 3B, Q4_K_M ~2.0 GiB, KV+overhead ~3GB at 2k ctx.
REPO="${LLM_HF_REPO:-bartowski/Qwen2.5-3B-Instruct-GGUF}"
FILE="${LLM_HF_FILE:-Qwen2.5-3B-Instruct-Q4_K_M.gguf}"
URL="https://huggingface.co/${REPO}/resolve/main/${FILE}"

mkdir -p "${DEST}"
echo "Fetching ${REPO}/${FILE}"
echo " -> ${DEST}/${FILE}"
curl -L --fail --retry 3 -C - -o "${DEST}/${FILE}.partial" "${URL}"
mv "${DEST}/${FILE}.partial" "${DEST}/${FILE}"
python3 - <<PY
import hashlib, json, os
path = "${DEST}/${FILE}"
h = hashlib.sha256()
with open(path, "rb") as f:
    for chunk in iter(lambda: f.read(1024 * 1024), b""):
        h.update(chunk)
size = os.path.getsize(path)
manifest = {
    "engine": "llm",
    "target": "iPhone 15 Pro (A17 Pro, 8GB)",
    "runtime": "llama.cpp Metal, context 2048",
    "huggingface_repo": "${REPO}",
    "file": "${FILE}",
    "quant": "Q4_K_M",
    "params": "3B",
    "license": "Apache-2.0 (Qwen2.5)",
    "bytes": size,
    "sha256": h.hexdigest(),
    "notes": "Fits 15 Pro with STT/TTS in-process if context stays ~2k. Do not use 7B+ on this device.",
}
out = "${DEST}/MANIFEST.json"
with open(out, "w") as f:
    json.dump(manifest, f, indent=2)
    f.write("\n")
print(json.dumps(manifest, indent=2))
PY
echo "done"
