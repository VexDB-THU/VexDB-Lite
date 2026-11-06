import Foundation
import FSKit

final class VexFSItem: FSItem {
    let lock = NSLock()
    var path: String
    var record: VexFSStatRecord
    var recordValidatedAt: TimeInterval
    var cacheGeneration: UInt64
    var parentID: FSItem.Identifier
    var handle: String?
    var handleWritable = false
    var dirtyGeneration: Int64?
    // 小型只读文件在一次 open/close 生命周期内保留内容快照，避免文本搜索为
    // 每个文件创建和关闭数据库 handle。写打开和大文件仍使用数据库 handle。
    var readCache: Data?
    var isUnlinked = false

    init(path: String, record: VexFSStatRecord, parentID: FSItem.Identifier,
         cacheGeneration: UInt64) {
        self.path = path
        self.record = record
        self.recordValidatedAt = ProcessInfo.processInfo.systemUptime
        self.cacheGeneration = cacheGeneration
        self.parentID = parentID
        super.init()
    }

    var itemType: FSItem.ItemType {
        switch record.kind {
        case "directory": return .directory
        case "symlink": return .symlink
        default: return .file
        }
    }

    var itemID: FSItem.Identifier {
        path == "/" ? .rootDirectory :
            (FSItem.Identifier(rawValue: record.inode + 1024) ?? .invalid)
    }
}
