import Foundation
import SwiftUI
import UIKit
import PhotosUI
import Security

struct ImageEmbeddingConfiguration: Sendable {
    let endpoint: String
    let apiKey: String
    let model: String

    func embeddingsURL() throws -> URL {
        guard var url = URL(string: endpoint.trimmingCharacters(in: .whitespacesAndNewlines)),
              let scheme = url.scheme?.lowercased(), (scheme == "https" || scheme == "http"), url.host != nil else {
            throw EmbeddingError.configuration("图片 API 地址需要是完整的 http 或 https URL")
        }
        if !url.path.hasSuffix("multimodal-embedding") { url.appendPathComponent("multimodal-embedding") }
        if scheme == "http", url.host != "127.0.0.1", url.host != "localhost" { throw EmbeddingError.configuration("非本机图片 API 必须使用 HTTPS") }
        return url
    }

    func validate() throws {
        _ = try embeddingsURL()
        guard !model.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else { throw EmbeddingError.configuration("请填写图片 Embedding 模型名称") }
        if EmbeddingConfiguration.requiresAPIKey(host: URL(string: endpoint)?.host), apiKey.isEmpty { throw EmbeddingError.configuration("该图片 API 需要填写 API Key") }
    }
}

actor ImageEmbeddingClient {
    private struct Body: Encodable {
        struct Input: Encodable { let contents: [[String: String]] }
        let model: String
        let input: Input
    }
    private struct Response: Decodable {
        struct Item: Decodable { let index: Int; let embedding: [Double] }
        struct Output: Decodable { let embeddings: [Item] }
        let output: Output
    }
    func embed(imageData: Data, configuration: ImageEmbeddingConfiguration) async throws -> [Double] {
        try configuration.validate()
        guard !imageData.isEmpty, imageData.count <= 12 * 1024 * 1024 else { throw EmbeddingError.invalidInput("图片不能为空，且不能超过 12 MB") }
        let mime = imageData.starts(with: [0x89, 0x50, 0x4e, 0x47]) ? "image/png" : "image/jpeg"
        let input = "data:\(mime);base64,\(imageData.base64EncodedString())"
        return try await embed(contents: [["image": input]], configuration: configuration)
    }
    func embed(text: String, configuration: ImageEmbeddingConfiguration) async throws -> [Double] {
        try configuration.validate()
        let value = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !value.isEmpty, value.count <= 2_000 else {
            throw EmbeddingError.invalidInput("查询文字不能为空，且不能超过 2000 个字符")
        }
        return try await embed(contents: [["text": value]], configuration: configuration)
    }
    private func embed(contents: [[String: String]],
                       configuration: ImageEmbeddingConfiguration) async throws -> [Double] {
        var request = URLRequest(url: try configuration.embeddingsURL()); request.httpMethod = "POST"; request.timeoutInterval = 90
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        if !configuration.apiKey.isEmpty { request.setValue("Bearer \(configuration.apiKey)", forHTTPHeaderField: "Authorization") }
        request.httpBody = try JSONEncoder().encode(Body(model: configuration.model, input: .init(contents: contents)))
        let (data, response): (Data, URLResponse)
        do { (data, response) = try await URLSession.shared.data(for: request) } catch { throw EmbeddingError.network("无法连接图片 Embedding API：\(error.localizedDescription)") }
        guard let http = response as? HTTPURLResponse else { throw EmbeddingError.invalidResponse("图片 API 没有返回 HTTP 响应") }
        guard (200..<300).contains(http.statusCode) else { throw EmbeddingError.server(status: http.statusCode, message: String(data: data.prefix(300), encoding: .utf8) ?? "未知错误") }
        guard let item = try? JSONDecoder().decode(Response.self, from: data).output.embeddings.first,
              !item.embedding.isEmpty, item.embedding.allSatisfy(\.isFinite) else { throw EmbeddingError.invalidResponse("图片 API 返回格式不兼容 OpenAI embeddings 格式") }
        return item.embedding
    }
}

@MainActor
final class ImageEmbeddingSettingsModel: ObservableObject {
    @Published var endpoint: String
    @Published var model: String
    @Published var apiKey: String
    @Published var isTesting = false
    @Published var message: String?
    @Published var succeeded = false
    private let defaults = UserDefaults.standard
    private let client = ImageEmbeddingClient()
    private static let service = "org.vexdb.lite.demo.image-embedding"

    init() {
        endpoint = defaults.string(forKey: "image.embedding.endpoint") ?? "https://dashscope.aliyuncs.com/api/v1/services/embeddings/multimodal-embedding/multimodal-embedding"
        model = defaults.string(forKey: "image.embedding.model") ?? "tongyi-embedding-vision-plus"
        apiKey = Self.loadKey() ?? ""
    }
    var isConfigured: Bool { !endpoint.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty && !model.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty && (!EmbeddingConfiguration.requiresAPIKey(host: URL(string: endpoint)?.host) || !apiKey.isEmpty) }
    func configuration() throws -> ImageEmbeddingConfiguration { let value = ImageEmbeddingConfiguration(endpoint: endpoint, apiKey: apiKey.trimmingCharacters(in: .whitespacesAndNewlines), model: model.trimmingCharacters(in: .whitespacesAndNewlines)); try value.validate(); return value }
    func save() throws { _ = try configuration(); defaults.set(endpoint.trimmingCharacters(in: .whitespacesAndNewlines), forKey: "image.embedding.endpoint"); defaults.set(model.trimmingCharacters(in: .whitespacesAndNewlines), forKey: "image.embedding.model"); try Self.saveKey(apiKey.trimmingCharacters(in: .whitespacesAndNewlines)) }
    func test(imageData: Data) async { guard !isTesting else { return }; isTesting = true; message = nil; succeeded = false; do { try save(); let vector = try await client.embed(imageData: imageData, configuration: configuration()); succeeded = true; message = "图片已生成向量，维度为 \(vector.count)" } catch { message = error.localizedDescription }; isTesting = false }
    func embed(_ data: Data) async throws -> [Double] { try await client.embed(imageData: data, configuration: configuration()) }
    func embed(_ text: String) async throws -> [Double] { try await client.embed(text: text, configuration: configuration()) }
    private static func loadKey() -> String? { var result: CFTypeRef?; let q: [String: Any] = [kSecClass as String:kSecClassGenericPassword, kSecAttrService as String:service, kSecAttrAccount as String:"api-key", kSecReturnData as String:true, kSecMatchLimit as String:kSecMatchLimitOne]; guard SecItemCopyMatching(q as CFDictionary, &result) == errSecSuccess, let data = result as? Data else { return nil }; return String(data: data, encoding: .utf8) }
    private static func saveKey(_ key: String) throws { let base: [String: Any] = [kSecClass as String:kSecClassGenericPassword, kSecAttrService as String:service, kSecAttrAccount as String:"api-key"]; if key.isEmpty { SecItemDelete(base as CFDictionary); return }; var item = base; item[kSecValueData as String] = Data(key.utf8); item[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly; SecItemDelete(base as CFDictionary); guard SecItemAdd(item as CFDictionary, nil) == errSecSuccess else { throw EmbeddingError.configuration("API Key 无法写入系统 Keychain") } }
}

enum DemoImageFactory {
    static func sampleData() -> Data {
        if let url = Bundle.main.url(forResource: "image-test-img01", withExtension: "jpg"),
           let data = try? Data(contentsOf: url) { return data }
        let renderer = UIGraphicsImageRenderer(size: CGSize(width: 800, height: 520))
        return renderer.pngData { context in
            let rect = CGRect(x: 0, y: 0, width: 800, height: 520)
            UIColor(red: 0.05, green: 0.12, blue: 0.20, alpha: 1).setFill(); context.fill(rect)
            UIColor(red: 0.20, green: 0.85, blue: 0.80, alpha: 1).setFill(); context.fill(CGRect(x: 0, y: 360, width: 800, height: 160))
            UIColor(red: 0.32, green: 0.54, blue: 0.70, alpha: 1).setFill(); context.fill(CGRect(x: 140, y: 170, width: 520, height: 190))
            UIColor.white.withAlphaComponent(0.9).setFill(); context.fill(CGRect(x: 210, y: 110, width: 380, height: 60))
        }
    }
}

enum ImageIndexScope: String, Sendable {
    case bundled
    case user
}

actor ImageVectorStore {
    private let bridge: VexDBBridge
    private let scope: ImageIndexScope
    init(scope: ImageIndexScope, databaseName: String = "vexdb.sqlite") {
        self.scope = scope
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first!
        try? FileManager.default.createDirectory(at: base, withIntermediateDirectories: true)
        bridge = VexDBBridge(databasePath: base.appendingPathComponent(databaseName).path)
    }
    func status() -> DemoSnapshot {
        convert(bridge.mediaIndexStatus(scope: scope.rawValue))
    }
    func clear() -> DemoSnapshot {
        convert(bridge.clearMedia(scope: scope.rawValue))
    }
    func importImage(label: String, embedding: [Double]) -> DemoSnapshot {
        importImages(labels: [label], embeddings: [embedding])
    }
    func importImages(labels: [String], embeddings: [[Double]]) -> DemoSnapshot {
        let values = embeddings.map { $0.map { NSNumber(value: $0) } }
        let snapshot = bridge.importMedia(scope: scope.rawValue, labels: labels, embeddings: values)
        return convert(snapshot)
    }
    func search(embedding: [Double], limit: Int) -> DemoSnapshot {
        let values = embedding.map { NSNumber(value: $0) }
        return convert(bridge.searchMedia(scope: scope.rawValue, embedding: values, limit: limit))
    }
    private func convert(_ snapshot: VexDemoSnapshot) -> DemoSnapshot {
        return DemoSnapshot(success: snapshot.success, message: snapshot.message, mode: snapshot.mode,
                            version: snapshot.version, queryPlan: snapshot.queryPlan,
                            rowCount: snapshot.rowCount, databaseBytes: snapshot.databaseBytes,
                            buildMilliseconds: snapshot.buildMilliseconds,
                            queryMilliseconds: snapshot.queryMilliseconds,
                            results: snapshot.results.map { SearchHit(id: $0.rowID, title: $0.title, category: $0.category, distance: $0.distance) })
    }
}
