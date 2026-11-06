import SwiftUI

struct UserDataView: View {
    @ObservedObject var settings: EmbeddingSettingsModel
    @StateObject private var model = UserKnowledgeViewModel()
    @State private var showSettings = false
    @State private var confirmReplacement = false
    @State private var smokeStarted = false
    @State private var sourceEditorExpanded = true
    @FocusState private var focusedField: Field?

    private enum Field { case source, query }

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: 24) {
                    statusHeader
                    embeddingSection
                    importSection
                    searchSection
                    if !model.results.isEmpty { resultsSection }
                    if model.hasIndex { planSection }
                    privacyNote
                }
                .padding(.horizontal, 18)
                .padding(.bottom, 34)
            }
            .scrollDismissesKeyboard(.interactively)
            .background(DemoPalette.background.ignoresSafeArea())
            .foregroundStyle(.white)
            .toolbar(.hidden, for: .navigationBar)
        }
        .sheet(isPresented: $showSettings) { SettingsView(settings: settings) }
        .task {
            await model.loadStatus()
            await runSmokeIfRequested()
        }
        .alert("VexDB Lite", isPresented: Binding(
            get: { model.errorMessage != nil },
            set: { if !$0 { model.errorMessage = nil } }
        )) {
            Button("知道了", role: .cancel) {}
        } message: {
            Text(model.errorMessage ?? "未知错误")
        }
        .confirmationDialog("替换已有的自定义文本索引？",
                            isPresented: $confirmReplacement,
                            titleVisibility: .visible) {
            Button("替换并重新生成", role: .destructive) { startImport() }
            Button("取消", role: .cancel) {}
        } message: {
            Text("生成成功后会替换当前的 \(model.rowCount) 个分片。")
        }
    }

    private var statusHeader: some View {
        VStack(alignment: .leading, spacing: 12) {
            Label("CUSTOM TEXT", systemImage: "square.and.pencil")
                .font(.caption2.weight(.bold))
                .tracking(1)
                .foregroundStyle(DemoPalette.violet)
            Text("检索自己的文本")
                .font(.system(size: 31, weight: .bold, design: .rounded))
            Text("设置模型、输入文本、确认分片，再生成向量。")
                .font(.title3.weight(.medium))
                .foregroundStyle(.white.opacity(0.68))
            HStack(spacing: 9) {
                Circle().fill(model.errorMessage == nil ? DemoPalette.cyan : .orange)
                    .frame(width: 8, height: 8)
                Text(model.status)
                    .font(.subheadline)
                    .foregroundStyle(.white.opacity(0.68))
            }
            if model.isBusy { ProgressView(value: model.progress).tint(DemoPalette.cyan) }
        }
        .padding(.top, 25)
    }

    private var embeddingSection: some View {
        Button { showSettings = true } label: {
            HStack(spacing: 13) {
                Image(systemName: settings.isConfigured ? "checkmark.shield.fill" : "key.horizontal.fill")
                    .font(.title3)
                    .foregroundStyle(settings.isConfigured ? DemoPalette.cyan : .orange)
                VStack(alignment: .leading, spacing: 4) {
                    Text("Embedding 模型")
                        .font(.headline)
                        .foregroundStyle(.white)
                    Text(settings.isConfigured
                         ? "\(settings.model) · \(settings.endpoint)"
                         : "填写 API 地址、模型和 API Key")
                        .font(.caption)
                        .foregroundStyle(.white.opacity(0.48))
                        .lineLimit(2)
                        .multilineTextAlignment(.leading)
                }
                Spacer()
                Text("设置")
                    .font(.caption.weight(.semibold))
                    .foregroundStyle(DemoPalette.cyan)
                Image(systemName: "chevron.right")
                    .font(.caption)
                    .foregroundStyle(.white.opacity(0.30))
            }
        }
        .buttonStyle(.plain)
        .demoCard()
    }

    private var importSection: some View {
        VStack(alignment: .leading, spacing: 16) {
            HStack(alignment: .top) {
                sectionTitle("1. 输入并分片", detail: "单次最多 20 万个字符，分片会在生成向量前展示")
                Spacer()
                if !model.sourceText.isEmpty {
                    Button(sourceEditorExpanded ? "收起输入框" : "展开编辑") {
                        focusedField = nil
                        withAnimation(.easeOut(duration: 0.22)) {
                            sourceEditorExpanded.toggle()
                        }
                    }
                    .font(.caption.weight(.semibold))
                    .foregroundStyle(DemoPalette.cyan)
                }
            }

            if model.sourceText.isEmpty || sourceEditorExpanded {
                ZStack(alignment: .topLeading) {
                    TextEditor(text: $model.sourceText)
                        .focused($focusedField, equals: .source)
                        .frame(minHeight: 185)
                        .scrollContentBackground(.hidden)
                        .padding(8)
                        .background(Color.black.opacity(0.20), in: RoundedRectangle(cornerRadius: 14))
                    if model.sourceText.isEmpty {
                        Text("在这里粘贴文章、笔记、产品资料或其他文本…")
                            .foregroundStyle(.white.opacity(0.30))
                            .padding(.horizontal, 13)
                            .padding(.vertical, 17)
                            .allowsHitTesting(false)
                    }
                }
            } else {
                Button {
                    withAnimation(.easeOut(duration: 0.22)) { sourceEditorExpanded = true }
                } label: {
                    VStack(alignment: .leading, spacing: 10) {
                        Text(sourcePreview)
                            .font(.subheadline)
                            .foregroundStyle(.white.opacity(0.72))
                            .multilineTextAlignment(.leading)
                            .fixedSize(horizontal: false, vertical: true)
                        Label("展开继续编辑", systemImage: "pencil")
                            .font(.caption.weight(.semibold))
                            .foregroundStyle(DemoPalette.cyan)
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(14)
                    .background(Color.black.opacity(0.18), in: RoundedRectangle(cornerRadius: 14))
                }
                .buttonStyle(.plain)
            }

            VStack(spacing: 12) {
                Stepper("每段约 \(model.chunkLength) 个字符",
                        value: $model.chunkLength, in: 100...2_000, step: 100)
                Stepper("相邻段重叠 \(model.overlapLength) 个字符",
                        value: $model.overlapLength,
                        in: 0...max(0, min(500, model.chunkLength - 1)), step: 25)
            }
            .font(.subheadline)

            if !model.previewChunks.isEmpty {
                VStack(alignment: .leading, spacing: 10) {
                    Text("分片预览 · \(model.previewChunks.count) 段")
                        .font(.caption.weight(.semibold))
                        .foregroundStyle(.white.opacity(0.54))
                    ForEach(Array(model.previewChunks.enumerated()), id: \.offset) { index, chunk in
                        ExpandableTextBlock(
                            title: "分片 \(index + 1)",
                            text: chunk,
                            trailingText: "\(chunk.count) 字",
                            accent: DemoPalette.violet,
                            collapsedLines: 2
                        )
                        if index < model.previewChunks.count - 1 {
                            Divider().overlay(.white.opacity(0.08))
                        }
                    }
                }
                .padding(.top, 2)
            }

            HStack {
                Text(model.previewChunkCount > 0
                     ? "将生成 \(model.previewChunkCount) 个向量"
                     : "输入文本后显示分片")
                    .font(.caption)
                    .foregroundStyle(.white.opacity(0.46))
                Spacer()
                Button {
                    focusedField = nil
                    if model.hasIndex { confirmReplacement = true } else { startImport() }
                } label: {
                    Label(model.isBusy ? "生成中…" : "生成向量", systemImage: "sparkles")
                        .font(.subheadline.weight(.semibold))
                        .padding(.horizontal, 16)
                        .padding(.vertical, 11)
                        .foregroundStyle(DemoPalette.background)
                        .background(DemoPalette.cyan, in: Capsule())
                }
                .disabled(model.isBusy || model.previewChunkCount == 0 || !settings.isConfigured)
            }
        }
        .demoCard()
    }

    private var searchSection: some View {
        VStack(alignment: .leading, spacing: 14) {
            sectionTitle("2. 搜索自己的内容", detail: "问题用同一模型生成向量，KNN 检索在本机完成")
            HStack(spacing: 10) {
                TextField("输入一个完整问题…", text: $model.queryText, axis: .vertical)
                    .focused($focusedField, equals: .query)
                    .lineLimit(1...3)
                    .padding(.horizontal, 13)
                    .padding(.vertical, 12)
                    .background(DemoPalette.panel, in: RoundedRectangle(cornerRadius: 14))
                    .submitLabel(.search)
                    .onSubmit { startSearch() }
                Button { startSearch() } label: {
                    Image(systemName: "arrow.up")
                        .font(.headline.weight(.bold))
                        .frame(width: 44, height: 44)
                        .foregroundStyle(DemoPalette.background)
                        .background(DemoPalette.violet, in: Circle())
                }
                .disabled(model.isBusy || !model.hasIndex || model.queryText
                    .trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
                .accessibilityLabel("搜索")
            }
            HStack(spacing: 18) {
                Label("\(model.rowCount) 个分片", systemImage: "square.stack.3d.up.fill")
                Label(model.databaseSize, systemImage: "internaldrive.fill")
                if model.queryMilliseconds > 0 {
                    Label(String(format: "%.2f ms", model.queryMilliseconds), systemImage: "timer")
                }
            }
            .font(.caption)
            .foregroundStyle(.white.opacity(0.46))
        }
    }

    private var resultsSection: some View {
        VStack(alignment: .leading, spacing: 14) {
            sectionTitle("搜索结果", detail: "距离越小越相似，点击结果展开原文")
            ForEach(Array(model.results.enumerated()), id: \.element.id) { index, hit in
                ExpandableTextBlock(
                    title: "#\(index + 1) · \(hit.title)",
                    text: hit.category,
                    trailingText: String(format: "%.4f", hit.distance),
                    accent: index == 0 ? DemoPalette.cyan : .white.opacity(0.62)
                )
                if index < model.results.count - 1 {
                    Divider().overlay(.white.opacity(0.08))
                }
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
        Label("文本和问题会发送给你设置的 Embedding API；向量、SQLite 索引和搜索结果保留在本机。",
              systemImage: "network.badge.shield.half.filled")
            .font(.caption)
            .foregroundStyle(.white.opacity(0.43))
            .fixedSize(horizontal: false, vertical: true)
    }

    private func sectionTitle(_ title: String, detail: String) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(title).font(.headline)
            Text(detail).font(.caption).foregroundStyle(.white.opacity(0.45))
        }
    }

    private var sourcePreview: String {
        let text = model.sourceText.trimmingCharacters(in: .whitespacesAndNewlines)
        let prefix = String(text.prefix(100))
        return text.count > 100 ? prefix + "…" : prefix
    }

    private func startImport() {
        do {
            let configuration = try settings.configuration()
            Task {
                await model.importText(configuration: configuration)
                if model.hasIndex {
                    withAnimation(.easeOut(duration: 0.22)) { sourceEditorExpanded = false }
                }
            }
        } catch {
            model.errorMessage = error.localizedDescription
            showSettings = true
        }
    }

    private func startSearch() {
        focusedField = nil
        do {
            let configuration = try settings.configuration()
            Task { await model.search(configuration: configuration) }
        } catch {
            model.errorMessage = error.localizedDescription
            showSettings = true
        }
    }

    private func runSmokeIfRequested() async {
        guard !smokeStarted,
              ProcessInfo.processInfo.arguments.contains("--user-smoke") else { return }
        smokeStarted = true
        model.chunkLength = 500
        model.overlapLength = 50
        model.sourceText = DemoDocument.text
        do {
            let configuration = try settings.configuration()
            await model.importText(configuration: configuration)
            if model.hasIndex {
                sourceEditorExpanded = false
                model.queryText = DemoDocument.queries[0]
                await model.search(configuration: configuration)
            }
        } catch {
            model.errorMessage = error.localizedDescription
        }
    }
}
