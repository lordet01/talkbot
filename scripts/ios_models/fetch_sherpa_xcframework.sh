#!/usr/bin/env bash
# sherpa-onnx iOS C API (onnxruntime statically linked). Device arm64 only.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DEST="${ROOT}/ios/Vendor"
VER="${SHERPA_ONNX_XC_VERSION:-1.13.8}"
ZIP_URL="https://github.com/k2-fsa/sherpa-onnx/releases/download/xcframework/sherpa-onnx-v${VER}-ios-shared-onnxruntime-static.xcframework.zip"
mkdir -p "${DEST}"
tmp="$(mktemp -d)"
trap 'rm -rf "${tmp}"' EXIT
curl -L --fail --retry 3 -o "${tmp}/sherpa.zip" "${ZIP_URL}"
unzip -q "${tmp}/sherpa.zip" -d "${tmp}"
python3 - <<PY
from pathlib import Path
import plistlib, shutil
src = next(Path("${tmp}").glob("*.xcframework"))
dst = Path("${DEST}") / src.name
if dst.exists():
    shutil.rmtree(dst)
dst.mkdir(parents=True)
shutil.copy2(src / "Info.plist", dst / "Info.plist")
arm = src / "ios-arm64"
if not arm.exists():
    raise SystemExit(f"missing ios-arm64 in {src}")
shutil.copytree(arm, dst / "ios-arm64")
info = plistlib.loads((dst / "Info.plist").read_bytes())
info["AvailableLibraries"] = [l for l in info.get("AvailableLibraries", []) if l.get("LibraryIdentifier") == "ios-arm64"]
(dst / "Info.plist").write_bytes(plistlib.dumps(info, fmt=plistlib.FMT_XML))
print(f"{dst.name} ios-arm64 ready")
PY
