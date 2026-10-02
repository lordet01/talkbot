# talkbot 제어판 (Cloudflare + 바닥 QR)

## 라이브 URL (고정 — 바꾸지 않음)

https://talkbot-control-panel.nine-raptorex.workers.dev

인형 바닥 QR · 펌웨어 `CONTROL_PANEL_URL` · `deployed-url.txt` 가 이 주소를 가리킵니다.
**호스트명을 갱신하지 마세요.** (임시 계정 `--temporary` 배포 금지)

QR 형식:

```
https://talkbot-control-panel.nine-raptorex.workers.dev/?d=<device_id>&t=<pair_secret>
```

찍으면 **즉시 제어판**이 열리고, 로그인/가입 후 해당 인형이 자동 연결됩니다.

## 배포

```bash
npx wrangler login   # 최초 1회, 본인 Cloudflare 계정
./scripts/deploy_control_panel.sh
```

에셋만 갱신되고 URL은 `control-panel/deployed-url.txt` 고정값을 유지합니다.

## 펌웨어

```bash
# .env — URL 변경 금지
CONTROL_PANEL_URL=https://talkbot-control-panel.nine-raptorex.workers.dev
./scripts/sync_secrets.sh
```

시리얼 `QR/open:` 과 바닥 스티커는 같은 URL을 씁니다 (`device_id`/`pair_secret`은 MAC에서 유도).
