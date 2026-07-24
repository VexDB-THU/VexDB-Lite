import Foundation
import FSKit

final class VexFSVolume: FSVolume,
                         FSVolume.ReadWriteOperations,
                         FSVolume.OpenCloseOperations,
                         FSVolume.XattrOperations {
    private struct CachedLookupRecord {
        let record: VexFSStatRecord
        let parentID: FSItem.Identifier
        let generation: UInt64
    }

    let backend: VexFSBackend
    let workspace: String
    let rootItem: VexFSItem
    private let cacheLock = NSLock()
    private var itemCache: [UInt64: VexFSItem] = [:]
    private var lookupRecordCache: [String: CachedLookupRecord] = [:]
    private let maximumLookupRecordCount = 100_000
    private let maximumReadCacheBytes: UInt64 = 1024 * 1024
    private let maximumTotalReadCacheBytes: UInt64 = 64 * 1024 * 1024
    private var readCacheBytes: UInt64 = 0
    private var observedCacheGeneration: UInt64

    init(backend: VexFSBackend, workspace: String, volumeID: UUID) throws {
        self.backend = backend
        self.workspace = workspace
        let visibility = try backend.refreshVisibility()
        self.observedCacheGeneration = visibility.generation
        let rootRecord = try backend.stat(path: "/")
        self.rootItem = VexFSItem(path: "/", record: rootRecord,
                                  parentID: .parentOfRoot,
                                  cacheGeneration: visibility.generation)
        super.init(volumeID: FSVolume.Identifier(uuid: volumeID),
                   volumeName: FSFileName(string: "VexFS \(workspace)"))
        itemCache[rootRecord.inode] = rootItem
    }

    func synchronizeNow() throws { try backend.synchronize() }

    func closeBackend() { backend.close() }

    func reactivateBackend() throws {
        try backend.reopen()
        let visibility = try backend.refreshVisibility()
        let rootRecord = try backend.stat(path: "/")

        rootItem.lock.lock()
        rootItem.record = rootRecord
        rootItem.recordValidatedAt = ProcessInfo.processInfo.systemUptime
        rootItem.cacheGeneration = visibility.generation
        rootItem.readCache = nil
        rootItem.isUnlinked = false
        rootItem.lock.unlock()

        cacheLock.lock()
        for item in itemCache.values { item.readCache = nil }
        itemCache.removeAll(keepingCapacity: true)
        lookupRecordCache.removeAll(keepingCapacity: true)
        itemCache[rootRecord.inode] = rootItem
        readCacheBytes = 0
        observedCacheGeneration = visibility.generation
        cacheLock.unlock()
    }

    // PRAGMA data_version 只在其他连接提交后变化。runtime 仅在变化时读取
    // workspace HEAD；这里保存一个 generation，已有 vnode 在下次访问时懒刷新。
    func currentCacheGeneration() throws -> UInt64 {
        let visibility = try backend.refreshVisibility()
        cacheLock.lock()
        if observedCacheGeneration != visibility.generation {
            // 目录枚举带回的元数据只对当时的 workspace generation 有效。
            // 其他 gateway 提交、rename 或 restore 后必须整体丢弃，不能让
            // lookup 为了省一次 stat 而返回旧路径或旧版本。
            lookupRecordCache.removeAll(keepingCapacity: true)
        }
        observedCacheGeneration = visibility.generation
        let generation = observedCacheGeneration
        cacheLock.unlock()
        return generation
    }

    func clearReadCache(_ item: VexFSItem) {
        cacheLock.lock()
        if let cached = item.readCache {
            readCacheBytes -= min(readCacheBytes, UInt64(cached.count))
            item.readCache = nil
        }
        cacheLock.unlock()
    }

    func installReadCache(_ data: Data, for item: VexFSItem) -> Bool {
        cacheLock.lock()
        defer { cacheLock.unlock() }
        if let previous = item.readCache {
            readCacheBytes -= min(readCacheBytes, UInt64(previous.count))
            item.readCache = nil
        }
        guard UInt64(data.count) <= maximumTotalReadCacheBytes - readCacheBytes else {
            return false
        }
        item.readCache = data
        readCacheBytes += UInt64(data.count)
        return true
    }

    func currentPath(for item: VexFSItem) throws -> String {
        let path = try backend.path(inode: item.record.inode)
        item.path = path
        return path
    }

    // 调用方已经刷新过 visibility 时，generation 相同就说明本地记录里的
    // 路径仍然有效。正常的只读遍历不需要为每个文件再跑一次 vexfs_path。
    func currentPath(for item: VexFSItem, generation: UInt64) throws -> String {
        cacheLock.lock()
        let cacheGeneration = observedCacheGeneration
        cacheLock.unlock()
        if !item.isUnlinked && generation == cacheGeneration &&
            item.cacheGeneration == generation {
            return item.path
        }
        return try currentPath(for: item)
    }

    func childPath(parent: VexFSItem, name: String) throws -> String {
        let parentPath = try currentPath(for: parent)
        return parentPath == "/" ? "/\(name)" : "\(parentPath)/\(name)"
    }

    func childPath(parent: VexFSItem, name: String, generation: UInt64) throws -> String {
        let parentPath = try currentPath(for: parent, generation: generation)
        return parentPath == "/" ? "/\(name)" : "\(parentPath)/\(name)"
    }

    func cachedLookupRecord(path: String,
                            generation: UInt64) -> (VexFSStatRecord, FSItem.Identifier)? {
        cacheLock.lock()
        defer { cacheLock.unlock() }
        guard let cached = lookupRecordCache[path], cached.generation == generation else {
            return nil
        }
        return (cached.record, cached.parentID)
    }

    func cachedItem(path: String, record: VexFSStatRecord,
                    parentID: FSItem.Identifier) -> VexFSItem {
        cacheLock.lock()
        defer { cacheLock.unlock() }
        if lookupRecordCache[path] == nil &&
            lookupRecordCache.count >= maximumLookupRecordCount {
            // 防止长时间遍历大量不同目录时缓存无上限增长。清空只会退回
            // 一次权威 stat，不影响内容或正确性。
            lookupRecordCache.removeAll(keepingCapacity: true)
        }
        lookupRecordCache[path] = CachedLookupRecord(
            record: record, parentID: parentID, generation: observedCacheGeneration)
        if let item = itemCache[record.inode] {
            item.path = path
            item.record = record
            item.recordValidatedAt = ProcessInfo.processInfo.systemUptime
            item.cacheGeneration = observedCacheGeneration
            item.parentID = parentID
            item.isUnlinked = false
            return item
        }
        let item = VexFSItem(path: path, record: record, parentID: parentID,
                             cacheGeneration: observedCacheGeneration)
        itemCache[record.inode] = item
        return item
    }

    func reclaimCachedItem(_ item: VexFSItem) {
        clearReadCache(item)
        cacheLock.lock()
        defer { cacheLock.unlock() }
        guard item !== rootItem,
              let cached = itemCache[item.record.inode],
              cached === item else { return }
        itemCache.removeValue(forKey: item.record.inode)
    }

    // record cache 只负责减少重复 stat。任何已经提交到数据库的元数据变更都要
    // 精确失效相关 vnode，不能等待 250 ms TTL，否则 Bash 会在 mutation 返回
    // 成功后立刻看到旧的 nlink、mtime 或 ctime。
    func invalidateRecord(_ item: VexFSItem) {
        item.lock.lock()
        item.recordValidatedAt = 0
        item.lock.unlock()
    }

    func recordDirectoryMutation(_ directory: VexFSItem) {
        directory.lock.lock()
        let wallClock = Int64(Date().timeIntervalSince1970 * 1000)
        // SQLite 时间戳是毫秒级。同一毫秒内连续 mutation 时仍要让调用方
        // 观察到单调推进，随后 TTL 刷新会与数据库权威记录重新对齐。
        let modified = max(wallClock, directory.record.updated_at + 1)
        let changed = max(wallClock, (directory.record.changed_at ??
                                     directory.record.updated_at) + 1)
        directory.record = VexFSStatRecord(
            path: directory.path, inode: directory.record.inode,
            kind: directory.record.kind, mode: directory.record.mode,
            size: directory.record.size, version: directory.record.version,
            created_at: directory.record.created_at,
            accessed_at: directory.record.accessed_at,
            updated_at: modified, changed_at: changed,
            uid: directory.record.uid, gid: directory.record.gid,
            link_count: directory.record.link_count)
        directory.recordValidatedAt = ProcessInfo.processInfo.systemUptime
        directory.lock.unlock()
    }

    func refreshRecord(_ item: VexFSItem, at path: String) throws {
        let record = try backend.stat(path: path)
        item.lock.lock()
        item.path = path
        item.record = record
        item.recordValidatedAt = ProcessInfo.processInfo.systemUptime
        item.cacheGeneration = observedCacheGeneration
        item.lock.unlock()
    }

    func attributes(for item: VexFSItem,
                    request: FSItem.GetAttributesRequest?) -> FSItem.Attributes {
        let attributes = FSItem.Attributes()
        let wanted: (FSItem.Attribute) -> Bool = { attribute in
            request == nil || request!.isAttributeWanted(attribute)
        }
        let created = timespec(tv_sec: Int(item.record.created_at / 1000),
                               tv_nsec: Int((item.record.created_at % 1000) * 1_000_000))
        let updated = timespec(tv_sec: Int(item.record.updated_at / 1000),
                               tv_nsec: Int((item.record.updated_at % 1000) * 1_000_000))
        let accessedMilliseconds = item.record.accessed_at ?? item.record.updated_at
        let changedMilliseconds = item.record.changed_at ?? item.record.updated_at
        let accessed = timespec(tv_sec: Int(accessedMilliseconds / 1000),
                                tv_nsec: Int((accessedMilliseconds % 1000) * 1_000_000))
        let changed = timespec(tv_sec: Int(changedMilliseconds / 1000),
                               tv_nsec: Int((changedMilliseconds % 1000) * 1_000_000))
        if wanted(.type) { attributes.type = item.itemType }
        if wanted(.mode) { attributes.mode = item.record.mode }
        if wanted(.linkCount) {
            attributes.linkCount = item.record.link_count ?? (item.itemType == .directory ? 2 : 1)
        }
        if wanted(.uid) { attributes.uid = item.record.uid ?? getuid() }
        if wanted(.gid) { attributes.gid = item.record.gid ?? getgid() }
        if wanted(.flags) { attributes.flags = 0 }
        if wanted(.size) { attributes.size = item.record.size }
        if wanted(.allocSize) { attributes.allocSize = (item.record.size + 4095) / 4096 * 4096 }
        if wanted(.fileID) { attributes.fileID = item.itemID }
        if wanted(.parentID) { attributes.parentID = item.parentID }
        if wanted(.accessTime) { attributes.accessTime = accessed }
        if wanted(.modifyTime) { attributes.modifyTime = updated }
        if wanted(.changeTime) { attributes.changeTime = changed }
        if wanted(.birthTime) { attributes.birthTime = created }
        return attributes
    }

    func openItem(_ item: FSItem, modes: FSVolume.OpenModes,
                  replyHandler: @escaping ((any Error)?) -> Void) {
        guard let item = item as? VexFSItem else { return replyHandler(POSIXError(.EINVAL)) }
        if item.itemType == .directory { return replyHandler(nil) }
        guard item.itemType == .file else { return replyHandler(POSIXError(.EINVAL)) }
        let generation: UInt64
        do { generation = try currentCacheGeneration() }
        catch { return replyHandler(error) }
        item.lock.lock()
        defer { item.lock.unlock() }
        do {
            let writable = modes.contains(.write)
            if writable {
                clearReadCache(item)
            }
            if item.cacheGeneration != generation && item.dirtyGeneration == nil {
                clearReadCache(item)
                let path = try currentPath(for: item)
                item.record = try backend.stat(path: path)
                item.recordValidatedAt = ProcessInfo.processInfo.systemUptime
                item.cacheGeneration = generation
            }
            if let oldHandle = item.handle, writable && !item.handleWritable {
                try backend.closeHandle(oldHandle, retain: true)
                item.handle = nil
                item.dirtyGeneration = nil
            }
            if item.handle == nil {
                let path = try currentPath(for: item, generation: generation)
                if !writable && item.record.size <= maximumReadCacheBytes {
                    let data = try backend.readFile(path: path)
                    if installReadCache(data, for: item) {
                        item.handleWritable = false
                    } else {
                        item.handle = try backend.openHandle(path: path, flags: "r")
                        item.handleWritable = false
                    }
                } else {
                    item.handle = try backend.openHandle(path: path,
                                                         flags: writable ? "rw" : "r")
                    item.handleWritable = writable
                }
            }
            replyHandler(nil)
        } catch { replyHandler(error) }
    }

    func closeItem(_ item: FSItem, modes: FSVolume.OpenModes,
                   replyHandler: @escaping ((any Error)?) -> Void) {
        guard let item = item as? VexFSItem else { return replyHandler(POSIXError(.EINVAL)) }
        if !modes.isEmpty || item.itemType == .directory { return replyHandler(nil) }
        item.lock.lock()
        defer { item.lock.unlock() }
        do {
            if let handle = item.handle {
                if let generation = item.dirtyGeneration {
                    let version = try backend.publishAndClose(handle: handle,
                                                              generation: generation)
                    item.handle = nil
                    item.dirtyGeneration = nil
                    if item.isUnlinked {
                        // unlink 后目录项已经不存在，但打开的 vnode 仍必须能正常
                        // publish 和 close。此时不能再通过路径刷新属性。
                        item.record = VexFSStatRecord(
                            path: item.path, inode: item.record.inode,
                            kind: item.record.kind, mode: item.record.mode,
                            size: item.record.size, version: UInt64(version),
                            created_at: item.record.created_at,
                            updated_at: Int64(Date().timeIntervalSince1970 * 1000),
                            uid: item.record.uid, gid: item.record.gid, link_count: 0)
                    } else {
                        item.record = try backend.stat(path: currentPath(for: item))
                    }
                } else {
                    try backend.closeHandle(handle, retain: true)
                }
            }
            item.handle = nil
            clearReadCache(item)
            item.handleWritable = false
            item.dirtyGeneration = nil
            replyHandler(nil)
        } catch { replyHandler(error) }
    }

    func read(from item: FSItem, at offset: off_t, length: Int,
              into buffer: FSMutableFileDataBuffer,
              replyHandler: @escaping (Int, (any Error)?) -> Void) {
        guard let item = item as? VexFSItem, item.itemType == .file,
              offset >= 0, length >= 0 else {
            return replyHandler(0, POSIXError(.EINVAL))
        }
        item.lock.lock()
        defer { item.lock.unlock() }
        do {
            let data: Data
            if let cached = item.readCache, item.handle == nil {
                let start = min(Int(offset), cached.count)
                data = cached.subdata(in: start..<min(start + length, cached.count))
            } else if let handle = item.handle {
                data = try backend.readHandle(handle: handle, offset: UInt64(offset),
                                              length: UInt64(length))
            } else {
                let complete = try backend.readFile(path: currentPath(for: item))
                let start = min(Int(offset), complete.count)
                data = complete.subdata(in: start..<min(start + length, complete.count))
            }
            _ = buffer.withUnsafeMutableBytes { destination in
                data.copyBytes(to: destination.bindMemory(to: UInt8.self))
            }
            replyHandler(data.count, nil)
        } catch { replyHandler(0, error) }
    }

    func write(contents: Data, to item: FSItem, at offset: off_t,
               replyHandler: @escaping (Int, (any Error)?) -> Void) {
        guard let item = item as? VexFSItem, item.itemType == .file, offset >= 0 else {
            return replyHandler(0, POSIXError(.EINVAL))
        }
        item.lock.lock()
        defer { item.lock.unlock() }
        do {
            if item.handle == nil || !item.handleWritable {
                if let old = item.handle { try backend.closeHandle(old, retain: true) }
                clearReadCache(item)
                item.handle = try backend.openHandle(path: currentPath(for: item), flags: "rw")
                item.handleWritable = true
                item.dirtyGeneration = nil
            }
            let generation = try backend.stageWrite(handle: item.handle!, offset: UInt64(offset),
                                                    data: contents)
            item.dirtyGeneration = generation
            item.record = VexFSStatRecord(path: item.path,
                inode: item.record.inode, kind: item.record.kind, mode: item.record.mode,
                size: max(item.record.size, UInt64(offset) + UInt64(contents.count)),
                version: item.record.version, created_at: item.record.created_at,
                updated_at: Int64(Date().timeIntervalSince1970 * 1000))
            replyHandler(contents.count, nil)
        } catch { replyHandler(0, error) }
    }

    var maximumLinkCount: Int { 65535 }
    var maximumNameLength: Int { 255 }
    var restrictsOwnershipChanges: Bool { true }
    var truncatesLongNames: Bool { false }
    var maximumFileSizeInBits: Int { 28 }
    var maximumXattrSizeInBits: Int { 16 }
}
