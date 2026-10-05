import Foundation

public struct GazePoint: Equatable, Sendable {
    public let x: Double
    public let y: Double
    public init(x: Double, y: Double) { self.x = x; self.y = y }
    public static let center = GazePoint(x: 0, y: 0)
}

public enum GazeGeometry: Sendable {
    /// Vision image space: origin bottom-left, unit square, typically mirrored selfie.
    public static func fromLandmarks(face: (midX: Double, midY: Double, width: Double, height: Double),
                                    pupil: (x: Double, y: Double)) -> GazePoint {
        let head = headPoint(midX: face.midX, midY: face.midY)
        let lookX = face.width > 0 ? (pupil.x - face.midX) / (face.width * 0.5) : 0
        let lookY = face.height > 0 ? (pupil.y - face.midY) / (face.height * 0.5) : 0
        return combine(head: head, look: GazePoint(x: clamp(lookX), y: clamp(lookY)))
    }

    public static func irisOffset(pupil: (x: Double, y: Double),
                                  eye: (midX: Double, midY: Double, width: Double, height: Double)) -> GazePoint {
        guard eye.width > 1e-4, eye.height > 1e-4 else { return .center }
        let x = (pupil.x - eye.midX) / (eye.width * 0.5)
        let y = (pupil.y - eye.midY) / (eye.height * 0.5)
        return GazePoint(x: clamp(x), y: clamp(y))
    }

    public static func headPoint(midX: Double, midY: Double) -> GazePoint {
        // Vision / mirrored selfie: origin bottom-left, +y is up in the frame.
        GazePoint(x: clamp((midX - 0.5) * 2.4), y: clamp((midY - 0.5) * 2.2))
    }

    public static func combine(head: GazePoint, look: GazePoint) -> GazePoint {
        GazePoint(
            x: clamp(head.x * 0.62 + look.x * 0.58),
            y: clamp(head.y * 0.48 + look.y * 0.52))
    }

    /// ARKit face space: +x is the face's right. Front camera UI is mirrored, so x is negated.
    public static func fromARFace(faceX: Double, faceY: Double,
                                  lookOutLeft: Double, lookInLeft: Double,
                                  lookOutRight: Double, lookInRight: Double,
                                  lookUpLeft: Double, lookDownLeft: Double,
                                  lookUpRight: Double, lookDownRight: Double) -> GazePoint {
        let horizontal = (lookOutRight + lookInLeft - lookOutLeft - lookInRight) * 0.5
        let vertical = (lookUpLeft + lookUpRight - lookDownLeft - lookDownRight) * 0.5
        let head = GazePoint(x: clamp(-faceX / 0.13), y: clamp(faceY / 0.10))
        let look = GazePoint(x: clamp(-horizontal * 1.9), y: clamp(vertical * 1.7))
        return combine(head: head, look: look)
    }

    public static func eyesOpen(leftSpan: Double, rightSpan: Double) -> Bool {
        min(leftSpan, rightSpan) > 0.026
    }

    public static func lookingAtScreen(point: GazePoint, eyesOpen: Bool, faceWidth: Double) -> Bool {
        eyesOpen && faceWidth > 0.11 && abs(point.x) < 0.42 && abs(point.y) < 0.55
    }

    public static func clamp(_ value: Double) -> Double {
        min(1, max(-1, value))
    }
}

public struct GazeFilter: Sendable {
    public private(set) var point: GazePoint = .center
    public private(set) var hasFace = false
    public private(set) var lookingAtScreen = false
    public private(set) var eyesOpen = true
    private var updatedAt: TimeInterval?
    private var vx = 0.0
    private var vy = 0.0

    public init() {}

    public mutating func update(x: Double, y: Double, eyeContact: Bool, now: TimeInterval, eyesOpen: Bool = true) {
        guard x.isFinite, y.isFinite else { return }
        let dt = updatedAt.map { max(0.001, min(1, now - $0)) } ?? 0.05
        hasFace = true
        lookingAtScreen = eyeContact
        self.eyesOpen = eyesOpen
        updatedAt = now
        guard eyesOpen else { return }
        let cx = GazeGeometry.clamp(x), cy = GazeGeometry.clamp(y)
        let rawVx = (cx - point.x) / dt
        let rawVy = (cy - point.y) / dt
        let dAlpha = 1 - exp(-dt / 0.035)
        vx += dAlpha * (rawVx - vx)
        vy += dAlpha * (rawVy - vy)
        let cutoff = 1.6 + 1.1 * hypot(vx, vy)
        let tau = 1 / (2 * .pi * cutoff)
        let alpha = 1 - exp(-dt / max(0.018, tau))
        point = GazePoint(x: point.x + alpha * (cx - point.x), y: point.y + alpha * (cy - point.y))
    }

    public mutating func tick(now: TimeInterval) {
        guard let updatedAt, now - updatedAt > 0.7 else { return }
        hasFace = false
        lookingAtScreen = false
        eyesOpen = true
        vx = 0
        vy = 0
        point = .center
    }
}
