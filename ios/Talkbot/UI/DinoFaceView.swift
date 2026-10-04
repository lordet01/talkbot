import SwiftUI
import TalkbotCore

struct DinoFaceView: View {
    let gaze: GazePoint
    let eyeContact: Bool
    let expression: TalkbotCore.Expression
    let speaking: Bool
    let listening: Bool
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    var body: some View {
        GeometryReader { geometry in
            let width = min(geometry.size.width * 0.8, geometry.size.height * 1.85)
            TimelineView(.animation(minimumInterval: reduceMotion ? 0.5 : 1.0 / 30)) { context in
                let time = context.date.timeIntervalSinceReferenceDate
                let blink = !reduceMotion && time.truncatingRemainder(dividingBy: 4.7) < 0.14
                let sleepy = expression == .sleepy
                let bob = reduceMotion ? 0 : sin(time * 1.8) * 3
                let mouth = speaking ? 12 + (sin(time * 15) + 1) * 9 : 5
                ZStack {
                    // Rounded silhouette is intentionally drawn in Swift, so
                    // expression and gaze remain live at every screen size.
                    HStack(spacing: width * 0.16) {
                        RoundedRectangle(cornerRadius: 18).fill(Color(red: 0.26, green: 0.65, blue: 0.45))
                            .frame(width: width * 0.12, height: width * 0.25).rotationEffect(.degrees(-25))
                        RoundedRectangle(cornerRadius: 18).fill(Color(red: 0.26, green: 0.65, blue: 0.45))
                            .frame(width: width * 0.12, height: width * 0.25).rotationEffect(.degrees(25))
                    }.offset(y: -width * 0.24)
                    RoundedRectangle(cornerRadius: width * 0.22, style: .continuous)
                        .fill(LinearGradient(colors: [Color(red: 0.56, green: 0.88, blue: 0.64),
                            Color(red: 0.32, green: 0.73, blue: 0.49)], startPoint: .top, endPoint: .bottom))
                        .frame(width: width, height: width * 0.60)
                        .shadow(color: .black.opacity(0.08), radius: 24, y: 12)
                    HStack(spacing: width * 0.14) {
                        eye(width: width * 0.20, blink: blink || sleepy)
                        eye(width: width * 0.20, blink: blink || sleepy)
                    }.offset(y: -width * 0.05)
                    HStack(spacing: width * 0.60) {
                        Ellipse().fill(Color.pink.opacity(expression == .delighted ? 0.35 : 0.18))
                        Ellipse().fill(Color.pink.opacity(expression == .delighted ? 0.35 : 0.18))
                    }.frame(width: width * 0.82, height: width * 0.065).offset(y: width * 0.13)
                    HStack(spacing: width * 0.06) {
                        Capsule().fill(Color(red: 0.19, green: 0.46, blue: 0.32))
                        Capsule().fill(Color(red: 0.19, green: 0.46, blue: 0.32))
                    }.frame(width: width * 0.12, height: width * 0.018).offset(y: width * 0.095)
                    if speaking {
                        Ellipse().fill(Color(red: 0.12, green: 0.28, blue: 0.23))
                            .overlay(alignment: .bottom) { Ellipse().fill(Color.pink.opacity(0.7)).frame(height: mouth * 0.45) }
                            .frame(width: width * 0.14, height: mouth).offset(y: width * 0.20)
                    } else {
                        SmileShape().stroke(Color(red: 0.12, green: 0.28, blue: 0.23),
                            style: StrokeStyle(lineWidth: 5, lineCap: .round))
                            .frame(width: width * (expression == .delighted ? 0.25 : 0.16), height: 14)
                            .offset(y: width * 0.19)
                    }
                    if listening {
                        Circle().stroke(Color.white.opacity(0.75), lineWidth: 3)
                            .frame(width: 14, height: 14).offset(x: width * 0.47, y: -width * 0.20)
                    }
                }
                .offset(x: reduceMotion ? 0 : gaze.x * 5, y: bob)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            }
        }
        .accessibilityElement(children: .ignore)
        .accessibilityLabel("디노")
        .accessibilityValue(speaking ? "말하는 중" : listening ? "듣는 중" : "기다리는 중")
    }

    private func eye(width: CGFloat, blink: Bool) -> some View {
        ZStack {
            Capsule().fill(Color.white).frame(width: width, height: blink ? 9 : width * 1.08)
            if !blink {
                Ellipse().fill(Color(red: 0.10, green: 0.20, blue: 0.18))
                    .frame(width: width * 0.48, height: width * (expression == .curious ? 0.65 : 0.58))
                    .overlay(alignment: .topLeading) {
                        Circle().fill(Color.white).frame(width: width * 0.15).padding(width * 0.075)
                    }
                    .offset(x: gaze.x * width * 0.20, y: gaze.y * width * 0.15)
                    .scaleEffect(eyeContact ? 1.06 : 1)
            }
            if expression == .comforting || expression == .thoughtful {
                Capsule().fill(Color(red: 0.12, green: 0.32, blue: 0.22))
                    .frame(width: width * 0.70, height: 6)
                    .rotationEffect(.degrees(expression == .comforting ? -8 : 8))
                    .offset(y: -width * 0.64)
            }
        }
        .frame(width: width, height: width * 1.15)
        .animation(.easeOut(duration: 0.12), value: gaze)
    }
}

private struct SmileShape: Shape {
    func path(in rect: CGRect) -> Path {
        var path = Path()
        path.move(to: CGPoint(x: 0, y: 0))
        path.addQuadCurve(to: CGPoint(x: rect.maxX, y: 0), control: CGPoint(x: rect.midX, y: rect.maxY * 1.5))
        return path
    }
}
