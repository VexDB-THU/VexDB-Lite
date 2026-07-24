import Foundation
import FSKit

extension VexFSVolume: FSVolume.Operations {
    var volumeStatistics: FSStatFSResult {
        let result = FSStatFSResult(fileSystemTypeName: "vexfs")
        result.blockSize = 4096
        result.ioSize = 128 * 1024
        result.fileSystemSubType = 0
        return result
    }

    var supportedVolumeCapabilities: FSVolume.SupportedCapabilities {
        let capabilities = FSVolume.SupportedCapabilities()
        capabilities.supportsPersistentObjectIDs = true
        capabilities.supportsJournal = true
        capabilities.supportsActiveJournal = true
        capabilities.supportsSparseFiles = false
        capabilities.supportsZeroRuns = false
        capabilities.supportsFastStatFS = false
        capabilities.supports2TBFiles = false
        capabilities.supports64BitObjectIDs = true
        capabilities.supportsSymbolicLinks = true
        capabilities.supportsHardLinks = true
        capabilities.supportsHiddenFiles = true
        capabilities.doesNotSupportVolumeSizes = true
        capabilities.doesNotSupportImmutableFiles = true
        capabilities.doesNotSupportSettingFilePermissions = false
        capabilities.caseFormat = .sensitive
        return capabilities
    }

    func activate(options: FSTaskOptions,
                  replyHandler: @escaping (FSItem?, (any Error)?) -> Void) {
        do {
            try reactivateBackend()
            replyHandler(rootItem, nil)
        } catch {
            replyHandler(nil, error)
        }
    }

    func deactivate(options: FSDeactivateOptions = [],
                    replyHandler: @escaping ((any Error)?) -> Void) {
        // FSKit calls synchronize and unmount before deactivate. Resource
        // teardown must not start another I/O round here.
        closeBackend()
        replyHandler(nil)
    }

    func mount(options: FSTaskOptions, replyHandler: @escaping ((any Error)?) -> Void) {
        replyHandler(nil)
    }

    func unmount(replyHandler: @escaping () -> Void) {
        try? synchronizeNow()
        closeBackend()
        replyHandler()
    }

    func synchronize(flags: FSSyncFlags,
                     replyHandler: @escaping ((any Error)?) -> Void) {
        do { try synchronizeNow(); replyHandler(nil) } catch { replyHandler(error) }
    }

    func getAttributes(_ desiredAttributes: FSItem.GetAttributesRequest, of item: FSItem,
                       replyHandler: @escaping (FSItem.Attributes?, (any Error)?) -> Void) {
        guard let item = item as? VexFSItem else {
            return replyHandler(nil, POSIXError(.EINVAL))
        }
        let generation: UInt64
        do { generation = try currentCacheGeneration() }
        catch { return replyHandler(nil, error) }
        item.lock.lock()
        defer { item.lock.unlock() }
        if item.isUnlinked {
            return replyHandler(attributes(for: item, request: desiredAttributes), nil)
        }
        do {
            let now = ProcessInfo.processInfo.systemUptime
            if item.dirtyGeneration == nil &&
                (item.cacheGeneration != generation || now - item.recordValidatedAt >= 0.25) {
                let path = try currentPath(for: item)
                item.record = try backend.stat(path: path)
                item.recordValidatedAt = now
                item.cacheGeneration = generation
            }
            replyHandler(attributes(for: item, request: desiredAttributes), nil)
        } catch {
            let nsError = error as NSError
            if nsError.domain == NSPOSIXErrorDomain && nsError.code == Int(ENOENT) {
                // rename 覆盖或 unlink 后，内核仍可能持有旧 vnode，并继续查询
                // 它的属性。目录项已经消失不代表这个 FSItem 立刻失效；返回
                // 最后一次已知属性，直到 reclaimItem 回收它。
                replyHandler(attributes(for: item, request: desiredAttributes), nil)
            } else {
                replyHandler(nil, error)
            }
        }
    }

    func setAttributes(_ newAttributes: FSItem.SetAttributesRequest, on item: FSItem,
                       replyHandler: @escaping (FSItem.Attributes?, (any Error)?) -> Void) {
        guard let item = item as? VexFSItem else {
            return replyHandler(nil, POSIXError(.EINVAL))
        }
        item.lock.lock()
        defer { item.lock.unlock() }
        do {
            let path = try currentPath(for: item)
            var localMode = item.record.mode
            var localSize = item.record.size
            if newAttributes.isValid(.size), item.itemType == .file {
                // truncate 会改变所有已打开描述符随后看到的文件内容。丢弃只读
                // 快照，避免同一个 vnode 上的旧读描述符继续返回截断前的数据。
                clearReadCache(item)
                let hadHandle = item.handle != nil
                if item.handle == nil || !item.handleWritable {
                    if let old = item.handle { try backend.closeHandle(old, retain: true) }
                    item.handle = try backend.openHandle(path: path, flags: "rw")
                    item.handleWritable = true
                    item.dirtyGeneration = nil
                }
                let generation = try backend.truncate(handle: item.handle!, size: newAttributes.size)
                item.dirtyGeneration = generation
                localSize = newAttributes.size
                newAttributes.consumedAttributes.insert(.size)
                if !hadHandle {
                    _ = try backend.publishAndClose(handle: item.handle!, generation: generation)
                    item.handle = nil
                    item.handleWritable = false
                    item.dirtyGeneration = nil
                    item.record = try backend.stat(path: path)
                } else {
                    item.record = VexFSStatRecord(path: path, inode: item.record.inode,
                        kind: item.record.kind, mode: localMode, size: localSize,
                        version: item.record.version, created_at: item.record.created_at,
                        updated_at: Int64(Date().timeIntervalSince1970 * 1000))
                }
            }
            if newAttributes.isValid(.mode), item.itemType != .symlink {
                localMode = newAttributes.mode & 0o777
                try backend.setMode(inode: item.record.inode, mode: localMode)
                newAttributes.consumedAttributes.insert(.mode)
            }
            if newAttributes.isValid(.uid) || newAttributes.isValid(.gid) {
                let uid: Int64 = newAttributes.isValid(.uid) ? Int64(newAttributes.uid) : -1
                let gid: Int64 = newAttributes.isValid(.gid) ? Int64(newAttributes.gid) : -1
                try backend.chown(inode: item.record.inode, uid: uid, gid: gid)
                if newAttributes.isValid(.uid) { newAttributes.consumedAttributes.insert(.uid) }
                if newAttributes.isValid(.gid) { newAttributes.consumedAttributes.insert(.gid) }
            }
            if newAttributes.isValid(.accessTime) || newAttributes.isValid(.modifyTime) {
                var mask: UInt32 = 0
                var accessedAt = item.record.accessed_at ?? item.record.updated_at
                var modifiedAt = item.record.updated_at
                if newAttributes.isValid(.accessTime) {
                    let value = newAttributes.accessTime
                    guard value.tv_sec >= 0, value.tv_nsec >= 0,
                          value.tv_nsec < 1_000_000_000 else {
                        throw POSIXError(.EINVAL)
                    }
                    accessedAt = Int64(value.tv_sec) * 1000 + Int64(value.tv_nsec) / 1_000_000
                    mask |= UInt32(VEXFS_MOUNT_TIME_ACCESS)
                    newAttributes.consumedAttributes.insert(.accessTime)
                }
                if newAttributes.isValid(.modifyTime) {
                    let value = newAttributes.modifyTime
                    guard value.tv_sec >= 0, value.tv_nsec >= 0,
                          value.tv_nsec < 1_000_000_000 else {
                        throw POSIXError(.EINVAL)
                    }
                    modifiedAt = Int64(value.tv_sec) * 1000 + Int64(value.tv_nsec) / 1_000_000
                    mask |= UInt32(VEXFS_MOUNT_TIME_MODIFY)
                    newAttributes.consumedAttributes.insert(.modifyTime)
                }
                try backend.setTimes(inode: item.record.inode, accessedAt: accessedAt,
                                     modifiedAt: modifiedAt, mask: mask)
            }
            if item.dirtyGeneration == nil {
                item.record = try backend.stat(path: path)
            } else {
                item.record = VexFSStatRecord(path: path, inode: item.record.inode,
                    kind: item.record.kind, mode: localMode, size: localSize,
                    version: item.record.version, created_at: item.record.created_at,
                    updated_at: Int64(Date().timeIntervalSince1970 * 1000))
            }
            replyHandler(attributes(for: item, request: nil), nil)
        } catch { replyHandler(nil, error) }
    }

    func lookupItem(named name: FSFileName, inDirectory directory: FSItem,
                    replyHandler: @escaping (FSItem?, FSFileName?, (any Error)?) -> Void) {
        guard let directory = directory as? VexFSItem,
              directory.itemType == .directory,
              let nameString = name.string else {
            return replyHandler(nil, nil, POSIXError(.ENOTDIR))
        }
        if nameString == "." { return replyHandler(directory, name, nil) }
        do {
            let generation = try currentCacheGeneration()
            let path = try childPath(parent: directory, name: nameString,
                                     generation: generation)
            if let (record, parentID) = cachedLookupRecord(path: path,
                                                           generation: generation) {
                return replyHandler(cachedItem(path: path, record: record,
                                               parentID: parentID), name, nil)
            }
            let record = try backend.stat(path: path)
            replyHandler(cachedItem(path: path, record: record, parentID: directory.itemID), name, nil)
        } catch { replyHandler(nil, nil, error) }
    }

    func reclaimItem(_ item: FSItem, replyHandler: @escaping ((any Error)?) -> Void) {
        guard let item = item as? VexFSItem else {
            return replyHandler(POSIXError(.EINVAL))
        }
        do {
            try backend.reclaim()
            reclaimCachedItem(item)
            replyHandler(nil)
        } catch { replyHandler(error) }
    }

    func readSymbolicLink(_ item: FSItem,
                          replyHandler: @escaping (FSFileName?, (any Error)?) -> Void) {
        guard let item = item as? VexFSItem, item.itemType == .symlink else {
            return replyHandler(nil, POSIXError(.EINVAL))
        }
        do {
            replyHandler(FSFileName(data: try backend.readlink(inode: item.record.inode)), nil)
        } catch { replyHandler(nil, error) }
    }

    func createItem(named name: FSFileName, type: FSItem.ItemType,
                    inDirectory directory: FSItem,
                    attributes newAttributes: FSItem.SetAttributesRequest,
                    replyHandler: @escaping (FSItem?, FSFileName?, (any Error)?) -> Void) {
        guard let directory = directory as? VexFSItem, let nameString = name.string else {
            return replyHandler(nil, nil, POSIXError(.EINVAL))
        }
        do {
            let path = try childPath(parent: directory, name: nameString)
            let defaultMode: UInt32 = type == .directory ? 0o755 : 0o644
            let mode = newAttributes.isValid(.mode) ? newAttributes.mode & 0o777 : defaultMode
            var createdHandle: String?
            switch type {
            case .directory: try backend.create(path: path, kind: "directory", mode: mode)
            case .file: createdHandle = try backend.createFileHandle(path: path, mode: mode)
            default: throw POSIXError(.ENOTSUP)
            }
            if newAttributes.isValid(.mode) { newAttributes.consumedAttributes.insert(.mode) }
            let record = try backend.stat(path: path)
            let item = cachedItem(path: path, record: record, parentID: directory.itemID)
            if let createdHandle {
                // createFileHandle has already published the empty file so the
                // directory entry is visible. Reuse its handle for later writes;
                // the next stage operation supplies the next dirty generation.
                item.handle = createdHandle
                item.handleWritable = true
                item.dirtyGeneration = nil
            }
            recordDirectoryMutation(directory)
            replyHandler(item, name, nil)
        } catch { replyHandler(nil, nil, error) }
    }

    func createSymbolicLink(named name: FSFileName, inDirectory directory: FSItem,
                            attributes newAttributes: FSItem.SetAttributesRequest,
                            linkContents contents: FSFileName,
                            replyHandler: @escaping (FSItem?, FSFileName?, (any Error)?) -> Void) {
        guard let directory = directory as? VexFSItem, let nameString = name.string else {
            return replyHandler(nil, nil, POSIXError(.EINVAL))
        }
        do {
            let path = try childPath(parent: directory, name: nameString)
            try backend.symlink(path: path, target: contents.data)
            if newAttributes.isValid(.mode) {
                newAttributes.consumedAttributes.insert(.mode)
            }
            let record = try backend.stat(path: path)
            let item = cachedItem(path: path, record: record, parentID: directory.itemID)
            recordDirectoryMutation(directory)
            replyHandler(item, name, nil)
        } catch { replyHandler(nil, nil, error) }
    }

    func createLink(to item: FSItem, named name: FSFileName, inDirectory directory: FSItem,
                    replyHandler: @escaping (FSFileName?, (any Error)?) -> Void) {
        guard let item = item as? VexFSItem,
              let directory = directory as? VexFSItem,
              let name = name.string else {
            return replyHandler(nil, POSIXError(.EINVAL))
        }
        do {
            let source = try currentPath(for: item)
            let destination = try childPath(parent: directory, name: name)
            try backend.link(source: source, destination: destination)
            // hardlink 改变的是同一个 inode。立即刷新源 vnode，确保 link(2)
            // 返回后紧接着的 stat(2) 能看到新的 st_nlink。
            try refreshRecord(item, at: source)
            recordDirectoryMutation(directory)
            replyHandler(FSFileName(string: name), nil)
        } catch { replyHandler(nil, error) }
    }

    func getXattr(named name: FSFileName, of item: FSItem,
                  replyHandler: @escaping (Data?, (any Error)?) -> Void) {
        guard let item = item as? VexFSItem, let name = name.string else {
            return replyHandler(nil, POSIXError(.EINVAL))
        }
        do {
            replyHandler(try backend.getXattr(inode: item.record.inode, name: name), nil)
        } catch { replyHandler(nil, error) }
    }

    func setXattr(named name: FSFileName, to value: Data?, on item: FSItem,
                  policy: FSVolume.SetXattrPolicy,
                  replyHandler: @escaping ((any Error)?) -> Void) {
        guard let item = item as? VexFSItem, let name = name.string else {
            return replyHandler(POSIXError(.EINVAL))
        }
        do {
            try backend.setXattr(inode: item.record.inode, name: name, value: value,
                                 policy: Int(policy.rawValue))
            replyHandler(nil)
        } catch { replyHandler(error) }
    }

    func listXattrs(of item: FSItem,
                    replyHandler: @escaping ([FSFileName]?, (any Error)?) -> Void) {
        guard let item = item as? VexFSItem else {
            return replyHandler(nil, POSIXError(.EINVAL))
        }
        do {
            let names = try backend.listXattrs(inode: item.record.inode)
                .map { FSFileName(string: $0) }
            replyHandler(names, nil)
        } catch { replyHandler(nil, error) }
    }

    func removeItem(_ item: FSItem, named name: FSFileName, fromDirectory directory: FSItem,
                    replyHandler: @escaping ((any Error)?) -> Void) {
        guard let item = item as? VexFSItem,
              let directory = directory as? VexFSItem,
              let name = name.string else { return replyHandler(POSIXError(.EINVAL)) }
        do {
            let path = try childPath(parent: directory, name: name)
            try backend.remove(path: path)
            item.lock.lock()
            item.isUnlinked = true
            item.recordValidatedAt = 0
            item.lock.unlock()
            recordDirectoryMutation(directory)
            replyHandler(nil)
        } catch { replyHandler(error) }
    }

    func renameItem(_ item: FSItem, inDirectory sourceDirectory: FSItem,
                    named sourceName: FSFileName, to destinationName: FSFileName,
                    inDirectory destinationDirectory: FSItem, overItem: FSItem?,
                    replyHandler: @escaping (FSFileName?, (any Error)?) -> Void) {
        guard let source = item as? VexFSItem,
              let sourceDirectory = sourceDirectory as? VexFSItem,
              let destinationDirectory = destinationDirectory as? VexFSItem,
              let sourceName = sourceName.string,
              let destination = destinationName.string else {
            return replyHandler(nil, POSIXError(.EINVAL))
        }
        do {
            // FSKit 给出的目录和名称代表这次 rename 请求看到的目录项。
            // 不要通过 inode 缓存反查源路径：Git 的 config.lock -> config
            // 原子替换会快速创建、关闭并重命名文件，缓存路径可能已经失效。
            let sourcePath = try childPath(parent: sourceDirectory, name: sourceName)
            let destinationPath = try childPath(parent: destinationDirectory, name: destination)
            try backend.rename(source: sourcePath, destination: destinationPath,
                               replace: overItem != nil)
            try refreshRecord(source, at: destinationPath)
            source.lock.lock()
            source.parentID = destinationDirectory.itemID
            source.isUnlinked = false
            source.lock.unlock()
            if let replaced = overItem as? VexFSItem {
                replaced.lock.lock()
                replaced.isUnlinked = true
                replaced.recordValidatedAt = 0
                replaced.lock.unlock()
            }
            recordDirectoryMutation(sourceDirectory)
            if destinationDirectory !== sourceDirectory {
                recordDirectoryMutation(destinationDirectory)
            }
            replyHandler(destinationName, nil)
        } catch {
            replyHandler(nil, error)
        }
    }

    func enumerateDirectory(_ directory: FSItem, startingAt cookie: FSDirectoryCookie,
                            verifier: FSDirectoryVerifier,
                            attributes request: FSItem.GetAttributesRequest?,
                            packer: FSDirectoryEntryPacker,
                            replyHandler: @escaping (FSDirectoryVerifier, (any Error)?) -> Void) {
        guard let directory = directory as? VexFSItem, directory.itemType == .directory else {
            return replyHandler(FSDirectoryVerifier(0), POSIXError(.ENOTDIR))
        }
        do {
            let generation = try currentCacheGeneration()
            let now = ProcessInfo.processInfo.systemUptime
            let directoryPath = directory.cacheGeneration == generation &&
                now - directory.recordValidatedAt < 0.25
                ? directory.path : try currentPath(for: directory)
            let listing = try backend.listVersioned(path: directoryPath)
            let currentVerifier = FSDirectoryVerifier(listing.version)
            if verifier.rawValue != 0 && verifier.rawValue != currentVerifier.rawValue {
                throw FSError(.invalidDirectoryCookie)
            }
            directory.record = VexFSStatRecord(
                path: directoryPath, inode: directory.record.inode,
                kind: directory.record.kind, mode: directory.record.mode,
                size: directory.record.size, version: listing.version,
                created_at: directory.record.created_at,
                accessed_at: directory.record.accessed_at,
                updated_at: directory.record.updated_at,
                changed_at: directory.record.changed_at,
                uid: directory.record.uid, gid: directory.record.gid,
                link_count: directory.record.link_count)
            directory.recordValidatedAt = now
            directory.cacheGeneration = generation
            let entries = listing.entries
            let prefixCount = request == nil ? 2 : 0
            let total = prefixCount + entries.count
            guard cookie.rawValue <= UInt64(total) else {
                throw FSError(.invalidDirectoryCookie)
            }
            let start = Int(cookie.rawValue)
            for index in start..<total {
                if request == nil && index < prefixCount {
                    let isCurrent = index == 0
                    let name = isCurrent ? "." : ".."
                    let itemID = isCurrent || directoryPath == "/"
                        ? directory.itemID : directory.parentID
                    let packed = packer.packEntry(name: FSFileName(string: name),
                        itemType: .directory, itemID: itemID,
                        nextCookie: FSDirectoryCookie(UInt64(index + 1)), attributes: nil)
                    if !packed { break }
                    continue
                }
                let entry = entries[index - prefixCount]
                let path = directoryPath == "/" ? "/\(entry.name)" :
                    "\(directoryPath)/\(entry.name)"
                let record = VexFSStatRecord(path: path, inode: entry.inode, kind: entry.kind,
                                             mode: entry.mode, size: entry.size,
                                             version: entry.version,
                                             created_at: entry.created_at,
                                             accessed_at: entry.accessed_at,
                                             updated_at: entry.updated_at,
                                             changed_at: entry.changed_at,
                                             uid: entry.uid, gid: entry.gid,
                                             link_count: entry.link_count)
                let item = cachedItem(path: path, record: record, parentID: directory.itemID)
                let packed = packer.packEntry(name: FSFileName(string: entry.name),
                    itemType: item.itemType, itemID: item.itemID,
                    nextCookie: FSDirectoryCookie(UInt64(index + 1)),
                    attributes: request == nil ? nil : attributes(for: item, request: request))
                if !packed { break }
            }
            replyHandler(currentVerifier, nil)
        } catch { replyHandler(FSDirectoryVerifier(0), error) }
    }
}
