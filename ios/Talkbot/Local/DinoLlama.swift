import Foundation
import llama

enum DinoLlamaError: LocalizedError {
    case missingWeights
    case loadFailed
    var errorDescription: String? {
        switch self {
        case .missingWeights: return "온디바이스 언어 모델 파일이 없어요."
        case .loadFailed: return "언어 모델을 불러오지 못했어요."
        }
    }
}

/// Qwen2.5-Instruct GGUF on Metal (iPhone).
actor DinoLlama {
    static let fileName = "Qwen2.5-3B-Instruct-Q4_K_M.gguf"
    private var model: OpaquePointer
    private var context: OpaquePointer
    private var vocab: OpaquePointer
    private var sampling: UnsafeMutablePointer<llama_sampler>
    private let nBatch: Int

    init(model: OpaquePointer, context: OpaquePointer, nBatch: Int) {
        self.model = model
        self.context = context
        self.nBatch = nBatch
        let sparams = llama_sampler_chain_default_params()
        self.sampling = llama_sampler_chain_init(sparams)
        llama_sampler_chain_add(self.sampling, llama_sampler_init_temp(0.4))
        llama_sampler_chain_add(self.sampling, llama_sampler_init_dist(UInt32.random(in: 1...10_000)))
        vocab = llama_model_get_vocab(model)
    }

    deinit {
        llama_sampler_free(sampling)
        llama_free(context)
        llama_model_free(model)
        llama_backend_free()
    }

    static func load() throws -> DinoLlama {
        guard let path = weightURL()?.path else { throw DinoLlamaError.missingWeights }
        llama_backend_init()
        llama_log_set({ _, text, _ in
            guard let text else { return }
            fputs("[llama] ", stderr)
            fputs(text, stderr)
        }, nil)
        dinoLog("모델 로드 \(path.split(separator: "/").last ?? "")")
        var modelParams = llama_model_default_params()
        modelParams.n_gpu_layers = 24
        guard let model = llama_model_load_from_file(path, modelParams) else {
            throw DinoLlamaError.loadFailed
        }
        let threads = max(1, min(4, ProcessInfo.processInfo.processorCount - 2))
        var ctxParams = llama_context_default_params()
        ctxParams.n_ctx = 768
        ctxParams.n_batch = 64
        ctxParams.n_ubatch = 64
        ctxParams.n_threads = Int32(threads)
        ctxParams.n_threads_batch = Int32(threads)
        guard let context = llama_init_from_model(model, ctxParams) else {
            llama_model_free(model)
            throw DinoLlamaError.loadFailed
        }
        dinoLog("모델 준비 ctx=\(ctxParams.n_ctx) batch=\(ctxParams.n_batch) gpu=24")
        return DinoLlama(model: model, context: context, nBatch: Int(ctxParams.n_batch))
    }

    static func weightURL() -> URL? {
        let name = fileName
        let candidates: [URL?] = [
            FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first?.appendingPathComponent(name),
            Bundle.main.url(forResource: "Qwen2.5-3B-Instruct-Q4_K_M", withExtension: "gguf")
        ]
        return candidates.compactMap { $0 }.first { FileManager.default.fileExists(atPath: $0.path) }
    }

    func complete(history: [(role: String, content: String)], user: String, system: String, maxTokens: Int = 48) -> String {
        var messages = [("system", system)] + history + [("user", user)]
        var prompt = renderChat(messages)
        let budget = Int(llama_n_ctx(context)) - maxTokens - 8
        while tokenize(prompt, addBos: false).count > budget, messages.count > 2 {
            messages.remove(at: 1)
            prompt = renderChat(messages)
        }
        llama_memory_clear(llama_get_memory(context), true)
        llama_sampler_reset(sampling)
        var tokens = tokenize(prompt, addBos: false)
        if tokens.count > budget {
            tokens = Array(tokens.suffix(max(1, budget)))
        }
        dinoLog("LLM 프롬프트 \(tokens.count)토큰")
        guard !tokens.isEmpty else { return "응, 한 번만 다시 말해 줄래?" }
        guard decode(tokens) else {
            dinoLog("LLM decode 실패")
            return "잠깐, 생각이 꼬였어. 다시 말해 줄래?"
        }
        var pieces: [CChar] = []
        var text = ""
        for _ in 0..<maxTokens {
            let token = llama_sampler_sample(sampling, context, -1)
            if llama_vocab_is_eog(vocab, token) { break }
            pieces.append(contentsOf: piece(token))
            if let chunk = String(validatingUTF8: pieces + [0]) {
                pieces.removeAll()
                text += chunk
            }
            var next = token
            if llama_decode(context, llama_batch_get_one(&next, 1)) != 0 { break }
        }
        let reply = sanitize(text)
        dinoLog("LLM 답 '\(reply.prefix(40))'")
        return reply
    }

    private func decode(_ tokens: [llama_token]) -> Bool {
        var index = 0
        while index < tokens.count {
            let end = min(index + nBatch, tokens.count)
            var slice = Array(tokens[index..<end])
            let status = slice.withUnsafeMutableBufferPointer { buffer -> Int32 in
                guard let base = buffer.baseAddress else { return -1 }
                return llama_decode(context, llama_batch_get_one(base, Int32(buffer.count)))
            }
            if status != 0 { return false }
            index = end
        }
        return true
    }

    private func sanitize(_ raw: String) -> String {
        var text = raw
            .replacingOccurrences(of: "<|im_end|>", with: "")
            .replacingOccurrences(of: "<|im_start|>", with: "")
            .replacingOccurrences(of: "*", with: "")
        if let range = text.range(of: "\n") {
            text = String(text[..<range.lowerBound])
        }
        text = text.trimmingCharacters(in: .whitespacesAndNewlines)
        return text.isEmpty ? "응, 한 번만 다시 말해 줄래?" : text
    }

    private func renderChat(_ messages: [(String, String)]) -> String {
        var owned: [llama_chat_message] = []
        owned.reserveCapacity(messages.count)
        for (role, content) in messages {
            owned.append(llama_chat_message(role: strdup(role), content: strdup(content)))
        }
        defer {
            for message in owned {
                free(UnsafeMutableRawPointer(mutating: message.role))
                free(UnsafeMutableRawPointer(mutating: message.content))
            }
        }
        let tmpl = llama_model_chat_template(model, nil)
        let needed = owned.withUnsafeBufferPointer { pointer in
            llama_chat_apply_template(tmpl, pointer.baseAddress, owned.count, true, nil, 0)
        }
        if needed <= 0 { return fallbackTemplate(messages) }
        var buffer = [CChar](repeating: 0, count: Int(needed) + 1)
        let written = owned.withUnsafeBufferPointer { pointer in
            llama_chat_apply_template(tmpl, pointer.baseAddress, owned.count, true, &buffer, needed)
        }
        guard written > 0 else { return fallbackTemplate(messages) }
        return String(cString: buffer)
    }

    private func fallbackTemplate(_ messages: [(String, String)]) -> String {
        messages.map { "<|im_start|>\($0.0)\n\($0.1)<|im_end|>\n" }.joined() + "<|im_start|>assistant\n"
    }

    private func tokenize(_ text: String, addBos: Bool) -> [llama_token] {
        text.withCString { cstr in
            let utf8Count = text.utf8.count
            let capacity = utf8Count + (addBos ? 1 : 0) + 8
            let tokens = UnsafeMutablePointer<llama_token>.allocate(capacity: capacity)
            defer { tokens.deallocate() }
            let count = llama_tokenize(vocab, cstr, Int32(utf8Count), tokens, Int32(capacity), addBos, true)
            guard count > 0 else { return [] }
            return Array(UnsafeBufferPointer(start: tokens, count: Int(count)))
        }
    }

    private func piece(_ token: llama_token) -> [CChar] {
        var buffer = [CChar](repeating: 0, count: 16)
        let n = llama_token_to_piece(vocab, token, &buffer, Int32(buffer.count), 0, false)
        if n < 0 {
            var big = [CChar](repeating: 0, count: Int(-n))
            let m = llama_token_to_piece(vocab, token, &big, -n, 0, false)
            return Array(big.prefix(Int(max(0, m))))
        }
        return Array(buffer.prefix(Int(n)))
    }
}
