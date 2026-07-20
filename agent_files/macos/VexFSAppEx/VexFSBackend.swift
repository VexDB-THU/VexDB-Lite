import Foundation

struct VexFSDescriptor: Codable {
    let version: Int
    let database_file: String
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
    let updated_at: Int64
}

struct VexFSDirectoryEntry: Codable {
    let name: String
    let inode: UInt64
    let kind: String
    let size: UInt64
    let version: UInt64
    let mode: UInt32
    let created_at: Int64
    let updated_at: Int64
}

final class VexFSBackend {
    private var session: OpaquePointer?

    init(databasePath: String, workspace: String) throws {
        var config = vexfs_mount_config()
        config.abi_version = UInt32(VEXFS_MOUNT_ABI_VERSION)
        config.busy_timeout_ms = 5_000
        config.flags = UInt32(VEXFS_MOUNT_EXCLUSIVE_GATEWAY)
        var opened: OpaquePointer?
        var error = vexfs_mount_error()
        let status = databasePath.withCString { databasePointer in
            workspace.withCString { workspacePointer in
                config.database_path = databasePointer
                config.workspace = workspacePointer
                return vexfs_mount_session_open(&config, &opened, &error)
            }
        }
        try Self.check(status, error: &error)
        session = opened
    }

    deinit {
        vexfs_mount_session_close(session)
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

    func mkdir(path: String) throws {
        var error = vexfs_mount_error()
        let status = path.withCString { vexfs_mount_mkdir(session, $0, &error) }
        try Self.check(status, error: &error)
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
        default: code = .EIO
        }
        throw POSIXError(code)
    }
}
