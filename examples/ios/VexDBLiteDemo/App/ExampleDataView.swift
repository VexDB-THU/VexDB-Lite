import SwiftUI

struct ExampleDataView: View {
    @ObservedObject var settings: EmbeddingSettingsModel
    @StateObject private var model = BundledExampleViewModel()
    @State private var showSettings = false
    @State private var showAllChunks = false
    @FocusState private var queryFocused: Bool

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: 24) {
                    hero
                    chunkSection
                    querySection
                    if !model.results.isEmpty { resultSection }
                    if model.queryPlan != "查询后显示执行计划" { planSection }
                    privacyNote
                }
                .padding(.horizontal, 18)
                .padding(.bottom, 30)
            }
            .scrollDismissesKeyboard(.interactively)
            .background(DemoPalette.background.ignoresSafeArea())
            .foregroundStyle(.white)
            .toolbar(.hidden, for: .navigationBar)
        }
        .task { await model.prepare() }
        .sheet(isPresented: $showSettings) { SettingsView(settings: settings) }
        .sheet(isPresented: $showAllChunks) {
            ExampleChunksSheet(chunks: model.chunks,
                               modelName: model.modelName,
                               dimensions: model.dimensions)
        }
        .alert("VexDB Lite", isPresented: Binding(
            get: { model.errorMessage != nil },
            set: { if !$0 { model.errorMessage = nil } }
        )) {
            Button("知道了", role: .cancel) {}
        } message: {
            Text(model.errorMessage ?? "未知错误")
        }
    }

    private var hero: some View {
        VStack(alignment: .leading, spacing: 13) {
            HStack {
                Label("DEFAULT EXAMPLE", systemImage: "iphone.gen3")
                    .font(.caption2.weight(.bold))
                    .tracking(1)
                    .foregroundStyle(DemoPalette.cyan)
                Spacer()
                if model.isBusy { ProgressView().tint(DemoPalette.cyan) }
                else { Image(systemName: "checkmark.seal.fill").foregroundStyle(DemoPalette.cyan) }
            }
            Text("VexDB Lite")
                .font(.system(size: 35, weight: .bold, design: .rounded))
            Text("用真实文档，在这台 iPhone 上完成向量检索。")
                .font(.title3.weight(.medium))
                .foregroundStyle(.white.opacity(0.70))
            HStack(spacing: 8) {
                Circle().fill(model.errorMessage == nil ? DemoPalette.cyan : .orange)
                    .frame(width: 8, height: 8)
                Text(model.status)
                    .font(.subheadline)
                    .foregroundStyle(.white.opacity(0.68))
            }
        }
        .padding(.top, 25)
    }

    private var chunkSection: some View {
        VStack(alignment: .leading, spacing: 14) {
            sectionTitle("默认示例", detail: "《Embedding 召回测试文档》")
            HStack(spacing: 16) {
                Label("\(model.chunks.count) 段", systemImage: "square.stack.3d.up.fill")
                Label("\(model.dimensions) 维", systemImage: "point.3.filled.connected.trianglepath.dotted")
                Text(model.modelName)
            }
            .font(.caption)
            .foregroundStyle(.white.opacity(0.48))
            Button { showAllChunks = true } label: {
                VStack(alignment: .leading, spacing: 11) {
                    Text(documentPreview)
                        .font(.subheadline)
                        .foregroundStyle(.white.opacity(0.76))
                        .multilineTextAlignment(.leading)
                        .fixedSize(horizontal: false, vertical: true)
                    HStack {
                        Text("查看全部分片和详情")
                            .font(.caption.weight(.semibold))
                            .foregroundStyle(DemoPalette.cyan)
                        Spacer()
                        Image(systemName: "arrow.up.right")
                            .font(.caption.weight(.bold))
                            .foregroundStyle(DemoPalette.cyan)
                    }
                }
                .padding(14)
                .background(Color.black.opacity(0.18), in: RoundedRectangle(cornerRadius: 14))
            }
            .buttonStyle(.plain)
        }
        .demoCard()
    }

    private var documentPreview: String {
        let text = DemoDocument.text.trimmingCharacters(in: .whitespacesAndNewlines)
        let prefix = String(text.prefix(100))
        return text.count > 100 ? prefix + "…" : prefix
    }

    private var querySection: some View {
        VStack(alignment: .leading, spacing: 14) {
            sectionTitle("提一个完整问题", detail: "先选择问题，再点击发送；预设问题无需 API Key")
            VStack(alignment: .leading, spacing: 8) {
                ForEach(model.queries) { query in
                    Button {
                        queryFocused = false
                        model.select(query)
                    } label: {
                        HStack(spacing: 10) {
                            Image(systemName: model.selectedQuery == query.text
                                  ? "checkmark.circle.fill" : "questionmark.circle")
                                .foregroundStyle(model.selectedQuery == query.text
                                                 ? DemoPalette.cyan : .white.opacity(0.40))
                            Text(query.text)
                                .font(.subheadline.weight(.medium))
                                .foregroundStyle(.white.opacity(0.82))
                                .multilineTextAlignment(.leading)
                            Spacer(minLength: 8)
                            Image(systemName: "arrow.right")
                                .font(.caption.weight(.bold))
                                .foregroundStyle(.white.opacity(0.26))
                        }
                        .padding(.vertical, 7)
                    }
                    .buttonStyle(.plain)
                    .disabled(model.isBusy || !model.hasIndex)
                }
            }

            Divider().overlay(.white.opacity(0.08))

            Button { showSettings = true } label: {
                HStack(spacing: 10) {
                    Image(systemName: settings.isConfigured
                          ? "checkmark.shield.fill" : "key.horizontal.fill")
                        .foregroundStyle(settings.isConfigured ? DemoPalette.cyan : .orange)
                    VStack(alignment: .leading, spacing: 3) {
                        Text("输入新问题")
                            .font(.subheadline.weight(.semibold))
                            .foregroundStyle(.white)
                        Text(settings.isConfigured
                             ? "Embedding API 已设置"
                             : "先设置 API Key，再输入自己的问题")
                            .font(.caption)
                            .foregroundStyle(.white.opacity(0.48))
                    }
                    Spacer()
                    Text("设置")
                        .font(.caption.weight(.semibold))
                        .foregroundStyle(DemoPalette.cyan)
                    Image(systemName: "chevron.right")
                        .font(.caption)
                        .foregroundStyle(.white.opacity(0.30))
                }
                .padding(.vertical, 2)
            }
            .buttonStyle(.plain)

            HStack(spacing: 10) {
                TextField("或者输入自己的问题…", text: $model.queryText, axis: .vertical)
                    .focused($queryFocused)
                    .lineLimit(1...3)
                    .padding(.horizontal, 13)
                    .padding(.vertical, 12)
                    .background(Color.black.opacity(0.20), in: RoundedRectangle(cornerRadius: 14))
                    .submitLabel(.search)
                    .onSubmit { searchCustom() }
                Button { searchCustom() } label: {
                    Image(systemName: "arrow.up")
                        .font(.headline.weight(.bold))
                        .frame(width: 44, height: 44)
                        .foregroundStyle(DemoPalette.background)
                        .background(DemoPalette.violet, in: Circle())
                }
                .disabled(model.isBusy || !model.hasIndex || model.queryText
                    .trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
                .accessibilityLabel("搜索问题")
            }
            if !settings.isConfigured, !model.queryText.trimmingCharacters(
                in: .whitespacesAndNewlines).isEmpty,
               !model.queries.contains(where: {
                   $0.text == model.queryText.trimmingCharacters(in: .whitespacesAndNewlines)
               }) {
                Button("输入新问题前，请先设置 API Key") { showSettings = true }
                    .font(.caption.weight(.semibold))
                    .foregroundStyle(.orange)
            }
        }
    }

    private var resultSection: some View {
        VStack(alignment: .leading, spacing: 14) {
            sectionTitle("搜索结果", detail: "距离越小越相似，点击结果展开原文")
            HStack(spacing: 16) {
                Label(String(format: "%.2f ms", model.queryMilliseconds), systemImage: "timer")
                Label(model.databaseSize, systemImage: "internaldrive.fill")
                Label("本机 KNN", systemImage: "iphone")
            }
            .font(.caption)
            .foregroundStyle(.white.opacity(0.48))
            ForEach(Array(model.results.enumerated()), id: \.element.id) { index, hit in
                ExpandableTextBlock(
                    title: "#\(index + 1) · \(hit.title)",
                    text: hit.category,
                    trailingText: String(format: "%.4f", hit.distance),
                    accent: index == 0 ? DemoPalette.cyan : .white.opacity(0.62)
                )
                if index < model.results.count - 1 { divider }
            }
        }
        .demoCard()
    }

    private var planSection: some View {
        VStack(alignment: .leading, spacing: 10) {
            sectionTitle("执行计划", detail: "确认查询走 VexDB 向量索引")
            Text(model.queryPlan)
                .font(.caption.monospaced())
                .foregroundStyle(DemoPalette.cyan)
                .fixedSize(horizontal: false, vertical: true)
        }
        .demoCard()
    }

    private var privacyNote: some View {
        Label("示例分片和预设问题向量已随 App 打包；SQLite 检索完全在本机完成。",
              systemImage: "lock.shield.fill")
            .font(.caption)
            .foregroundStyle(.white.opacity(0.43))
            .fixedSize(horizontal: false, vertical: true)
    }

    private var divider: some View {
        Divider().overlay(.white.opacity(0.08))
    }

    private func sectionTitle(_ title: String, detail: String) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(title).font(.headline)
            Text(detail).font(.caption).foregroundStyle(.white.opacity(0.45))
        }
    }

    private func searchCustom() {
        queryFocused = false
        let value = model.queryText.trimmingCharacters(in: .whitespacesAndNewlines)
        if let bundled = model.queries.first(where: { $0.text == value }) {
            Task { await model.search(bundled) }
            return
        }
        do {
            let configuration = try settings.configuration()
            Task { await model.searchCustom(configuration: configuration) }
        } catch {
            model.errorMessage = error.localizedDescription
            showSettings = true
        }
    }
}

private struct ExampleChunksSheet: View {
    let chunks: [BundledExample.Chunk]
    let modelName: String
    let dimensions: Int
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: 14) {
                    Text("共 \(chunks.count) 个分片 · \(dimensions) 维 · \(modelName)")
                        .font(.caption)
                        .foregroundStyle(.white.opacity(0.48))
                    ForEach(Array(chunks.enumerated()), id: \.element.id) { index, chunk in
                        ExpandableTextBlock(
                            title: "分片 \(index + 1)",
                            text: chunk.text,
                            trailingText: "\(chunk.text.count) 字",
                            collapsedLines: 4
                        )
                        if index < chunks.count - 1 {
                            Divider().overlay(.white.opacity(0.08))
                        }
                    }
                }
                .padding(18)
            }
            .background(DemoPalette.background.ignoresSafeArea())
            .foregroundStyle(.white)
            .navigationTitle("全部分片")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    Button("完成") { dismiss() }
                        .foregroundStyle(DemoPalette.cyan)
                }
            }
        }
        .presentationDetents([.large])
    }
}
