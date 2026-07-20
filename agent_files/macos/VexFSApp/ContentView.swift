import SwiftUI

struct ContentView: View {
    private var databasePath: String {
        FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/Application Support/VexFS/vexfs.sqlite3").path
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 18) {
            Label("VexFS", systemImage: "externaldrive.connected.to.line.below")
                .font(.system(size: 32, weight: .semibold))
            Text("由 SQLite 管理、可直接通过 Bash 使用的文件系统。")
                .font(.title3)
            GroupBox("启用步骤") {
                VStack(alignment: .leading, spacing: 10) {
                    Text("1. 在 系统设置 → 通用 → 登录项与扩展 → 文件系统扩展 中启用 VexFS。")
                    Text("2. 运行 vexfs setup 初始化数据库，再用 vexfs doctor 检查状态。")
                    Text("3. 运行 vexfs mount ~/VexFS 挂载，然后直接使用 ls、cat、grep、cp、mv 等命令。")
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(4)
            }
            Text("默认数据库：\(databasePath)")
                .font(.system(.footnote, design: .monospaced))
                .textSelection(.enabled)
                .foregroundStyle(.secondary)
            Spacer()
        }
        .padding(28)
    }
}
