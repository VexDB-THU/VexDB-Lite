import Foundation

private struct BundledImageSearchSmokePayload: Decodable {
    struct Image: Decodable {
        let resource: String
        let embedding: [Double]
    }

    struct Query: Decodable {
        let text: String
        let goldResource: String
        let embedding: [Double]

        enum CodingKeys: String, CodingKey {
            case text
            case goldResource = "gold_resource"
            case embedding
        }
    }

    let dimensions: Int
    let images: [Image]
    let queries: [Query]
}

private struct BundledImageSearchSmokeResult: Codable {
    struct QueryResult: Codable {
        let text: String
        let expected: String
        let actual: String
        let plan: String
        let passed: Bool
    }

    let passed: Bool
    let imageCount: Int
    let queryCount: Int
    let dimensions: Int
    let queries: [QueryResult]
    let error: String
}

enum BundledImageSearchSmoke {
    @MainActor private static var hasRun = false

    @MainActor
    static func runIfRequested(arguments: [String] = ProcessInfo.processInfo.arguments) async {
        guard arguments.contains("--image-query-smoke"), !hasRun else { return }
        hasRun = true

        let base = FileManager.default.urls(for: .applicationSupportDirectory,
                                            in: .userDomainMask).first!
        let resultURL = base.appendingPathComponent("bundled-image-search-smoke.json")
        try? FileManager.default.removeItem(at: resultURL)

        var imageCount = 0
        var queryCount = 0
        var dimensions = 0
        var queryResults: [BundledImageSearchSmokeResult.QueryResult] = []
        var errorMessage = ""

        do {
            guard let url = Bundle.main.url(forResource: "image-demo-vectors",
                                            withExtension: "json") else {
                throw EmbeddingError.invalidResponse("App 中缺少默认图片向量")
            }
            let payload = try JSONDecoder().decode(BundledImageSearchSmokePayload.self,
                                                   from: Data(contentsOf: url))
            imageCount = payload.images.count
            queryCount = payload.queries.count
            dimensions = payload.dimensions
            guard imageCount == 20, queryCount == 3, dimensions == 1_152,
                  payload.images.allSatisfy({ $0.embedding.count == dimensions }),
                  payload.queries.allSatisfy({ $0.embedding.count == dimensions }) else {
                throw EmbeddingError.invalidResponse("默认图片测试数据不完整")
            }

            let databaseName = "vexdb-image-query-smoke-\(UUID().uuidString).sqlite"
            let store = ImageVectorStore(scope: .bundled, databaseName: databaseName)
            let build = await store.importImages(
                labels: payload.images.map(\.resource),
                embeddings: payload.images.map(\.embedding)
            )
            guard build.success, build.rowCount == imageCount else {
                throw EmbeddingError.invalidResponse(build.message)
            }

            for query in payload.queries {
                let search = await store.search(embedding: query.embedding, limit: 5)
                let actual = search.results.first?.title ?? ""
                let passed = search.success && actual == query.goldResource &&
                    search.queryPlan.contains("bundled_media_vectors")
                queryResults.append(.init(
                    text: query.text,
                    expected: query.goldResource,
                    actual: actual,
                    plan: search.queryPlan,
                    passed: passed
                ))
            }
        } catch {
            errorMessage = error.localizedDescription
        }

        let result = BundledImageSearchSmokeResult(
            passed: imageCount == 20 && queryCount == 3 && queryResults.count == queryCount &&
                queryResults.allSatisfy(\.passed) && errorMessage.isEmpty,
            imageCount: imageCount,
            queryCount: queryCount,
            dimensions: dimensions,
            queries: queryResults,
            error: errorMessage
        )
        if let data = try? JSONEncoder().encode(result) {
            try? data.write(to: resultURL, options: .atomic)
        }
    }
}
