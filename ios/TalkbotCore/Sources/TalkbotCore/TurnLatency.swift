import Foundation

public struct LatencySample: Identifiable, Sendable {
    public let id: UUID
    public let milliseconds: Double
    public var metTarget: Bool { milliseconds <= 1_000 }
}

/// Monotonic timestamps only. Starts at last locally detected voice frame,
/// ends when player render time advances, not when an API chunk arrives.
/// This is an acoustic estimate, not a validated phoneme endpoint detector.
public struct TurnLatency: Sendable {
    public private(set) var samples: [LatencySample] = []
    public private(set) var timedOutTurns = 0
    public private(set) var endpoint: TimeInterval?
    public private(set) var deadlineMissed = false
    private var recorded = false
    public init() {}
    public mutating func start(lastVoice: TimeInterval?, serverStop: TimeInterval) {
        // Do not call a missing local endpoint an end-to-end measurement.
        endpoint = lastVoice.flatMap { $0 <= serverStop && serverStop - $0 < 4 ? $0 : nil }
        deadlineMissed = false; recorded = false
    }
    public mutating func tick(now: TimeInterval) {
        if let endpoint, !recorded, now - endpoint > 1 { deadlineMissed = true }
    }
    @discardableResult public mutating func rendered(at now: TimeInterval) -> LatencySample? {
        guard let endpoint, !recorded, now >= endpoint else { return nil }
        recorded = true
        let sample = LatencySample(id: UUID(), milliseconds: (now - endpoint) * 1_000)
        samples.append(sample)
        if samples.count > 100 { samples.removeFirst(samples.count - 100) }
        deadlineMissed = !sample.metTarget
        return sample
    }
    public mutating func abandon(countTimeout: Bool = false) {
        if countTimeout && !recorded { timedOutTurns += 1 }
        endpoint = nil; recorded = false; deadlineMissed = false
    }
    public var p95: Double? {
        guard !samples.isEmpty else { return nil }
        let sorted = samples.map(\.milliseconds).sorted()
        return sorted[max(0, Int(ceil(Double(sorted.count) * 0.95)) - 1)]
    }
}
