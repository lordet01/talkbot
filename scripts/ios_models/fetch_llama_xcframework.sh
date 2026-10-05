#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DEST="${ROOT}/ios/Vendor"
VER="${LLAMA_XCFRAMEWORK_TAG:-b11401}"
ZIP_URL="https://github.com/ggml-org/llama.cpp/releases/download/${VER}/llama-${VER}-xcframework.zip"
mkdir -p "${DEST}"
tmp="$(mktemp -d)"
trap 'rm -rf "${tmp}"' EXIT
curl -L --fail --retry 3 -o "${tmp}/llama.zip" "${ZIP_URL}"
unzip -q "${tmp}/llama.zip" "build-apple/llama.xcframework/Info.plist" "build-apple/llama.xcframework/ios-arm64/llama.framework/*" -d "${tmp}"
rm -rf "${DEST}/llama.xcframework"
mkdir -p "${DEST}"
cp -R "${tmp}/build-apple/llama.xcframework" "${DEST}/llama.xcframework"
rm -rf "${DEST}/llama.xcframework/ios-arm64/dSYMs"
python3 - <<PY
from pathlib import Path
import plistlib
p = Path("${DEST}/llama.xcframework/Info.plist")
info = plistlib.loads(p.read_bytes())
info["AvailableLibraries"] = [l for l in info.get("AvailableLibraries", []) if l.get("LibraryIdentifier") == "ios-arm64"]
for lib in info["AvailableLibraries"]:
    lib.pop("DebugSymbolsPath", None)
p.write_bytes(plistlib.dumps(info, fmt=plistlib.FMT_XML))
print("llama.xcframework ready")
PY
