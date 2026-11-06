import Foundation

private struct ImageAssetSmokePayload: Decodable {
    let images: [BundledImageItem]
}

private struct ImageAssetSmokeResult: Codable {
    let passed: Bool
    let imageCount: Int
    let decodedCount: Int
    let failedResources: [String]
    let error: String
}

enum ImageAssetSmoke {
    @MainActor private static var hasRun = false

    @MainActor
    static func runIfRequested(arguments: [String] = ProcessInfo.processInfo.arguments) async {
        guard arguments.contains("--image-asset-smoke"), !hasRun else { return }
        hasRun = true

        let base = FileManager.default.urls(for: .applicationSupportDirectory,
                                            in: .userDomainMask).first!
        let resultURL = base.appendingPathComponent("image-asset-smoke.json")
        try? FileManager.default.removeItem(at: resultURL)

        var imageCount = 0
        var decodedCount = 0
        var failedResources: [String] = []
        var errorMessage = ""

        do {
            guard let url = Bundle.main.url(forResource: "image-demo-vectors",
                                            withExtension: "json") else {
                throw EmbeddingError.invalidResponse("App 中缺少默认图片向量")
            }
            let payload = try JSONDecoder().decode(ImageAssetSmokePayload.self,
                                                   from: Data(contentsOf: url))
            imageCount = payload.images.count
            for item in payload.images {
                if ImageAssetCache.shared.bundledImage(resource: item.resource,
                                                       maxPixelSize: 256) != nil {
                    decodedCount += 1
                } else {
                    failedResources.append(item.resource)
                }
            }
        } catch {
            errorMessage = error.localizedDescription
        }

        let result = ImageAssetSmokeResult(
            passed: imageCount == 20 && decodedCount == imageCount && errorMessage.isEmpty,
            imageCount: imageCount,
            decodedCount: decodedCount,
            failedResources: failedResources,
            error: errorMessage
        )
        if let data = try? JSONEncoder().encode(result) {
            try? data.write(to: resultURL, options: .atomic)
        }
    }
}
