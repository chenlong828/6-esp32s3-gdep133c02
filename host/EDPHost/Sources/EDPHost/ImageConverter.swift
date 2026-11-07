import CoreGraphics
import CoreImage
import Foundation
import ImageIO

struct ConversionResult {
    let indices: [UInt8]        // 1200×1600 调色板索引
    let packed: [UInt8]         // 960 000 B（4bpp）
    let preview: CGImage        // 面板实测色预览
    let colorShares: [Double]   // 6 色占比，顺序同 Spectrum6.colorNames
    let elapsed: TimeInterval
}

enum ImageConverterError: LocalizedError {
    case cannotOpen(String)
    case cannotRender
    case tooSmall

    var errorDescription: String? {
        switch self {
        case .cannotOpen(let name): return "无法读取图片：\(name)"
        case .cannotRender: return "图像渲染失败（尺寸或色彩空间不受支持）"
        case .tooSmall: return "图片有效像素太小"
        }
    }
}

enum ImageConverter {

    // MARK: 载入

    /// 原图（用于真正转换）。EXIF 方向已归正，长边不缩。
    static func loadImage(url: URL) throws -> CGImage {
        let (image, orientation) = try sourceImage(url: url)
        return applyOrientation(image, orientation)
    }

    /// 缩略图（用于左侧裁切编辑器）。长边限制在 maxPixel，EXIF 方向同样归正。
    /// 裁切框用归一化坐标，所以缩略图和原图的裁切结果一致。
    static func loadThumbnail(url: URL, maxPixel: Int = 2048) throws -> CGImage {
        let (image, orientation) = try sourceImage(url: url, maxPixel: maxPixel)
        return applyOrientation(image, orientation)
    }

    private static func sourceImage(url: URL,
                                    maxPixel: Int? = nil) throws -> (CGImage, CGImagePropertyOrientation) {
        guard let source = CGImageSourceCreateWithURL(url as CFURL, nil) else {
            throw ImageConverterError.cannotOpen(url.lastPathComponent)
        }

        let props = CGImageSourceCopyPropertiesAtIndex(source, 0, nil) as? [CFString: Any]
        let raw = (props?[kCGImagePropertyOrientation] as? NSNumber)?.uint32Value ?? 1
        let orientation = CGImagePropertyOrientation(rawValue: raw) ?? .up

        if let maxPixel {
            let options: [CFString: Any] = [
                kCGImageSourceCreateThumbnailFromImageAlways: true,
                /* 变换自己统一做，保证缩略图和原图的处理路径一致 */
                kCGImageSourceCreateThumbnailWithTransform: false,
                kCGImageSourceThumbnailMaxPixelSize: maxPixel,
                kCGImageSourceShouldCacheImmediately: true,
            ]
            guard let thumb = CGImageSourceCreateThumbnailAtIndex(source, 0, options as CFDictionary) else {
                throw ImageConverterError.cannotOpen(url.lastPathComponent)
            }
            return (thumb, orientation)
        }

        let options: [CFString: Any] = [kCGImageSourceShouldCache: false]
        guard let image = CGImageSourceCreateImageAtIndex(source, 0, options as CFDictionary) else {
            throw ImageConverterError.cannotOpen(url.lastPathComponent)
        }
        return (image, orientation)
    }

    /// 按 EXIF 方向把图转正（手机竖拍照片常见 6/8）
    private static func applyOrientation(_ image: CGImage,
                                         _ orientation: CGImagePropertyOrientation) -> CGImage {
        guard orientation != .up else { return image }
        let ci = CIImage(cgImage: image)
            .oriented(forExifOrientation: Int32(orientation.rawValue))
        let context = CIContext(options: [.useSoftwareRenderer: false])
        return context.createCGImage(ci, from: ci.extent) ?? image
    }

    // MARK: 主流程

    /// - Parameter crop: 归一化裁切框（相对**旋转后**的源图）；nil = 居中最大裁切。
    ///   只在 `fit == .cover` 时生效。
    static func convert(cgImage: CGImage,
                        settings: ConvertSettings,
                        crop: CGRect? = nil,
                        progress: ((Double) -> Void)? = nil) throws -> ConversionResult {
        let started = Date()

        progress?(0.05)
        let rgb = try renderRGB(cgImage: cgImage, settings: settings, crop: crop)
        progress?(0.45)

        let indices = Dither.quantize(rgb: rgb,
                                      mode: settings.dither,
                                      strength: settings.strength)
        progress?(0.9)

        let packed = Packing.pack(indices)
        guard let preview = PreviewRenderer.image(indices: indices,
                                                  palette: Spectrum6.realPalette) else {
            throw ImageConverterError.cannotRender
        }
        let shares = colorShares(indices)

        progress?(1.0)

        return ConversionResult(indices: indices,
                                packed: packed,
                                preview: preview,
                                colorShares: shares,
                                elapsed: Date().timeIntervalSince(started))
    }

    // MARK: 渲染成 1200×1600 RGB

    private static func renderRGB(cgImage: CGImage,
                                  settings: ConvertSettings,
                                  crop: CGRect?) throws -> [Float] {
        let w = Spectrum6.width
        let h = Spectrum6.height

        var source = cgImage
        if settings.rotate % 360 != 0 {
            source = try rotate(source, clockwiseDegrees: settings.rotate)
        }

        guard let space = CGColorSpace(name: CGColorSpace.sRGB),
              let ctx = CGContext(data: nil,
                                  width: w,
                                  height: h,
                                  bitsPerComponent: 8,
                                  bytesPerRow: w * 4,
                                  space: space,
                                  bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue)
        else {
            throw ImageConverterError.cannotRender
        }

        ctx.interpolationQuality = .high
        ctx.setFillColor(CGColor(red: 1, green: 1, blue: 1, alpha: 1))
        ctx.fill(CGRect(x: 0, y: 0, width: w, height: h))
        ctx.clip(to: CGRect(x: 0, y: 0, width: w, height: h))

        let srcW = CGFloat(source.width)
        let srcH = CGFloat(source.height)
        guard srcW > 0, srcH > 0 else { throw ImageConverterError.tooSmall }

        let target = CGSize(width: w, height: h)
        let imageSize = CGSize(width: srcW, height: srcH)

        switch settings.fit {
        case .cover:
            /* 打开「允许超出原图」时框可以超出画面，超出的地方保持画布的白底 */
            let overscan = settings.allowOverscan
            let fallback = CropMath.defaultCrop(imageSize: imageSize,
                                                aspect: CropMath.panelAspect,
                                                allowsOverscan: overscan)
            let rect = CropMath.sanitized(crop ?? fallback,
                                          imageSize: imageSize,
                                          allowsOverscan: overscan)
            let cropPx = CGRect(x: rect.minX * srcW,
                                y: rect.minY * srcH,
                                width: rect.width * srcW,
                                height: rect.height * srcH)
            guard cropPx.width > 1, cropPx.height > 1 else {
                throw ImageConverterError.tooSmall
            }

            /* 裁切框比例 = 面板比例，scale 两轴相同；max 只是防浮点误差 */
            let scale = max(target.width / cropPx.width, target.height / cropPx.height)

            /*
             * 源图按 top-left 坐标裁切，而 CGContext 用户空间是 y 向上：
             *   crop 左上角 → 面板左上角(0, panelH)
             *   crop 右下角 → 面板右下角(panelW, 0)
             * 所以 y 方向要按“裁切框下边界到图底”的距离反推。
             */
            let drawRect = CGRect(x: -cropPx.minX * scale,
                                  y: -(srcH - cropPx.maxY) * scale,
                                  width: srcW * scale,
                                  height: srcH * scale)
            ctx.draw(source, in: drawRect)

        case .contain:
            let scale = min(target.width / srcW, target.height / srcH)
            let dw = srcW * scale
            let dh = srcH * scale
            ctx.draw(source, in: CGRect(x: (target.width - dw) / 2,
                                        y: (target.height - dh) / 2,
                                        width: dw,
                                        height: dh))

        case .stretch:
            ctx.draw(source, in: CGRect(origin: .zero, size: target))
        }

        guard let data = ctx.data else { throw ImageConverterError.cannotRender }
        let base = data.bindMemory(to: UInt8.self, capacity: w * h * 4)

        var rgb = [Float](repeating: 0, count: w * h * 3)
        rgb.withUnsafeMutableBufferPointer { out in
            var src = 0
            var dst = 0
            while dst < out.count {
                out[dst] = Float(base[src])
                out[dst + 1] = Float(base[src + 1])
                out[dst + 2] = Float(base[src + 2])
                src += 4
                dst += 3
            }
        }
        return rgb
    }

    // MARK: 旋转（顺时针 90° 的倍数，不重采样）

    static func rotate(_ image: CGImage, clockwiseDegrees degrees: Int) throws -> CGImage {
        let d = ((degrees % 360) + 360) % 360
        if d == 0 { return image }

        let swap = (d == 90 || d == 270)
        let w = swap ? image.height : image.width
        let h = swap ? image.width : image.height

        guard let space = image.colorSpace ?? CGColorSpace(name: CGColorSpace.sRGB),
              let ctx = CGContext(data: nil,
                                  width: w,
                                  height: h,
                                  bitsPerComponent: 8,
                                  bytesPerRow: 0,
                                  space: space,
                                  bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)
        else {
            throw ImageConverterError.cannotRender
        }

        ctx.interpolationQuality = .none
        ctx.translateBy(x: CGFloat(w) / 2, y: CGFloat(h) / 2)
        /* CGContext 的正角是逆时针；顺时针用负角 */
        ctx.rotate(by: -CGFloat(d) * .pi / 180)
        ctx.translateBy(x: -CGFloat(image.width) / 2, y: -CGFloat(image.height) / 2)
        ctx.draw(image, in: CGRect(x: 0, y: 0,
                                   width: CGFloat(image.width),
                                   height: CGFloat(image.height)))

        guard let out = ctx.makeImage() else { throw ImageConverterError.cannotRender }
        return out
    }

    // MARK: 统计

    static func colorShares(_ indices: [UInt8]) -> [Double] {
        var counts = [Int](repeating: 0, count: Spectrum6.colorCount)
        for v in indices where Int(v) < counts.count {
            counts[Int(v)] += 1
        }
        let total = Double(max(1, indices.count))
        return counts.map { Double($0) / total }
    }
}

// MARK: - 预览渲染

enum PreviewRenderer {
    /// 索引 → CGImage（用给定调色板着色）
    static func image(indices: [UInt8], palette: [Float]) -> CGImage? {
        let w = Spectrum6.width
        let h = Spectrum6.height
        guard indices.count == w * h else { return nil }

        var rgba = [UInt8](repeating: 255, count: w * h * 4)
        rgba.withUnsafeMutableBufferPointer { out in
            var i = 0
            var o = 0
            while i < indices.count {
                let c = Int(indices[i]) * 3
                out[o] = UInt8(max(0, min(255, palette[c])))
                out[o + 1] = UInt8(max(0, min(255, palette[c + 1])))
                out[o + 2] = UInt8(max(0, min(255, palette[c + 2])))
                out[o + 3] = 255
                i += 1
                o += 4
            }
        }

        guard let provider = CGDataProvider(data: Data(rgba) as CFData),
              let space = CGColorSpace(name: CGColorSpace.sRGB)
        else { return nil }

        return CGImage(width: w,
                       height: h,
                       bitsPerComponent: 8,
                       bitsPerPixel: 32,
                       bytesPerRow: w * 4,
                       space: space,
                       bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.noneSkipLast.rawValue),
                       provider: provider,
                       decode: nil,
                       shouldInterpolate: false,
                       intent: .defaultIntent)
    }
}
