import Foundation

struct BundledImageItem: Decodable, Identifiable {
    var id: String { resource }
    let resource: String
    let embedding: [Double]
}

struct BundledImageQuery: Decodable, Identifiable {
    var id: String { text }
    let text: String
    let embedding: [Double]
}

private struct BundledImagePayload: Decodable {
    let model: String
    let dimensions: Int
    let images: [BundledImageItem]
    let queries: [BundledImageQuery]
}

@MainActor
final class BundledImageExampleViewModel: ObservableObject {
    @Published var images: [BundledImageItem] = []
    @Published var queries: [BundledImageQuery] = []
    @Published var selectedQuery = ""
    @Published var results: [SearchHit] = []
    @Published var status = "正在准备内置图片索引…"
    @Published var queryPlan = "查询后显示执行计划"
    @Published var queryMilliseconds = 0.0
    @Published var isBusy = false
    @Published var errorMessage: String?
    private(set) var modelName = ""
    private(set) var dimensions = 0
    private let store = ImageVectorStore(scope: .bundled)
    private var prepared = false

    func prepare() async {
        guard !prepared, !isBusy else { return }
        isBusy = true
        defer { isBusy = false }
        do {
            guard let url = Bundle.main.url(forResource: "image-demo-vectors", withExtension: "json") else {
                throw EmbeddingError.invalidResponse("App 中缺少默认图片向量")
            }
            let payload = try JSONDecoder().decode(BundledImagePayload.self, from: Data(contentsOf: url))
            guard payload.images.count >= 3, !payload.queries.isEmpty,
                  payload.dimensions > 0,
                  payload.images.allSatisfy({ $0.embedding.count == payload.dimensions }),
                  payload.queries.allSatisfy({ $0.embedding.count == payload.dimensions }),
                  payload.images.allSatisfy({ BundledImageAsset.url(for: $0.resource) != nil }) else {
                throw EmbeddingError.invalidResponse("默认图片向量数据不完整")
            }
            images = payload.images
            queries = payload.queries
            modelName = payload.model
            dimensions = payload.dimensions
            let snapshot = await store.importImages(labels: payload.images.map(\.resource),
                                                     embeddings: payload.images.map(\.embedding))
            guard snapshot.success else { throw EmbeddingError.invalidResponse(snapshot.message) }
            prepared = true
            status = "\(payload.images.count) 张图片已写入本机索引"
        } catch {
            errorMessage = error.localizedDescription
            status = "默认图片索引准备失败"
        }
    }

    func select(_ query: BundledImageQuery) {
        selectedQuery = query.text
        results = []
        queryPlan = "点击发送后显示执行计划"
    }

    func search() async {
        guard !isBusy, let query = queries.first(where: { $0.text == selectedQuery }) else {
            if selectedQuery.isEmpty { errorMessage = "请先选择一个示例问题" }
            return
        }
        isBusy = true
        defer { isBusy = false }
        status = "正在本机搜索全部图片…"
        let snapshot = await store.search(embedding: query.embedding, limit: min(5, images.count))
        if snapshot.success {
            results = snapshot.results
            queryMilliseconds = snapshot.queryMilliseconds
            queryPlan = snapshot.queryPlan
            status = "已在全部 \(images.count) 张图片中完成本机检索"
        } else {
            errorMessage = snapshot.message
            status = "图片检索失败"
        }
    }

    func image(for hit: SearchHit) -> BundledImageItem? {
        images.first(where: { $0.resource == hit.title })
    }
}
