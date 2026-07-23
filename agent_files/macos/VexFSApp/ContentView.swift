import SwiftUI

struct ContentView: View {
    private var databasePath: String {
        FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/Application Support/VexDB-Lite/default.sqlite3").path
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 18) {
            Label("VexDB-Lite", systemImage: "externaldrive.connected.to.line.below")
                .font(.system(size: 32, weight: .semibold))
            Text("一个安装包，提供 SQLite、向量检索和 VexFS 文件能力。")
                .font(.title3)
            GroupBox("启用步骤") {
                VStack(alignment: .leading, spacing: 10) {
                    Text("1. 打开 系统设置 → 通用 → 登录项与扩展，滚动到扩展区域，切换到“按类别”，点“文件系统扩展”右侧 ⓘ，再打开 VexDB Lite。")
                    Text("2. 运行 vexdb fs setup 初始化数据库，再用 vexdb fs doctor 检查状态。")
                    Text("3. 运行 vexdb fs mount ~/VexDB 挂载，然后直接使用 ls、cat、grep、cp、mv 等命令。")
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(4)
            }
            Text("默认数据库：\(databasePath)")
                .font(.system(.footnote, design: .monospaced))
                .textSelection(.enabled)
                .foregroundStyle(.secondary)
            Text("如果看到的是“VexDB Lite / FSKit Modules”只读详情，说明当前在“按 App”视图。返回并切换到“按类别”后启用。")
                .font(.footnote)
                .foregroundStyle(.secondary)
            Spacer()
        }
        .padding(28)
    }
}
