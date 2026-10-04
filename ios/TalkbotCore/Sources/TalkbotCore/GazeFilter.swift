import Foundation

public struct GazePoint: Equatable, Sendable {
    public let x: Double
    public let y: Double
    public init(x: Double, y: Double) { self.x = x; self.y = y }
    public static let center = GazePoint(x: 0, y: 0)
}

public struct GazeFilter: Sendable {
    public private(set) var point: GazePoint = .center
    public private(set) var hasFace = false
    public private(set) var lookingAtScreen = false
    private var updatedAt: TimeInterval?
    public init() {}
    public mutating func update(x: Double, y: Double, eyeContact: Bool, now: TimeInterval) {
        guard x.isFinite, y.isFinite else { return }
        let dt = updatedAt.map { max(0, min(1, now - $0)) } ?? 0.1
        let alpha = 1 - exp(-dt / 0.12)
        let cx = min(1, max(-1, x)), cy = min(1, max(-1, y))
        point = GazePoint(x: point.x + alpha * (cx - point.x), y: point.y + alpha * (cy - point.y))
        hasFace = true; lookingAtScreen = eyeContact; updatedAt = now
    }
    public mutating func tick(now: TimeInterval) {
        guard let updatedAt, now - updatedAt > 0.7 else { return }
        hasFace = false; lookingAtScreen = false; point = .center
    }
}
