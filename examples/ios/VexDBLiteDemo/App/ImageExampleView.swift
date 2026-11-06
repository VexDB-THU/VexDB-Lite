import SwiftUI

struct ImageExampleView: View {
    @StateObject private var model = BundledImageExampleViewModel()

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: 24) {
                    header
                    gallery
                    querySection
                    if !model.results.isEmpty { resultsSection }
                    if model.queryPlan != "查询后显示执行计划" &&
                        model.queryPlan != "点击发送后显示执行计划" { planSection }
                    privacyNote
                }
                .padding(.horizontal, 18)
                .padding(.bottom, 36)
            }
            .background(DemoPalette.background.ignoresSafeArea())
            .foregroundStyle(.white)
            .toolbar(.hidden, for: .navigationBar)
        }
        .task {
            await model.prepare()
            if ProcessInfo.processInfo.arguments.contains("--image-query-smoke"),
               let first = model.queries.first {
                model.select(first)
                await model.search()
            }
        }
        .alert("默认图片示例", isPresented: Binding(
            get: { model.errorMessage != nil },
            set: { if !$0 { model.errorMessage = nil } }
        )) {
            Button("知道了", role: .cancel) {}
        } message: {
            Text(model.errorMessage ?? "未知错误")
        }
    }

    private var header: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Label("IMAGE SEARCH", systemImage: "photo.on.rectangle.angled")
                    .font(.caption2.weight(.bold)).tracking(1)
                    .foregroundStyle(DemoPalette.cyan)
                Spacer()
                if model.isBusy { ProgressView().tint(DemoPalette.cyan) }
                else { Image(systemName: "checkmark.seal.fill").foregroundStyle(DemoPalette.cyan) }
            }
            Text("默认图片示例")
                .font(.system(size: 32, weight: .bold, design: .rounded))
            Text("选择一个问题，在全部内置图片中搜索最相似的结果。")
                .foregroundStyle(.white.opacity(0.62))
            HStack(spacing: 8) {
                Circle().fill(model.errorMessage == nil ? DemoPalette.cyan : .orange)
                    .frame(width: 8, height: 8)
                Text(model.status).font(.subheadline).foregroundStyle(.white.opacity(0.68))
            }
        }
        .padding(.top, 25)
    }

    private var gallery: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("内置图片库").font(.headline)
            HStack(spacing: 16) {
                Label("\(model.images.count) 张", systemImage: "photo.stack")
                Label("\(model.dimensions) 维", systemImage: "point.3.filled.connected.trianglepath.dotted")
                Text(model.modelName)
            }
            .font(.caption).foregroundStyle(.white.opacity(0.46))
            ScrollView(.horizontal, showsIndicators: false) {
                LazyHStack(spacing: 14) {
                    ForEach(model.images) { item in
                        ZStack {
                            Color.white
                            if let image = bundledImage(item.resource) {
                                Image(uiImage: image).resizable().scaledToFit().padding(10)
                            }
                        }
                        .frame(width: 278, height: 178)
                        .clipShape(RoundedRectangle(cornerRadius: 14))
                    }
                }
                .scrollTargetLayout()
            }
            .scrollTargetBehavior(.viewAligned)
            Text("图片及其向量已随 App 打包，无需选择图片或填写 API Key。")
                .font(.caption).foregroundStyle(.white.opacity(0.44))
        }
    }

    private var querySection: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("选择一个示例问题").font(.headline)
            Text("先选择问题，再点击发送；查询全部图片并展示最接近的 5 张。")
                .font(.caption).foregroundStyle(.white.opacity(0.46))
            VStack(alignment: .leading, spacing: 6) {
                ForEach(model.queries) { query in
                    Button { model.select(query) } label: {
                        HStack(spacing: 10) {
                            Image(systemName: model.selectedQuery == query.text
                                  ? "checkmark.circle.fill" : "questionmark.circle")
                                .foregroundStyle(model.selectedQuery == query.text
                                                 ? DemoPalette.cyan : .white.opacity(0.40))
                            Text(query.text)
                                .font(.subheadline.weight(.medium))
                                .foregroundStyle(.white.opacity(0.84))
                                .multilineTextAlignment(.leading)
                            Spacer(minLength: 8)
                        }
                        .padding(.vertical, 8)
                    }
                    .buttonStyle(.plain)
                    .disabled(model.isBusy || model.images.isEmpty)
                }
            }
            Button { Task { await model.search() } } label: {
                HStack {
                    Spacer()
                    Label(model.isBusy ? "正在搜索…" : "发送并搜索全部图片",
                          systemImage: "arrow.up.circle.fill")
                        .font(.subheadline.weight(.semibold))
                    Spacer()
                }
                .padding(.vertical, 13)
            }
            .buttonStyle(.borderedProminent)
            .tint(DemoPalette.violet)
            .disabled(model.isBusy || model.selectedQuery.isEmpty || model.images.isEmpty)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .demoCard()
    }

    private var resultsSection: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("搜索结果").font(.headline)
            Label("距离越接近 0，图片越相似；最接近的结果排在最前面。",
                  systemImage: "arrow.down.right.circle.fill")
                .font(.caption.weight(.medium)).foregroundStyle(DemoPalette.cyan)
            ForEach(Array(model.results.enumerated()), id: \.element.id) { index, hit in
                if let item = model.image(for: hit), let image = bundledImage(item.resource) {
                    VStack(alignment: .leading, spacing: 10) {
                        HStack {
                            Text("#\(index + 1)")
                                .font(.subheadline.weight(.bold))
                                .foregroundStyle(index == 0 ? DemoPalette.cyan : .white.opacity(0.66))
                            if index == 0 {
                                Text("最接近")
                                    .font(.caption2.weight(.bold))
                                    .padding(.horizontal, 9).padding(.vertical, 5)
                                    .foregroundStyle(DemoPalette.background)
                                    .background(DemoPalette.cyan, in: Capsule())
                            }
                            Spacer()
                            HStack(spacing: 5) {
                                Image(systemName: "arrow.down")
                                Text("距离 \(String(format: "%.4f", hit.distance))")
                            }
                            .font(.caption.monospaced().weight(.semibold))
                            .foregroundStyle(index == 0 ? DemoPalette.cyan : .white.opacity(0.58))
                            .padding(.horizontal, 9).padding(.vertical, 6)
                            .background(.white.opacity(0.07), in: Capsule())
                        }
                        Image(uiImage: image)
                            .resizable().scaledToFit()
                            .frame(maxWidth: .infinity).frame(maxHeight: 210)
                            .padding(8).background(.white, in: RoundedRectangle(cornerRadius: 12))
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
            Text("执行计划").font(.headline)
            Text(model.queryPlan).font(.caption.monospaced()).foregroundStyle(DemoPalette.cyan)
        }
        .demoCard()
    }

    private var privacyNote: some View {
        Label("图片和示例问题向量均已内置；相似度检索完全在本机完成。",
              systemImage: "lock.shield.fill")
            .font(.caption).foregroundStyle(.white.opacity(0.43))
    }

    private func bundledImage(_ resource: String) -> UIImage? {
        ImageAssetCache.shared.bundledImage(resource: resource)
    }
}
