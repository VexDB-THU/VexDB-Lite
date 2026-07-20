import Foundation
import FSKit

final class VexFSVolume: FSVolume,
                         FSVolume.ReadWriteOperations,
                         FSVolume.OpenCloseOperations {
    let backend: VexFSBackend
    let workspace: String
    let rootItem: VexFSItem
    private let cacheLock = NSLock()
    private var itemCache: [UInt64: VexFSItem] = [:]

    init(backend: VexFSBackend, workspace: String, volumeID: UUID) throws {
        self.backend = backend
        self.workspace = workspace
        let rootRecord = try backend.stat(path: "/")
        self.rootItem = VexFSItem(path: "/", record: rootRecord, parentID: .parentOfRoot)
        super.init(volumeID: FSVolume.Identifier(uuid: volumeID),
                   volumeName: FSFileName(string: "VexFS \(workspace)"))
        itemCache[rootRecord.inode] = rootItem
    }

    func synchronizeNow() throws { try backend.synchronize() }

    func currentPath(for item: VexFSItem) throws -> String {
        let path = try backend.path(inode: item.record.inode)
        item.path = path
        return path
    }

    func childPath(parent: VexFSItem, name: String) throws -> String {
        let parentPath = try currentPath(for: parent)
        return parentPath == "/" ? "/\(name)" : "\(parentPath)/\(name)"
    }

    func cachedItem(path: String, record: VexFSStatRecord,
                    parentID: FSItem.Identifier) -> VexFSItem {
        cacheLock.lock()
        defer { cacheLock.unlock() }
        if let item = itemCache[record.inode] {
            item.path = path
            item.record = record
            item.parentID = parentID
            return item
        }
        let item = VexFSItem(path: path, record: record, parentID: parentID)
        itemCache[record.inode] = item
        return item
    }

    func dropCachedItems(exceptRoot: Bool = true) {
        cacheLock.lock()
        itemCache.removeAll(keepingCapacity: true)
        if exceptRoot { itemCache[rootItem.record.inode] = rootItem }
        cacheLock.unlock()
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
        if wanted(.type) { attributes.type = item.itemType }
        if wanted(.mode) { attributes.mode = item.record.mode }
        if wanted(.linkCount) { attributes.linkCount = item.itemType == .directory ? 2 : 1 }
        if wanted(.uid) { attributes.uid = getuid() }
        if wanted(.gid) { attributes.gid = getgid() }
        if wanted(.flags) { attributes.flags = 0 }
        if wanted(.size) { attributes.size = item.record.size }
        if wanted(.allocSize) { attributes.allocSize = (item.record.size + 4095) / 4096 * 4096 }
        if wanted(.fileID) { attributes.fileID = item.itemID }
        if wanted(.parentID) { attributes.parentID = item.parentID }
        if wanted(.accessTime) { attributes.accessTime = updated }
        if wanted(.modifyTime) { attributes.modifyTime = updated }
        if wanted(.changeTime) { attributes.changeTime = updated }
        if wanted(.birthTime) { attributes.birthTime = created }
        return attributes
    }

    func openItem(_ item: FSItem, modes: FSVolume.OpenModes,
                  replyHandler: @escaping ((any Error)?) -> Void) {
        guard let item = item as? VexFSItem else { return replyHandler(POSIXError(.EINVAL)) }
        if item.itemType == .directory { return replyHandler(nil) }
        item.lock.lock()
        defer { item.lock.unlock() }
        do {
            let writable = modes.contains(.write)
            if let oldHandle = item.handle, writable && !item.handleWritable {
                try backend.closeHandle(oldHandle, retain: true)
                item.handle = nil
                item.dirtyGeneration = nil
            }
            if item.handle == nil {
                item.handle = try backend.openHandle(path: currentPath(for: item),
                                                     flags: writable ? "rw" : "r")
                item.handleWritable = writable
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
                    _ = try backend.publish(handle: handle, generation: generation)
                    item.record = try backend.stat(path: currentPath(for: item))
                }
                try backend.closeHandle(handle, retain: true)
            }
            item.handle = nil
            item.handleWritable = false
            item.dirtyGeneration = nil
            replyHandler(nil)
        } catch { replyHandler(error) }
    }

    func read(from item: FSItem, at offset: off_t, length: Int,
              into buffer: FSMutableFileDataBuffer,
              replyHandler: @escaping (Int, (any Error)?) -> Void) {
        guard let item = item as? VexFSItem, offset >= 0, length >= 0 else {
            return replyHandler(0, POSIXError(.EINVAL))
        }
        item.lock.lock()
        defer { item.lock.unlock() }
        do {
            let data: Data
            if let handle = item.handle {
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

    var maximumLinkCount: Int { 1 }
    var maximumNameLength: Int { 255 }
    var restrictsOwnershipChanges: Bool { true }
    var truncatesLongNames: Bool { false }
    var maximumFileSizeInBits: Int { 28 }
    var maximumXattrSizeInBits: Int { 0 }
}
