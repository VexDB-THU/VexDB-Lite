import Foundation

private struct ImageIndexIsolationSmokeResult: Codable {
    let passed: Bool
    let bundledCountBefore: Int
    let bundledCountAfter: Int
    let userCountBefore: Int
    let userCountAfter: Int
    let userBuildCount: Int
    let userSearchBeforeLabels: [String]
    let bundledPlan: String
    let userPlan: String
}

enum ImageIndexIsolationSmoke {
    @MainActor private static var hasRun = false

    @MainActor
    static func runIfRequested(arguments: [String] = ProcessInfo.processInfo.arguments) async {
        guard arguments.contains("--image-isolation-smoke"), !hasRun else { return }
        hasRun = true

        let base = FileManager.default.urls(for: .applicationSupportDirectory,
                                            in: .userDomainMask).first!
        let url = base.appendingPathComponent("image-index-isolation-smoke.json")
        try? FileManager.default.removeItem(at: url)

        let databaseName = "vexdb-image-isolation-smoke-\(UUID().uuidString).sqlite"
        let bundled = ImageVectorStore(scope: .bundled, databaseName: databaseName)
        let user = ImageVectorStore(scope: .user, databaseName: databaseName)
        _ = await bundled.clear()
        _ = await user.clear()

        let bundledBuild = await bundled.importImages(
            labels: ["bundled-a", "bundled-b", "bundled-c"],
            embeddings: [[1, 0, 0, 0], [0.9, 0.1, 0, 0], [0, 1, 0, 0]]
        )
        let userBuild = await user.importImages(
            labels: ["user-a"],
            embeddings: [[0, 0, 1, 0]]
        )
        let bundledBefore = await bundled.status()
        let userBefore = await user.status()
        let userSearchBefore = await user.search(embedding: [0, 0, 1, 0], limit: 10)

        let userRebuild = await user.importImages(
            labels: ["user-b", "user-c"],
            embeddings: [[0, 0, 0.9, 0.1], [0, 0, 0, 1]]
        )
        let bundledAfter = await bundled.status()
        let userAfter = await user.status()
        let bundledSearch = await bundled.search(embedding: [1, 0, 0, 0], limit: 2)
        let userSearch = await user.search(embedding: [0, 0, 1, 0], limit: 2)

        let passed = bundledBuild.success && userBuild.success && userRebuild.success &&
            bundledBefore.rowCount == 3 && bundledAfter.rowCount == 3 &&
            userBefore.rowCount == 1 && userAfter.rowCount == 2 &&
            bundledSearch.success && userSearch.success &&
            bundledSearch.queryPlan.contains("bundled_media_vectors") &&
            userSearch.queryPlan.contains("user_media_vectors")
        let result = ImageIndexIsolationSmokeResult(
            passed: passed,
            bundledCountBefore: bundledBefore.rowCount,
            bundledCountAfter: bundledAfter.rowCount,
            userCountBefore: userBefore.rowCount,
            userCountAfter: userAfter.rowCount,
            userBuildCount: userBuild.rowCount,
            userSearchBeforeLabels: userSearchBefore.results.map(\.title),
            bundledPlan: bundledSearch.queryPlan,
            userPlan: userSearch.queryPlan
        )

        if let data = try? JSONEncoder().encode(result) {
            try? data.write(to: url, options: .atomic)
        }
    }
}
