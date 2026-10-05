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
    @Published var cameraEnabled = true
    @Published var age = ChildAge(rawValue: UserDefaults.standard.string(forKey: "childAge") ?? "") ?? .earlySchool
    @Published var turnTaking = TurnTaking(rawValue: UserDefaults.standard.string(forKey: "turnTaking") ?? "") ?? .patient
    @Published var engineStatus = "로컬 대기"
    let tracker = EyeContactTracker()

    private var audio: AudioDuplex?
    private var conversation = ConversationState()
    private var history: [(role: String, content: String)] = []
    private var sessionGeneration = 0
    private var startTask: Task<Void, Never>?
    private var loopTask: Task<Void, Never>?
    private var lastVoice: TimeInterval?
    private var ignoreUntil: TimeInterval = 0
    private var capturing = false
    private let speech = KoreanSpeech()
    private let voice = KoreanVoice()
    private static var llama: DinoLlama?
    var isRunning: Bool { ![.idle, .failed].contains(phase) }

    func saveSettings(commitKey: Bool = false) {
        let defaults = UserDefaults.standard
        defaults.set(cameraEnabled, forKey: "cameraEnabled")
        defaults.set(age.rawValue, forKey: "childAge")
        defaults.set(turnTaking.rawValue, forKey: "turnTaking")
    }

    func start() {
        guard !isRunning else { return }
        sessionGeneration += 1
        let current = sessionGeneration
        errorMessage = nil
        phase = .connecting
        caption = "디노가 생각하고 있어"
        engineStatus = "모델 준비"
        conversation = ConversationState()
        history = []
        expression = .warm
        lastVoice = nil
        startTask = Task { [weak self] in
            guard let self else { return }
            do {
                let mic = await withCheckedContinuation { continuation in
                    AVAudioSession.sharedInstance().requestRecordPermission { continuation.resume(returning: $0) }
                }
                guard current == self.sessionGeneration, !Task.isCancelled else { return }
                guard mic else { throw SetupError.microphonePermission }
                try await self.speech.prepare()
                self.voice.prepare()
                if Self.llama == nil {
                    Self.llama = try await Task.detached(priority: .userInitiated) {
                        try DinoLlama.load()
                    }.value
                }
                guard current == self.sessionGeneration, !Task.isCancelled else { return }
                if self.cameraEnabled { await self.tracker.start() }
                let audio = AudioDuplex()
                self.audio = audio
                audio.onPCM = { [weak self] data, level, timestamp in
                    Task { @MainActor in
                        guard let self, current == self.sessionGeneration else { return }
                        self.microphoneLevel = min(1, level * 12)
                        guard self.capturing, timestamp >= self.ignoreUntil else { return }
                        if level > 0.02 { self.lastVoice = timestamp }
                        self.speech.appendPCM(data, level: level, now: timestamp)
                        let silence = max(0.55, Double(self.turnTaking.silenceMilliseconds) / 1000)
                        if let text = self.speech.maybeEndSilence(silence: silence, now: timestamp) {
                            await self.handleUser(text, generation: current)
                        }
                    }
                }
                audio.onFailure = { [weak self] error in
                    Task { @MainActor in
                        guard let self, current == self.sessionGeneration else { return }
                        self.fail(error.localizedDescription)
                    }
                }
                try await audio.start()
                guard current == self.sessionGeneration else { return }
                self.engineStatus = self.voice.usingNeural ? "STT·LLM·Supertonic TTS" : "STT·LLM·AVSpeech"
                TalkbotLog.shared.add("로컬 대화 시작")
                self.beginListen(generation: current)
            } catch {
                guard current == self.sessionGeneration, !Task.isCancelled else { return }
                self.fail(error.localizedDescription)
            }
        }
    }

    func stop(message: String? = nil) {
        sessionGeneration += 1
        startTask?.cancel(); startTask = nil
        loopTask?.cancel(); loopTask = nil
        capturing = false
        speech.cancel()
        voice.stop()
        audio?.stop(); audio = nil
        latency.abandon()
        microphoneLevel = 0
        errorMessage = nil
        phase = .idle
        caption = message ?? "다음에 또 같이 놀자"
        expression = .warm
        engineStatus = "로컬 대기"
    }

    private func fail(_ message: String) {
        stop()
        phase = .failed
        errorMessage = message
        caption = "잠깐 쉬었다 다시 만나자"
    }

    private func beginListen(generation: Int) {
        guard generation == sessionGeneration else { return }
        capturing = true
        phase = .listening
        expression = conversation.expression
        caption = "네 이야기를 듣고 있어"
        speech.startTurn { [weak self] partial in
            Task { @MainActor in
                guard let self, generation == self.sessionGeneration, self.phase == .listening else { return }
                self.caption = partial
                self.expression = .curious
            }
        } onFinal: { [weak self] text in
            Task { @MainActor in
                guard let self else { return }
                await self.handleUser(text, generation: generation)
            }
        }
    }

    private func handleUser(_ raw: String, generation: Int) async {
        guard generation == sessionGeneration, phase == .listening || phase == .connecting else { return }
        let text = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        guard text.count >= 1 else { return }
        capturing = false
        speech.cancel()
        let plan = conversation.hear(text, age: age)
        expression = conversation.expression
        phase = .thinking
        caption = "생각 중"
        TalkbotLog.shared.add("STT '\(text.prefix(40))' act=\(plan.activity.rawValue) beat=\(plan.beat.rawValue)")
        let now = ProcessInfo.processInfo.systemUptime
        latency.start(lastVoice: lastVoice, serverStop: now)
        tracker.pauseCapture()
        let historySnapshot = history
        let reply: String
        if let canned = plan.canned {
            reply = canned
            TalkbotLog.shared.add("전처리 고정 답")
        } else {
            guard let llama = Self.llama else {
                fail("언어 모델이 준비되지 않았어요.")
                return
            }
            let tokens = plan.activity == .sleep ? 32 : 48
            reply = await llama.complete(
                history: historySnapshot,
                user: plan.userPrompt,
                system: plan.systemPrompt,
                maxTokens: tokens
            )
        }
        tracker.resumeCapture()
        guard generation == sessionGeneration else { return }
        history.append((role: "user", content: text))
        history.append((role: "assistant", content: reply))
        if history.count > 20 { history.removeFirst(history.count - 20) }
        conversation.reply(reply)
        expression = conversation.expression
        caption = reply
        phase = .speaking
        TalkbotLog.shared.add(voice.usingNeural ? "TTS Supertonic" : "TTS AVSpeech")
        voice.onStart = { [weak self] in
            Task { @MainActor in
                guard let self, generation == self.sessionGeneration else { return }
                self.latency.rendered(at: ProcessInfo.processInfo.systemUptime)
            }
        }
        await voice.speak(reply) { [weak self] pcm, sampleRate in
            guard let self, let audio = self.audio else { return }
            await audio.playPCM16(pcm, sampleRate: sampleRate)
        }
        guard generation == sessionGeneration else { return }
        ignoreUntil = ProcessInfo.processInfo.systemUptime + 0.45
        lastVoice = nil
        beginListen(generation: generation)
        TalkbotLog.shared.add("다시 듣기")
    }

    enum SetupError: LocalizedError {
        case microphonePermission
        var errorDescription: String? { "설정에서 마이크 권한을 켜 주세요." }
    }
}
