import AVFoundation
import SwiftUI
import TalkbotCore

@MainActor
final class TalkbotModel: ObservableObject {
    enum Phase: String { case idle, connecting, listening, thinking, speaking, failed }
    @Published private(set) var phase: Phase = .idle
    @Published private(set) var caption = "안녕, 난 디노야"
    @Published private(set) var expression: TalkbotCore.Expression = .warm
    @Published private(set) var microphoneLevel: Float = 0
    @Published private(set) var latency = TurnLatency()
    @Published private(set) var errorMessage: String?
    @Published var cameraEnabled = UserDefaults.standard.object(forKey: "cameraEnabled") as? Bool ?? false
    @Published var age = ChildAge(rawValue: UserDefaults.standard.string(forKey: "childAge") ?? "") ?? .earlySchool
    @Published var turnTaking = TurnTaking(rawValue: UserDefaults.standard.string(forKey: "turnTaking") ?? "") ?? .responsive
    @Published var modelName = UserDefaults.standard.string(forKey: "realtimeModel") ?? RealtimeProtocol.defaultModel
    @Published var credentialMode = CredentialProvider.Mode(rawValue: UserDefaults.standard.string(forKey: "credentialMode") ?? "") ?? .parentKey
    @Published var tokenEndpoint = UserDefaults.standard.string(forKey: "tokenEndpoint") ?? ""
    let tracker = EyeContactTracker()

    private var audio: AudioDuplex?
    private var client: RealtimeClient?
    private var gate = ResponseGate()
    private var conversation = ConversationState()
    private var sessionGeneration = 0
    private var startTask: Task<Void, Never>?
    private var eventTask: Task<Void, Never>?
    private var eventContinuation: AsyncStream<ServerEvent>.Continuation?
    private var watchdog: Task<Void, Never>?
    private var lastVoice: TimeInterval?
    private var pendingTurnSince: TimeInterval?
    private var playingItemID: String?
    private var assistantTranscript = ""
    private var interruptionObserver: NSObjectProtocol?
    private var routeObserver: NSObjectProtocol?
    private var configurationObserver: NSObjectProtocol?
    var isRunning: Bool { ![.idle, .failed].contains(phase) }

    init() {
        interruptionObserver = NotificationCenter.default.addObserver(
            forName: AVAudioSession.interruptionNotification, object: nil, queue: .main) { [weak self] _ in
                Task { @MainActor in self?.stop(message: "음성 대화가 일시 중단됐어요. 다시 시작해 주세요.") }
            }
        routeObserver = NotificationCenter.default.addObserver(
            forName: AVAudioSession.routeChangeNotification, object: nil, queue: .main) { [weak self] note in
                guard let reason = note.userInfo?[AVAudioSessionRouteChangeReasonKey] as? UInt,
                      reason != AVAudioSession.RouteChangeReason.categoryChange.rawValue else { return }
                Task { @MainActor in
                    guard let self, self.isRunning else { return }
                    self.stop(message: "오디오 연결이 바뀌었어요. 다시 시작해 주세요.")
                }
            }
        configurationObserver = NotificationCenter.default.addObserver(
            forName: .AVAudioEngineConfigurationChange, object: nil, queue: .main) { [weak self] _ in
                Task { @MainActor in
                    guard let self, self.phase == .listening || self.phase == .speaking else { return }
                    self.stop(message: "오디오 설정이 바뀌었어요. 다시 시작해 주세요.")
                }
            }
    }

    func saveSettings(secret: String) throws {
        try KeychainStore.write(secret.trimmingCharacters(in: .whitespacesAndNewlines), account: "connection-secret")
        let defaults = UserDefaults.standard
        defaults.set(cameraEnabled, forKey: "cameraEnabled")
        defaults.set(age.rawValue, forKey: "childAge")
        defaults.set(turnTaking.rawValue, forKey: "turnTaking")
        defaults.set(modelName.trimmingCharacters(in: .whitespacesAndNewlines), forKey: "realtimeModel")
        defaults.set(credentialMode.rawValue, forKey: "credentialMode")
        defaults.set(tokenEndpoint, forKey: "tokenEndpoint")
    }

    func start() {
        guard !isRunning else { return }
        sessionGeneration += 1
        let current = sessionGeneration
        errorMessage = nil; phase = .connecting; caption = "디노가 준비하고 있어"
        gate = ResponseGate(); conversation = ConversationState(); expression = .warm
        lastVoice = nil; assistantTranscript = ""; playingItemID = nil
        startTask = Task { [weak self] in
            guard let self else { return }
            do {
                let granted = await withCheckedContinuation { continuation in
                    AVAudioSession.sharedInstance().requestRecordPermission { continuation.resume(returning: $0) }
                }
                guard current == self.sessionGeneration, !Task.isCancelled else { return }
                guard granted else { throw SetupError.microphonePermission }
                let provider = CredentialProvider(mode: self.credentialMode, endpoint: self.tokenEndpoint,
                    secret: KeychainStore.read("connection-secret"))
                if self.cameraEnabled { await self.tracker.start() }
                guard current == self.sessionGeneration, !Task.isCancelled else { return }
                // Mint after camera consent: a long permission prompt must not
                // consume the short-lived token's connection window.
                let credential = try await provider.credential(model: self.modelName)
                guard current == self.sessionGeneration, !Task.isCancelled else { return }
                let audio = AudioDuplex()
                let client = RealtimeClient()
                self.audio = audio; self.client = client
                audio.onPCM = { [weak client, weak self] data, level, timestamp in
                    client?.appendAudio(data)
                    Task { @MainActor in
                        guard let self, current == self.sessionGeneration else { return }
                        self.microphoneLevel = min(1, level * 12)
                        if level > 0.015 && self.phase != .speaking { self.lastVoice = timestamp }
                    }
                }
                audio.onFirstRender = { [weak self] itemID, timestamp in
                    Task { @MainActor in
                        guard let self, current == self.sessionGeneration, itemID == self.playingItemID else { return }
                        self.latency.rendered(at: timestamp)
                        self.pendingTurnSince = nil
                        self.phase = .speaking
                    }
                }
                audio.onPlaybackDrained = { [weak self] itemID in
                    Task { @MainActor in
                        guard let self, current == self.sessionGeneration, itemID == self.playingItemID,
                              self.phase == .speaking || self.phase == .thinking else { return }
                        self.phase = .listening; self.expression = self.conversation.expression
                        self.lastVoice = nil
                    }
                }
                audio.onFailure = { [weak self] error in
                    Task { @MainActor in
                        guard let self, current == self.sessionGeneration else { return }
                        self.fail(error.localizedDescription)
                    }
                }
                client.onFailure = { [weak self] _ in
                    Task { @MainActor in
                        guard let self, current == self.sessionGeneration else { return }
                        self.fail("연결이 끊겼어요. 네트워크를 확인하고 다시 시작해 주세요.")
                    }
                }
                var continuation: AsyncStream<ServerEvent>.Continuation!
                let events = AsyncStream<ServerEvent>(bufferingPolicy: .bufferingOldest(256)) { continuation = $0 }
                let sink = continuation!
                self.eventContinuation = sink
                client.onEvent = { [weak self] event in
                    if case .dropped = sink.yield(event) {
                        Task { @MainActor in
                            guard let self, current == self.sessionGeneration else { return }
                            self.fail("음성 처리가 밀렸어요. 다시 시작해 주세요.")
                        }
                    }
                }
                self.eventTask = Task { [weak self] in
                    for await event in events {
                        guard let self, !Task.isCancelled, current == self.sessionGeneration else { return }
                        await self.handle(event)
                    }
                }
                // Capture starts only after session.updated; no lost leading words
                // while authentication/configuration is still pending.
                client.connect(credential: credential, model: self.modelName,
                    configuration: RealtimeProtocol.session(age: self.age, turnTaking: self.turnTaking, model: self.modelName))
                self.startWatchdog(generation: current)
            } catch {
                guard current == self.sessionGeneration, !Task.isCancelled else { return }
                self.fail(error.localizedDescription)
            }
        }
    }

    func stop(message: String? = nil) {
        sessionGeneration += 1; startTask?.cancel(); startTask = nil
        eventTask?.cancel(); eventTask = nil; eventContinuation?.finish(); eventContinuation = nil
        watchdog?.cancel(); watchdog = nil
        client?.close(); client = nil; audio?.stop(); audio = nil
        tracker.stop(); gate.interrupt(); latency.abandon(); pendingTurnSince = nil
        playingItemID = nil; microphoneLevel = 0; errorMessage = nil
        phase = .idle; caption = message ?? "다음에 또 같이 놀자"; expression = .warm
    }

    private func fail(_ message: String) {
        stop(); phase = .failed; errorMessage = message; caption = "잠깐 쉬었다 다시 만나자"
    }

    private func handle(_ event: ServerEvent) async {
        switch event.type {
        case "session.updated":
            guard phase == .connecting else { return }
            let current = sessionGeneration
            do { try await audio?.start() }
            catch { if current == sessionGeneration { fail(error.localizedDescription) }; return }
            guard current == sessionGeneration else { return }
            phase = .listening; caption = "네 이야기를 듣고 있어"
        case "input_audio_buffer.speech_started":
            let current = sessionGeneration
            gate.interrupt(); latency.abandon(); pendingTurnSince = nil
            // server_vad interrupt_response already cancels generation server-side.
            playingItemID = nil
            phase = .listening; expression = .curious; caption = "응, 듣고 있어"
            if let position = await audio?.interrupt(), current == sessionGeneration {
                client?.send(["type": "conversation.item.truncate", "item_id": position.itemID,
                    "content_index": position.contentIndex, "audio_end_ms": position.milliseconds])
            }
        case "input_audio_buffer.speech_stopped":
            let now = ProcessInfo.processInfo.systemUptime
            latency.start(lastVoice: lastVoice, serverStop: now)
            pendingTurnSince = now; phase = .thinking; expression = .thoughtful
        case "response.created":
            if let response = event.response, gate.begin(response.id) {
                assistantTranscript = ""
            }
        case "response.output_audio.delta":
            guard gate.accepts(event.responseID), let encoded = event.delta,
                  let data = Data(base64Encoded: encoded), let itemID = event.itemID else { return }
            playingItemID = itemID
            audio?.enqueue(data, itemID: itemID, contentIndex: event.contentIndex ?? 0)
        case "response.output_audio_transcript.delta":
            guard gate.accepts(event.responseID) else { return }
            assistantTranscript += event.delta ?? ""
            caption = String(assistantTranscript.suffix(180))
        case "response.output_audio_transcript.done":
            guard gate.accepts(event.responseID) else { return }
            let text = event.transcript ?? assistantTranscript
            conversation.reply(text); expression = conversation.expression
        case "conversation.item.input_audio_transcription.completed":
            if let text = event.transcript { conversation.hear(text) }
        case "response.done":
            guard gate.accepts(event.response?.id) else { return }
            if event.response?.status == "failed" {
                fail("디노가 답을 만들지 못했어요. 다시 시작해 주세요.")
            } else { audio?.finishOutput() }
        case "error":
            // A cancel racing a completed response is benign; other API errors
            // require an explicit restart. Do not expose raw request/key details.
            if event.error?.code != "response_cancel_not_active" {
                fail("음성 서비스 요청에 실패했어요. 보호자 설정의 키와 모델을 확인해 주세요.")
            }
        default: break
        }
    }

    private func startWatchdog(generation: Int) {
        let connectedAt = ProcessInfo.processInfo.systemUptime
        watchdog = Task { [weak self] in
            while !Task.isCancelled {
                try? await Task.sleep(nanoseconds: 100_000_000)
                guard !Task.isCancelled, let self, generation == self.sessionGeneration else { return }
                let now = ProcessInfo.processInfo.systemUptime
                self.latency.tick(now: now)
                if self.phase == .connecting && now - connectedAt > 15 {
                    self.fail("음성 연결 준비가 오래 걸려요. 네트워크와 연결 정보를 확인해 주세요.")
                    return
                }
                if let pending = self.pendingTurnSince, now - pending > 8 {
                    self.latency.abandon(countTimeout: true)
                    self.fail("답이 너무 늦어 대화를 멈췄어요. 다시 시작해 주세요.")
                    return
                }
            }
        }
    }
    enum SetupError: LocalizedError {
        case microphonePermission
        var errorDescription: String? { "설정에서 마이크 권한을 켜 주세요." }
    }
}
