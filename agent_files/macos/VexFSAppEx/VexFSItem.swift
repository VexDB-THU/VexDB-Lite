import Foundation
import FSKit

final class VexFSItem: FSItem {
    let lock = NSLock()
    var path: String
    var record: VexFSStatRecord
    var parentID: FSItem.Identifier
    var handle: String?
    var handleWritable = false
    var dirtyGeneration: Int64?

    init(path: String, record: VexFSStatRecord, parentID: FSItem.Identifier) {
        self.path = path
        self.record = record
        self.parentID = parentID
        super.init()
    }

    var itemType: FSItem.ItemType {
        record.kind == "directory" ? .directory : .file
    }

    var itemID: FSItem.Identifier {
        path == "/" ? .rootDirectory :
            (FSItem.Identifier(rawValue: record.inode + 1024) ?? .invalid)
    }
}
