import Foundation

public enum RealtimeProtocol {
    public static let sampleRate = 24_000
    public static let defaultModel = "gpt-realtime"

    public static func session(age: ChildAge, turnTaking: TurnTaking, model: String) -> [String: Any] {
        [
            "type": "realtime", "model": model,
            "instructions": ConversationPolicy.instructions(age: age),
            "output_modalities": ["audio"], "max_output_tokens": 180,
            "audio": [
                "input": [
                    "format": ["type": "audio/pcm", "rate": sampleRate],
                    "noise_reduction": ["type": "near_field"],
                    // Transcription is for captions only; never wait for it to reply.
                    "transcription": ["model": "gpt-4o-mini-transcribe", "language": "ko"],
                    "turn_detection": ["type": "server_vad", "threshold": 0.5,
                        "prefix_padding_ms": 300, "silence_duration_ms": turnTaking.silenceMilliseconds,
                        "create_response": true, "interrupt_response": true]
                ],
                "output": ["format": ["type": "audio/pcm", "rate": sampleRate], "voice": "marin"]
            ]
        ]
    }

    public static func encode(_ event: [String: Any]) throws -> String {
        String(decoding: try JSONSerialization.data(withJSONObject: event, options: [.sortedKeys]), as: UTF8.self)
    }
}

public struct ServerEvent: Decodable, Sendable {
    public struct Response: Decodable, Sendable {
        public let id: String
        public let status: String?
    }
    public struct APIError: Decodable, Sendable {
        public let code: String?
        public let message: String?
    }
    public let type: String
    public let responseID: String?
    public let itemID: String?
    public let contentIndex: Int?
    public let delta: String?
    public let transcript: String?
    public let response: Response?
    public let error: APIError?
    enum CodingKeys: String, CodingKey {
        case type, delta, transcript, response, error
        case responseID = "response_id", itemID = "item_id", contentIndex = "content_index"
    }
}

/// Prevent already queued audio/transcript events from a canceled turn leaking
/// into a new turn. IDs are session-scoped and reset on reconnect.
public struct ResponseGate: Sendable {
    public private(set) var activeID: String?
    private var canceled: Set<String> = []
    public init() {}
    public mutating func begin(_ id: String) -> Bool {
        guard !canceled.contains(id) else { return false }
        activeID = id; return true
    }
    public mutating func interrupt() {
        if let activeID { canceled.insert(activeID) }
        activeID = nil
    }
    public func accepts(_ id: String?) -> Bool {
        guard let id else { return false }
        return id == activeID && !canceled.contains(id)
    }
}
