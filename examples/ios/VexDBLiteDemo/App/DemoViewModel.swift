import Foundation

enum IndexMode: String, CaseIterable, Identifiable, Sendable {
    case plain
    case pq
    case rabitq

    var id: String { rawValue }

    var title: String {
        switch self {
        case .plain: "Plain"
        case .pq: "PQ Compact"
        case .rabitq: "RaBitQ Compact"
        }
    }

    var caption: String {
        switch self {
        case .plain: "精确向量"
        case .pq: "乘积量化"
        case .rabitq: "二值量化"
        }
    }
}

enum QueryPreset: String, CaseIterable, Identifiable, Sendable {
    case focus
    case nature
    case energy
    case city
    case coffee
    case music

    var id: String { rawValue }

    var title: String {
        switch self {
        case .focus: "哪些内容适合专注工作？"
        case .nature: "哪些内容能让人自然放松？"
        case .energy: "哪些内容适合运动时使用？"
        case .city: "有哪些值得探索的城市内容？"
        case .coffee: "哪些内容适合咖啡时间？"
        case .music: "哪些音乐内容能激发灵感？"
        }
    }

    var symbol: String {
        switch self {
        case .focus: "scope"
        case .nature: "leaf.fill"
        case .energy: "bolt.fill"
        case .city: "building.2.fill"
        case .coffee: "cup.and.saucer.fill"
        case .music: "waveform"
        }
    }
}

struct SearchHit: Identifiable, Sendable {
    let id: Int
    let title: String
    let category: String
    let distance: Double
}

struct DemoSnapshot: Sendable {
    let success: Bool
    let message: String
    let mode: String
    let version: String
    let queryPlan: String
    let rowCount: Int
    let databaseBytes: Int64
    let buildMilliseconds: Double
    let queryMilliseconds: Double
    let results: [SearchHit]
}

actor VectorDemoStore {
    private let bridge: VexDBBridge

    init() {
        let base = FileManager.default.urls(for: .applicationSupportDirectory,
                                            in: .userDomainMask).first!
        try? FileManager.default.createDirectory(at: base,
                                                 withIntermediateDirectories: true)
        bridge = VexDBBridge(databasePath: base.appendingPathComponent("vexdb-demo.sqlite").path)
    }

    func rebuild(mode: IndexMode) -> DemoSnapshot {
        convert(bridge.rebuild(mode: mode.rawValue))
    }

    func search(preset: QueryPreset) -> DemoSnapshot {
        convert(bridge.search(preset: preset.rawValue, limit: 5))
    }

    func reopenAndSearch(preset: QueryPreset) -> DemoSnapshot {
        convert(bridge.reopenAndSearch(preset: preset.rawValue, limit: 5))
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
final class DemoViewModel: ObservableObject {
    @Published var mode: IndexMode = .plain
    @Published var preset: QueryPreset = .focus
    @Published var results: [SearchHit] = []
    @Published var status = "准备本地数据库"
    @Published var version = "VexDB Lite"
    @Published var queryPlan = "等待首次查询"
    @Published var rowCount = 0
    @Published var databaseBytes: Int64 = 0
    @Published var buildMilliseconds = 0.0
    @Published var queryMilliseconds = 0.0
    @Published var isBusy = false
    @Published var hasBuilt = false
    @Published var errorMessage: String?

    private let store = VectorDemoStore()
    private var verifyExistingOnLaunch = false

    init(arguments: [String] = ProcessInfo.processInfo.arguments) {
        verifyExistingOnLaunch = arguments.contains("--reopen-only")
        if let position = arguments.firstIndex(of: "--mode"),
           arguments.indices.contains(position + 1),
           let requested = IndexMode(rawValue: arguments[position + 1]) {
            mode = requested
        }
        if let position = arguments.firstIndex(of: "--preset"),
           arguments.indices.contains(position + 1),
           let requested = QueryPreset(rawValue: arguments[position + 1]) {
            preset = requested
        }
    }

    func prepareIfNeeded() async {
        guard !hasBuilt else { return }
        if verifyExistingOnLaunch {
            isBusy = true
            status = "正在重新打开已有数据库…"
            let snapshot = await store.reopenAndSearch(preset: preset)
            apply(snapshot)
            hasBuilt = snapshot.success
            isBusy = false
            return
        }
        await rebuild()
    }

    func rebuild() async {
        guard !isBusy else { return }
        isBusy = true
        status = "正在本机训练并建立索引…"
        let snapshot = await store.rebuild(mode: mode)
        apply(snapshot)
        hasBuilt = snapshot.success
        isBusy = false
        if snapshot.success, preset != .focus {
            await search()
        }
    }

    func search() async {
        guard !isBusy, hasBuilt else { return }
        isBusy = true
        status = "正在执行 KNN 检索…"
        let snapshot = await store.search(preset: preset)
        apply(snapshot)
        isBusy = false
    }

    func reopen() async {
        guard !isBusy, hasBuilt else { return }
        isBusy = true
        status = "正在关闭并重新打开数据库…"
        let snapshot = await store.reopenAndSearch(preset: preset)
        apply(snapshot)
        isBusy = false
    }

    func select(_ preset: QueryPreset) async {
        self.preset = preset
        await search()
    }

    func select(_ mode: IndexMode) async {
        guard self.mode != mode else { return }
        self.mode = mode
        await rebuild()
    }

    private func apply(_ snapshot: DemoSnapshot) {
        status = snapshot.message
        version = snapshot.version.isEmpty ? version : snapshot.version
        queryPlan = snapshot.queryPlan.isEmpty ? queryPlan : snapshot.queryPlan
        rowCount = snapshot.rowCount
        databaseBytes = snapshot.databaseBytes
        if snapshot.buildMilliseconds > 0 {
            buildMilliseconds = snapshot.buildMilliseconds
        }
        queryMilliseconds = snapshot.queryMilliseconds
        results = snapshot.results
        errorMessage = snapshot.success ? nil : snapshot.message
    }

    var databaseSize: String {
        ByteCountFormatter.string(fromByteCount: databaseBytes, countStyle: .file)
    }
}
