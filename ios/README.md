# 디노 — Swift iPhone / iPad 앱

기존 ESP32 펌웨어와 별도로 동작하는 **iOS 16+ 네이티브 앱**입니다. 휴대폰을 세로로 들면 화면 속 민트 공룡 디노가 대화하고 아이의 얼굴 위치를 따라봅니다. SwiftUI, AVAudioEngine, ARKit / Vision, URLSession만 사용합니다. 별도 음성 프레임워크나 Python 중계 서버는 필요하지 않습니다.

## 실행

1. Mac의 Xcode 16+에서 `ios/Talkbot.xcodeproj`를 엽니다.
2. `Talkbot` 타깃의 Signing & Capabilities에서 본인의 Team과 고유 Bundle Identifier를 설정합니다.
3. 실제 iPhone / iPad에 실행합니다. 기기 암호가 설정되어 있어야 보호자 설정을 열 수 있습니다.
4. 오른쪽 위 방패 버튼을 1.2초 길게 누르고 Face ID / Touch ID / 기기 암호로 확인합니다.
5. 보호자 소유 OpenAI API 키를 입력하거나 아래 규격의 HTTPS 토큰 서버를 연결합니다. 기본 모델은 `gpt-realtime`이며 계정에서 사용 가능한 Realtime 모델로 바꿀 수 있습니다.
6. 연령, 말 끝 기다리기, 카메라 눈맞춤을 설정하고 저장합니다. 카메라는 기본 OFF입니다.
7. **디노와 이야기**를 누르고 마이크 권한을 허용합니다. 카메라를 켰다면 카메라 권한도 허용합니다.

실기기 인증 정보는 Keychain의 `WhenUnlockedThisDeviceOnly` 항목에 저장합니다. 코드·Info.plist·UserDefaults에 키를 넣지 마세요. 앱을 배포할 때 회사 공용 API 키를 앱에 포함하면 안 됩니다. 보호자 개인 키 방식은 개인 개발·검증용이며, 서비스 배포는 인증·사용량 제한이 있는 토큰 서버 방식을 사용하세요.

## 연결 방식

**내 API 키:** 앱이 HTTPS `POST /v1/realtime/client_secrets`로 60초 유효 연결 토큰을 만든 뒤, 그 토큰으로 Realtime WebSocket을 엽니다. 장기 키는 WebSocket에 전달하지 않습니다. 현재 Groq/Google 키는 이 음성 API의 인증에 사용할 수 없습니다.

**토큰 서버:** 앱이 보호자 설정의 HTTPS 주소에 `Authorization: Bearer <서버 인증 토큰>`과 아래 JSON을 POST합니다.

```json
{"model":"gpt-realtime"}
```

서버는 인증된 사용자에 대해서만 다음 형식으로 응답합니다. `expires_at`은 UNIX 초입니다.

```json
{"value":"ek_...","expires_at":1900000000}
```

이 저장소의 기존 `control-panel` Worker에 이 엔드포인트가 배포된 것은 아닙니다. 개인 키 방식은 추가 서버 없이 실행할 수 있으며, 토큰 서버 방식은 위 계약을 구현한 서버가 있을 때 사용할 수 있습니다. 음성 API 사용료는 별도입니다.

## 저지연 동작과 1초 목표

- 세션을 열어 둔 채 마이크 PCM16 / mono / 24kHz를 스트리밍합니다.
- 음성 모델이 바로 음성을 반환합니다. STT → 별도 LLM 호출 → 별도 TTS 호출을 순차로 기다리지 않습니다.
- 받아쓰기 완료를 기다리지 않고 답합니다. 받아쓰기는 자막·로컬 대화 UI 상태에만 사용합니다.
- 기본 말 끝 쉼은 350ms, 어린이 생각 쉼을 더 기다리는 옵션은 650ms입니다. 끊김이 많으면 기다리는 옵션을 사용하세요.
- 첫 오디오 청크부터 재생합니다. 모든 음성을 다 받을 때까지 기다리지 않습니다.
- 음성 처리 오디오 그래프로 마이크와 스피커를 동시에 사용하며 시스템 AEC를 켭니다.
- 아이가 끼어들면 로컬 재생을 멈추고, 듣지 못한 답변 음성을 서버 대화에서도 잘라 냅니다.
- WebSocket 이벤트를 순서대로 처리하고 취소된 응답의 늦은 오디오·자막을 버립니다.
- 전송 큐 / 수신 이벤트 / 재생 큐는 제한됩니다. 네트워크가 막히면 오래된 음성을 무한히 쌓지 않고 대화를 중단합니다.

**현재 코드가 모든 턴 1초 이내를 보장하지는 않습니다.** 외부 API와 네트워크의 최악 지연은 앱에서 제한할 수 없습니다. 보호자 화면은 `마지막 로컬 음성 프레임 → 첫 오디오 렌더 관찰` 시간, 1초 성공 횟수, P95, 8초 타임아웃을 표시합니다. 가짜 맞장구를 먼저 재생해서 답변 지연을 숨기지 않습니다.

측정은 RMS 임계값에 따른 음성 종료 추정이며 16ms 렌더 관찰 주기가 더해집니다. 스피커 실제 출력·오디오 경로 지연까지 측정한 값은 아닙니다. 로컬 종료 시점이 없거나 오래됐으면 측정 표본을 만들지 않습니다. API 청크 도착 시각을 스피커 응답 시간으로 표시하지 않습니다. 8초 이상 첫 렌더가 없으면 대화를 멈추고 재시작을 안내합니다. 중단된 턴은 성공 표본에 포함하지 않습니다.

## 자연스러운 어린이 대화

- 3–4세 / 5–7세에 맞춘 쉬운 말과 짧은 1–2문장 응답.
- 매번 질문하지 않고 아이의 말에 먼저 반응. 질문은 한 턴 최대 하나.
- 영어놀이 중 `강아지는?`, 이야기 중 `그다음` 등 생략된 맥락을 유지.
- `몰라`는 힌트로, 반복되면 함께 답하기. 정답 시험·조롱·점수화 금지.
- `아니 그게 아니라`는 수정, `싫어 / 그만`은 중단으로 처리.
- 공감하고 시도에 구체적으로 반응. 눈맞춤과 대답을 강요하지 않기.
- 위험 상황은 보호자 도움으로 연결. 개인정보·비밀 약속을 요구하지 않기.

대화 지침은 하나의 Realtime 세션에 적용합니다. 활동의 실제 진행과 맥락은 모델의 세션 대화가 관리합니다. `ConversationState`는 로컬 UI 상태이며, 비동기 받아쓰기 결과로 모델의 지침을 덮어쓰지 않습니다. 프롬프트는 대화 품질·안전성의 완전한 보장이 아닙니다. 실제 어린이 대화 평가가 필요합니다.

## 눈맞춤·감정 표현

- 지원 기기: ARKit 얼굴 위치와 눈 방향 블렌드셰이프(`eyeLookIn/Out/Up/Down`)로 시선을 추정합니다. 의료·연구용 정밀 아이트래커가 아닙니다.
- 미지원 또는 AR 오류: 전면 카메라 + Vision 눈 윤곽 대비 동공 위치로 가장 큰 얼굴을 추적합니다.
- 원본처럼 세로 타원 검은자가 눈을 거의 채웁니다. 동공은 소켓 안에서만 움직이고, 위쪽 흰자가 크게 보이지 않게 아래로 살짝 앉힙니다. 아이가 깜빡이면 디노도 감습니다. 얼굴을 700ms 이상 놓치면 중앙으로 돌아갑니다.
- 몸은 입을 연/감은 앉은 인형과 뒷모습만 바꾸고, 좌우 시선은 눈 오버레이가 담당합니다. 입 움직임은 말하기 상태 애니메이션이며 음소별 립싱크가 아닙니다.
- 표정은 대화 내용과 앱 상태에 따라 바뀝니다. 카메라로 아이의 실제 감정을 진단하지 않습니다.
- 영상·시선 좌표·얼굴 식별자는 기기 밖으로 보내거나 기록하지 않습니다. 음성은 대화 API에 전송합니다.
- 보호자 인증을 열거나 대화를 중단하면 카메라와 마이크를 멈춥니다. 백그라운드에서도 멈추며 자동 재개하지 않습니다.

## 개발·검증

```bash
swift test --package-path ios/TalkbotCore
python3 ios/scripts/generate_project.py
xcodebuild -project ios/Talkbot.xcodeproj -scheme Talkbot \
  -configuration Debug -sdk iphonesimulator \
  -destination 'generic/platform=iOS Simulator' \
  CODE_SIGNING_ALLOWED=NO build
```

Xcode 프로젝트와 Info.plist는 생성 결과를 커밋합니다. Swift 파일을 추가하면 `generate_project.py`를 다시 실행하세요. 앱은 로컬 `TalkbotCore` 프레임워크를 함께 빌드하며, 같은 소스의 Swift Package는 회귀 테스트에 사용합니다. GitHub Actions `Swift companion`이 코어 테스트, 생성 결과 일치, 시뮬레이터 빌드를 수행합니다.

실기기 확인 항목과 측정 기준: [`../docs/ios-companion-validation.md`](../docs/ios-companion-validation.md).

## 공식 구현 참고

- [Realtime conversations / 오디오 스트리밍·중단·truncate](https://developers.openai.com/api/docs/guides/realtime-conversations)
- [Realtime WebSockets](https://developers.openai.com/api/docs/guides/realtime-websocket)
- [VAD 설정](https://developers.openai.com/api/docs/guides/realtime-vad)
- [Client secret 발급](https://developers.openai.com/api/reference/resources/realtime/subresources/client_secrets/methods/create)
- [ARFaceAnchor](https://developer.apple.com/documentation/arkit/arfaceanchor)
- [ARFaceAnchor.lookAtPoint](https://developer.apple.com/documentation/arkit/arfaceanchor/lookatpoint)
