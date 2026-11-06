import Foundation
import FSKit

@main
@available(macOS 26.0, *)
struct VexFSAppEx: UnaryFileSystemExtension {
    typealias FileSystem = FSUnaryFileSystem & FSUnaryFileSystemOperations

    var fileSystem: FSUnaryFileSystem & FSUnaryFileSystemOperations {
        VexFSFileSystem()
    }
}
