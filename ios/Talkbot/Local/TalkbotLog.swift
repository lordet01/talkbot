import Foundation
import os

@MainActor
final class TalkbotLog: ObservableObject {
    static let shared = TalkbotLog()
    @Published private(set) var lines: [String] = []
    private let formatter: DateFormatter = {
        let formatter = DateFormatter()
        formatter.dateFormat = "HH:mm:ss.SSS"
        return formatter
    }()

    func add(_ message: String) {
        let line = "\(formatter.string(from: Date())) \(message)"
        lines.append(line)
        if lines.count > 14 { lines.removeFirst(lines.count - 14) }
        Logger(subsystem: "org.talkbot.companion", category: "dino").notice("\(message, privacy: .public)")
        print("[디노] \(message)")
    }
}

func dinoLog(_ message: String) {
    Task { @MainActor in TalkbotLog.shared.add(message) }
}
