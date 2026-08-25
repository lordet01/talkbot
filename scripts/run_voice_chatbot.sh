#!/usr/bin/env bash
# Groq 음성 챗봇 실행
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

if [[ ! -d .venv ]]; then
  python3 -m venv .venv
fi
source .venv/bin/activate
pip install -q -r requirements.txt

exec python scripts/voice_chatbot.py "$@"
