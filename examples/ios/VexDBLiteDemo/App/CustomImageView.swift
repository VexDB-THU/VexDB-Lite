import SwiftUI
import PhotosUI

struct CustomImageView: View {
    @StateObject private var settings = ImageEmbeddingSettingsModel()
    @StateObject private var model = CustomImageViewModel()
    @State private var pickerItems: [PhotosPickerItem] = []
    @State private var showSettings = false
    @State private var confirmClear = false
    @FocusState private var queryFocused: Bool

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: 24) {
                    statusHeader
                    embeddingSection
                    importSection
                    searchSection
                    if !model.results.isEmpty { resultsSection }
                    if !model.results.isEmpty { planSection }
                    privacyNote
                }
                .padding(.horizontal, 18)
                .padding(.bottom, 36)
            }
            .scrollDismissesKeyboard(.interactively)
            .background(DemoPalette.background.ignoresSafeArea())
            .foregroundStyle(.white)
            .toolbar(.hidden, for: .navigationBar)
        }
        .task { await model.load() }
        .onChange(of: pickerItems) { _, items in
            Task {
                await model.loadSelection(items)
                pickerItems = []
            }
        }
        .sheet(isPresented: $showSettings) { ImageSettingsView(settings: settings) }
        .alert("自定义图片", isPresented: Binding(
            get: { model.errorMessage != nil },
            set: { if !$0 { model.errorMessage = nil } }
        )) {
            Button("知道了", role: .cancel) {}
        } message: {
            Text(model.errorMessage ?? "未知错误")
        }
        .confirmationDialog("清空用户图片库？",
                            isPresented: $confirmClear,
                            titleVisibility: .visible) {
            Button("清空图片和索引", role: .destructive) {
                Task { await model.clear() }
            }
            Button("取消", role: .cancel) {}
        } message: {
            Text("将删除本机保存的 \(model.records.count) 张图片及其用户图片索引，默认图片不受影响。")
        }
    }

    private var statusHeader: some View {
        VStack(alignment: .leading, spacing: 11) {
            Label("CUSTOM IMAGE", systemImage: "photo.badge.plus")
                .font(.caption2.weight(.bold))
                .tracking(1)
                .foregroundStyle(DemoPalette.violet)
            Text("检索自己的图片")
                .font(.system(size: 31, weight: .bold, design: .rounded))
            Text("选择多张图片生成索引，再用文字描述查找图片。")
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
                    Text("图片 Embedding 模型")
                        .font(.headline)
                        .foregroundStyle(.white)
                    Text(settings.isConfigured
                         ? "\(settings.model) · 图片与文字使用同一向量空间"
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
                sectionTitle("1. 添加图片", detail: "一次最多选择 12 张；成功后加入现有用户图片库")
                Spacer()
                if model.hasIndex {
                    Button("清空") { confirmClear = true }
                        .font(.caption.weight(.semibold))
                        .foregroundStyle(.orange)
                }
            }

            PhotosPicker(selection: $pickerItems,
                         maxSelectionCount: 12,
                         matching: .images) {
                Label("从照片中选择多张图片", systemImage: "photo.stack")
                    .font(.subheadline.weight(.semibold))
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 12)
            }
            .buttonStyle(.bordered)
            .tint(DemoPalette.violet)
            .disabled(model.isBusy)

            if !model.pendingImages.isEmpty {
                imageGrid(images: model.pendingImages)
                HStack {
                    Text("已选择 \(model.pendingImages.count) 张")
                        .font(.caption)
                        .foregroundStyle(.white.opacity(0.48))
                    Spacer()
                    Button { startImport() } label: {
                        Label(model.isBusy ? "生成中…" : "生成并加入索引", systemImage: "sparkles")
                            .font(.subheadline.weight(.semibold))
                            .padding(.horizontal, 15)
                            .padding(.vertical, 10)
                            .foregroundStyle(DemoPalette.background)
                            .background(DemoPalette.cyan, in: Capsule())
                    }
                    .disabled(model.isBusy || !settings.isConfigured)
                }
            }

            if model.hasIndex {
                Divider().overlay(.white.opacity(0.08))
                HStack {
                    Text("本机图片库 · \(model.records.count) 张")
                        .font(.caption.weight(.semibold))
                        .foregroundStyle(.white.opacity(0.56))
                    Spacer()
                    Text("\(model.dimensions) 维")
                        .font(.caption.monospacedDigit())
                        .foregroundStyle(DemoPalette.cyan)
                }
                ScrollView(.horizontal, showsIndicators: false) {
                    LazyHStack(spacing: 10) {
                        ForEach(model.records) { record in
                            if let image = storedImage(record, maxPixelSize: 480) {
                                Image(uiImage: image)
                                    .resizable()
                                    .scaledToFill()
                                    .frame(width: 104, height: 104)
                                    .clipShape(RoundedRectangle(cornerRadius: 13))
                            }
                        }
                    }
                }
            } else if model.pendingImages.isEmpty {
                Label("图片向量生成成功后，原图和图索引都会保存在本机。",
                      systemImage: "internaldrive")
                    .font(.caption)
                    .foregroundStyle(.white.opacity(0.42))
            }
        }
        .demoCard()
    }

    private var searchSection: some View {
        VStack(alignment: .leading, spacing: 14) {
            sectionTitle("2. 用文字搜索图片", detail: "文字生成查询向量后，KNN 检索在本机完成")
            HStack(spacing: 10) {
                TextField("例如：草地上的羊和汽车", text: $model.queryText, axis: .vertical)
                    .focused($queryFocused)
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
                .accessibilityLabel("搜索图片")
            }
            HStack(spacing: 16) {
                Label("\(model.records.count) 张", systemImage: "photo.stack.fill")
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
            sectionTitle("搜索结果", detail: "距离越接近 0，图片越相似")
            ForEach(Array(model.results.enumerated()), id: \.element.id) { index, hit in
                if let record = model.record(for: hit),
                   let image = storedImage(record, maxPixelSize: 1_000) {
                    VStack(alignment: .leading, spacing: 10) {
                        HStack {
                            Text("#\(index + 1)")
                                .font(.subheadline.weight(.bold))
                                .foregroundStyle(index == 0 ? DemoPalette.cyan : .white.opacity(0.66))
                            if index == 0 {
                                Text("最接近")
                                    .font(.caption2.weight(.bold))
                                    .padding(.horizontal, 9)
                                    .padding(.vertical, 5)
                                    .foregroundStyle(DemoPalette.background)
                                    .background(DemoPalette.cyan, in: Capsule())
                            }
                            Spacer()
                            Label("距离 \(String(format: "%.4f", hit.distance))",
                                  systemImage: "arrow.down")
                                .font(.caption.monospacedDigit().weight(.semibold))
                                .foregroundStyle(index == 0 ? DemoPalette.cyan : .white.opacity(0.58))
                        }
                        Image(uiImage: image)
                            .resizable()
                            .scaledToFit()
                            .frame(maxWidth: .infinity)
                            .frame(maxHeight: 260)
                            .background(.white, in: RoundedRectangle(cornerRadius: 12))
                    }
                    if index < model.results.count - 1 {
                        Divider().overlay(.white.opacity(0.08))
                    }
                }
            }
        }
        .demoCard()
    }

    private var planSection: some View {
        VStack(alignment: .leading, spacing: 10) {
            sectionTitle("执行计划", detail: "确认查询使用用户图片的 VexDB 图索引")
            Text(model.queryPlan)
                .font(.caption.monospaced())
                .foregroundStyle(DemoPalette.cyan)
                .fixedSize(horizontal: false, vertical: true)
        }
        .demoCard()
    }

    private var privacyNote: some View {
        Label("图片和查询文字会发送给你设置的 Embedding API；原图、向量、SQLite 图索引和搜索结果保留在本机。",
              systemImage: "network.badge.shield.half.filled")
            .font(.caption)
            .foregroundStyle(.white.opacity(0.43))
            .fixedSize(horizontal: false, vertical: true)
    }

    private func imageGrid(images: [PendingCustomImage]) -> some View {
        LazyVGrid(columns: [GridItem(.flexible()), GridItem(.flexible())], spacing: 10) {
            ForEach(images) { item in
                if let image = ImageAssetCache.shared.image(data: item.data,
                                                            key: item.id.uuidString,
                                                            maxPixelSize: 640) {
                    Image(uiImage: image)
                        .resizable()
                        .scaledToFill()
                        .frame(maxWidth: .infinity)
                        .frame(height: 132)
                        .clipShape(RoundedRectangle(cornerRadius: 14))
                }
            }
        }
    }

    private func storedImage(_ record: CustomImageRecord,
                             maxPixelSize: CGFloat) -> UIImage? {
        ImageAssetCache.shared.image(at: model.imageURL(for: record),
                                     maxPixelSize: maxPixelSize)
    }

    private func sectionTitle(_ title: String, detail: String) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(title).font(.headline)
            Text(detail).font(.caption).foregroundStyle(.white.opacity(0.45))
        }
    }

    private func startImport() {
        do {
            try settings.save()
            let configuration = try settings.configuration()
            Task { await model.addSelected(configuration: configuration) }
        } catch {
            model.errorMessage = error.localizedDescription
            showSettings = true
        }
    }

    private func startSearch() {
        queryFocused = false
        do {
            try settings.save()
            let configuration = try settings.configuration()
            Task { await model.search(configuration: configuration) }
        } catch {
            model.errorMessage = error.localizedDescription
            showSettings = true
        }
    }
}

struct ImageSettingsView: View {
    @ObservedObject var settings: ImageEmbeddingSettingsModel
    @Environment(\.dismiss) private var dismiss
    @State private var saveError: String?
    var body: some View {
        NavigationStack {
            Form {
                Section("图片 Embedding API") {
                    TextField("API 地址", text: $settings.endpoint)
                        .textInputAutocapitalization(.never).autocorrectionDisabled()
                    TextField("模型", text: $settings.model)
                        .textInputAutocapitalization(.never).autocorrectionDisabled()
                    SecureField("API Key", text: $settings.apiKey)
                        .textInputAutocapitalization(.never).autocorrectionDisabled()
                    Text("默认使用多模态向量接口：图片以 data URL 发送，返回 output.embeddings[0].embedding。")
                        .font(.caption).foregroundStyle(.secondary)
                }
                Section {
                    Button { Task { await test() } } label: {
                        Label(settings.isTesting ? "正在测试…" : "测试图片向量", systemImage: "checkmark.seal")
                    }.disabled(settings.isTesting)
                    if let message = settings.message {
                        Text(message).foregroundStyle(settings.succeeded ? .green : .orange)
                    }
                }
            }
            .navigationTitle("图片设置")
            .toolbar { ToolbarItem(placement: .topBarTrailing) { Button("完成") { dismiss() } } }
        }
        .alert("无法保存设置", isPresented: Binding(get: { saveError != nil }, set: { if !$0 { saveError = nil } })) {
            Button("知道了", role: .cancel) {}
        } message: { Text(saveError ?? "未知错误") }
        .preferredColorScheme(.dark)
    }
    private func test() async { do { try settings.save(); await settings.test(imageData: DemoImageFactory.sampleData()) } catch let failure { saveError = failure.localizedDescription } }
}
