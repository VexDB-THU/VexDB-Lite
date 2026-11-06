import Foundation
import ImageIO
import UIKit

enum BundledImageAsset {
    static func url(for resource: String) -> URL? {
        let source = URL(fileURLWithPath: resource)
        let name = source.deletingPathExtension().lastPathComponent
        return Bundle.main.url(forResource: name, withExtension: source.pathExtension)
    }
}

final class ImageAssetCache {
    static let shared = ImageAssetCache()

    private let images = NSCache<NSString, UIImage>()

    private init() {
        images.countLimit = 48
        images.totalCostLimit = 24 * 1024 * 1024
    }

    func bundledImage(resource: String, maxPixelSize: CGFloat = 1_200) -> UIImage? {
        guard let url = BundledImageAsset.url(for: resource) else { return nil }
        return image(at: url, maxPixelSize: maxPixelSize)
    }

    func image(at url: URL, maxPixelSize: CGFloat) -> UIImage? {
        let key = "file:\(url.path)#\(Int(maxPixelSize))" as NSString
        if let cached = images.object(forKey: key) { return cached }
        guard let source = CGImageSourceCreateWithURL(url as CFURL, nil),
              let image = downsample(source: source, maxPixelSize: maxPixelSize) else { return nil }
        store(image, key: key)
        return image
    }

    func image(data: Data, key value: String, maxPixelSize: CGFloat) -> UIImage? {
        let key = "data:\(value)#\(Int(maxPixelSize))" as NSString
        if let cached = images.object(forKey: key) { return cached }
        guard let source = CGImageSourceCreateWithData(data as CFData, nil),
              let image = downsample(source: source, maxPixelSize: maxPixelSize) else { return nil }
        store(image, key: key)
        return image
    }

    private func downsample(source: CGImageSource, maxPixelSize: CGFloat) -> UIImage? {
        let options: [CFString: Any] = [
            kCGImageSourceCreateThumbnailFromImageAlways: true,
            kCGImageSourceCreateThumbnailWithTransform: true,
            kCGImageSourceShouldCacheImmediately: true,
            kCGImageSourceThumbnailMaxPixelSize: max(1, Int(maxPixelSize))
        ]
        guard let image = CGImageSourceCreateThumbnailAtIndex(source, 0, options as CFDictionary)
        else { return nil }
        return UIImage(cgImage: image)
    }

    private func store(_ image: UIImage, key: NSString) {
        let cost = image.cgImage.map { $0.bytesPerRow * $0.height } ?? 0
        images.setObject(image, forKey: key, cost: cost)
    }
}
