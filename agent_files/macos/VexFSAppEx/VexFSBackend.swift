import Foundation
import OSLog

struct VexFSDescriptor: Codable {
    let version: Int
    let backend: String
    let connection: String
    let workspace: String
}

struct VexFSStatRecord: Codable {
    let path: String
    let inode: UInt64
    let kind: String
    let mode: UInt32
    let size: UInt64
    let version: UInt64
    let created_at: Int64
    let accessed_at: Int64?
    let updated_at: Int64
    let changed_at: Int64?
    let uid: UInt32?
    let gid: UInt32?
    let link_count: UInt32?

    init(path: String, inode: UInt64, kind: String, mode: UInt32, size: UInt64,
         version: UInt64, created_at: Int64, accessed_at: Int64? = nil,
         updated_at: Int64, changed_at: Int64? = nil,
         uid: UInt32? = nil, gid: UInt32? = nil, link_count: UInt32? = nil) {
        self.path = path
        self.inode = inode
        self.kind = kind
        self.mode = mode
        self.size = size
        self.version = version
        self.created_at = created_at
        self.accessed_at = accessed_at
        self.updated_at = updated_at
        self.changed_at = changed_at
        self.uid = uid
        self.gid = gid
        self.link_count = link_count
    }
}

struct VexFSDirectoryEntry: Codable {
    let name: String
    let inode: UInt64
    let kind: String
    let size: UInt64
    let version: UInt64
    let mode: UInt32
    let created_at: Int64
    let accessed_at: Int64?
    let updated_at: Int64
    let changed_at: Int64?
    let uid: UInt32?
    let gid: UInt32?
    let link_count: UInt32?

    init(name: String, inode: UInt64, kind: String, size: UInt64, version: UInt64,
         mode: UInt32, created_at: Int64, accessed_at: Int64? = nil,
         updated_at: Int64, changed_at: Int64? = nil,
         uid: UInt32? = nil, gid: UInt32? = nil, link_count: UInt32? = nil) {
        self.name = name
        self.inode = inode
        self.kind = kind
        self.size = size
        self.version = version
        self.mode = mode
        self.created_at = created_at
        self.accessed_at = accessed_at
        self.updated_at = updated_at
        self.changed_at = changed_at
        self.uid = uid
        self.gid = gid
        self.link_count = link_count
    }
}

struct VexFSDirectoryListing: Codable {
    let version: UInt64
    let entries: [VexFSDirectoryEntry]
}

final class VexFSBackend {
    private var session: OpaquePointer?
    private let backend: String
    private let connection: String
    private let workspace: String

    init(backend: String, connection: String, workspace: String) throws {
        self.backend = backend
        self.connection = connection
        self.workspace = workspace
        try reopen()
    }

    func reopen() throws {
        guard session == nil else { return }
        var config = vexfs_mount_config()
        config.abi_version = UInt32(VEXFS_RUNTIME_ABI_VERSION)
        config.operation_timeout_ms = 5_000
        config.flags = UInt32(VEXFS_RUNTIME_EXCLUSIVE_GATEWAY)
        var opened: OpaquePointer?
        var error = vexfs_mount_error()
        let principal = backend == "sqlite" ? "local" : ""
        let status = backend.withCString { backendPointer in
            connection.withCString { connectionPointer in
                workspace.withCString { workspacePointer in
                    principal.withCString { principalPointer in
                        config.backend = backendPointer
                        config.connection = connectionPointer
                        config.workspace = workspacePointer
                        config.principal = backend == "sqlite" ? principalPointer : nil
                        return vexfs_mount_session_open(&config, &opened, &error)
                    }
                }
            }
        }
        try Self.check(status, error: &error)
        session = opened
    }

    deinit {
        close()
    }

    func close() {
        guard let activeSession = session else { return }
        session = nil
        vexfs_mount_session_close(activeSession)
    }

    func stat(path: String) throws -> VexFSStatRecord {
        var output = vexfs_mount_bytes()
        var error = vexfs_mount_error()
        let status = path.withCString { vexfs_mount_stat(session, $0, &output, &error) }
        try Self.check(status, error: &error)
        return try JSONDecoder().decode(VexFSStatRecord.self, from: Self.take(&output))
    }

    func path(inode: UInt64) throws -> String {
        var output = vexfs_mount_bytes()
        var error = vexfs_mount_error()
        let status = vexfs_mount_path_for_inode(session, Int64(bitPattern: inode), &output, &error)
        try Self.check(status, error: &error)
        guard let value = String(data: Self.take(&output), encoding: .utf8) else {
            throw POSIXError(.EIO)
        }
        return value
    }

    func list(path: String) throws -> [VexFSDirectoryEntry] {
        var output = vexfs_mount_bytes()
        var error = vexfs_mount_error()
        let status = path.withCString { vexfs_mount_list(session, $0, &output, &error) }
        try Self.check(status, error: &error)
        return try JSONDecoder().decode([VexFSDirectoryEntry].self, from: Self.take(&output))
    }

    func listVersioned(path: String) throws -> VexFSDirectoryListing {
        var output = vexfs_mount_bytes()
        var error = vexfs_mount_error()
        let status = path.withCString {
            vexfs_mount_list_versioned(session, $0, &output, &error)
        }
        try Self.check(status, error: &error)
        return try JSONDecoder().decode(VexFSDirectoryListing.self, from: Self.take(&output))
    }

    func mkdir(path: String) throws {
        var error = vexfs_mount_error()
        let status = path.withCString { vexfs_mount_mkdir(session, $0, &error) }
        try Self.check(status, error: &error)
    }

    func create(path: String, kind: String, mode: UInt32) throws {
        var error = vexfs_mount_error()
        let status = path.withCString { pathPointer in
            kind.withCString { kindPointer in
                vexfs_mount_create(session, pathPointer, kindPointer, mode, &error)
            }
        }
        do {
            try Self.check(status, error: &error)
        } catch let failure as POSIXError where failure.code == .EBUSY {
            throw POSIXError(.EEXIST)
        }
    }

    func setMode(inode: UInt64, mode: UInt32) throws {
        var error = vexfs_mount_error()
        let status = vexfs_mount_set_mode(session, Int64(bitPattern: inode), mode, &error)
        try Self.check(status, error: &error)
    }

    func setTimes(inode: UInt64, accessedAt: Int64, modifiedAt: Int64,
                  mask: UInt32) throws {
        var error = vexfs_mount_error()
        let status = vexfs_mount_set_times(session, Int64(bitPattern: inode), accessedAt,
                                           modifiedAt, mask, &error)
        try Self.check(status, error: &error)
    }

    func symlink(path: String, target: Data) throws {
        var error = vexfs_mount_error()
        let status = path.withCString { pathPointer in
            target.withUnsafeBytes { bytes in
                vexfs_mount_symlink(session, pathPointer, bytes.baseAddress,
                                    UInt64(bytes.count), &error)
            }
        }
        do {
            try Self.check(status, error: &error)
        } catch let failure as POSIXError where failure.code == .EBUSY {
            throw POSIXError(.EEXIST)
        }
    }

    func link(source: String, destination: String) throws {
        var error = vexfs_mount_error()
        let status = source.withCString { sourcePointer in
            destination.withCString { destinationPointer in
                vexfs_mount_link(session, sourcePointer, destinationPointer, &error)
            }
        }
        try Self.check(status, error: &error)
    }

    func chown(inode: UInt64, uid: Int64, gid: Int64) throws {
        var error = vexfs_mount_error()
        let status = vexfs_mount_chown(session, Int64(bitPattern: inode), uid, gid, &error)
        try Self.check(status, error: &error)
    }

    func readlink(inode: UInt64) throws -> Data {
        var output = vexfs_mount_bytes()
        var error = vexfs_mount_error()
        let status = vexfs_mount_readlink(session, Int64(bitPattern: inode), &output, &error)
        try Self.check(status, error: &error)
        return Self.take(&output)
    }

    @discardableResult
    func writeFile(path: String, data: Data) throws -> Int64 {
        var version: Int64 = 0
        var error = vexfs_mount_error()
        let status = path.withCString { pathPointer in
            data.withUnsafeBytes { bytes in
                vexfs_mount_write_file(session, pathPointer, bytes.baseAddress,
                                       UInt64(bytes.count), &version, &error)
            }
        }
        try Self.check(status, error: &error)
        return version
    }

    func readFile(path: String) throws -> Data {
        var output = vexfs_mount_bytes()
        var error = vexfs_mount_error()
        let status = path.withCString { vexfs_mount_read_file(session, $0, &output, &error) }
        try Self.check(status, error: &error)
        return Self.take(&output)
    }

    func move(source: String, destination: String) throws {
        var error = vexfs_mount_error()
        let status = source.withCString { sourcePointer in
            destination.withCString { destinationPointer in
                vexfs_mount_move(session, sourcePointer, destinationPointer, &error)
            }
        }
        try Self.check(status, error: &error)
    }

    func rename(source: String, destination: String, replace: Bool) throws {
        var error = vexfs_mount_error()
        let status = source.withCString { sourcePointer in
            destination.withCString { destinationPointer in
                vexfs_mount_rename(session, sourcePointer, destinationPointer,
                                   replace ? 1 : 0, &error)
            }
        }
        try Self.check(status, error: &error)
    }

    func remove(path: String, recursive: Bool = false) throws {
        var error = vexfs_mount_error()
        let status = path.withCString {
            vexfs_mount_remove(session, $0, recursive ? 1 : 0, &error)
        }
        try Self.check(status, error: &error)
    }

    func getXattr(inode: UInt64, name: String) throws -> Data {
        var output = vexfs_mount_bytes()
        var error = vexfs_mount_error()
        let status = name.withCString {
            vexfs_mount_xattr_get(session, Int64(bitPattern: inode), $0, &output, &error)
        }
        do {
            try Self.check(status, error: &error)
        } catch let failure as POSIXError where failure.code == .ENOENT {
            throw POSIXError(.ENOATTR)
        }
        return Self.take(&output)
    }

    func listXattrs(inode: UInt64) throws -> [String] {
        var output = vexfs_mount_bytes()
        var error = vexfs_mount_error()
        let status = vexfs_mount_xattr_list(session, Int64(bitPattern: inode), &output, &error)
        try Self.check(status, error: &error)
        return try JSONDecoder().decode([String].self, from: Self.take(&output))
    }

    func setXattr(inode: UInt64, name: String, value: Data?, policy: Int) throws {
        var error = vexfs_mount_error()
        let status = name.withCString { namePointer in
            if let value {
                return value.withUnsafeBytes { bytes in
                    vexfs_mount_xattr_set(session, Int64(bitPattern: inode), namePointer,
                                           bytes.baseAddress, UInt64(bytes.count), Int32(policy),
                                           &error)
                }
            }
            return vexfs_mount_xattr_set(session, Int64(bitPattern: inode), namePointer,
                                         nil, 0, Int32(policy), &error)
        }
        do {
            try Self.check(status, error: &error)
        } catch let failure as POSIXError {
            if policy == 1 && failure.code == .EBUSY { throw POSIXError(.EEXIST) }
            if (policy == 2 || policy == 3) && failure.code == .ENOENT {
                throw POSIXError(.ENOATTR)
            }
            throw failure
        }
    }

    func openHandle(path: String, flags: String) throws -> String {
        var output = vexfs_mount_bytes()
        var error = vexfs_mount_error()
        let request = UUID().uuidString
        let status = path.withCString { pathPointer in
            flags.withCString { flagsPointer in
                request.withCString { requestPointer in
                    vexfs_mount_handle_open(session, pathPointer, flagsPointer, requestPointer,
                                            &output, &error)
                }
            }
        }
        try Self.check(status, error: &error)
        guard let value = String(data: Self.take(&output), encoding: .utf8) else {
            throw POSIXError(.EIO)
        }
        return value
    }

    func createFileHandle(path: String, mode: UInt32) throws -> String {
        var output = vexfs_mount_bytes()
        var error = vexfs_mount_error()
        let request = UUID().uuidString
        let status = path.withCString { pathPointer in
            request.withCString { requestPointer in
                vexfs_mount_handle_create(session, pathPointer, mode, requestPointer,
                                          &output, &error)
            }
        }
        do {
            try Self.check(status, error: &error)
        } catch let failure as POSIXError where failure.code == .EBUSY {
            throw POSIXError(.EEXIST)
        }
        guard let value = String(data: Self.take(&output), encoding: .utf8) else {
            throw POSIXError(.EIO)
        }
        do {
            // handle_create prepares an empty private generation.  POSIX create
            // must make the directory entry visible before FSKit asks for the
            // returned item's attributes, so publish generation 1 now and keep
            // the same handle open for a later write.
            _ = try publish(handle: value, generation: 1)
        } catch {
            try? closeHandle(value, retain: true)
            throw error
        }
        return value
    }

    func stageWrite(handle: String, offset: UInt64, data: Data) throws -> Int64 {
        var generation: Int64 = 0
        var error = vexfs_mount_error()
        let request = UUID().uuidString
        let status = handle.withCString { handlePointer in
            request.withCString { requestPointer in
                data.withUnsafeBytes { bytes in
                    vexfs_mount_handle_stage_write(session, handlePointer, offset,
                        bytes.baseAddress, UInt64(bytes.count), requestPointer, &generation, &error)
                }
            }
        }
        try Self.check(status, error: &error)
        return generation
    }

    func truncate(handle: String, size: UInt64) throws -> Int64 {
        var generation: Int64 = 0
        var error = vexfs_mount_error()
        let request = UUID().uuidString
        let status = handle.withCString { handlePointer in
            request.withCString { requestPointer in
                vexfs_mount_handle_truncate(session, handlePointer, size, requestPointer,
                                            &generation, &error)
            }
        }
        try Self.check(status, error: &error)
        return generation
    }

    func readHandle(handle: String, offset: UInt64, length: UInt64) throws -> Data {
        var output = vexfs_mount_bytes()
        var error = vexfs_mount_error()
        let status = handle.withCString {
            vexfs_mount_handle_read(session, $0, offset, length, &output, &error)
        }
        try Self.check(status, error: &error)
        return Self.take(&output)
    }

    func publish(handle: String, generation: Int64) throws -> Int64 {
        var version: Int64 = 0
        var error = vexfs_mount_error()
        let request = UUID().uuidString
        let status = handle.withCString { handlePointer in
            "data".withCString { durabilityPointer in
                request.withCString { requestPointer in
                    vexfs_mount_handle_publish(session, handlePointer, generation,
                        durabilityPointer, requestPointer, &version, &error)
                }
            }
        }
        try Self.check(status, error: &error)
        return version
    }

    func publishAndClose(handle: String, generation: Int64) throws -> Int64 {
        var version: Int64 = 0
        var error = vexfs_mount_error()
        let status = handle.withCString { handlePointer in
            "data".withCString { durabilityPointer in
                vexfs_mount_handle_publish_close(session, handlePointer, generation,
                                                 durabilityPointer, &version, &error)
            }
        }
        try Self.check(status, error: &error)
        return version
    }

    func closeHandle(_ handle: String, retain: Bool) throws {
        var output = vexfs_mount_bytes()
        var error = vexfs_mount_error()
        let request = UUID().uuidString
        let status = handle.withCString { handlePointer in
            request.withCString { requestPointer in
                vexfs_mount_handle_close(session, handlePointer, retain ? 1 : 0,
                                         requestPointer, &output, &error)
            }
        }
        try Self.check(status, error: &error)
        _ = Self.take(&output)
    }

    func synchronize() throws {
        var published: Int64 = 0
        var error = vexfs_mount_error()
        let request = UUID().uuidString
        let status = request.withCString {
            vexfs_mount_synchronize(session, $0, &published, &error)
        }
        try Self.check(status, error: &error)
    }

    func refreshVisibility() throws -> (head: Int64, generation: UInt64, external: Bool) {
        var visibility = vexfs_mount_visibility()
        var error = vexfs_mount_error()
        let status = vexfs_mount_refresh_visibility(session, &visibility, &error)
        try Self.check(status, error: &error)
        return (visibility.workspace_head, visibility.cache_generation,
                visibility.external_commit != 0)
    }

    func reclaim() throws {
        var reclaimed: Int64 = 0
        var error = vexfs_mount_error()
        let request = UUID().uuidString
        let status = request.withCString { vexfs_mount_reclaim(session, $0, &reclaimed, &error) }
        try Self.check(status, error: &error)
    }

    private static func take(_ bytes: inout vexfs_mount_bytes) -> Data {
        guard let pointer = bytes.data, bytes.size > 0 else {
            vexfs_mount_free(bytes.data)
            bytes.data = nil
            bytes.size = 0
            return Data()
        }
        let data = Data(bytes: pointer, count: Int(bytes.size))
        vexfs_mount_free(pointer)
        bytes.data = nil
        bytes.size = 0
        return data
    }

    private static func check(_ status: vexfs_mount_status,
                              error: inout vexfs_mount_error) throws {
        guard status != VEXFS_MOUNT_OK else { return }
        let detail = withUnsafePointer(to: &error.message) { pointer in
            pointer.withMemoryRebound(to: CChar.self, capacity: 512) {
                String(cString: $0)
            }
        }
        if !detail.isEmpty {
            Logger.vexfs.error(
                "runtime rejected operation: \(detail, privacy: .public)")
        }
        let code: POSIXError.Code
        switch status {
        case VEXFS_MOUNT_NOT_FOUND: code = .ENOENT
        case VEXFS_MOUNT_CONFLICT: code = .EBUSY
        case VEXFS_MOUNT_READ_ONLY: code = .EROFS
        case VEXFS_MOUNT_BUSY: code = .EBUSY
        case VEXFS_MOUNT_INVALID_ARGUMENT: code = .EINVAL
        case VEXFS_MOUNT_PERMISSION_DENIED: code = .EACCES
        case VEXFS_MOUNT_NO_SPACE: code = .ENOSPC
        case VEXFS_MOUNT_CORRUPTION: code = .EIO
        case VEXFS_MOUNT_UNSUPPORTED: code = .ENOTSUP
        case VEXFS_MOUNT_NOT_EMPTY: code = .ENOTEMPTY
        default: code = .EIO
        }
        throw POSIXError(code)
    }
}
