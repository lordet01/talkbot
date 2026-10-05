import SwiftUI
import UIKit

@MainActor
struct CompanionView: View {
    @ObservedObject var model: TalkbotModel
    @ObservedObject var tracker: EyeContactTracker
    @Environment(\.scenePhase) private var scenePhase
    @State private var showingSettings = false

    var body: some View {
        GeometryReader { geometry in
            ZStack {
                LinearGradient(colors: [Color(red: 0.96, green: 0.95, blue: 0.88),
                    Color(red: 0.88, green: 0.95, blue: 0.90)], startPoint: .top, endPoint: .bottom)
                    .ignoresSafeArea()
                VStack(spacing: 10) {
                    HStack {
                        HStack(spacing: 8) {
                            Circle().fill(model.isRunning ? Color.green : Color.gray).frame(width: 7, height: 7)
                            Text(statusText).font(.subheadline.weight(.medium)).foregroundStyle(.secondary)
                        }
                        Spacer()
                        Button { showingSettings = true } label: {
                            Image(systemName: "gearshape.fill").font(.title3).padding(10)
                        }
                        .accessibilityLabel("설정")
                    }.padding(.horizontal, 20)
                    DinoFaceView(gaze: tracker.point, eyeContact: tracker.eyeContact,
                        eyesOpen: tracker.eyesOpen,
                        expression: model.expression, speaking: model.phase == .speaking,
                        listening: model.phase == .listening, thinking: model.phase == .thinking
                            || model.phase == .connecting)
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                        .padding(.horizontal, 8)
                    Text(model.caption)
                        .font(.system(.title3, design: .rounded).weight(.medium))
                        .foregroundStyle(Color(red: 0.16, green: 0.32, blue: 0.26))
                        .multilineTextAlignment(.center).lineLimit(3)
                        .minimumScaleFactor(0.82)
                        .frame(maxWidth: 420, minHeight: 56)
                        .padding(.horizontal, 24)
                    DebugLogView()
                    HStack(spacing: 18) {
                        if model.isRunning {
                            Button { model.stop() } label: { Label("잠깐 쉬기", systemImage: "pause.fill") }
                        } else {
                            Button { model.start() } label: { Label("디노와 이야기", systemImage: "mic.fill") }
                        }
                    }.buttonStyle(.borderedProminent).tint(Color(red: 0.20, green: 0.48, blue: 0.33))
                        .controlSize(.large).padding(.bottom, 16)
                }
            }
            .onAppear {
                updateViewport(geometry.size)
                Task { await tracker.start() }
            }
            .onChange(of: geometry.size) { updateViewport($0) }
            .onReceive(NotificationCenter.default.publisher(for: UIDevice.orientationDidChangeNotification)) { _ in
                updateViewport(geometry.size)
            }
        }
        .sheet(isPresented: $showingSettings) { ParentSettingsView(model: model, tracker: tracker) }
        .alert("디노", isPresented: Binding(get: { model.errorMessage != nil }, set: { _ in })) {
            Button("확인") { model.stop() }
        } message: { Text(model.errorMessage ?? "") }
        .onChange(of: scenePhase) { phase in
            if phase == .background {
                model.stop()
                tracker.stop()
                UIApplication.shared.isIdleTimerDisabled = false
            } else if phase == .active {
                Task { await tracker.start() }
            }
        }
        .onChange(of: model.cameraEnabled) { enabled in
            Task {
                if enabled { await tracker.start() } else { tracker.stop() }
            }
        }
        .onChange(of: model.isRunning) { UIApplication.shared.isIdleTimerDisabled = $0 }
    }

    private var statusText: String {
        switch model.phase {
        case .idle, .failed: return tracker.hasFace ? tracker.status : "디노와 함께"
        case .connecting: return "준비 중"
        case .listening: return "듣고 있어"
        case .thinking: return "생각 중"
        case .speaking: return "이야기 중"
        }
    }
    private func updateViewport(_ size: CGSize) {
        let orientation = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }.first?.interfaceOrientation ?? .portrait
        tracker.setViewport(size, orientation: orientation)
    }
}

private struct DebugLogView: View {
    @ObservedObject private var log = TalkbotLog.shared
    var body: some View {
        ScrollView {
            Text(log.lines.suffix(8).joined(separator: "\n"))
                .font(.system(size: 10, design: .monospaced))
                .foregroundStyle(.secondary)
                .frame(maxWidth: .infinity, alignment: .leading)
        }
        .frame(maxHeight: 88)
        .padding(.horizontal, 16)
    }
}
