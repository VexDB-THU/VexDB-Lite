import Foundation
import FSKit
import OSLog
import CryptoKit

extension Logger {
    static let vexfs = Logger(subsystem: "io.vexdb.vexfs", category: "filesystem")
}

enum VexFSIdentity {
    static func uuid(databasePath: String, workspace: String) -> UUID {
        let normalized = URL(fileURLWithPath: databasePath).standardizedFileURL.path
        let digest = SHA256.hash(data: Data("\(normalized)\u{0}\(workspace)".utf8))
        var bytes = Array(digest.prefix(16))
        bytes[6] = (bytes[6] & 0x0f) | 0x50
        bytes[8] = (bytes[8] & 0x3f) | 0x80
        return UUID(uuid: (bytes[0], bytes[1], bytes[2], bytes[3],
                           bytes[4], bytes[5], bytes[6], bytes[7],
                           bytes[8], bytes[9], bytes[10], bytes[11],
                           bytes[12], bytes[13], bytes[14], bytes[15]))
    }
}

@objc
@available(macOS 26.0, *)
final class VexFSFileSystem: FSUnaryFileSystem & FSUnaryFileSystemOperations {
    private static let descriptorName = ".vexfs-volume.json"
    private var resource: FSPathURLResource?
    private var volume: VexFSVolume?

    private func configuration(for resource: FSPathURLResource) throws
        -> (descriptor: VexFSDescriptor, databaseURL: URL) {
        let root = resource.url.standardizedFileURL
        var isDirectory: ObjCBool = false
        guard FileManager.default.fileExists(atPath: root.path, isDirectory: &isDirectory),
              isDirectory.boolValue else {
            throw POSIXError(.ENOTDIR)
        }
        let descriptorURL = root.appendingPathComponent(Self.descriptorName, isDirectory: false)
        let descriptor = try JSONDecoder().decode(
            VexFSDescriptor.self, from: Data(contentsOf: descriptorURL))
        guard descriptor.version == 2,
              !descriptor.database_file.isEmpty,
              descriptor.database_file != ".",
              descriptor.database_file != "..",
              URL(fileURLWithPath: descriptor.database_file).lastPathComponent ==
                descriptor.database_file else {
            throw POSIXError(.EINVAL)
        }
        let databaseURL = root.appendingPathComponent(
            descriptor.database_file, isDirectory: false).standardizedFileURL
        guard databaseURL.deletingLastPathComponent() == root else {
            throw POSIXError(.EACCES)
        }
        return (descriptor, databaseURL)
    }

    func probeResource(resource: FSResource,
                       replyHandler: @escaping (FSProbeResult?, (any Error)?) -> Void) {
        guard let pathResource = resource as? FSPathURLResource else {
            return replyHandler(nil, POSIXError(.ENODEV))
        }
        do {
            let (descriptor, databaseURL) = try configuration(for: pathResource)
            let identifier = FSContainerIdentifier(uuid:
                VexFSIdentity.uuid(databasePath: databaseURL.path,
                                   workspace: descriptor.workspace))
            replyHandler(.usable(name: "VexFS \(descriptor.workspace)", containerID: identifier), nil)
        } catch {
            Logger.vexfs.debug("probe rejected resource: \(error.localizedDescription)")
            replyHandler(.notRecognized, nil)
        }
    }

    func loadResource(resource: FSResource, options: FSTaskOptions,
                      replyHandler: @escaping (FSVolume?, (any Error)?) -> Void) {
        guard let pathResource = resource as? FSPathURLResource else {
            return replyHandler(nil, POSIXError(.EINVAL))
        }
        for option in options.taskOptions where option.contains("-f") {
            return replyHandler(nil, POSIXError(.ENOTSUP))
        }
        guard pathResource.url.startAccessingSecurityScopedResource() else {
            return replyHandler(nil, POSIXError(.EACCES))
        }
        do {
            let (descriptor, databaseURL) = try configuration(for: pathResource)
            let backend = try VexFSBackend(databasePath: databaseURL.path,
                                           workspace: descriptor.workspace)
            let identity = VexFSIdentity.uuid(databasePath: databaseURL.path,
                                              workspace: descriptor.workspace)
            let loadedVolume = try VexFSVolume(backend: backend, workspace: descriptor.workspace,
                                               volumeID: identity)
            containerStatus = .ready
            self.resource = pathResource
            self.volume = loadedVolume
            replyHandler(loadedVolume, nil)
        } catch {
            Logger.vexfs.error("load rejected resource: \(error.localizedDescription)")
            pathResource.url.stopAccessingSecurityScopedResource()
            replyHandler(nil, error)
        }
    }

    func unloadResource(resource: FSResource, options: FSTaskOptions,
                        replyHandler: @escaping ((any Error)?) -> Void) {
        guard let pathResource = resource as? FSPathURLResource,
              pathResource.url == self.resource?.url else {
            return replyHandler(POSIXError(.EINVAL))
        }
        do { try volume?.synchronizeNow() } catch {
            Logger.vexfs.error("unload synchronize failed: \(error.localizedDescription)")
        }
        self.volume = nil
        self.resource = nil
        pathResource.url.stopAccessingSecurityScopedResource()
        replyHandler(nil)
    }
}
