# 음성인식 프로토타입 환경 구축 가이드

MacBook + Seeed Studio **ReSpeaker Lite** + **XIAO ESP32S3 통합보드**로 talkbot 음성인식 프로토타입을 구축하는 절차입니다.

## 목차

1. [하드웨어 개요](#1-하드웨어-개요)
2. [개발 경로 선택](#2-개발-경로-선택)
3. [ReSpeaker Lite USB 펌웨어 설정](#3-respeaker-lite-usb-펌웨어-설정)
4. [ESP32 + I2S 경로 (임베디드)](#4-esp32--i2s-경로-임베디드)
5. [macOS Python 개발 환경](#5-macos-python-개발-환경)
6. [프로토타입 검증 절차](#6-프로토타입-검증-절차)
7. [트러블슈팅](#7-트러블슈팅)
8. [참고 링크](#8-참고-링크)

---

## 1. 하드웨어 개요

### ReSpeaker Lite (XMOS XU316)

- 2채널 디지털 마이크 배열 (원거리 음성, AEC/NS 내장)
- 샘플레이트: **16 kHz** (음성인식에 적합)
- USB Type-C 포트: 전원 + 데이터 (XMOS 쪽)
- 3.5 mm 헤드폰 잭 / 스피커 커넥터 (재생용)

### XIAO ESP32S3 (통합보드에 사전 납땜)

- ESP32-S3 듀얼코어, 8 MB Flash, 8 MB PSRAM
- Wi-Fi / BLE
- ReSpeaker와 **I2S + I2C** 버스 공유

### USB 포트 구분 (중요)

| 포트 | 용도 | Mac에서 보이는 장치 |
|------|------|---------------------|
| ReSpeaker USB-C (XMOS) | 오디오 / XMOS DFU | `ReSpeaker Lite` (UAC 또는 DFU) |
| XIAO ESP32S3 USB | ESP32 플래시 / UART | `/dev/cu.usbmodem*` |

> **현재 상태:** XMOS 포트만 연결된 것으로 보이며, DFU 모드로 인식 중입니다.  
> 상세: [device-connection-status.md](./device-connection-status.md)

---

## 2. 개발 경로 선택

### 경로 A — Mac 호스트 ASR (권장: 프로토타입 1단계)

```
[ReSpeaker Lite] --USB Audio--> [MacBook] --Python/Whisper--> [텍스트/명령]
```

- **장점:** 빠른 반복, 풍부한 Python ASR 생태계
- **필요:** ReSpeaker **USB 펌웨어** (v2.0.7+)
- **적합:** 알고리즘 검증, UX 실험, 대화형 봇 프로토타입

### 경로 B — ESP32 엣지 + Mac 게이트웨이

```
[ReSpeaker] --I2S--> [ESP32-S3] --Wi-Fi/MQTT/HTTP--> [Mac/서버 ASR]
```

- **장점:** 독립 동작, 저지연 웨이크워드, 오프라인 일부 처리
- **필요:** ReSpeaker **I2S 펌웨어**, ESP32 USB 연결
- **적합:** 임베디드 보이스 어시스턴트, ESPHome/Arduino

### 경로 C — ESP32 온디바이스 (고급)

```
[ReSpeaker] --I2S--> [ESP32-S3 + Edge Impulse / microWakeWord]
```

- ESP32-S3 PSRAM 활용한 경량 모델
- Mac은 초기 개발·플래시 도구로만 사용

**talkbot 1차 목표:** 경로 B(ESP32/I2S)로 I2S 오디오 캡처 → TFLite KWS(yes/no)까지 검증합니다.

> **구현 완료 (2026-08-24):** `arduino/`, `scripts/`, `firmware/` 구조 및 테스트 스케치 3종이 talkbot 저장소에 추가되었습니다.

---

## 3. ReSpeaker Lite USB 펌웨어 설정

Mac에서 USB 마이크로 쓰려면 **USB 버전 XMOS 펌웨어**가 필요합니다.

### 3.1 사전 도구 설치

```bash
brew install dfu-util sox portaudio
```

| 도구 | 용도 |
|------|------|
| `dfu-util` | XMOS 펌웨어 플래시 |
| `sox` | CLI 녹음/재생 테스트 |
| `portaudio` | Python `sounddevice` 백엔드 |

### 3.2 펌웨어 다운로드

```bash
mkdir -p ~/firmware/respeaker-lite
cd ~/firmware/respeaker-lite

# 공식 GitHub 릴리스
curl -LO https://github.com/respeaker/ReSpeaker_Lite/raw/master/xmos_firmwares/respeaker_lite_usb_dfu_firmware_v2.0.7.bin
```

### 3.3 연결 확인

ReSpeaker Lite를 Mac에 USB-C로 연결합니다. **XMOS 쪽 포트**(3.5 mm 잭 근처)를 사용하세요.

```bash
dfu-util -l
```

`[2886:0019]` 장치와 `DFU UPGRADE` / `DFU FACTORY` 인터페이스가 보이면 정상입니다.

### 3.4 USB 펌웨어 플래시

```bash
dfu-util -R -e -a 1 -D respeaker_lite_usb_dfu_firmware_v2.0.7.bin
```

- `-a 1`: DFU UPGRADE 파티션
- `-R`: 플래시 후 재부팅
- 완료 후 USB 케이블을 한 번 뽑았다 다시 연결

### 3.5 플래시 후 확인

```bash
# DFU 목록에 더 이상 안 보여야 함
dfu-util -l

# 오디오 장치 등록 확인
system_profiler SPAudioDataType | grep -i -A5 respeaker
```

**시스템 설정 → 사운드 → 입력**에서 `ReSpeaker Lite`를 선택합니다.

### 3.6 빠른 녹음 테스트

```bash
# 3초 mono 16kHz WAV 녹음 (입력 장치 번호는 환경마다 다름)
rec -d 1 -r 16000 -c 1 test.wav trim 0 3

# 재생
play test.wav
```

---

## 4. ESP32 + I2S 경로 (임베디드)

ESP32에서 I2S로 마이크 데이터를 받으려면 ReSpeaker **I2S 펌웨어**가 필요합니다.

### 4.1 I2S 펌웨어 플래시

```bash
curl -LO https://github.com/respeaker/ReSpeaker_Lite/raw/master/xmos_firmwares/respeaker_lite_i2s_dfu_firmware_v1.0.9.bin

dfu-util -R -e -a 1 -D respeaker_lite_i2s_dfu_firmware_v1.0.9.bin
```

> I2S 모드에서는 Mac USB 오디오 입력으로 ReSpeaker가 **나타나지 않습니다**.  
> ESP32 USB를 Mac에 연결해 개발합니다.

### 4.2 ESP32 USB 연결 및 포트 확인

1. XIAO ESP32S3 USB-C를 Mac에 연결
2. 포트 확인:

```bash
ls /dev/cu.usbmodem*
```

3. 칩 ID 확인:

```bash
pip install esptool
python -m esptool --port /dev/cu.usbmodemXXXX chip_id
```

### 4.3 Arduino IDE 설정

1. [Arduino IDE 2.x](https://www.arduino.cc/en/software) 설치
2. **보드 매니저:** `esp32` by Espressif (3.x+)
3. **보드:** `Seeed XIAO ESP32S3`
4. **포트:** `/dev/cu.usbmodem*`
5. 예제: [ReSpeaker Lite Arduino Examples](https://github.com/respeaker/ReSpeaker_Lite/tree/master/xiao_esp32s3_arduino_examples)

### 4.4 I2C 버스 (참고)

ReSpeaker Lite에서 I2C는 ESP32와 XMOS, 오디오 코덱(TLV320AIC3204)이 공유합니다.

| 장치 | I2C 주소 |
|------|----------|
| XMOS XU316 (RESID 0xF0, 0xF1) | 프로토콜 기반 |
| TLV320AIC3204 (코덱) | `0x18` |
| XIAO ESP32S3 | SDA=GPIO5, SCL=GPIO6 |

---

## 5. macOS Python 개발 환경

프로젝트 루트(`/Users/int/Projects/talkbot`) 기준입니다.

### 5.1 가상환경 생성

```bash
cd /Users/int/Projects/talkbot
python3 -m venv .venv
source .venv/bin/activate
pip install --upgrade pip
```

### 5.2 핵심 패키지 (경로 A: Mac ASR)

`requirements.txt` (권장):

```text
# 오디오 I/O
sounddevice>=0.4.6
numpy>=1.26.0
scipy>=1.11.0

# ASR — 로컬 (오프라인)
openai-whisper>=20231117
# 또는 faster-whisper (CPU/GPU 가속, 권장)
faster-whisper>=1.0.0

# ASR — 클라우드 (선택)
# openai>=1.0.0

# VAD (음성 구간 검출)
webrtcvad>=2.0.10

# CLI / 유틸
python-dotenv>=1.0.0
rich>=13.0.0
```

```bash
pip install -r requirements.txt
```

### 5.3 Whisper 모델 사전 다운로드 (선택)

```bash
python -c "import whisper; whisper.load_model('base')"
# 또는
python -c "from faster_whisper import WhisperModel; WhisperModel('base', device='cpu', compute_type='int8')"
```

| 모델 | 크기 | 용도 |
|------|------|------|
| `tiny` | ~75 MB | 파이프라인 연결 테스트 |
| `base` | ~150 MB | 프로토타입 기본 |
| `small` | ~500 MB | 정확도 향상 |

ReSpeaker Lite는 **16 kHz mono** 입력이므로, Whisper 내부 16 kHz 리샘플링과 잘 맞습니다.

### 5.4 마이크 장치 ID 확인 스크립트

`scripts/list_audio_devices.py` (생성 예정):

```python
import sounddevice as sd

print(sd.query_devices())
default_in = sd.query_devices(kind="input")
print(f"\nDefault input: {default_in['name']} (index {default_in['index']})")
```

```bash
python scripts/list_audio_devices.py
```

ReSpeaker Lite의 `device index`를 `.env`에 저장:

```env
AUDIO_INPUT_DEVICE=3   # 환경마다 다름
SAMPLE_RATE=16000
CHANNELS=1
WHISPER_MODEL=base
```

---

## 6. 프로토타입 검증 절차

### Phase 1 — 하드웨어 (경로 A)

| # | 작업 | 성공 기준 |
|---|------|-----------|
| 1 | USB 펌웨어 플래시 | `system_profiler`에 ReSpeaker 입력 표시 |
| 2 | sox 녹음 | `test.wav`에 음성 파형 확인 |
| 3 | Python sounddevice | 3초 버퍼 캡처, RMS > 0 |

### Phase 2 — ASR 파이프라인

| # | 작업 | 성공 기준 |
|---|------|-----------|
| 4 | Whisper 단발 인식 | WAV → 한국어/영어 텍스트 출력 |
| 5 | VAD + 스트리밍 | 말할 때만 인식 트리거 |
| 6 | 명령 파싱 | "안녕" → intent/action 매핑 |

### Phase 3 — talkbot 통합 (목표)

```
마이크 입력 → VAD → Whisper → NLU/명령 → (TTS/액션)
```

### 최소 검증 코드 (Whisper)

```python
import sounddevice as sd
import numpy as np
from faster_whisper import WhisperModel

SAMPLE_RATE = 16000
DURATION_SEC = 5

print("Recording...")
audio = sd.rec(int(SAMPLE_RATE * DURATION_SEC), samplerate=SAMPLE_RATE, channels=1, dtype="float32")
sd.wait()

model = WhisperModel("base", device="cpu", compute_type="int8")
segments, _ = model.transcribe(audio.flatten(), language="ko")
for seg in segments:
    print(seg.text)
```

---

## 7. 트러블슈팅

### ReSpeaker가 오디오 장치로 안 보임

| 원인 | 해결 |
|------|------|
| I2S 펌웨어 적용됨 | USB 펌웨어로 재플래시 (§ 3.4) |
| DFU 모드 | `dfu-util -l` 확인 후 USB 펌웨어 플래시 |
| 잘못된 USB 포트 | XMOS 쪽 Type-C (3.5 mm 잭 인접) 사용 |
| 충전 전용 케이블 | 데이터 지원 USB-C 케이블 교체 |
| 허브 문제 | Mac 본체 포트에 직접 연결 |

### ESP32 포트가 안 보임

1. XIAO ESP32S3 USB-C를 **별도로** Mac에 연결
2. BOOT 버튼 누른 채 USB 연결 → 다운로드 모드
3. `ls /dev/cu.*` 전후 diff
4. Arduino IDE 보드: `Seeed XIAO ESP32S3` 선택

### dfu-util: Permission denied (드물게)

macOS에서는 일반적으로 sudo 불필요. 실패 시:

```bash
# USB 재연결 후 재시도
dfu-util -l
dfu-util -R -e -a 1 -D respeaker_lite_usb_dfu_firmware_v2.0.7.bin
```

### 녹음은 되는데 Whisper 결과가 빈 문자열

- 입력 장치가 MacBook 내장 마이크로 잡혀 있는지 확인
- `sounddevice`에서 `device=` 파라미터로 ReSpeaker index 지정
- 샘플레이트 16000 Hz, mono 채널 확인
- 입력 볼륨(게인)을 시스템 설정에서 올림

### Python sounddevice: PortAudio error

```bash
brew install portaudio
pip install --force-reinstall sounddevice
```

---

## 8. 참고 링크

| 자료 | URL |
|------|-----|
| ReSpeaker Lite GitHub | https://github.com/respeaker/ReSpeaker_Lite |
| Seeed Wiki — reSpeaker Lite | https://wiki.seeedstudio.com/reSpeaker_usb_v3/ |
| Seeed Wiki — Voice Assistant Kit | https://wiki.seeedstudio.com/xiao_respeaker/ |
| XMOS DFU 가이드 | https://github.com/respeaker/ReSpeaker_Lite/blob/master/xmos_firmwares/dfu_guide.md |
| faster-whisper | https://github.com/SYSTRAN/faster-whisper |
| XIAO ESP32S3 보드 정의 | https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/ |

---

## 부록: 현재 Mac 환경 (2026-08-24, 구축 완료)

| 항목 | 상태 |
|------|------|
| OS | macOS (darwin 25.5.0) |
| dfu-util | ✅ 설치됨 |
| arduino-cli + esp32:esp32@3.3.11 | ✅ 설치됨 |
| Python venv + requirements.txt | ✅ 설치됨 |
| ReSpeaker I2S 펌웨어 | ✅ v1.0.9 플래시 완료 |
| Arduino 라이브러리 | ✅ arduino-audio-tools, Chirale_TensorFlowLite, respeaker_arduino_library |
| 스케치 컴파일 (01~03) | ✅ 성공 |
| ESP32 Serial | ❌ 미연결 — XIAO USB-C 연결 후 업로드 |

### 실행 명령 (경로 B)

```bash
./scripts/check_devices.sh
./scripts/flash_respeaker_i2s.sh          # 최초 1회
source .venv/bin/activate
python scripts/check_esp32.py
./scripts/upload_sketch.sh 01_firmware_version
./scripts/upload_sketch.sh 02_i2s_csv_stream
python scripts/serial_csv_to_wav.py
./scripts/upload_sketch.sh 03_kws_tflite
arduino-cli monitor -p /dev/cu.usbmodemXXXX -c baudrate=115200
```
