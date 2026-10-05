import AVFoundation
import Foundation

/// One voice-processing audio graph: microphone capture remains active during
/// playback, with Apple's echo cancellation rather than a post-TTS dead period.
final class AudioDuplex {
    struct PlaybackPosition { let itemID: String; let contentIndex: Int; let milliseconds: Int }
    var onPCM: ((Data, Float, TimeInterval) -> Void)?
    var onFirstRender: ((String, TimeInterval) -> Void)?
    var onPlaybackDrained: ((String) -> Void)?
    var onFailure: ((Error) -> Void)?

    private let queue = DispatchQueue(label: "talkbot.audio", qos: .userInteractive)
    private let engine = AVAudioEngine()
    private let player = AVAudioPlayerNode()
    private let wireFormat = AVAudioFormat(commonFormat: .pcmFormatInt16,
        sampleRate: 24_000, channels: 1, interleaved: true)!
    private let playFormat = AVAudioFormat(commonFormat: .pcmFormatFloat32,
        sampleRate: 24_000, channels: 1, interleaved: false)!
    private var renderTimer: DispatchSourceTimer?
    private var hasTap = false
    private var generation = 0
    private var pendingBuffers = 0
    private var scheduledFrames: Int64 = 0
    private var reportedFirst = false
    private var outputEnded = false
    private var itemID: String?
    private var contentIndex = 0
    private var playWait: CheckedContinuation<Void, Never>?

    init() {
        engine.attach(player)
        engine.connect(player, to: engine.mainMixerNode, format: playFormat)
    }

    func start() async throws {
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            queue.async {
                do {
                    let session = AVAudioSession.sharedInstance()
                    try session.setCategory(.playAndRecord, mode: .voiceChat,
                        options: [.defaultToSpeaker, .allowBluetooth])
                    try session.setPreferredSampleRate(48_000)
                    try session.setPreferredIOBufferDuration(0.01)
                    try session.setActive(true)
                    let input = self.engine.inputNode
                    try input.setVoiceProcessingEnabled(true)
                    let inputFormat = input.outputFormat(forBus: 0)
                    guard inputFormat.sampleRate > 0,
                          let converter = AVAudioConverter(from: inputFormat, to: self.wireFormat) else {
                        throw AudioError.unavailable
                    }
                    // ~20ms at hardware rate. The tap converts synchronously while its
                    // buffer is valid; network work is handed to a separate send queue.
                    input.installTap(onBus: 0, bufferSize: AVAudioFrameCount(inputFormat.sampleRate * 0.02),
                                     format: inputFormat) { [weak self] buffer, _ in
                        guard let self else { return }
                        let capacity = AVAudioFrameCount(ceil(Double(buffer.frameLength) * 24_000 / inputFormat.sampleRate) + 32)
                        guard let out = AVAudioPCMBuffer(pcmFormat: self.wireFormat, frameCapacity: capacity) else { return }
                        var supplied = false
                        var conversionError: NSError?
                        let result = converter.convert(to: out, error: &conversionError) { _, status in
                            if supplied { status.pointee = .noDataNow; return nil }
                            supplied = true; status.pointee = .haveData; return buffer
                        }
                        if result == .error {
                            self.onFailure?(conversionError ?? AudioError.conversion as NSError)
                            return
                        }
                        guard out.frameLength > 0, let samples = out.int16ChannelData?[0] else { return }
                        let count = Int(out.frameLength)
                        var power: Float = 0
                        for index in 0..<count {
                            let value = Float(samples[index]) / 32768
                            power += value * value
                        }
                        let data = Data(bytes: samples, count: count * MemoryLayout<Int16>.size)
                        self.onPCM?(data, sqrt(power / Float(count)), ProcessInfo.processInfo.systemUptime)
                    }
                    self.hasTap = true
                    self.engine.prepare()
                    try self.engine.start()
                    let timer = DispatchSource.makeTimerSource(queue: self.queue)
                    timer.schedule(deadline: .now(), repeating: .milliseconds(16))
                    timer.setEventHandler { [weak self] in self?.observeRender() }
                    self.renderTimer = timer
                    timer.resume()
                    continuation.resume()
                } catch {
                    self.stopOnQueue()
                    continuation.resume(throwing: error)
                }
            }
        }
    }

    func enqueue(_ data: Data, itemID: String, contentIndex: Int) {
        queue.async { self.enqueueOnQueue(data, itemID: itemID, contentIndex: contentIndex) }
    }

    func finishOutput() {
        queue.async {
            self.outputEnded = true
            if self.pendingBuffers == 0, let itemID = self.itemID { self.onPlaybackDrained?(itemID) }
        }
    }

    /// Play 16-bit little-endian PCM and wait until the player is empty.
    func playPCM16(_ data: Data, sampleRate: Int) async {
        let pcm: Data
        if sampleRate == 24_000 {
            pcm = data
        } else {
            pcm = Self.resampleInt16(data, from: sampleRate, to: 24_000)
        }
        await withCheckedContinuation { (continuation: CheckedContinuation<Void, Never>) in
            queue.async {
                self.finishPlayWait()
                self.playWait = continuation
                let item = "tts-\(UUID().uuidString)"
                self.enqueueOnQueue(pcm, itemID: item, contentIndex: 0)
                self.outputEnded = true
                if self.pendingBuffers == 0 { self.finishPlayWait() }
            }
        }
    }

    private func enqueueOnQueue(_ data: Data, itemID: String, contentIndex: Int) {
        guard engine.isRunning, !data.isEmpty, data.count % 2 == 0 else { return }
        if self.itemID != itemID {
            resetPlayback(resumeWait: false)
            self.itemID = itemID
            self.contentIndex = contentIndex
        }
        let frames = AVAudioFrameCount(data.count / 2)
        guard scheduledFrames - playedFrames + Int64(frames) <= 24_000 * 24,
              let buffer = AVAudioPCMBuffer(pcmFormat: playFormat, frameCapacity: frames),
              let target = buffer.floatChannelData?[0] else {
            onFailure?(AudioError.outputBacklog)
            return
        }
        buffer.frameLength = frames
        data.withUnsafeBytes { bytes in
            for index in 0..<Int(frames) {
                let bits = UInt16(bytes[index * 2]) | UInt16(bytes[index * 2 + 1]) << 8
                target[index] = Float(Int16(bitPattern: bits)) / 32768
            }
        }
        let generation = self.generation
        pendingBuffers += 1
        scheduledFrames += Int64(frames)
        player.scheduleBuffer(buffer, completionCallbackType: .dataPlayedBack) { [weak self] _ in
            guard let self else { return }
            self.queue.async {
                guard self.generation == generation else { return }
                self.pendingBuffers -= 1
                if self.pendingBuffers == 0 && self.outputEnded, let itemID = self.itemID {
                    self.onPlaybackDrained?(itemID)
                    self.finishPlayWait()
                }
            }
        }
        if !player.isPlaying { player.play() }
    }

    private static func resampleInt16(_ data: Data, from srcRate: Int, to dstRate: Int) -> Data {
        guard srcRate > 0, dstRate > 0, srcRate != dstRate, data.count >= 2 else { return data }
        let srcCount = data.count / 2
        let dstCount = max(1, srcCount * dstRate / srcRate)
        var out = Data(count: dstCount * 2)
        data.withUnsafeBytes { srcRaw in
            let src = srcRaw.bindMemory(to: Int16.self)
            out.withUnsafeMutableBytes { dstRaw in
                let dst = dstRaw.bindMemory(to: Int16.self)
                for i in 0..<dstCount {
                    let srcIndex = min(srcCount - 1, i * srcRate / dstRate)
                    dst[i] = src[srcIndex]
                }
            }
        }
        return out
    }

    func interrupt() async -> PlaybackPosition? {
        await withCheckedContinuation { continuation in
            queue.async {
                let position = self.itemID.map {
                    PlaybackPosition(itemID: $0, contentIndex: self.contentIndex,
                        milliseconds: Int(Double(self.playedFrames) / 24))
                }
                self.resetPlayback()
                continuation.resume(returning: position)
            }
        }
    }

    func stop() { queue.async { self.stopOnQueue() } }

    private var playedFrames: Int64 {
        guard let time = player.lastRenderTime, let position = player.playerTime(forNodeTime: time) else { return 0 }
        return max(0, min(scheduledFrames, position.sampleTime))
    }
    private func observeRender() {
        if let itemID, !reportedFirst && playedFrames > 0 {
            reportedFirst = true
            onFirstRender?(itemID, ProcessInfo.processInfo.systemUptime)
        }
    }
    private func finishPlayWait() {
        let wait = playWait
        playWait = nil
        wait?.resume()
    }

    private func resetPlayback(resumeWait: Bool = true) {
        generation += 1; player.stop(); pendingBuffers = 0; scheduledFrames = 0
        itemID = nil; reportedFirst = false; outputEnded = false
        if resumeWait { finishPlayWait() }
    }
    private func stopOnQueue() {
        renderTimer?.cancel(); renderTimer = nil
        if hasTap { engine.inputNode.removeTap(onBus: 0); hasTap = false }
        engine.stop(); resetPlayback()
        try? AVAudioSession.sharedInstance().setActive(false, options: .notifyOthersOnDeactivation)
    }
    enum AudioError: LocalizedError {
        case unavailable, conversion, outputBacklog
        var errorDescription: String? {
            switch self {
            case .unavailable: return "마이크를 사용할 수 없어요. 연결을 확인해 주세요."
            case .conversion: return "음성 변환에 실패했어요. 다시 시작해 주세요."
            case .outputBacklog: return "음성이 너무 오래 쌓였어요. 다시 시작해 주세요."
            }
        }
    }
}
