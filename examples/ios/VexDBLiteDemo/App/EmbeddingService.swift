import Foundation
import Security

struct EmbeddingConfiguration: Sendable {
    let endpoint: String
    let apiKey: String
    let model: String

    func embeddingsURL() throws -> URL {
        let value = endpoint.trimmingCharacters(in: .whitespacesAndNewlines)
        guard var url = URL(string: value),
              let scheme = url.scheme?.lowercased(),
              scheme == "https" || scheme == "http",
              url.host != nil else {
            throw EmbeddingError.configuration("API 地址需要是完整的 http 或 https URL")
        }
        if !url.path.hasSuffix("/embeddings") {
            url.appendPathComponent("embeddings")
        }
        if scheme == "http", url.host != "127.0.0.1", url.host != "localhost" {
            throw EmbeddingError.configuration("非本机 Embedding API 必须使用 HTTPS")
        }
        return url
    }

    func validate() throws {
        let url = try embeddingsURL()
        guard !model.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else {
            throw EmbeddingError.configuration("请填写 Embedding 模型名称")
        }
        if Self.requiresAPIKey(host: url.host), apiKey.isEmpty {
            throw EmbeddingError.configuration("该 Embedding API 需要填写 API Key")
        }
    }

    static func requiresAPIKey(host: String?) -> Bool {
        guard let host = host?.lowercased() else { return false }
        return host == "api.openai.com" || host == "dashscope.aliyuncs.com" ||
            host.hasSuffix(".maas.aliyuncs.com")
    }
}

enum EmbeddingError: LocalizedError {
    case configuration(String)
    case invalidInput(String)
    case network(String)
    case server(status: Int, message: String)
    case invalidResponse(String)

    var errorDescription: String? {
        switch self {
        case .configuration(let message), .invalidInput(let message),
             .network(let message), .invalidResponse(let message):
            return message
        case .server(let status, let message):
            if status == 401 || status == 403 { return "API 鉴权失败，请检查 API Key" }
            if status == 429 { return "Embedding API 请求过于频繁，请稍后重试" }
            return "Embedding API 返回 \(status)：\(message)"
        }
    }
}

actor EmbeddingClient {
    private struct RequestBody: Encodable {
        let model: String
        let input: [String]
    }

    private struct ResponseBody: Decodable {
        struct Item: Decodable {
            let index: Int
            let embedding: [Double]
        }
        let data: [Item]
    }

    private struct ErrorBody: Decodable {
        struct Detail: Decodable { let message: String }
        let error: Detail
    }

    func embed(_ inputs: [String], configuration: EmbeddingConfiguration) async throws -> [[Double]] {
        try configuration.validate()
        guard !inputs.isEmpty, inputs.count <= 64,
              inputs.allSatisfy({ !$0.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty }) else {
            throw EmbeddingError.invalidInput("每次需要提交 1 到 64 个非空文本分片")
        }

        var request = URLRequest(url: try configuration.embeddingsURL())
        request.httpMethod = "POST"
        request.timeoutInterval = 60
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        if !configuration.apiKey.isEmpty {
            request.setValue("Bearer \(configuration.apiKey)", forHTTPHeaderField: "Authorization")
        }
        request.httpBody = try JSONEncoder().encode(
            RequestBody(model: configuration.model, input: inputs)
        )

        let data: Data
        let response: URLResponse
        do {
            (data, response) = try await URLSession.shared.data(for: request)
        } catch is CancellationError {
            throw CancellationError()
        } catch {
            throw EmbeddingError.network("无法连接 Embedding API：\(error.localizedDescription)")
        }
        guard let http = response as? HTTPURLResponse else {
            throw EmbeddingError.invalidResponse("Embedding API 没有返回 HTTP 响应")
        }
        guard (200..<300).contains(http.statusCode) else {
            let detail = (try? JSONDecoder().decode(ErrorBody.self, from: data).error.message)
                ?? String(data: data.prefix(300), encoding: .utf8)
                ?? "未知错误"
            throw EmbeddingError.server(status: http.statusCode, message: detail)
        }

        let decoded: ResponseBody
        do {
            decoded = try JSONDecoder().decode(ResponseBody.self, from: data)
        } catch {
            throw EmbeddingError.invalidResponse("Embedding API 返回格式不兼容 OpenAI embeddings 格式")
        }
        let ordered = decoded.data.sorted { $0.index < $1.index }
        guard ordered.count == inputs.count else {
            throw EmbeddingError.invalidResponse("Embedding API 返回的向量数量与文本数量不一致")
        }
        guard let dimensions = ordered.first?.embedding.count, dimensions > 0,
              ordered.allSatisfy({ item in
                  item.embedding.count == dimensions && item.embedding.allSatisfy(\.isFinite)
              }) else {
            throw EmbeddingError.invalidResponse("Embedding API 返回了空向量、无效数值或不同维度")
        }
        return ordered.map(\.embedding)
    }
}

enum TextChunker {
    static let maximumCharacters = 200_000

    static func split(_ source: String, length: Int, overlap: Int) throws -> [String] {
        let text = source.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !text.isEmpty else { throw EmbeddingError.invalidInput("请先粘贴需要导入的文本") }
        guard text.count <= maximumCharacters else {
            throw EmbeddingError.invalidInput("演示版单次最多导入 20 万个字符")
        }
        guard (100...2_000).contains(length) else {
            throw EmbeddingError.invalidInput("每个分片长度需要在 100 到 2000 个字符之间")
        }
        guard overlap >= 0, overlap < length else {
            throw EmbeddingError.invalidInput("重叠长度必须小于分片长度")
        }

        let lines = text
            .components(separatedBy: .newlines)
            .map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }
            .filter { !$0.isEmpty }
        let topHeadings = Set("一二三四五六七八九十百千万".map(String.init))
        var groups: [(key: String, lines: [String])] = []
        var currentKey = "intro"
        var currentLines: [String] = []
        for line in lines {
            if let first = line.first,
               topHeadings.contains(String(first)),
               line.dropFirst().first == "、" {
                if !currentLines.isEmpty { groups.append((currentKey, currentLines)) }
                currentKey = String(first)
                currentLines = [line]
            } else {
                currentLines.append(line)
            }
        }
        if !currentLines.isEmpty { groups.append((currentKey, currentLines)) }
        if groups.count > 1, groups[0].key == "intro" {
            let intro = groups.removeFirst()
            groups[0] = (groups[0].key, intro.lines + groups[0].lines)
        }

        var sections: [(key: String, text: String)] = []
        for group in groups {
            var block: [String] = []
            for line in group.lines {
                if !block.isEmpty, isSubheading(line) {
                    sections.append((group.key, block.joined(separator: "\n")))
                    block = []
                }
                block.append(line)
            }
            if !block.isEmpty { sections.append((group.key, block.joined(separator: "\n"))) }
        }

        var chunks: [String] = []
        var pending = ""
        var pendingKey = ""
        func flush() {
            let value = pending.trimmingCharacters(in: .whitespacesAndNewlines)
            if !value.isEmpty { chunks.append(value) }
            pending = ""
        }

        for section in sections {
            let value = section.text.trimmingCharacters(in: .whitespacesAndNewlines)
            let pieces = value.count > length ? splitSentences(value) : [value]
            for piece in pieces {
                if piece.count > length {
                    flush()
                    let characters = Array(piece)
                    var start = 0
                    while start < characters.count {
                        var end = min(start + length, characters.count)
                        if end < characters.count {
                            let searchStart = start + max(1, length / 2)
                            if let whitespace = characters[searchStart..<end].lastIndex(where: { $0 == " " || $0 == "\t" }) {
                                end = whitespace
                            }
                        }
                        let part = String(characters[start..<end]).trimmingCharacters(in: .whitespacesAndNewlines)
                        if !part.isEmpty { chunks.append(part) }
                        start = max(start + 1, end)
                    }
                    pendingKey = ""
                } else if !pending.isEmpty,
                          pendingKey == section.key,
                          pending.count + 1 + piece.count <= length {
                    pending += "\n" + piece
                } else {
                    let carry = pendingKey == section.key ? overlapTail(pending, limit: overlap) : ""
                    flush()
                    pending = !carry.isEmpty && carry.count + 1 + piece.count <= length
                        ? carry + "\n" + piece : piece
                    pendingKey = section.key
                }
            }
        }
        flush()
        guard !chunks.isEmpty else { throw EmbeddingError.invalidInput("文本分片后没有可导入内容") }
        return chunks
    }

    private static func isSubheading(_ line: String) -> Bool {
        line.range(of: #"^\d+(?:\.\d+)+\s"#, options: .regularExpression) != nil
    }

    private static func splitSentences(_ text: String) -> [String] {
        let characters = Array(text)
        var sentences: [String] = []
        var start = 0
        for index in characters.indices {
            let character = characters[index]
            guard character == "。" || character == "！" || character == "？" ||
                    character == "!" || character == "?" || character == "\n" else { continue }
            let value = String(characters[start...index]).trimmingCharacters(in: .whitespacesAndNewlines)
            if !value.isEmpty { sentences.append(value) }
            start = index + 1
        }
        if start < characters.count {
            let value = String(characters[start...]).trimmingCharacters(in: .whitespacesAndNewlines)
            if !value.isEmpty { sentences.append(value) }
        }
        return sentences
    }

    private static func overlapTail(_ text: String, limit: Int) -> String {
        guard limit > 0 else { return "" }
        let sentences = splitSentences(text)
        guard let tail = sentences.last, tail.count <= limit else { return "" }
        return tail
    }
}

@MainActor
final class EmbeddingSettingsModel: ObservableObject {
    @Published var endpoint: String
    @Published var model: String
    @Published var apiKey: String
    @Published var isTesting = false
    @Published var testMessage: String?
    @Published var testSucceeded = false

    private let defaults = UserDefaults.standard
    private let client = EmbeddingClient()
    private static let keychainService = "org.vexdb.lite.demo.embedding"

    init(arguments: [String] = ProcessInfo.processInfo.arguments) {
        endpoint = defaults.string(forKey: "embedding.endpoint") ??
            "https://dashscope.aliyuncs.com/compatible-mode/v1"
        model = defaults.string(forKey: "embedding.model") ?? "text-embedding-v4"
        apiKey = Self.loadAPIKey() ?? ""
        if let value = Self.argument("--embedding-endpoint", in: arguments) { endpoint = value }
        if let value = Self.argument("--embedding-model", in: arguments) { model = value }
        if let value = Self.argument("--embedding-key", in: arguments) { apiKey = value }
    }

    var isConfigured: Bool {
        guard !endpoint.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty,
              !model.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else { return false }
        if let host = URL(string: endpoint.trimmingCharacters(in: .whitespacesAndNewlines))?.host,
           EmbeddingConfiguration.requiresAPIKey(host: host) {
            return !apiKey.isEmpty
        }
        return true
    }

    func configuration() throws -> EmbeddingConfiguration {
        let value = EmbeddingConfiguration(
            endpoint: endpoint,
            apiKey: apiKey.trimmingCharacters(in: .whitespacesAndNewlines),
            model: model.trimmingCharacters(in: .whitespacesAndNewlines)
        )
        try value.validate()
        return value
    }

    func save() throws {
        _ = try configuration()
        defaults.set(endpoint.trimmingCharacters(in: .whitespacesAndNewlines), forKey: "embedding.endpoint")
        defaults.set(model.trimmingCharacters(in: .whitespacesAndNewlines), forKey: "embedding.model")
        try Self.saveAPIKey(apiKey.trimmingCharacters(in: .whitespacesAndNewlines))
    }

    func testConnection() async {
        guard !isTesting else { return }
        isTesting = true
        testMessage = nil
        testSucceeded = false
        do {
            try save()
            let vectors = try await client.embed(["你好"], configuration: configuration())
            testSucceeded = true
            testMessage = "“你好”已生成向量，维度为 \(vectors[0].count)"
        } catch {
            testMessage = error.localizedDescription
        }
        isTesting = false
    }

    private static func loadAPIKey() -> String? {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: keychainService,
            kSecAttrAccount as String: "api-key",
            kSecReturnData as String: true,
            kSecMatchLimit as String: kSecMatchLimitOne
        ]
        var result: CFTypeRef?
        guard SecItemCopyMatching(query as CFDictionary, &result) == errSecSuccess,
              let data = result as? Data else { return nil }
        return String(data: data, encoding: .utf8)
    }

    private static func argument(_ name: String, in arguments: [String]) -> String? {
        guard let index = arguments.firstIndex(of: name),
              arguments.indices.contains(index + 1) else { return nil }
        return arguments[index + 1]
    }

    private static func saveAPIKey(_ key: String) throws {
        let base: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: keychainService,
            kSecAttrAccount as String: "api-key"
        ]
        if key.isEmpty {
            SecItemDelete(base as CFDictionary)
            return
        }
        let values: [String: Any] = [kSecValueData as String: Data(key.utf8)]
        var status = SecItemUpdate(base as CFDictionary, values as CFDictionary)
        if status == errSecItemNotFound {
            var item = base
            item[kSecValueData as String] = Data(key.utf8)
            item[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
            status = SecItemAdd(item as CFDictionary, nil)
        }
        guard status == errSecSuccess else {
            throw EmbeddingError.configuration("API Key 无法写入系统 Keychain（\(status)）")
        }
    }
}
