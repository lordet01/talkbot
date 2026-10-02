#!/usr/bin/env bash
# Deploy talkbot control panel to Cloudflare Workers + D1.
#
# POLICY: Do NOT change CONTROL_PANEL_URL / workers.dev hostname.
# Floor QR stickers and firmware depend on a stable URL.
# Temporary (--temporary) deploys are DISABLED (they mint new hostnames).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PANEL="$ROOT/control-panel"
PINNED_FILE="$PANEL/deployed-url.txt"
export PATH="${HOME}/.local/node22/bin:${PATH}"
cd "$PANEL"

if [[ ! -d node_modules/wrangler ]]; then
  npm install
fi

if [[ ! -f "$PINNED_FILE" ]]; then
  echo "ERROR: missing $PINNED_FILE (pinned control-panel URL)."
  exit 1
fi
PINNED="$(tr -d '[:space:]' < "$PINNED_FILE")"
PINNED="${PINNED%/}"
if [[ -z "$PINNED" ]]; then
  echo "ERROR: empty pinned URL in $PINNED_FILE"
  exit 1
fi
echo "==> Pinned URL (will NOT change): $PINNED"

if ! npx wrangler whoami >/dev/null 2>&1; then
  echo "ERROR: Cloudflare login required. Temporary preview deploys are disabled"
  echo "       because they create a new *.workers.dev hostname and break QR codes."
  echo "  1) npx wrangler login"
  echo "  2) Claim/link the existing worker to this account if needed"
  echo "  3) Re-run ./scripts/deploy_control_panel.sh"
  exit 1
fi
npx wrangler whoami

DB_NAME="talkbot-control"
CURRENT_ID="$(python3 - <<'PY'
import pathlib,re
t=pathlib.Path("wrangler.toml").read_text()
m=re.search(r'database_id = "([^"]+)"', t)
print(m.group(1) if m else "")
PY
)"

if [[ -z "$CURRENT_ID" || "$CURRENT_ID" == "REPLACE_AFTER_wrangler_d1_create" ]]; then
  echo "ERROR: D1 database_id missing in wrangler.toml."
  echo "Create/bind the EXISTING DB for this account; do not mint a new workers hostname."
  exit 1
fi

echo "==> Migrating schema (remote)"
npx wrangler d1 execute "$DB_NAME" --remote --file=./schema.sql
if [[ -d ./migrations ]]; then
  for f in ./migrations/*.sql; do
    echo "==> Migration $(basename "$f")"
    npx wrangler d1 execute "$DB_NAME" --remote --file="$f" || true
  done
fi

echo "==> Deploying (assets refresh only; URL stays pinned)"
DEPLOY_OUT="$(npx wrangler deploy 2>&1)"
echo "$DEPLOY_OUT"

URL="$(echo "$DEPLOY_OUT" | grep -Eo 'https://[a-zA-Z0-9.-]+\.workers\.dev' | head -1 || true)"
URL="${URL%/}"
if [[ -n "$URL" && "$URL" != "$PINNED" ]]; then
  echo ""
  echo "WARNING: deploy reported $URL but pinned URL is $PINNED"
  echo "         Not updating deployed-url.txt / .env / firmware defaults."
  echo "         Fix Cloudflare routes so the pinned hostname keeps working."
  exit 1
fi

echo ""
echo "Deployed to pinned URL: $PINNED"
echo "Floor QR: ${PINNED}/?d=<device_id>&t=<pair_secret>"
echo "(CONTROL_PANEL_URL left unchanged)"
