import Foundation
import TalkbotCore

/// Ordered WebSocket writer. Bounds the queue instead of accumulating stale
/// microphone packets when the network stalls. Never log credentials or audio.
final class RealtimeClient {
    var onEvent: ((ServerEvent) -> Void)?
    var onFailure: ((Error) -> Void)?
    private let queue = DispatchQueue(label: "talkbot.websocket", qos: .userInitiated)
    private var socket: URLSessionWebSocketTask?
    private var session: URLSession?
    private var outgoing: [String] = []
    private var sending = false
    private var ready = false
    private var generation = 0

    func connect(credential: String, model: String, configuration: [String: Any]) {
        queue.async {
            self.closeOnQueue()
            var parts = URLComponents(string: "wss://api.openai.com/v1/realtime")!
            parts.queryItems = [URLQueryItem(name: "model", value: model)]
            var request = URLRequest(url: parts.url!)
            request.setValue("Bearer \(credential)", forHTTPHeaderField: "Authorization")
            let configurationObject = URLSessionConfiguration.ephemeral
            configurationObject.timeoutIntervalForRequest = 15
            let session = URLSession(configuration: configurationObject)
            self.session = session
            let socket = session.webSocketTask(with: request)
            self.socket = socket
            socket.resume()
            do {
                self.outgoing.append(try RealtimeProtocol.encode(["type": "session.update", "session": configuration]))
                self.pump()
                self.receive(socket, generation: self.generation)
            } catch { self.fail(error) }
        }
    }

    func send(_ event: [String: Any]) {
        do { enqueue(try RealtimeProtocol.encode(event)) }
        catch { queue.async { self.fail(error) } }
    }

    func appendAudio(_ data: Data) {
        queue.async {
            guard self.ready else { return }
            do {
                self.enqueueOnQueue(try RealtimeProtocol.encode([
                    "type": "input_audio_buffer.append", "audio": data.base64EncodedString()]))
            } catch { self.fail(error) }
        }
    }

    func close() { queue.async { self.closeOnQueue() } }

    private func enqueue(_ text: String) { queue.async { self.enqueueOnQueue(text) } }
    private func enqueueOnQueue(_ text: String) {
        guard socket != nil else { return }
        guard outgoing.count < 32 else { fail(ConnectionError.backlog); return }
        outgoing.append(text); pump()
    }
    private func pump() {
        guard !sending, let socket, !outgoing.isEmpty else { return }
        sending = true
        let message = outgoing.removeFirst()
        let current = generation
        socket.send(.string(message)) { [weak self] error in
            guard let self else { return }
            self.queue.async {
                guard current == self.generation else { return }
                self.sending = false
                if let error { self.fail(error) } else { self.pump() }
            }
        }
    }
    private func receive(_ socket: URLSessionWebSocketTask, generation: Int) {
        socket.receive { [weak self] result in
            guard let self else { return }
            self.queue.async {
                guard generation == self.generation else { return }
                switch result {
                case .failure(let error): self.fail(error)
                case .success(let message):
                    do {
                        let data: Data
                        switch message {
                        case .data(let value): data = value
                        case .string(let value): data = Data(value.utf8)
                        @unknown default: throw ConnectionError.invalidEvent
                        }
                        let event = try JSONDecoder().decode(ServerEvent.self, from: data)
                        if event.type == "session.updated" { self.ready = true }
                        self.onEvent?(event)
                        self.receive(socket, generation: generation)
                    } catch { self.fail(error) }
                }
            }
        }
    }
    private func fail(_ error: Error) { closeOnQueue(); onFailure?(error) }
    private func closeOnQueue() {
        generation += 1; ready = false; outgoing.removeAll(); sending = false
        socket?.cancel(with: .normalClosure, reason: nil); socket = nil
        session?.invalidateAndCancel(); session = nil
    }
    enum ConnectionError: LocalizedError {
        case backlog, invalidEvent
        var errorDescription: String? { "연결이 느리거나 끊겼어요. 네트워크를 확인하고 다시 시작해 주세요." }
    }
}
