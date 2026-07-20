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
        capabilities.supportsSymbolicLinks = false
        capabilities.supportsHardLinks = false
        capabilities.supportsHiddenFiles = true
        capabilities.doesNotSupportVolumeSizes = true
        capabilities.doesNotSupportImmutableFiles = true
        capabilities.doesNotSupportSettingFilePermissions = true
        capabilities.caseFormat = .sensitive
        return capabilities
    }

    func activate(options: FSTaskOptions,
                  replyHandler: @escaping (FSItem?, (any Error)?) -> Void) {
        replyHandler(rootItem, nil)
    }

    func deactivate(options: FSDeactivateOptions = [],
                    replyHandler: @escaping ((any Error)?) -> Void) {
        do { try synchronizeNow(); replyHandler(nil) } catch { replyHandler(error) }
    }

    func mount(options: FSTaskOptions, replyHandler: @escaping ((any Error)?) -> Void) {
        replyHandler(nil)
    }

    func unmount(replyHandler: @escaping () -> Void) {
        try? synchronizeNow()
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
        item.lock.lock()
        defer { item.lock.unlock() }
        do {
            let path = try currentPath(for: item)
            if item.dirtyGeneration == nil {
                item.record = try backend.stat(path: path)
            }
            replyHandler(attributes(for: item, request: desiredAttributes), nil)
        } catch { replyHandler(nil, error) }
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
            if newAttributes.isValid(.size), item.itemType == .file {
                let hadHandle = item.handle != nil
                if item.handle == nil || !item.handleWritable {
                    if let old = item.handle { try backend.closeHandle(old, retain: true) }
                    item.handle = try backend.openHandle(path: path, flags: "rw")
                    item.handleWritable = true
                    item.dirtyGeneration = nil
                }
                let generation = try backend.truncate(handle: item.handle!, size: newAttributes.size)
                item.dirtyGeneration = generation
                newAttributes.consumedAttributes.insert(.size)
                if !hadHandle {
                    _ = try backend.publish(handle: item.handle!, generation: generation)
                    try backend.closeHandle(item.handle!, retain: true)
                    item.handle = nil
                    item.handleWritable = false
                    item.dirtyGeneration = nil
                    item.record = try backend.stat(path: path)
                } else {
                    item.record = VexFSStatRecord(path: path, inode: item.record.inode,
                        kind: item.record.kind, mode: item.record.mode, size: newAttributes.size,
                        version: item.record.version, created_at: item.record.created_at,
                        updated_at: Int64(Date().timeIntervalSince1970 * 1000))
                }
            } else {
                item.record = try backend.stat(path: path)
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
            let path = try childPath(parent: directory, name: nameString)
            let record = try backend.stat(path: path)
            replyHandler(cachedItem(path: path, record: record, parentID: directory.itemID), name, nil)
        } catch { replyHandler(nil, nil, error) }
    }

    func reclaimItem(_ item: FSItem, replyHandler: @escaping ((any Error)?) -> Void) {
        do { try backend.reclaim(); replyHandler(nil) } catch { replyHandler(error) }
    }

    func readSymbolicLink(_ item: FSItem,
                          replyHandler: @escaping (FSFileName?, (any Error)?) -> Void) {
        replyHandler(nil, POSIXError(.ENOTSUP))
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
            switch type {
            case .directory: try backend.mkdir(path: path)
            case .file: _ = try backend.writeFile(path: path, data: Data())
            default: throw POSIXError(.ENOTSUP)
            }
            let record = try backend.stat(path: path)
            let item = cachedItem(path: path, record: record, parentID: directory.itemID)
            replyHandler(item, name, nil)
        } catch { replyHandler(nil, nil, error) }
    }

    func createSymbolicLink(named name: FSFileName, inDirectory directory: FSItem,
                            attributes newAttributes: FSItem.SetAttributesRequest,
                            linkContents contents: FSFileName,
                            replyHandler: @escaping (FSItem?, FSFileName?, (any Error)?) -> Void) {
        replyHandler(nil, nil, POSIXError(.ENOTSUP))
    }

    func createLink(to item: FSItem, named name: FSFileName, inDirectory directory: FSItem,
                    replyHandler: @escaping (FSFileName?, (any Error)?) -> Void) {
        replyHandler(nil, POSIXError(.ENOTSUP))
    }

    func removeItem(_ item: FSItem, named name: FSFileName, fromDirectory directory: FSItem,
                    replyHandler: @escaping ((any Error)?) -> Void) {
        guard let item = item as? VexFSItem else { return replyHandler(POSIXError(.EINVAL)) }
        do {
            try backend.remove(path: currentPath(for: item))
            dropCachedItems()
            replyHandler(nil)
        } catch { replyHandler(error) }
    }

    func renameItem(_ item: FSItem, inDirectory sourceDirectory: FSItem,
                    named sourceName: FSFileName, to destinationName: FSFileName,
                    inDirectory destinationDirectory: FSItem, overItem: FSItem?,
                    replyHandler: @escaping (FSFileName?, (any Error)?) -> Void) {
        guard let source = item as? VexFSItem,
              let destinationDirectory = destinationDirectory as? VexFSItem,
              let destination = destinationName.string else {
            return replyHandler(nil, POSIXError(.EINVAL))
        }
        do {
            let sourcePath = try currentPath(for: source)
            let destinationPath = try childPath(parent: destinationDirectory, name: destination)
            try backend.rename(source: sourcePath, destination: destinationPath,
                               replace: overItem != nil)
            source.path = destinationPath
            source.record = try backend.stat(path: destinationPath)
            source.parentID = destinationDirectory.itemID
            dropCachedItems()
            replyHandler(destinationName, nil)
        } catch { replyHandler(nil, error) }
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
            let directoryPath = try currentPath(for: directory)
            directory.record = try backend.stat(path: directoryPath)
            let currentVerifier = FSDirectoryVerifier(directory.record.version)
            if verifier.rawValue != 0 && verifier.rawValue != currentVerifier.rawValue {
                throw FSError(.invalidDirectoryCookie)
            }
            let entries = try backend.list(path: directoryPath)
            let verifiedRecord = try backend.stat(path: directoryPath)
            guard verifiedRecord.version == directory.record.version else {
                throw FSError(.invalidDirectoryCookie)
            }
            directory.record = verifiedRecord
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
                                             updated_at: entry.updated_at)
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
