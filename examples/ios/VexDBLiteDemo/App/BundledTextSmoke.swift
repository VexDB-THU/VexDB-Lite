import Foundation

private struct BundledTextSmokeResult: Codable {
    let passed: Bool
    let rowCount: Int
    let queryCount: Int
    let top1Matches: Int
    let indexedPlans: Int
    let error: String
}

enum BundledTextSmoke {
    @MainActor private static var hasRun = false

    @MainActor
    static func runIfRequested(arguments: [String] = ProcessInfo.processInfo.arguments) async {
        guard arguments.contains("--bundled-text-smoke"), !hasRun else { return }
        hasRun = true

        let base = FileManager.default.urls(for: .applicationSupportDirectory,
                                            in: .userDomainMask).first!
        let resultURL = base.appendingPathComponent("bundled-text-smoke.json")
        try? FileManager.default.removeItem(at: resultURL)

        var rowCount = 0
        var expectedRowCount = 0
        var queryCount = 0
        var top1Matches = 0
        var indexedPlans = 0
        var errorMessage = ""

        do {
            let example = try BundledExample.load()
            expectedRowCount = example.chunks.count
            queryCount = example.queries.count
            let store = UserKnowledgeStore(
                databaseName: "bundled-text-smoke-\(UUID().uuidString).sqlite"
            )
            let build = try await store.importText(
                chunks: example.chunks.map(\.text),
                embeddings: example.chunks.map(\.embedding)
            )
            rowCount = build.rowCount

            for query in example.queries {
                let expected = example.chunks.min { left, right in
                    squaredL2(query.embedding, left.embedding) <
                        squaredL2(query.embedding, right.embedding)
                }?.id
                let search = await store.search(embedding: query.embedding, limit: 5)
                if search.results.first?.id == expected { top1Matches += 1 }
                if search.queryPlan.contains("user_vectors") &&
                    search.queryPlan.contains("VIRTUAL TABLE INDEX 1") {
                    indexedPlans += 1
                }
            }
        } catch {
            errorMessage = error.localizedDescription
        }

        let result = BundledTextSmokeResult(
            passed: rowCount == expectedRowCount && queryCount == 7 &&
                top1Matches == queryCount && indexedPlans == queryCount &&
                errorMessage.isEmpty,
            rowCount: rowCount,
            queryCount: queryCount,
            top1Matches: top1Matches,
            indexedPlans: indexedPlans,
            error: errorMessage
        )
        if let data = try? JSONEncoder().encode(result) {
            try? data.write(to: resultURL, options: .atomic)
        }
    }

    private static func squaredL2(_ left: [Double], _ right: [Double]) -> Double {
        zip(left, right).reduce(0) { partial, pair in
            let difference = pair.0 - pair.1
            return partial + difference * difference
        }
    }
}
