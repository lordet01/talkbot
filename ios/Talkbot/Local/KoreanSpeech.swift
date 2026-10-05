import AVFoundation
import Foundation
import Speech

enum KoreanSpeechError: LocalizedError {
    case permission
    case unavailable
    case cloudOnly
    var errorDescription: String? {
        switch self {
        case .permission: return "설정에서 음성 인식 권한을 켜 주세요."
        case .unavailable: return "한국어 음성 인식을 쓸 수 없어요. 설정에서 한국어 받아쓰기를 받아 주세요."
        case .cloudOnly: return "기기 안 한국어 인식이 없어요. 설정 → 일반 → 키보드에서 한국어 받아쓰기를 다운로드해 주세요."
        }
    }
}

final class KoreanSpeech {
    private let locale = Locale(identifier: "ko-KR")
    private var recognizer: SFSpeechRecognizer?
    private var request: SFSpeechAudioBufferRecognitionRequest?
    private var task: SFSpeechRecognitionTask?
    private var format: AVAudioFormat?
    private var lastPartial = ""
    private var lastVoice: TimeInterval = 0
    private var heardSpeech = false

    func prepare() async throws {
        let status: SFSpeechRecognizerAuthorizationStatus = await withCheckedContinuation { continuation in
            SFSpeechRecognizer.requestAuthorization { continuation.resume(returning: $0) }
        }
        guard status == .authorized else { throw KoreanSpeechError.permission }
        let recognizer = SFSpeechRecognizer(locale: locale)
        guard let recognizer, recognizer.isAvailable else { throw KoreanSpeechError.unavailable }
        self.recognizer = recognizer
        format = AVAudioFormat(commonFormat: .pcmFormatInt16, sampleRate: 24_000, channels: 1, interleaved: true)
    }

    func startTurn(onPartial: @escaping (String) -> Void, onFinal: @escaping (String) -> Void) {
        cancel()
        lastPartial = ""
        heardSpeech = false
        lastVoice = ProcessInfo.processInfo.systemUptime
        let request = SFSpeechAudioBufferRecognitionRequest()
        request.shouldReportPartialResults = true
        request.requiresOnDeviceRecognition = true
        request.taskHint = .dictation
        self.request = request
        task = recognizer?.recognitionTask(with: request) { [weak self] result, error in
            guard let self else { return }
            if let result {
                let text = result.bestTranscription.formattedString.trimmingCharacters(in: .whitespacesAndNewlines)
                if !text.isEmpty {
                    self.lastPartial = text
                    self.heardSpeech = true
                    self.lastVoice = ProcessInfo.processInfo.systemUptime
                    onPartial(text)
                }
                if result.isFinal {
                    let finalText = text.isEmpty ? self.lastPartial : text
                    self.request = nil
                    self.task = nil
                    self.heardSpeech = false
                    if !finalText.isEmpty { onFinal(finalText) }
                }
            } else if error != nil {
                let leftover = self.lastPartial
                self.request = nil
                self.task = nil
                self.heardSpeech = false
                if !leftover.isEmpty { onFinal(leftover) }
            }
        }
    }

    func appendPCM(_ data: Data, level: Float, now: TimeInterval) {
        guard let request, let format else { return }
        if level > 0.02 {
            lastVoice = now
            heardSpeech = true
        }
        let frames = data.count / MemoryLayout<Int16>.size
        guard frames > 0, let buffer = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: AVAudioFrameCount(frames)) else { return }
        buffer.frameLength = AVAudioFrameCount(frames)
        data.copyBytes(to: UnsafeMutableBufferPointer(start: buffer.int16ChannelData![0], count: frames))
        request.append(buffer)
    }

    func maybeEndSilence(silence: TimeInterval, now: TimeInterval) -> String? {
        guard heardSpeech, now - lastVoice >= silence, !lastPartial.isEmpty else { return nil }
        let text = lastPartial
        cancel()
        return text
    }

    func cancel() {
        request?.endAudio()
        task?.cancel()
        request = nil
        task = nil
        heardSpeech = false
    }
}
