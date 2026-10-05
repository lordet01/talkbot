import SwiftUI
import TalkbotCore

@MainActor
struct ParentSettingsView: View {
    @ObservedObject var model: TalkbotModel
    @ObservedObject var tracker: EyeContactTracker
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            Form {
                Section("로컬 대화") {
                    LabeledContent("엔진", value: model.engineStatus)
                    Text("음성은 이 아이폰 안에서만 처리해요. 한국어 받아쓰기 · Qwen 3B · sherpa-onnx Supertonic-3 TTS.")
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                }
                Section("아이와 대화") {
                    Picker("연령", selection: $model.age) {
                        ForEach(ChildAge.allCases, id: \.self) { Text($0.rawValue).tag($0) }
                    }
                    Picker("말 끝 기다리기", selection: $model.turnTaking) {
                        ForEach(TurnTaking.allCases, id: \.self) { Text($0.rawValue).tag($0) }
                    }
                    Toggle("카메라 눈맞춤", isOn: $model.cameraEnabled)
                }
                Section("이번 실행") {
                    LabeledContent("카메라", value: tracker.status)
                    LabeledContent("측정된 응답", value: "\(model.latency.samples.count)회")
                }
            }
            .navigationTitle("설정")
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("닫기") {
                        model.saveSettings()
                        dismiss()
                    }
                }
            }
            .onDisappear { model.saveSettings() }
        }
    }
}
