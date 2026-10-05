import SwiftUI
import TalkbotCore

private enum DinoEyeLayout {
    static let left = CGPoint(x: 129.4 / 400, y: 195.9 / 440)
    static let right = CGPoint(x: 263.0 / 400, y: 196.2 / 440)
    static let open = CGSize(width: 34.0 / 400, height: 48.0 / 440)
    static let shut = CGSize(width: 40.0 / 400, height: 22.0 / 440)
    static let lookX: CGFloat = 0.024
    static let lookY: CGFloat = 0.007
    static let restY: CGFloat = 0.0035
    static let irisScale: CGFloat = 1.08
}

enum DinoBodyPose: String {
    case idle = "DinoIdleBlank"
    case talk = "DinoTalkBlank"
    case back = "DinoBack"

    static let canvasAspect: CGFloat = 400.0 / 440.0

    static func choose(
        expression: TalkbotCore.Expression,
        speaking: Bool,
        thinking: Bool,
        eyeContact: Bool,
        time: TimeInterval
    ) -> DinoBodyPose {
        if thinking { return .idle }
        if expression == .sleepy, !speaking { return .back }
        if !speaking, !eyeContact {
            let away = time.truncatingRemainder(dividingBy: 13)
            if away > 10.4 { return .back }
        }
        if speaking {
            return sin(time * 16) > 0.05 ? .talk : .idle
        }
        return .idle
    }
}

struct DinoFaceView: View {
    let gaze: GazePoint
    let eyeContact: Bool
    let eyesOpen: Bool
    let expression: TalkbotCore.Expression
    let speaking: Bool
    let listening: Bool
    let thinking: Bool
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    var body: some View {
        GeometryReader { geometry in
            let aspect = DinoBodyPose.canvasAspect
            let width = min(geometry.size.width, geometry.size.height * aspect)
            let height = width / aspect
            TimelineView(.animation(minimumInterval: reduceMotion ? 0.5 : 1.0 / 30)) { context in
                let time = context.date.timeIntervalSinceReferenceDate
                let idleBlink = !reduceMotion && time.truncatingRemainder(dividingBy: 4.7) < 0.14
                let closed = !eyesOpen || idleBlink
                let pose = DinoBodyPose.choose(
                    expression: expression, speaking: speaking, thinking: thinking,
                    eyeContact: eyeContact, time: reduceMotion ? (speaking ? 0.1 : 0) : time)
                let bob = reduceMotion ? 0 : sin(time * (speaking ? 3.4 : 1.6)) * (speaking ? 5 : 3)
                let breath = reduceMotion ? 1 : 1 + sin(time * 1.15) * 0.012
                let look = eyeLook(width: width, height: height)
                ZStack {
                    Ellipse()
                        .fill(Color.black.opacity(0.10))
                        .frame(width: width * 0.62, height: width * 0.10)
                        .offset(y: width * 0.46)
                    Image(pose.rawValue)
                        .resizable()
                        .interpolation(.high)
                        .scaledToFit()
                        .scaleEffect(breath * (eyeContact ? 1.02 : 1))
                    if pose != .back {
                        eye(open: !closed, at: DinoEyeLayout.left, width: width, height: height, look: look, flip: false)
                        eye(open: !closed, at: DinoEyeLayout.right, width: width, height: height, look: look, flip: true)
                    }
                    if listening {
                        Circle()
                            .fill(Color.white.opacity(0.92))
                            .frame(width: 12, height: 12)
                            .overlay(Circle().stroke(Color(red: 0.20, green: 0.48, blue: 0.33), lineWidth: 2))
                            .offset(x: width * 0.38, y: -width * 0.36)
                    }
                }
                .offset(y: bob)
                .frame(width: width, height: height)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            }
        }
        .accessibilityElement(children: .ignore)
        .accessibilityLabel("디노")
        .accessibilityValue(speaking ? "말하는 중" : listening ? "듣는 중" : thinking ? "생각 중" : "기다리는 중")
    }

    private func eyeLook(width: CGFloat, height: CGFloat) -> CGSize {
        if reduceMotion { return .zero }
        let x = CGFloat(max(-1, min(1, gaze.x))) * width * DinoEyeLayout.lookX
        let y = CGFloat(max(-1, min(1, -gaze.y))) * height * DinoEyeLayout.lookY
            + height * DinoEyeLayout.restY
        return CGSize(width: x, height: y)
    }

    private func eye(open: Bool, at unit: CGPoint, width: CGFloat, height: CGFloat, look: CGSize, flip: Bool) -> some View {
        let socket = CGSize(width: width * DinoEyeLayout.open.width, height: height * DinoEyeLayout.open.height)
        let origin = CGSize(width: (unit.x - 0.5) * width, height: (unit.y - 0.5) * height)
        return ZStack {
            if open {
                Image("DinoEyeOpen")
                    .resizable()
                    .interpolation(.high)
                    .frame(
                        width: socket.width * DinoEyeLayout.irisScale,
                        height: socket.height * DinoEyeLayout.irisScale)
                    .offset(x: look.width, y: look.height)
            } else {
                Ellipse().fill(Color(red: 0.97, green: 0.91, blue: 0.82))
                Image("DinoEyeShut")
                    .resizable()
                    .interpolation(.high)
                    .scaleEffect(x: flip ? -1 : 1, y: 1)
                    .frame(
                        width: width * DinoEyeLayout.shut.width,
                        height: height * DinoEyeLayout.shut.height)
            }
        }
        .frame(width: socket.width, height: socket.height)
        .clipShape(Ellipse())
        .offset(x: origin.width, y: origin.height)
    }
}
