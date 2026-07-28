import SwiftUI

struct SettingsView: View {
    @ObservedObject var settings: EmbeddingSettingsModel
    @Environment(\.dismiss) private var dismiss
    @State private var saveMessage: String?

    private let background = Color(red: 0.035, green: 0.055, blue: 0.105)
    private let panel = Color.white.opacity(0.07)
    private let cyan = Color(red: 0.25, green: 0.91, blue: 0.88)

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: 24) {
                    VStack(alignment: .leading, spacing: 8) {
                        Text("Embedding 设置")
                            .font(.system(size: 30, weight: .bold, design: .rounded))
                        Text("默认使用 text-embedding-v4，也支持其他 OpenAI-compatible 接口")
                            .foregroundStyle(.white.opacity(0.58))
                    }

                    VStack(alignment: .leading, spacing: 18) {
                        settingField("API 地址", hint: "支持 OpenAI 兼容接口") {
                            TextField("https://…/v1", text: $settings.endpoint)
                                .keyboardType(.URL)
                                .textInputAutocapitalization(.never)
                                .autocorrectionDisabled()
                        }
                        settingField("模型", hint: "例如 text-embedding-v4") {
                            TextField("text-embedding-v4", text: $settings.model)
                                .textInputAutocapitalization(.never)
                                .autocorrectionDisabled()
                        }
                        settingField("API Key", hint: "只保存在系统 Keychain") {
                            SecureField("sk-…", text: $settings.apiKey)
                                .textInputAutocapitalization(.never)
                                .autocorrectionDisabled()
                        }
                    }

                    if let message = settings.testMessage {
                        Label(message,
                              systemImage: settings.testSucceeded ? "checkmark.circle.fill" : "exclamationmark.triangle.fill")
                            .font(.subheadline)
                            .foregroundStyle(settings.testSucceeded ? cyan : .orange)
                            .fixedSize(horizontal: false, vertical: true)
                    }

                    VStack(spacing: 12) {
                        Button {
                            Task { await settings.testConnection() }
                        } label: {
                            HStack {
                                if settings.isTesting { ProgressView().tint(background) }
                                Text(settings.isTesting ? "正在生成…" : "保存并测试向量")
                            }
                            .frame(maxWidth: .infinity)
                            .padding(.vertical, 14)
                            .foregroundStyle(background)
                            .background(cyan, in: RoundedRectangle(cornerRadius: 15))
                        }
                        .disabled(settings.isTesting)

                        Button("仅保存设置") {
                            do {
                                try settings.save()
                                dismiss()
                            } catch {
                                saveMessage = error.localizedDescription
                            }
                        }
                        .foregroundStyle(.white.opacity(0.68))
                    }

                    Label("导入文本和查询问题会发送给你配置的 API 生成向量；原文、向量索引和相似度检索保存在本机。",
                          systemImage: "lock.shield.fill")
                        .font(.caption)
                        .foregroundStyle(.white.opacity(0.48))
                        .fixedSize(horizontal: false, vertical: true)
                }
                .padding(20)
            }
            .background(background.ignoresSafeArea())
            .foregroundStyle(.white)
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    Button("完成") { dismiss() }.foregroundStyle(cyan)
                }
            }
        }
        .alert("无法保存设置", isPresented: Binding(
            get: { saveMessage != nil },
            set: { if !$0 { saveMessage = nil } }
        )) {
            Button("知道了", role: .cancel) {}
        } message: {
            Text(saveMessage ?? "未知错误")
        }
        .preferredColorScheme(.dark)
    }

    private func settingField<Content: View>(_ title: String, hint: String,
                                             @ViewBuilder content: () -> Content) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(title).font(.subheadline.weight(.semibold))
            content()
                .padding(.horizontal, 13)
                .padding(.vertical, 12)
                .background(panel, in: RoundedRectangle(cornerRadius: 13))
            Text(hint).font(.caption2).foregroundStyle(.white.opacity(0.42))
        }
    }
}
