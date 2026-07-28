import Foundation

struct BundledExample: Decodable, Sendable {
    struct Chunk: Decodable, Identifiable, Sendable {
        let id: Int
        let text: String
        let embedding: [Double]
    }

    struct Query: Decodable, Identifiable, Sendable {
        let text: String
        let embedding: [Double]
        var id: String { text }
    }

    let model: String
    let dimensions: Int
    let chunkLength: Int
    let overlapLength: Int
    let chunks: [Chunk]
    let queries: [Query]

    static func load() throws -> BundledExample {
        guard let url = Bundle.main.url(forResource: "embedding-demo-vectors",
                                        withExtension: "json") else {
            throw EmbeddingError.invalidResponse("默认示例向量文件不存在")
        }
        let value = try JSONDecoder().decode(BundledExample.self, from: Data(contentsOf: url))
        guard value.dimensions > 0,
              !value.chunks.isEmpty,
              value.chunks.allSatisfy({ $0.embedding.count == value.dimensions }),
              value.queries.allSatisfy({ $0.embedding.count == value.dimensions }) else {
            throw EmbeddingError.invalidResponse("默认示例向量文件格式不正确")
        }
        return value
    }
}

@MainActor
final class BundledExampleViewModel: ObservableObject {
    @Published private(set) var example: BundledExample?
    @Published var queryText = ""
    @Published var selectedQuery: String?
    @Published var results: [SearchHit] = []
    @Published var status = "正在载入默认示例…"
    @Published var rowCount = 0
    @Published var databaseBytes: Int64 = 0
    @Published var buildMilliseconds = 0.0
    @Published var queryMilliseconds = 0.0
    @Published var queryPlan = "查询后显示执行计划"
    @Published var isBusy = false
    @Published var errorMessage: String?

    private let store = UserKnowledgeStore(databaseName: "vexdb-example.sqlite")
    private let client = EmbeddingClient()
    private var prepared = false

    var chunks: [BundledExample.Chunk] { example?.chunks ?? [] }
    var queries: [BundledExample.Query] { example?.queries ?? [] }
    var modelName: String { example?.model ?? "text-embedding-v4" }
    var dimensions: Int { example?.dimensions ?? 0 }
    var hasIndex: Bool { rowCount > 0 }
    var databaseSize: String {
        ByteCountFormatter.string(fromByteCount: databaseBytes, countStyle: .file)
    }

    func prepare() async {
        guard !prepared else { return }
        prepared = true
        isBusy = true
        errorMessage = nil
        do {
            let loaded = try BundledExample.load()
            example = loaded
            status = "正在把预生成向量写入本地 VexDB…"
            let snapshot = try await store.importText(
                chunks: loaded.chunks.map(\.text),
                embeddings: loaded.chunks.map(\.embedding)
            )
            apply(snapshot)
            if snapshot.success {
                status = "默认示例已就绪，选择问题后点击发送"
            }
        } catch {
            errorMessage = error.localizedDescription
            status = "默认示例载入失败"
        }
        isBusy = false
    }

    func search(_ query: BundledExample.Query) async {
        guard !isBusy, hasIndex else { return }
        selectedQuery = query.text
        queryText = query.text
        await search(embedding: query.embedding)
    }

    func select(_ query: BundledExample.Query) {
        guard !isBusy, hasIndex else { return }
        selectedQuery = query.text
        queryText = query.text
        results = []
        queryMilliseconds = 0
        queryPlan = "查询后显示执行计划"
        status = "问题已填入，点击发送开始查询"
    }

    func searchCustom(configuration: EmbeddingConfiguration) async {
        let value = queryText.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !value.isEmpty else {
            errorMessage = "请输入一个完整问题"
            return
        }
        selectedQuery = nil
        guard !isBusy, hasIndex else { return }
        isBusy = true
        errorMessage = nil
        status = "正在生成问题向量…"
        do {
            let vectors = try await client.embed([value], configuration: configuration)
            await search(embedding: vectors[0], managesBusyState: false)
        } catch {
            errorMessage = error.localizedDescription
            status = "问题向量生成失败"
        }
        isBusy = false
    }

    private func search(embedding: [Double], managesBusyState: Bool = true) async {
        if managesBusyState { isBusy = true }
        errorMessage = nil
        status = "正在这台 iPhone 上执行向量检索…"
        let snapshot = await store.search(embedding: embedding, limit: 5)
        apply(snapshot)
        if snapshot.success { status = "检索已在本机完成" }
        if managesBusyState { isBusy = false }
    }

    private func apply(_ snapshot: DemoSnapshot) {
        rowCount = snapshot.rowCount
        databaseBytes = snapshot.databaseBytes
        if snapshot.buildMilliseconds > 0 { buildMilliseconds = snapshot.buildMilliseconds }
        queryMilliseconds = snapshot.queryMilliseconds
        results = snapshot.results
        if !snapshot.queryPlan.isEmpty { queryPlan = snapshot.queryPlan }
        if !snapshot.success { errorMessage = snapshot.message }
    }
}
