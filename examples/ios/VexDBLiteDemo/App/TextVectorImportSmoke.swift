import Foundation

private struct TextVectorImportSmokeResult: Codable {
    let passed: Bool
    let rowCount: Int
    let firstTitle: String
    let queryPlan: String
    let oversizedInputRejected: Bool
    let error: String
}

enum TextVectorImportSmoke {
    @MainActor private static var hasRun = false

    @MainActor
    static func runIfRequested(arguments: [String] = ProcessInfo.processInfo.arguments) async {
        guard arguments.contains("--text-vector-import-smoke"), !hasRun else { return }
        hasRun = true

        let base = FileManager.default.urls(for: .applicationSupportDirectory,
                                            in: .userDomainMask).first!
        let resultURL = base.appendingPathComponent("text-vector-import-smoke.json")
        try? FileManager.default.removeItem(at: resultURL)

        let store = UserKnowledgeStore(
            databaseName: "text-vector-import-smoke-\(UUID().uuidString).sqlite"
        )
        var rowCount = 0
        var firstTitle = ""
        var queryPlan = ""
        var errorMessage = ""
        var oversizedInputRejected = false

        do {
            let build = try await store.importText(
                chunks: ["第一段", "第二段", "第三段"],
                embeddings: [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0]]
            )
            let search = await store.search(embedding: [1, 0, 0, 0], limit: 2)
            rowCount = build.rowCount
            firstTitle = search.results.first?.title ?? ""
            queryPlan = search.queryPlan
        } catch {
            errorMessage = error.localizedDescription
        }

        do {
            try UserKnowledgeStore.validateVectorCount(chunks: 5_000, dimensions: 8_192)
        } catch {
            oversizedInputRejected = true
        }

        let passed = rowCount == 3 && firstTitle == "文本片段 1" &&
            queryPlan.contains("user_vectors") &&
            queryPlan.contains("VIRTUAL TABLE INDEX 1") && oversizedInputRejected
        let result = TextVectorImportSmokeResult(
            passed: passed,
            rowCount: rowCount,
            firstTitle: firstTitle,
            queryPlan: queryPlan,
            oversizedInputRejected: oversizedInputRejected,
            error: errorMessage
        )
        if let data = try? JSONEncoder().encode(result) {
            try? data.write(to: resultURL, options: .atomic)
        }
    }
}
