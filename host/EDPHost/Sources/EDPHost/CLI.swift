import CoreGraphics
import Foundation
import ImageIO

/// 无界面模式：给脚本和“与 Python 版逐字节对齐”的验证用。
///
///   EDPHost --dither-raw 1200 1600 in.rgb out.bin
///   EDPHost --convert photo.jpg out.bin [--rotate 90] [--fit cover]
///                                      [--dither fs-ser] [--strength 1.0] [--frame-only]
///   EDPHost --crc32 file
///
/// 输出一行 key=value 摘要，方便脚本 grep。
enum CLI {

    static func run(arguments: [String]) {
        guard let command = arguments.first else {
            printUsage()
            exit(2)
        }
        let rest = Array(arguments.dropFirst())

        switch command {
        case "--dither-raw":
            ditherRaw(rest)
        case "--convert":
            convert(rest)
        case "--crc32":
            crc(rest)
        case "--api-selftest":
            apiSelftest(rest)
        case "--render-crop":
            renderCrop(rest)
        case "--selftest-crop":
            selftestCrop()
        case "--oneclick-selftest":
            oneClickSelftest(rest)
        case "--help", "-h":
            printUsage()
            exit(0)
        default:
            printUsage()
            exit(2)
        }
    }

    private static func printUsage() {
        print("""
        EDPHost —— GDEP133C02 上位机

        图形界面：直接运行（无参数）

        命令行（用于验证 / 批处理）：
          --dither-raw <w> <h> <in.rgb> <out.bin> [mode] [strength]
                                    裸 RGB888 → 抖动 → 打包（不加 EPD6BIN 头）
          --convert <in> <out.bin> [选项]           完整管线，默认输出 EPD6BIN
              --rotate 0|90|180|270   顺时针旋转（默认 0）
              --fit cover|contain|stretch        （默认 cover）
              --dither fs-ser|fs|atkinson|stucki|jjn|none （默认 fs-ser）
              --strength 0..1                    （默认 1.0）
              --crop x,y,w,h                     归一化裁切框（相对旋转后的图，0…1）
              --frame-only                       只写 960000 B 帧数据
          --crc32 <file>
          --api-selftest <http://host:port>  对（真机或 mock）跑一遍完整 HTTP 流程
          --render-crop <in> <out.png> [w h] [rotate]
                                            离屏渲染裁切编辑器（调试用）
          --selftest-crop                   裁切几何自检（比例 / 旋转映射）
          --oneclick-selftest <image> <ip>  无界面跑一遍“一键生成并上传”（真机联调用）
        """)
    }

    private static func fail(_ message: String) -> Never {
        FileHandle.standardError.write(Data("错误：\(message)\n".utf8))
        exit(1)
    }

    // MARK: 裸 RGB 抖动（用于和 Python 版逐字节比对）

    private static func ditherRaw(_ args: [String]) {
        guard args.count >= 4,
              let width = Int(args[0]),
              let height = Int(args[1]) else {
            fail("用法：--dither-raw <w> <h> <in.rgb> <out.bin>")
        }
        guard width == Spectrum6.width, height == Spectrum6.height else {
            fail("尺寸必须是 \(Spectrum6.width)x\(Spectrum6.height)")
        }

        let inputPath = args[2]
        let outputPath = args[3]

        guard let data = try? Data(contentsOf: URL(fileURLWithPath: inputPath)) else {
            fail("读不到 \(inputPath)")
        }
        let expected = width * height * 3
        guard data.count == expected else {
            fail("裸 RGB 长度是 \(data.count)，应为 \(expected)")
        }

        var rgb = [Float](repeating: 0, count: expected)
        data.withUnsafeBytes { raw in
            let bytes = raw.bindMemory(to: UInt8.self)
            for i in 0..<bytes.count {
                rgb[i] = Float(bytes[i])
            }
        }

        let mode = args.count > 4 ? (DitherMode(rawValue: args[4]) ?? .fsSer) : .fsSer
        let strength = args.count > 5 ? (Double(args[5]) ?? 1.0) : 1.0

        let indices = Dither.quantize(rgb: rgb, mode: mode, strength: strength)
        let packed = Packing.pack(indices)
        let output = Data(packed)

        do {
            try output.write(to: URL(fileURLWithPath: outputPath))
        } catch {
            fail("写 \(outputPath) 失败：\(error.localizedDescription)")
        }

        print("bytes=\(packed.count) crc32=\(CRC32.hex(CRC32.compute(output))) mode=\(mode.rawValue)")
    }

    // MARK: 完整转换

    private static func convert(_ args: [String]) {
        var positional: [String] = []
        var settings = ConvertSettings()
        var crop: CGRect?
        var frameOnly = false

        var i = 0
        while i < args.count {
            let arg = args[i]
            switch arg {
            case "--rotate":
                i += 1
                settings.rotate = Int(args[safe: i] ?? "0") ?? 0
            case "--fit":
                i += 1
                settings.fit = FitMode(rawValue: args[safe: i] ?? "cover") ?? .cover
            case "--dither":
                i += 1
                settings.dither = DitherMode(rawValue: args[safe: i] ?? "fs-ser") ?? .fsSer
            case "--strength":
                i += 1
                settings.strength = Double(args[safe: i] ?? "1") ?? 1.0
            case "--crop":
                i += 1
                crop = parseCrop(args[safe: i])
            case "--frame-only":
                frameOnly = true
            default:
                positional.append(arg)
            }
            i += 1
        }

        guard positional.count >= 2 else {
            fail("用法：--convert <in> <out.bin> [选项]")
        }

        let inputPath = positional[0]
        let outputPath = positional[1]

        do {
            let image = try ImageConverter.loadImage(url: URL(fileURLWithPath: inputPath))
            let result = try ImageConverter.convert(cgImage: image,
                                                    settings: settings,
                                                    crop: crop)
            let output = frameOnly ? Data(result.packed) : Packing.makeBin(frame: result.packed)

            try output.write(to: URL(fileURLWithPath: outputPath))

            let shares = result.colorShares.map { String(format: "%.4f", $0) }.joined(separator: ",")
            print("bytes=\(output.count) crc32=\(CRC32.hex(CRC32.compute(output))) "
                  + "elapsed=\(String(format: "%.2f", result.elapsed)) shares=\(shares)")
        } catch {
            fail(error.localizedDescription)
        }
    }

    // MARK: HTTP 端到端自检

    private final class Box<T>: @unchecked Sendable {
        var value: Result<T, Error>?
    }

    private static func runAsync<T>(_ operation: @escaping () async throws -> T) throws -> T {
        let semaphore = DispatchSemaphore(value: 0)
        let box = Box<T>()
        Task {
            do {
                box.value = .success(try await operation())
            } catch {
                box.value = .failure(error)
            }
            semaphore.signal()
        }
        semaphore.wait()
        guard let result = box.value else {
            throw DeviceError.network("异步任务没有返回结果")
        }
        return try result.get()
    }

    private static func apiSelftest(_ args: [String]) {
        guard let raw = args.first, let url = URL(string: raw),
              let host = url.host else {
            fail("用法：--api-selftest http://127.0.0.1:8080")
        }

        let device = DiscoveredDevice(ip: host,
                                      mac: "selftest",
                                      hostname: "selftest",
                                      httpPort: UInt16(url.port ?? 80))
        let client = DeviceClient()
        var failures = 0

        func check(_ name: String, _ body: () throws -> String) {
            do {
                let detail = try body()
                print("通过  \(name)：\(detail)")
            } catch {
                failures += 1
                print("失败  \(name)：\(error.localizedDescription)")
            }
        }

        check("GET /api/v1/status") {
            let status = try runAsync { try await client.status(for: device) }
            return "\(status.hostname) 固件 \(status.firmware) 面板 \(status.panel.width)x\(status.panel.height) "
                + "SD=\(status.sd.mounted) 屏幕=\(status.display.currentName)"
        }

        let frame = [UInt8](repeating: 0x11, count: Spectrum6.bytesPerFrame)
        let bin = Packing.makeBin(frame: frame)

        var uploadedFile = ""
        check("POST /api/v1/image") {
            let result = try runAsync {
                try await client.upload(bin: bin,
                                        imageName: "selftest.bin",
                                        timestamp: Date(),
                                        to: device)
            }
            uploadedFile = result.file
            return "\(result.file) \(result.bytes) B crc=\(CRC32.hex(result.crc32)) "
                + "设备端 \(result.receivedMs) ms queued=\(result.queued)"
        }

        check("GET /api/v1/history") {
            let response = try runAsync { try await client.history(for: device) }
            return "归档 \(response.count) 张，当前 \(response.current)"
        }

        if !uploadedFile.isEmpty {
            check("POST /api/v1/display") {
                try runAsync { try await client.show(file: uploadedFile, on: device) }
                return "已请求重新显示 \(uploadedFile)"
            }
        }

        check("POST /api/v1/clear") {
            try runAsync { try await client.clear(color: "white", on: device) }
            return "已请求整屏白色"
        }

        if failures > 0 {
            fail("自检失败 \(failures) 项")
        }
        print("全部通过")
    }

    /// 裁切几何自检：确认“先裁切后旋转”映射后仍然是面板 3:4
    private static func selftestCrop() {
        let panel = CGFloat(Spectrum6.width) / CGFloat(Spectrum6.height)
        var failures = 0

        func expect(_ name: String, _ value: CGFloat, _ target: CGFloat, tolerance: CGFloat = 1e-6) {
            let ok = abs(value - target) <= tolerance
            if !ok { failures += 1 }
            print(String(format: "%@ %@ = %.6f（期望 %.6f）", ok ? "通过" : "失败", name, value, target))
        }

        /* 1) 旋转决定裁切框在源图里的比例 */
        expect("frameAspect(0)", CropMath.frameAspect(rotate: 0), panel)
        expect("frameAspect(90)", CropMath.frameAspect(rotate: 90), 1 / panel)
        expect("frameAspect(180)", CropMath.frameAspect(rotate: 180), panel)
        expect("frameAspect(270)", CropMath.frameAspect(rotate: 270), 1 / panel)

        /* 2) 自动裁切 + 旋转映射后，落到面板上的像素比必须还是 3:4 */
        let portraits = [CGSize(width: 1200, height: 1600),
                         CGSize(width: 900, height: 1600),
                         CGSize(width: 1200, height: 900)]
        for size in portraits {
            for rotate in [0, 90, 180, 270] {
                let aspect = CropMath.frameAspect(rotate: rotate)
                let crop = CropMath.centeredMax(imageSize: size, aspect: aspect)
                let mapped = CropMath.rotated(crop, rotate: rotate)
                let swapped = (rotate == 90 || rotate == 270)
                let rotatedSize = swapped ? CGSize(width: size.height, height: size.width) : size
                let px = CropMath.pixelSize(mapped, imageSize: rotatedSize)
                let ratio = px.width / px.height
                expect("映射后比例 \(Int(size.width))×\(Int(size.height)) rot=\(rotate)",
                       ratio, panel, tolerance: 1e-5)
            }
        }

        /* 3) 90° 转四次回到原位 */
        var rect = CGRect(x: 0.1, y: 0.2, width: 0.3, height: 0.4)
        for _ in 0..<4 { rect = CropMath.rotated(rect, rotate: 90) }
        expect("四次 90° 回原位 x", rect.minX, 0.1, tolerance: 1e-9)
        expect("四次 90° 回原位 y", rect.minY, 0.2, tolerance: 1e-9)

        if failures > 0 {
            fail("裁切几何自检失败 \(failures) 项")
        }
        print("裁切几何自检全部通过")
    }

    private static func oneClickSelftest(_ args: [String]) {
        guard args.count >= 2 else {
            fail("用法：--oneclick-selftest <image> <ip>")
        }
        if #available(macOS 14.0, *) {
            do {
                try MainActor.assumeIsolated {
                    try DevRender.oneClickSelftest(imagePath: args[0], ip: args[1])
                }
            } catch {
                fail(error.localizedDescription)
            }
        } else {
            fail("--oneclick-selftest 需要 macOS 14+")
        }
    }

    private static func renderCrop(_ args: [String]) {
        guard args.count >= 2 else {
            fail("用法：--render-crop <in> <out.png> [宽 高] [rotate]")
        }
        let width = args.count >= 4 ? CGFloat(Double(args[2]) ?? 520) : 520
        let height = args.count >= 4 ? CGFloat(Double(args[3]) ?? 640) : 640
        let rotate = args.count >= 5 ? (Int(args[4]) ?? 0) : 0

        if #available(macOS 14.0, *) {
            do {
                try MainActor.assumeIsolated {
                    try DevRender.renderCrop(imagePath: args[0], output: args[1],
                                             width: width, height: height,
                                             rotate: rotate)
                }
            } catch {
                fail(error.localizedDescription)
            }
        } else {
            fail("--render-crop 需要 macOS 14+")
        }
    }

    private static func parseCrop(_ text: String?) -> CGRect? {
        guard let text else { return nil }
        let parts = text.split(separator: ",").compactMap { Double($0.trimmingCharacters(in: .whitespaces)) }
        guard parts.count == 4 else { return nil }
        return CGRect(x: parts[0], y: parts[1], width: parts[2], height: parts[3])
    }

    private static func crc(_ args: [String]) {
        guard let path = args.first,
              let data = try? Data(contentsOf: URL(fileURLWithPath: path)) else {
            fail("用法：--crc32 <file>")
        }
        print("bytes=\(data.count) crc32=\(CRC32.hex(CRC32.compute(data)))")
    }
}

private extension Array {
    subscript(safe index: Int) -> Element? {
        indices.contains(index) ? self[index] : nil
    }
}
