# USB 장치 연결 상태

> **스냅샷 일시:** 2026-08-24 (macOS, MacBook Air)  
> 재확인: `./scripts/check_devices.sh`

## 현재 연결 요약

| 장치 | USB 인식 | 오디오 입력 | 시리얼 포트 | 상태 |
|------|----------|-------------|-------------|------|
| **ReSpeaker Lite** (Seeed Studio) | ✅ | ❌ (I2S 모드 정상) | ❌ | I2S 펌웨어 v1.0.9 플래시 완료 |
| **XIAO ESP32S3** (통합보드) | ❌ | — | ❌ | Mac에 **미인식** — ESP32 USB-C 별도 연결 필요 |

### ReSpeaker Lite — USB 상세

| 항목 | 값 |
|------|-----|
| 제품명 | `ReSpeaker Lite` |
| 제조사 | `Seeed Studio` |
| Vendor ID | `0x2886` (10374) |
| Product ID | `0x0019` (25) |
| 시리얼 | `0000000001` |
| USB 인터페이스 | Class `0xFE` (Application Specific), SubClass `0x01` (DFU) |

**해석:** 장치가 **USB Audio Class(UAC)** 가 아닌 **DFU(Device Firmware Upgrade)** 인터페이스로 잡혀 있습니다.  
이 상태에서는 macOS **시스템 설정 → 사운드 → 입력**에 ReSpeaker가 나타나지 않습니다.

### XIAO ESP32S3 — 미인식

`/dev/cu.usb*` 또는 `/dev/cu.wchusb*` 경로가 없습니다. 가능한 원인:

- 통합보드의 **ESP32 전용 USB-C 포트**가 Mac에 연결되지 않음 (XMOS 포트만 연결된 경우)
- ESP32에 아직 펌웨어가 없거나 부트로더 진입 실패
- 케이블이 **충전 전용**(데이터 미지원)

ReSpeaker Lite Voice Assistant Kit 구성:

- **USB-C (XMOS 쪽):** ReSpeaker 오디오/XMOS 펌웨어 업데이트용
- **XIAO ESP32S3 USB:** ESP32 플래시·시리얼 디버그용 (별도 연결 필요할 수 있음)

---

## 연결 확인 명령어 (macOS)

### 1. USB 장치 목록

```bash
# 방법 A: ioreg (상세)
ioreg -p IOUSB -l -w 0 | grep -E '"USB Product Name"|"idVendor"|"idProduct"|"USB Vendor Name"'

# 방법 B: system_profiler
system_profiler SPUSBDataType
```

**ReSpeaker Lite가 정상 연결되면** `USB Product Name = "ReSpeaker Lite"`, `idVendor = 10374`, `idProduct = 25` 정도로 보입니다.

### 2. 오디오 입력 장치

```bash
system_profiler SPAudioDataType
```

**USB 펌웨어가 적용된 후** 입력 목록에 `ReSpeaker Lite` 또는 `Seeed` 관련 장치가 나타나야 합니다.

macOS GUI: **시스템 설정 → 사운드 → 입력**

### 3. ESP32 시리얼 포트

```bash
ls /dev/cu.*
ls /dev/cu.usb* 2>/dev/null
```

ESP32(XIAO ESP32S3) 연결 시 일반적으로 다음 중 하나가 생성됩니다:

- `/dev/cu.usbmodem*` (USB CDC, ESP32-S3 기본)
- `/dev/cu.wchusbserial*` (CH340/CP2102 어댑터 사용 시)

포트 확인:

```bash
# 연결 전후 diff
ls /dev/cu.* > /tmp/before.txt
# USB 케이블 연결/해제
ls /dev/cu.* > /tmp/after.txt
diff /tmp/before.txt /tmp/after.txt
```

### 4. DFU 모드 / 펌웨어 확인

```bash
brew install dfu-util   # 미설치 시
dfu-util -l
```

**DFU 모드 예시 출력:**

```
Found DFU: [2886:0019] ... name="DFU UPGRADE" ...
Found DFU: [2886:0019] ... name="DFU FACTORY" ...
```

**USB 오디오 펌웨어 적용 후**에는 `dfu-util -l`에 DFU가 보이지 않고, 대신 오디오 장치로 인식됩니다.

### 5. 실시간 USB 이벤트 모니터링

```bash
log stream --predicate 'subsystem == "com.apple.iokit.IOUSBHostFamily"' --level debug
```

케이블을 꽂고 뺄 때 장치 등록/해제 이벤트를 확인할 수 있습니다.

---

## 상태별 체크리스트

### ✅ 정상: Mac에서 USB 마이크로 사용

- [ ] `system_profiler SPAudioDataType`에 ReSpeaker 입력 장치 표시
- [ ] 시스템 설정에서 ReSpeaker Lite를 입력 장치로 선택 가능
- [ ] `rec -d` 또는 Python `sounddevice`로 16 kHz mono 녹음 성공

### ✅ 정상: ESP32 개발

- [ ] `/dev/cu.usbmodem*` 포트 존재
- [ ] `esptool.py chip_id` 또는 Arduino IDE 포트 목록에 표시
- [ ] ReSpeaker Lite **I2S 펌웨어** 적용 (ESP32와 I2S 연동 시)

### 현재 Mac 상태 (2026-08-24, 환경 구축 후)

- [x] ReSpeaker Lite USB 물리 연결 확인
- [x] I2S 펌웨어 v1.0.9 플래시 (`./scripts/flash_respeaker_i2s.sh`)
- [x] dfu-util, arduino-cli, Python venv, Arduino 라이브러리 설치
- [x] Arduino 스케치 01~03 컴파일 검증
- [ ] ESP32 시리얼 포트 — **미인식** (하드웨어 연결 후 `./scripts/upload_sketch.sh` 실행)

---

## 다음 단계

1. XIAO ESP32S3 USB-C를 Mac에 연결 → `./scripts/check_devices.sh`
2. `./scripts/upload_sketch.sh 01_firmware_version` → Serial Monitor에서 펌웨어 버전 확인
3. `./scripts/upload_sketch.sh 02_i2s_csv_stream` → `python scripts/serial_csv_to_wav.py`
4. `./scripts/upload_sketch.sh 03_kws_tflite` → "yes"/"no" 키워드 테스트
