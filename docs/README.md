# talkbot — ESP32/I2S 음성인식 프로토타입

Seeed Studio **ReSpeaker Lite** + **XIAO ESP32S3 통합보드** 기반 임베디드 음성인식 프로토타입입니다.

## 문서

| 문서 | 설명 |
|------|------|
| [device-connection-status.md](./device-connection-status.md) | USB/DFU/시리얼 연결 상태 |
| [voice-recognition-prototype-setup.md](./voice-recognition-prototype-setup.md) | 환경 구축 및 실행 가이드 |
| [conversation-scenarios.md](./conversation-scenarios.md) | 교육용 인형 대화 카테고리·시나리오 설계 |

## 빠른 실행 (경로 B: ESP32/I2S)

```bash
# 1. 장치 확인
./scripts/check_devices.sh

# 2. I2S 펌웨어 (최초 1회)
./scripts/flash_respeaker_i2s.sh

# 3. Python 환경
python3 -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt
cp .env.example .env   # SERIAL_PORT 수정

# 4. ESP32 연결 확인
python scripts/check_esp32.py

# 5. 스케치 업로드 (ESP32 USB 연결 후)
./scripts/upload_sketch.sh 01_firmware_version
./scripts/upload_sketch.sh 02_i2s_csv_stream
python scripts/serial_csv_to_wav.py
./scripts/upload_sketch.sh 03_kws_tflite

# 6. Groq 음성 챗봇 (Wi-Fi + Groq API 필요)
# .env 에 WIFI_SSID, WIFI_PASSWORD, GROQ_API 설정 후:
./scripts/sync_secrets.sh
./scripts/upload_sketch.sh 05_groq_voice_chat
arduino-cli monitor -p "$SERIAL_PORT" -c baudrate=115200
```

## 프로젝트 구조

```
talkbot/
├── arduino/          # ESP32 테스트 스케치 (01~03)
├── firmware/         # ReSpeaker I2S DFU 바이너리
├── scripts/          # 점검·플래시·업로드·캡처 스크립트
├── output/           # WAV 캡처 출력
└── docs/
```
