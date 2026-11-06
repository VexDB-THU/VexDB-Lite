import SwiftUI
import Network
import OSLog
import AppKit
import Darwin

private struct LocalNetworkProbeRequest: Codable {
    let version: Int
    let nonce: String
    let host: String
    let port: Int
}

private struct LocalNetworkProbeResponse: Codable {
    let version: Int
    let nonce: String
    let allowed: Bool
    let detail: String
}

final class LocalNetworkAccess: ObservableObject {
    @Published private(set) var status = "尚未检查局域网权限"
    private var browser: NWBrowser?
    private var probeConnection: NWConnection?
    private var probeNonce: String?
    private var probeDirectorySource: DispatchSourceFileSystemObject?
    private let logger = Logger(subsystem: "io.vexdb.vexfs", category: "LocalNetwork")

    private var supportDirectory: URL {
        FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/Application Support/VexDB-Lite", isDirectory: true)
    }

    private var probeRequestURL: URL {
        supportDirectory.appendingPathComponent("local-network-probe.json")
    }

    private var probeResponseURL: URL {
        supportDirectory.appendingPathComponent("local-network-probe-response.json")
    }

    init() {
        do {
            try FileManager.default.createDirectory(
                at: supportDirectory, withIntermediateDirectories: true,
                attributes: [.posixPermissions: 0o700])
            let descriptor = open(supportDirectory.path, O_EVTONLY | O_CLOEXEC)
            if descriptor >= 0 {
                let source = DispatchSource.makeFileSystemObjectSource(
                    fileDescriptor: descriptor,
                    eventMask: [.write, .extend, .attrib, .rename],
                    queue: .main)
                source.setEventHandler { [weak self] in
                    self?.processPendingProbe()
                }
                source.setCancelHandler {
                    close(descriptor)
                }
                probeDirectorySource = source
                source.resume()
            }
        } catch {
            logger.error("cannot watch local network requests: \(error.localizedDescription, privacy: .public)")
        }
        DispatchQueue.main.async { [weak self] in
            self?.processPendingProbe()
        }
    }

    private func updateStatus(_ value: String) {
        status = value
        UserDefaults.standard.set(value, forKey: "localNetworkStatus")
        logger.notice("local network status: \(value, privacy: .public)")
    }

    func request() {
        browser?.cancel()
        updateStatus("正在请求局域网权限…")
        let parameters = NWParameters()
        parameters.includePeerToPeer = true
        let browser = NWBrowser(
            for: .bonjour(type: "_vexdb._tcp", domain: nil), using: parameters)
        self.browser = browser
        browser.stateUpdateHandler = { [weak self] state in
            DispatchQueue.main.async {
                guard let self else { return }
                switch state {
                case .ready:
                    self.updateStatus("局域网检查已就绪，请确认系统设置中的 VexDB Lite 已打开")
                case .failed(let error):
                    self.updateStatus("局域网权限不可用：\(error.localizedDescription)")
                case .waiting(let error):
                    self.updateStatus("等待局域网权限：\(error.localizedDescription)")
                case .cancelled:
                    break
                default:
                    self.updateStatus("正在请求局域网权限…")
                }
            }
        }
        browser.start(queue: DispatchQueue(label: "io.vexdb.local-network-access"))
    }

    private func finishProbe(_ request: LocalNetworkProbeRequest, allowed: Bool,
                             detail: String) {
        guard probeNonce == request.nonce else { return }
        probeConnection?.cancel()
        probeConnection = nil
        probeNonce = nil
        let response = LocalNetworkProbeResponse(
            version: 1, nonce: request.nonce, allowed: allowed, detail: detail)
        do {
            try FileManager.default.createDirectory(
                at: supportDirectory, withIntermediateDirectories: true,
                attributes: [.posixPermissions: 0o700])
            let data = try JSONEncoder().encode(response)
            try data.write(to: probeResponseURL, options: .atomic)
            try FileManager.default.setAttributes(
                [.posixPermissions: 0o600], ofItemAtPath: probeResponseURL.path)
        } catch {
            logger.error("cannot write local network response: \(error.localizedDescription, privacy: .public)")
        }
        updateStatus(allowed
            ? "局域网连接已允许：\(request.host):\(request.port)"
            : "局域网连接不可用：\(detail)")
    }

    func processPendingProbe() {
        guard probeNonce == nil,
              FileManager.default.fileExists(atPath: probeRequestURL.path) else { return }
        do {
            let attributes = try FileManager.default.attributesOfItem(atPath: probeRequestURL.path)
            guard let permissions = attributes[.posixPermissions] as? NSNumber,
                  permissions.intValue & 0o077 == 0,
                  let owner = attributes[.ownerAccountID] as? NSNumber,
                  owner.uint32Value == getuid() else {
                try? FileManager.default.removeItem(at: probeRequestURL)
                updateStatus("局域网检查请求的文件权限不安全")
                return
            }
            let data = try Data(contentsOf: probeRequestURL)
            let request = try JSONDecoder().decode(LocalNetworkProbeRequest.self, from: data)
            try? FileManager.default.removeItem(at: probeRequestURL)
            guard request.version == 1, !request.nonce.isEmpty,
                  !request.host.isEmpty, request.host.count <= 253,
                  (1...65535).contains(request.port),
                  let port = NWEndpoint.Port(rawValue: UInt16(request.port)) else {
                updateStatus("局域网检查请求无效")
                return
            }

            probeNonce = request.nonce
            updateStatus("正在连接 \(request.host):\(request.port)，请在系统提示中点“允许”…")
            let connection = NWConnection(host: NWEndpoint.Host(request.host), port: port, using: .tcp)
            probeConnection = connection
            connection.stateUpdateHandler = { [weak self] state in
                DispatchQueue.main.async {
                    guard let self, self.probeNonce == request.nonce else { return }
                    switch state {
                    case .ready:
                        self.finishProbe(request, allowed: true, detail: "connected")
                    case .failed(let error):
                        self.finishProbe(request, allowed: false,
                                         detail: error.localizedDescription)
                    case .waiting(let error):
                        self.updateStatus("等待局域网权限：\(error.localizedDescription)")
                    case .cancelled:
                        break
                    default:
                        break
                    }
                }
            }
            connection.start(queue: DispatchQueue(label: "io.vexdb.local-network-probe"))
            DispatchQueue.main.asyncAfter(deadline: .now() + 30) { [weak self] in
                guard let self, self.probeNonce == request.nonce else { return }
                self.finishProbe(request, allowed: false,
                                 detail: "等待系统授权超时，请在本地网络设置中允许 VexDB Lite")
            }
        } catch {
            try? FileManager.default.removeItem(at: probeRequestURL)
            updateStatus("局域网检查请求失败：\(error.localizedDescription)")
        }
    }
}

struct ContentView: View {
    @StateObject private var localNetwork = LocalNetworkAccess()
    private let probeTimer = Timer.publish(every: 0.5, on: .main, in: .common).autoconnect()

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
            GroupBox("跨电脑 PostgreSQL 工作区") {
                HStack(spacing: 12) {
                    VStack(alignment: .leading, spacing: 4) {
                        Text(localNetwork.status)
                        Text("只有连接局域网中的 PostgreSQL 时需要此权限；本机 SQLite 不需要。")
                            .font(.footnote)
                            .foregroundStyle(.secondary)
                    }
                    Spacer()
                    Button("检查权限") {
                        localNetwork.request()
                    }
                    Button("打开系统设置") {
                        if let url = URL(string:
                            "x-apple.systempreferences:com.apple.preference.security?Privacy_LocalNetwork") {
                            NSWorkspace.shared.open(url)
                        }
                    }
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
        .onAppear {
            localNetwork.request()
            localNetwork.processPendingProbe()
        }
        .onReceive(probeTimer) { _ in
            localNetwork.processPendingProbe()
        }
    }
}
