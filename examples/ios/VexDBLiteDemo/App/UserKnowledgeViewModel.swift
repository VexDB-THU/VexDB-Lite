import Foundation

actor UserKnowledgeStore {
    static let maximumVectorValues = 2_500_000

    private let bridge: VexDBBridge

    init(databaseName: String = "vexdb-user.sqlite") {
        let base = FileManager.default.urls(for: .applicationSupportDirectory,
                                            in: .userDomainMask).first!
        try? FileManager.default.createDirectory(at: base, withIntermediateDirectories: true)
        bridge = VexDBBridge(databasePath: base.appendingPathComponent(databaseName).path)
    }

    func status() -> DemoSnapshot { convert(bridge.userIndexStatus()) }

    static func validateVectorCount(chunks: Int, dimensions: Int) throws {
        let (total, overflow) = chunks.multipliedReportingOverflow(by: dimensions)
        guard chunks > 0, dimensions > 0, !overflow, total <= maximumVectorValues else {
            throw EmbeddingError.invalidInput(
                "分片数量与向量维度过大，请增加分片长度或减少导入文本"
            )
        }
    }

    func importText(chunks: [String], embeddings: [[Double]]) throws -> DemoSnapshot {
        guard let dimensions = embeddings.first?.count,
              chunks.count == embeddings.count,
              embeddings.allSatisfy({ vector in
                  vector.count == dimensions && vector.allSatisfy { $0.isFinite && Float($0).isFinite }
              }) else {
            throw EmbeddingError.invalidResponse("Embedding API 返回了无效数值或不同维度的向量")
        }
        try Self.validateVectorCount(chunks: chunks.count, dimensions: dimensions)

        var packed = Data(count: chunks.count * dimensions * MemoryLayout<Float>.stride)
        packed.withUnsafeMutableBytes { rawBuffer in
            let values = rawBuffer.bindMemory(to: Float.self)
            var offset = 0
            for embedding in embeddings {
                for value in embedding {
                    values[offset] = Float(value)
                    offset += 1
                }
            }
        }
        return convert(bridge.importUser(chunks: chunks,
                                         embeddingData: packed,
                                         dimensions: dimensions))
    }

    func search(embedding: [Double], limit: Int) -> DemoSnapshot {
        let values = embedding.map { NSNumber(value: $0) }
        return convert(bridge.searchUser(embedding: values, limit: limit))
    }

    private func convert(_ value: VexDemoSnapshot) -> DemoSnapshot {
        DemoSnapshot(
            success: value.success,
            message: value.message,
            mode: value.mode,
            version: value.version,
            queryPlan: value.queryPlan,
            rowCount: value.rowCount,
            databaseBytes: value.databaseBytes,
            buildMilliseconds: value.buildMilliseconds,
            queryMilliseconds: value.queryMilliseconds,
            results: value.results.map {
                SearchHit(id: $0.rowID, title: $0.title,
                          category: $0.category, distance: $0.distance)
            }
        )
    }
}

@MainActor
final class UserKnowledgeViewModel: ObservableObject {
    @Published var sourceText = ""
    @Published var queryText = ""
    @Published var chunkLength = 500
    @Published var overlapLength = 50
    @Published var rowCount = 0
    @Published var databaseBytes: Int64 = 0
    @Published var buildMilliseconds = 0.0
    @Published var queryMilliseconds = 0.0
    @Published var results: [SearchHit] = []
    @Published var queryPlan = "导入并查询后显示"
    @Published var status = "尚未导入用户文本"
    @Published var isBusy = false
    @Published var progress = 0.0
    @Published var errorMessage: String?

    private let store = UserKnowledgeStore()
    private let client = EmbeddingClient()

    var previewChunkCount: Int {
        previewChunks.count
    }

    var previewChunks: [String] {
        (try? TextChunker.split(sourceText, length: chunkLength, overlap: overlapLength)) ?? []
    }

    var hasIndex: Bool { rowCount > 0 }

    var databaseSize: String {
        ByteCountFormatter.string(fromByteCount: databaseBytes, countStyle: .file)
    }

    func loadStatus() async {
        guard !isBusy else { return }
        apply(await store.status())
    }

    func importText(configuration: EmbeddingConfiguration) async {
        guard !isBusy else { return }
        isBusy = true
        progress = 0
        errorMessage = nil
        do {
            let chunks = try TextChunker.split(sourceText, length: chunkLength,
                                               overlap: overlapLength)
            status = "已分成 \(chunks.count) 段，正在请求 Embedding API…"
            var embeddings: [[Double]] = []
            embeddings.reserveCapacity(chunks.count)
            var expectedDimensions: Int?
            // 默认 OpenAI 兼容接口每批最多接收 10 条。
            let batchSize = 10
            for start in stride(from: 0, to: chunks.count, by: batchSize) {
                try Task.checkCancellation()
                let end = min(start + batchSize, chunks.count)
                let batch = Array(chunks[start..<end])
                let values = try await client.embed(batch, configuration: configuration)
                guard let dimensions = values.first?.count else {
                    throw EmbeddingError.invalidResponse("Embedding API 返回了空向量")
                }
                if let expectedDimensions, expectedDimensions != dimensions {
                    throw EmbeddingError.invalidResponse("Embedding API 返回了不同维度的向量")
                }
                try UserKnowledgeStore.validateVectorCount(chunks: chunks.count,
                                                           dimensions: dimensions)
                expectedDimensions = dimensions
                embeddings.append(contentsOf: values)
                progress = Double(end) / Double(chunks.count) * 0.85
                status = "正在生成向量：\(end) / \(chunks.count)"
            }
            status = "向量生成完成，正在写入本地 VexDB…"
            let snapshot = try await store.importText(chunks: chunks, embeddings: embeddings)
            apply(snapshot)
            if snapshot.success {
                progress = 1
                queryText = ""
                results = []
                queryPlan = "输入问题并查询后显示"
            }
        } catch is CancellationError {
            errorMessage = "导入已取消"
            status = "导入已取消，原有索引未改变"
        } catch {
            errorMessage = error.localizedDescription
            status = "导入失败，原有索引未改变"
        }
        isBusy = false
    }

    func search(configuration: EmbeddingConfiguration) async {
        guard !isBusy else { return }
        let query = queryText.trimmingCharacters(in: .whitespacesAndNewlines)
        guard hasIndex else {
            errorMessage = "请先导入自己的文本并建立索引"
            return
        }
        guard !query.isEmpty else {
            errorMessage = "请输入一个完整问题"
            return
        }
        guard query.count <= 2_000 else {
            errorMessage = "查询内容不能超过 2000 个字符"
            return
        }

        isBusy = true
        progress = 0
        errorMessage = nil
        status = "正在为问题生成查询向量…"
        do {
            let vectors = try await client.embed([query], configuration: configuration)
            progress = 0.55
            status = "查询向量已生成，正在本机检索…"
            let snapshot = await store.search(embedding: vectors[0], limit: 5)
            apply(snapshot)
            progress = snapshot.success ? 1 : 0
        } catch is CancellationError {
            status = "查询已取消"
        } catch {
            errorMessage = error.localizedDescription
            status = "查询失败"
        }
        isBusy = false
    }

    private func apply(_ snapshot: DemoSnapshot) {
        status = snapshot.message
        rowCount = snapshot.rowCount
        databaseBytes = snapshot.databaseBytes
        if snapshot.buildMilliseconds > 0 { buildMilliseconds = snapshot.buildMilliseconds }
        queryMilliseconds = snapshot.queryMilliseconds
        if !snapshot.queryPlan.isEmpty { queryPlan = snapshot.queryPlan }
        results = snapshot.results
        if !snapshot.success { errorMessage = snapshot.message }
    }
}
