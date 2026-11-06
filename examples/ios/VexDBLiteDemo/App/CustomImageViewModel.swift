import Foundation
import PhotosUI
import SwiftUI
import UIKit

struct PendingCustomImage: Identifiable, Sendable {
    let id = UUID()
    let data: Data
}

struct CustomImageRecord: Codable, Identifiable, Sendable {
    let id: String
    let fileName: String
    let embedding: [Double]
}

private struct CustomImageManifest: Codable, Sendable {
    let model: String
    let dimensions: Int
    let records: [CustomImageRecord]
}

struct CustomImageLibraryState: Sendable {
    let records: [CustomImageRecord]
    let model: String
    let snapshot: DemoSnapshot
}

actor CustomImageLibrary {
    private let store = ImageVectorStore(scope: .user)
    private let directory: URL
    private let manifestURL: URL

    init() {
        let base = FileManager.default.urls(for: .applicationSupportDirectory,
                                            in: .userDomainMask).first!
        directory = base.appendingPathComponent("user-images", isDirectory: true)
        manifestURL = base.appendingPathComponent("vexdb-user-images.json")
    }

    static func imageURL(fileName: String) -> URL {
        let base = FileManager.default.urls(for: .applicationSupportDirectory,
                                            in: .userDomainMask).first!
        return base.appendingPathComponent("user-images", isDirectory: true)
            .appendingPathComponent(fileName)
    }

    func load() async -> CustomImageLibraryState {
        try? FileManager.default.createDirectory(at: directory,
                                                 withIntermediateDirectories: true)
        guard let manifest = readManifest(), !manifest.records.isEmpty else {
            let snapshot = await store.clear()
            return CustomImageLibraryState(records: [], model: "", snapshot: snapshot)
        }

        let records = manifest.records
        guard !records.isEmpty else {
            let snapshot = await store.clear()
            return CustomImageLibraryState(records: [], model: "", snapshot: snapshot)
        }

        let snapshot = await store.importImages(labels: records.map(\.id),
                                                embeddings: records.map(\.embedding))
        return CustomImageLibraryState(records: records,
                                       model: manifest.model,
                                       snapshot: snapshot)
    }

    func add(images: [PendingCustomImage], embeddings: [[Double]], model: String) async throws
        -> CustomImageLibraryState {
        guard !images.isEmpty, images.count == embeddings.count else {
            throw EmbeddingError.invalidInput("选择的图片与生成的向量数量不一致")
        }
        let dimensions = embeddings[0].count
        guard dimensions > 0,
              embeddings.allSatisfy({ $0.count == dimensions && $0.allSatisfy(\.isFinite) }) else {
            throw EmbeddingError.invalidResponse("图片向量维度不一致")
        }

        let existing = readManifest()
        if let existing, !existing.records.isEmpty {
            guard existing.model == model else {
                throw EmbeddingError.configuration(
                    "现有图片库使用 \(existing.model)。更换模型前请先清空图片库。"
                )
            }
            guard existing.dimensions == dimensions else {
                throw EmbeddingError.invalidResponse(
                    "新向量为 \(dimensions) 维，现有索引为 \(existing.dimensions) 维。请先清空图片库。"
                )
            }
        }

        let oldRecords = existing?.records ?? []
        guard oldRecords.count + images.count <= 50 else {
            throw EmbeddingError.invalidInput("自定义图片库最多保存 50 张图片")
        }

        try FileManager.default.createDirectory(at: directory,
                                                withIntermediateDirectories: true)
        var newRecords: [CustomImageRecord] = []
        var createdURLs: [URL] = []
        do {
            for (image, embedding) in zip(images, embeddings) {
                let identifier = UUID().uuidString.lowercased()
                let fileName = identifier + (image.data.starts(with: [0x89, 0x50, 0x4e, 0x47])
                                             ? ".png" : ".jpg")
                let url = directory.appendingPathComponent(fileName)
                try image.data.write(to: url, options: .atomic)
                createdURLs.append(url)
                newRecords.append(CustomImageRecord(id: identifier,
                                                    fileName: fileName,
                                                    embedding: embedding))
            }

            let records = oldRecords + newRecords
            let snapshot = await store.importImages(labels: records.map(\.id),
                                                    embeddings: records.map(\.embedding))
            guard snapshot.success else {
                throw EmbeddingError.invalidResponse(snapshot.message)
            }

            let manifest = CustomImageManifest(model: model,
                                               dimensions: dimensions,
                                               records: records)
            do {
                try writeManifest(manifest)
            } catch {
                if oldRecords.isEmpty {
                    _ = await store.clear()
                } else {
                    _ = await store.importImages(labels: oldRecords.map(\.id),
                                                 embeddings: oldRecords.map(\.embedding))
                }
                throw error
            }
            return CustomImageLibraryState(records: records, model: model, snapshot: snapshot)
        } catch {
            for url in createdURLs { try? FileManager.default.removeItem(at: url) }
            throw error
        }
    }

    func search(embedding: [Double], limit: Int) async -> DemoSnapshot {
        await store.search(embedding: embedding, limit: limit)
    }

    func clear() async -> DemoSnapshot {
        let snapshot = await store.clear()
        if FileManager.default.fileExists(atPath: directory.path) {
            try? FileManager.default.removeItem(at: directory)
        }
        try? FileManager.default.removeItem(at: manifestURL)
        return snapshot
    }

    private func readManifest() -> CustomImageManifest? {
        guard let data = try? Data(contentsOf: manifestURL),
              let manifest = try? JSONDecoder().decode(CustomImageManifest.self, from: data),
              manifest.dimensions > 0 else { return nil }
        let records = manifest.records.filter {
            !$0.embedding.isEmpty &&
            $0.embedding.count == manifest.dimensions &&
            FileManager.default.fileExists(atPath: directory
                .appendingPathComponent($0.fileName).path)
        }
        return CustomImageManifest(model: manifest.model,
                                   dimensions: manifest.dimensions,
                                   records: records)
    }

    private func writeManifest(_ manifest: CustomImageManifest) throws {
        let data = try JSONEncoder().encode(manifest)
        try data.write(to: manifestURL, options: .atomic)
    }
}

@MainActor
final class CustomImageViewModel: ObservableObject {
    @Published var pendingImages: [PendingCustomImage] = []
    @Published var records: [CustomImageRecord] = []
    @Published var queryText = ""
    @Published var results: [SearchHit] = []
    @Published var status = "正在读取本机图片库…"
    @Published var queryPlan = "生成图片向量并查询后显示"
    @Published var queryMilliseconds = 0.0
    @Published var buildMilliseconds = 0.0
    @Published var databaseBytes: Int64 = 0
    @Published var progress = 0.0
    @Published var isBusy = false
    @Published var errorMessage: String?
    private(set) var indexedModel = ""

    private let library = CustomImageLibrary()
    private let client = ImageEmbeddingClient()

    var hasIndex: Bool { !records.isEmpty }
    var dimensions: Int { records.first?.embedding.count ?? 0 }
    var databaseSize: String {
        ByteCountFormatter.string(fromByteCount: databaseBytes, countStyle: .file)
    }

    func load() async {
        guard !isBusy else { return }
        isBusy = true
        let state = await library.load()
        apply(state)
        status = records.isEmpty ? "请选择多张图片并生成向量" : "已加载 \(records.count) 张本机图片"
        isBusy = false
    }

    func loadSelection(_ items: [PhotosPickerItem]) async {
        guard !isBusy, !items.isEmpty else { return }
        isBusy = true
        errorMessage = nil
        progress = 0
        var loaded: [PendingCustomImage] = []
        for (index, item) in items.prefix(12).enumerated() {
            do {
                guard let data = try await item.loadTransferable(type: Data.self),
                      !data.isEmpty, data.count <= 20 * 1024 * 1024,
                      let normalized = normalizedImageData(data),
                      normalized.count <= 12 * 1024 * 1024 else {
                    throw EmbeddingError.invalidInput("第 \(index + 1) 张图片无法读取或超过 12 MB")
                }
                loaded.append(PendingCustomImage(data: normalized))
                progress = Double(index + 1) / Double(min(items.count, 12))
            } catch {
                errorMessage = error.localizedDescription
                status = "图片读取失败"
                isBusy = false
                return
            }
        }
        pendingImages = loaded
        results = []
        queryPlan = "生成图片向量并查询后显示"
        status = "已选择 \(loaded.count) 张图片，可以生成向量"
        progress = 0
        isBusy = false
    }

    func addSelected(configuration: ImageEmbeddingConfiguration) async {
        guard !isBusy, !pendingImages.isEmpty else { return }
        isBusy = true
        errorMessage = nil
        progress = 0
        var embeddings: [[Double]] = []
        do {
            for (index, image) in pendingImages.enumerated() {
                status = "正在生成图片向量：\(index + 1) / \(pendingImages.count)"
                embeddings.append(try await client.embed(imageData: image.data,
                                                         configuration: configuration))
                progress = Double(index + 1) / Double(pendingImages.count) * 0.82
            }
            status = "正在重建用户图片图索引…"
            let state = try await library.add(images: pendingImages,
                                              embeddings: embeddings,
                                              model: configuration.model)
            apply(state)
            pendingImages = []
            results = []
            queryPlan = "输入文字并查询后显示"
            progress = 1
            status = "\(records.count) 张图片已写入用户图片图索引"
        } catch {
            errorMessage = error.localizedDescription
            status = "生成失败，原有图片索引未改变"
            progress = 0
        }
        isBusy = false
    }

    func search(configuration: ImageEmbeddingConfiguration) async {
        guard !isBusy else { return }
        let query = queryText.trimmingCharacters(in: .whitespacesAndNewlines)
        guard hasIndex else {
            errorMessage = "请先添加图片并生成用户图片索引"
            return
        }
        guard !query.isEmpty else {
            errorMessage = "请输入要查找的图片内容"
            return
        }
        guard indexedModel == configuration.model else {
            errorMessage = "当前图片库使用 \(indexedModel)。请切回该模型，或清空图片库后重新生成。"
            return
        }

        isBusy = true
        errorMessage = nil
        progress = 0
        status = "正在为文字生成查询向量…"
        do {
            let embedding = try await client.embed(text: query, configuration: configuration)
            guard embedding.count == dimensions else {
                throw EmbeddingError.invalidResponse(
                    "查询向量为 \(embedding.count) 维，图片索引为 \(dimensions) 维"
                )
            }
            progress = 0.6
            status = "查询向量已生成，正在本机搜索图片…"
            let snapshot = await library.search(embedding: embedding,
                                                limit: min(5, records.count))
            apply(snapshot)
            progress = snapshot.success ? 1 : 0
            status = snapshot.success
                ? "已在全部 \(records.count) 张用户图片中完成本机检索"
                : snapshot.message
        } catch {
            errorMessage = error.localizedDescription
            status = "图片查询失败"
            progress = 0
        }
        isBusy = false
    }

    func clear() async {
        guard !isBusy else { return }
        isBusy = true
        let snapshot = await library.clear()
        records = []
        pendingImages = []
        results = []
        queryText = ""
        indexedModel = ""
        queryPlan = "生成图片向量并查询后显示"
        queryMilliseconds = 0
        buildMilliseconds = 0
        databaseBytes = snapshot.databaseBytes
        status = snapshot.success ? "用户图片库已清空" : snapshot.message
        if !snapshot.success { errorMessage = snapshot.message }
        isBusy = false
    }

    func record(for hit: SearchHit) -> CustomImageRecord? {
        records.first(where: { $0.id == hit.title })
    }

    func imageURL(for record: CustomImageRecord) -> URL {
        CustomImageLibrary.imageURL(fileName: record.fileName)
    }

    private func apply(_ state: CustomImageLibraryState) {
        records = state.records
        indexedModel = state.model
        apply(state.snapshot)
    }

    private func apply(_ snapshot: DemoSnapshot) {
        databaseBytes = snapshot.databaseBytes
        if snapshot.buildMilliseconds > 0 { buildMilliseconds = snapshot.buildMilliseconds }
        queryMilliseconds = snapshot.queryMilliseconds
        if !snapshot.queryPlan.isEmpty { queryPlan = snapshot.queryPlan }
        results = snapshot.results
        if !snapshot.success { errorMessage = snapshot.message }
    }

    private func normalizedImageData(_ data: Data) -> Data? {
        guard let image = UIImage(data: data) else { return nil }
        let maxEdge: CGFloat = 1_600
        let longest = max(image.size.width, image.size.height)
        let scale = longest > maxEdge ? maxEdge / longest : 1
        let size = CGSize(width: max(1, floor(image.size.width * scale)),
                          height: max(1, floor(image.size.height * scale)))
        let format = UIGraphicsImageRendererFormat()
        format.scale = 1
        format.opaque = true
        let rendered = UIGraphicsImageRenderer(size: size, format: format).image { context in
            UIColor.white.setFill()
            context.fill(CGRect(origin: .zero, size: size))
            image.draw(in: CGRect(origin: .zero, size: size))
        }
        return rendered.jpegData(compressionQuality: 0.88)
    }
}
