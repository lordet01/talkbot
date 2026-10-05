import AVFoundation
import Foundation

final class KoreanVoice: NSObject, AVSpeechSynthesizerDelegate {
    private let synthesizer = AVSpeechSynthesizer()
    private var continuation: CheckedContinuation<Void, Never>?
    private var cancelled = false
    private(set) var usingNeural = false
    var onStart: (() -> Void)?

    override init() {
        super.init()
        synthesizer.delegate = self
    }

    func prepare() {
        usingNeural = false
        dinoLog("TTS \(Self.preferredVoice()?.name ?? "Yuna") 여자아이")
    }

    func speak(_ text: String, playPCM: ((Data, Int) async -> Void)?) async {
        stop()
        cancelled = false
        await speakApple(text)
    }

    func stop() {
        cancelled = true
        if let continuation {
            self.continuation = nil
            continuation.resume()
        }
        synthesizer.stopSpeaking(at: .immediate)
    }

    func speechSynthesizer(_ synthesizer: AVSpeechSynthesizer, didStart utterance: AVSpeechUtterance) {
        onStart?()
    }

    func speechSynthesizer(_ synthesizer: AVSpeechSynthesizer, didFinish utterance: AVSpeechUtterance) {
        resumeSpeak()
    }

    func speechSynthesizer(_ synthesizer: AVSpeechSynthesizer, didCancel utterance: AVSpeechUtterance) {
        resumeSpeak()
    }

    private func speakApple(_ text: String) async {
        let utterance = AVSpeechUtterance(string: text)
        utterance.voice = Self.preferredVoice()
        utterance.rate = AVSpeechUtteranceDefaultSpeechRate * 0.98
        utterance.pitchMultiplier = 1.18
        utterance.preUtteranceDelay = 0.04
        utterance.postUtteranceDelay = 0.04
        await withCheckedContinuation { continuation in
            self.continuation = continuation
            synthesizer.speak(utterance)
        }
    }

    private func resumeSpeak() {
        let pending = continuation
        continuation = nil
        pending?.resume()
    }

    private static func preferredVoice() -> AVSpeechSynthesisVoice? {
        let identifiers = [
            "com.apple.voice.premium.ko-KR.Yuna",
            "com.apple.voice.enhanced.ko-KR.Yuna",
            "com.apple.voice.compact.ko-KR.Yuna",
            "com.apple.ttsbundle.Yuna-compact",
        ]
        for identifier in identifiers {
            if let voice = AVSpeechSynthesisVoice(identifier: identifier) { return voice }
        }
        let korean = AVSpeechSynthesisVoice.speechVoices().filter { $0.language.hasPrefix("ko") }
        if let yuna = korean.first(where: {
            $0.name.localizedCaseInsensitiveContains("yuna") || $0.name.contains("유나")
        }) {
            return yuna
        }
        return korean.first { $0.gender == .female } ?? AVSpeechSynthesisVoice(language: "ko-KR")
    }
}
