import Foundation
import Security

enum ConnectionStore {
    private static let overrideKey = "connectionSecretOverride"
    static func loadOverride() -> String {
        UserDefaults.standard.string(forKey: overrideKey) ?? ""
    }
    static func saveOverride(_ value: String) {
        let trimmed = value.trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmed.isEmpty {
            UserDefaults.standard.removeObject(forKey: overrideKey)
            try? KeychainStore.write("", account: "connection-secret")
            return
        }
        UserDefaults.standard.set(trimmed, forKey: overrideKey)
        try? KeychainStore.write(trimmed, account: "connection-secret")
    }
    static func resolvedKey(override: String) -> String {
        let typed = override.trimmingCharacters(in: .whitespacesAndNewlines)
        if !typed.isEmpty { return typed }
        let saved = loadOverride().trimmingCharacters(in: .whitespacesAndNewlines)
        if !saved.isEmpty { return saved }
        return BundledSecrets.openAIAPIKey
    }
}

enum KeychainStore {
    private static let service = "org.talkbot.ios"
    static func read(_ account: String) -> String {
        let query: [String: Any] = [kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service, kSecAttrAccount as String: account,
            kSecReturnData as String: true, kSecMatchLimit as String: kSecMatchLimitOne]
        var result: CFTypeRef?
        guard SecItemCopyMatching(query as CFDictionary, &result) == errSecSuccess,
              let data = result as? Data else { return "" }
        return String(decoding: data, as: UTF8.self)
    }
    static func write(_ value: String, account: String) throws {
        let trimmed = value.trimmingCharacters(in: .whitespacesAndNewlines)
        let query: [String: Any] = [kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service, kSecAttrAccount as String: account]
        SecItemDelete(query as CFDictionary)
        guard !trimmed.isEmpty else { return }
        var attributes = query
        attributes[kSecValueData as String] = Data(trimmed.utf8)
        attributes[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
        guard SecItemAdd(attributes as CFDictionary, nil) == errSecSuccess else { throw CredentialError.storage }
    }
}

enum CredentialError: LocalizedError {
    case missing, storage, insecureURL, rejected(Int), invalidResponse
    var errorDescription: String? {
        switch self {
        case .missing: return "설정에서 OpenAI API 키를 입력해 주세요."
        case .storage: return "연결 정보를 저장하지 못했어요."
        case .insecureURL: return "토큰 서버는 HTTPS 주소여야 해요."
        case .rejected(let status): return "음성 연결 인증에 실패했어요 (HTTP \(status))."
        case .invalidResponse: return "음성 연결 토큰을 받지 못했어요."
        }
    }
}

struct CredentialProvider {
    enum Mode: String, CaseIterable { case parentKey = "내 API 키", tokenServer = "토큰 서버" }
    let mode: Mode
    let endpoint: String
    let secret: String

    func credential(model: String) async throws -> String {
        guard !secret.isEmpty else { throw CredentialError.missing }
        let url: URL
        if mode == .parentKey {
            url = URL(string: "https://api.openai.com/v1/realtime/client_secrets")!
        } else {
            guard let parsed = URL(string: endpoint), parsed.scheme == "https", parsed.host != nil,
                  parsed.user == nil, parsed.password == nil else { throw CredentialError.insecureURL }
            url = parsed
        }
        var request = URLRequest(url: url, timeoutInterval: 15)
        request.httpMethod = "POST"
        request.setValue("Bearer \(secret)", forHTTPHeaderField: "Authorization")
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        let body: [String: Any] = mode == .parentKey
            ? ["expires_after": ["anchor": "created_at", "seconds": 60],
               "session": ["type": "realtime", "model": model]]
            : ["model": model]
        request.httpBody = try JSONSerialization.data(withJSONObject: body)
        let session = URLSession(configuration: .ephemeral)
        defer { session.invalidateAndCancel() }
        let (data, response) = try await session.data(for: request)
        guard let http = response as? HTTPURLResponse, (200..<300).contains(http.statusCode) else {
            throw CredentialError.rejected((response as? HTTPURLResponse)?.statusCode ?? 0)
        }
        struct Secret: Decodable { let value: String; let expires_at: TimeInterval }
        let result = try JSONDecoder().decode(Secret.self, from: data)
        guard !result.value.isEmpty, result.expires_at > Date().timeIntervalSince1970 else {
            throw CredentialError.invalidResponse
        }
        return result.value
    }
}
