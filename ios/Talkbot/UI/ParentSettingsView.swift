import SwiftUI
import TalkbotCore

@MainActor
struct ParentSettingsView: View {
    @ObservedObject var model: TalkbotModel
    @ObservedObject var tracker: EyeContactTracker
    @Environment(\.dismiss) private var dismiss
    @State private var secret = KeychainStore.read("connection-secret")
    @State private var saveError: String?

    var body: some View {
        NavigationStack {
            Form {
                Section("아이와 대화") {
                    Picker("연령", selection: $model.age) {
                        ForEach(ChildAge.allCases, id: \.self) { Text($0.rawValue).tag($0) }
                    }
                    Picker("말 끝 기다리기", selection: $model.turnTaking) {
                        ForEach(TurnTaking.allCases, id: \.self) { Text($0.rawValue).tag($0) }
                    }
                    Text("빠르게는 350ms, 천천히는 650ms의 말 끝 쉼을 기다립니다. 문장이 자주 잘리면 천천히로 바꿔 주세요.")
                        .font(.footnote).foregroundStyle(.secondary)
                    Toggle("카메라 눈맞춤", isOn: $model.cameraEnabled)
                    Text("카메라는 얼굴 위치와 시선 추정에만 사용하며 영상은 기기 밖으로 보내거나 저장하지 않습니다. 지원하지 않는 기기는 얼굴 위치를 따라봅니다.")
                        .font(.footnote).foregroundStyle(.secondary)
                }
                Section("음성 연결") {
                    Picker("연결 방식", selection: $model.credentialMode) {
                        ForEach(CredentialProvider.Mode.allCases, id: \.self) { Text($0.rawValue).tag($0) }
                    }.onChange(of: model.credentialMode) { _ in secret = "" }
                    if model.credentialMode == .tokenServer {
                        TextField("HTTPS 토큰 서버 주소", text: $model.tokenEndpoint)
                            .textInputAutocapitalization(.never).autocorrectionDisabled()
                    }
                    SecureField(model.credentialMode == .parentKey ? "보호자 소유 OpenAI API 키" : "토큰 서버 인증 토큰", text: $secret)
                        .textInputAutocapitalization(.never).autocorrectionDisabled()
                    TextField("Realtime 모델", text: $model.modelName)
                        .textInputAutocapitalization(.never).autocorrectionDisabled()
                    Text("음성은 대화를 위해 음성 API로 전송됩니다. API 이용료가 발생합니다. 연결 정보는 이 기기의 Keychain에 저장됩니다.")
                        .font(.footnote).foregroundStyle(.secondary)
                }
                Section("응답 품질 · 이번 실행") {
                    LabeledContent("측정된 응답", value: "\(model.latency.samples.count)회")
                    LabeledContent("1초 이내", value: "\(model.latency.samples.filter(\.metTarget).count)회")
                    LabeledContent("응답 시간 P95", value: model.latency.p95.map { "\(Int($0))ms" } ?? "측정 전")
                    LabeledContent("8초 응답 중단", value: "\(model.latency.timedOutTurns)회")
                    LabeledContent("카메라", value: tracker.status)
                    Text("마지막 감지 음성부터 오디오 렌더까지의 추정값입니다. 실제 스피커 소리와 어린이 음성으로 확인해야 합니다. 네트워크·API 지연에 따라 1초를 넘길 수 있습니다.")
                        .font(.footnote).foregroundStyle(.secondary)
                }
                if let saveError { Text(saveError).foregroundStyle(.red) }
            }
            .navigationTitle("보호자 설정")
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("저장") {
                        do { try model.saveSettings(secret: secret); dismiss() }
                        catch { saveError = error.localizedDescription }
                    }
                }
            }
        }
        .interactiveDismissDisabled()
    }
}
