import LocalAuthentication
import SwiftUI
import UIKit

@MainActor
struct CompanionView: View {
    @ObservedObject var model: TalkbotModel
    @ObservedObject var tracker: EyeContactTracker
    @Environment(\.scenePhase) private var scenePhase
    @State private var showingSettings = false
    @State private var authenticating = false
    @State private var authError: String?

    var body: some View {
        GeometryReader { geometry in
            ZStack {
                LinearGradient(colors: [Color(red: 0.96, green: 0.95, blue: 0.88),
                    Color(red: 0.88, green: 0.95, blue: 0.90)], startPoint: .top, endPoint: .bottom)
                    .ignoresSafeArea()
                VStack(spacing: 6) {
                    HStack {
                        HStack(spacing: 8) {
                            Circle().fill(model.isRunning ? Color.green : Color.gray).frame(width: 7, height: 7)
                            Text(statusText).font(.subheadline.weight(.medium)).foregroundStyle(.secondary)
                        }
                        Spacer()
                        Button {} label: { Image(systemName: "lock.shield").font(.title3).padding(14) }
                            .simultaneousGesture(LongPressGesture(minimumDuration: 1.2).onEnded { _ in openSettings() })
                            .accessibilityLabel("보호자 설정, 길게 누르기")
                            .accessibilityAction { openSettings() }
                            .disabled(authenticating)
                    }.padding(.horizontal, 24)
                    DinoFaceView(gaze: tracker.point, eyeContact: tracker.eyeContact,
                        expression: model.expression, speaking: model.phase == .speaking,
                        listening: model.phase == .listening)
                    Text(model.caption)
                        .font(.system(.title3, design: .rounded).weight(.medium))
                        .foregroundStyle(Color(red: 0.16, green: 0.32, blue: 0.26))
                        .multilineTextAlignment(.center).lineLimit(2).frame(maxWidth: 620, minHeight: 48)
                        .padding(.horizontal, 30)
                    HStack(spacing: 18) {
                        if model.isRunning {
                            Button { model.stop() } label: { Label("잠깐 쉬기", systemImage: "pause.fill") }
                        } else {
                            Button { model.start() } label: { Label("디노와 이야기", systemImage: "mic.fill") }
                        }
                    }.buttonStyle(.borderedProminent).tint(Color(red: 0.20, green: 0.48, blue: 0.33))
                        .controlSize(.large).padding(.bottom, 12)
                }
            }
            .onAppear { updateViewport(geometry.size) }
            .onChange(of: geometry.size) { updateViewport($0) }
            .onReceive(NotificationCenter.default.publisher(for: UIDevice.orientationDidChangeNotification)) { _ in
                updateViewport(geometry.size)
            }
        }
        .sheet(isPresented: $showingSettings) { ParentSettingsView(model: model, tracker: tracker) }
        .alert("보호자 확인", isPresented: Binding(get: { authError != nil }, set: { if !$0 { authError = nil } })) {
            Button("확인") { authError = nil }
        } message: { Text(authError ?? "") }
        .alert("연결 확인", isPresented: Binding(get: { model.errorMessage != nil }, set: { _ in })) {
            Button("확인") { model.stop() }
        } message: { Text(model.errorMessage ?? "") }
        .onChange(of: scenePhase) { phase in
            // System permission sheets can make the scene inactive during setup.
            // Always stop in background; an active conversation also stops when
            // focus is lost. Setup may finish after its permission sheet closes.
            if phase == .background || (phase == .inactive && model.phase != .connecting) {
                model.stop(); UIApplication.shared.isIdleTimerDisabled = false
            }
        }
        .onChange(of: model.isRunning) { UIApplication.shared.isIdleTimerDisabled = $0 }
    }

    private var statusText: String {
        switch model.phase {
        case .idle, .failed: return "디노와 함께"
        case .connecting: return "준비 중"
        case .listening: return "듣고 있어"
        case .thinking: return "생각 중"
        case .speaking: return "이야기 중"
        }
    }
    private func updateViewport(_ size: CGSize) {
        let orientation = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }.first?.interfaceOrientation ?? .landscapeRight
        tracker.setViewport(size, orientation: orientation)
    }
    private func openSettings() {
        guard !authenticating else { return }
        // Opening a camera-backed Face ID prompt first stops ARKit/microphone.
        model.stop(); authenticating = true
        let context = LAContext()
        var error: NSError?
        guard context.canEvaluatePolicy(.deviceOwnerAuthentication, error: &error) else {
            authenticating = false
            authError = "기기 암호를 설정하면 보호자 설정을 열 수 있어요."
            return
        }
        Task { @MainActor in
            do {
                let granted = try await context.evaluatePolicy(.deviceOwnerAuthentication, localizedReason: "디노의 보호자 설정을 열어요")
                if granted { showingSettings = true }
            } catch { /* Canceled authentication leaves the child screen paused. */ }
            authenticating = false
        }
    }
}
