import Foundation

// MARK: - 面板与图像参数

/// E Ink Spectra 6（6 色、4bpp）面板参数，与固件 GDEP133C02.h 对应。
enum Spectrum6 {
    static let width = 1200
    static let height = 1600
    static let bytesPerFrame = width * height / 2   // 960 000
    static let colorCount = 6
    static let colorNames = ["白", "黑", "红", "黄", "蓝", "绿"]

    /// 面板实测色：抖动误差必须对着它算，不能对着纯 RGB
    static let realPalette: [Float] = [
        255, 255, 255,   // 0 白
        0, 0, 0,         // 1 黑
        160, 32, 32,     // 2 红
        240, 224, 80,    // 3 黄
        80, 128, 184,    // 4 蓝
        96, 128, 80,     // 5 绿
    ]

    /// 预览用纯色（更接近“屏幕上的观感”）
    static let driverPalette: [Float] = [
        255, 255, 255,
        0, 0, 0,
        255, 0, 0,
        255, 255, 0,
        0, 0, 255,
        0, 255, 0,
    ]

    /// 4bit 像素码：白1 黑0 黄2 红3 蓝5 绿6（4/7 不用）
    static let codes: [UInt8] = [0x1, 0x0, 0x3, 0x2, 0x5, 0x6]

    static let magic = Array("EPD6BIN".utf8)
}

// MARK: - 转换选项

enum FitMode: String, CaseIterable, Identifiable {
    case cover
    case contain
    case stretch

    var id: String { rawValue }

    var label: String {
        switch self {
        case .cover: return "铺满裁切"
        case .contain: return "完整留白"
        case .stretch: return "拉伸铺满"
        }
    }

    var hint: String {
        switch self {
        case .cover: return "在裁切框内铺满面板，可拖动/缩放裁切框（推荐）"
        case .contain: return "等比缩小到完整可见，四周补白（不裁切）"
        case .stretch: return "直接拉伸到 1200×1600，会变形（不裁切）"
        }
    }
}

enum DitherMode: String, CaseIterable, Identifiable {
    case fsSer = "fs-ser"
    case fs
    case atkinson
    case stucki
    case jjn
    case none

    var id: String { rawValue }

    var label: String {
        switch self {
        case .fsSer: return "FS 蛇形（推荐）"
        case .fs: return "FS 逐行"
        case .atkinson: return "Atkinson"
        case .stucki: return "Stucki"
        case .jjn: return "Jarvis-Judice-Ninke"
        case .none: return "不抖动"
        }
    }
}

struct ConvertSettings: Equatable {
    var rotate: Int = 0          // 顺时针 0/90/180/270
    var fit: FitMode = .cover
    var dither: DitherMode = .fsSer
    var strength: Double = 1.0
    /// 裁切框允许超出原图（空白填白）；关掉时裁切框只能落在画面内
    var allowOverscan: Bool = false
}

// MARK: - 抖动

enum Dither {
    struct Step {
        let dx: Int
        let dy: Int
        let weight: Double
    }

    /// 权重与官网 JS / host/e6_convert.py 完全一致
    static func kernel(_ mode: DitherMode) -> [Step] {
        switch mode {
        case .fsSer, .fs:
            return [Step(dx: 1, dy: 0, weight: 7.0 / 16.0),
                    Step(dx: -1, dy: 1, weight: 3.0 / 16.0),
                    Step(dx: 0, dy: 1, weight: 5.0 / 16.0),
                    Step(dx: 1, dy: 1, weight: 1.0 / 16.0)]
        case .atkinson:
            return [Step(dx: 1, dy: 0, weight: 1.0 / 8.0),
                    Step(dx: 2, dy: 0, weight: 1.0 / 8.0),
                    Step(dx: -1, dy: 1, weight: 1.0 / 8.0),
                    Step(dx: 0, dy: 1, weight: 1.0 / 8.0),
                    Step(dx: 1, dy: 1, weight: 1.0 / 8.0),
                    Step(dx: 0, dy: 2, weight: 1.0 / 8.0)]
        case .stucki:
            return [Step(dx: 1, dy: 0, weight: 8.0 / 42.0),
                    Step(dx: 2, dy: 0, weight: 4.0 / 42.0),
                    Step(dx: -2, dy: 1, weight: 2.0 / 42.0),
                    Step(dx: -1, dy: 1, weight: 4.0 / 42.0),
                    Step(dx: 0, dy: 1, weight: 8.0 / 42.0),
                    Step(dx: 1, dy: 1, weight: 4.0 / 42.0),
                    Step(dx: 2, dy: 1, weight: 2.0 / 42.0),
                    Step(dx: -2, dy: 2, weight: 1.0 / 42.0),
                    Step(dx: -1, dy: 2, weight: 2.0 / 42.0),
                    Step(dx: 0, dy: 2, weight: 4.0 / 42.0),
                    Step(dx: 1, dy: 2, weight: 2.0 / 42.0),
                    Step(dx: 2, dy: 2, weight: 1.0 / 42.0)]
        case .jjn:
            return [Step(dx: 1, dy: 0, weight: 7.0 / 48.0),
                    Step(dx: 2, dy: 0, weight: 5.0 / 48.0),
                    Step(dx: -2, dy: 1, weight: 3.0 / 48.0),
                    Step(dx: -1, dy: 1, weight: 5.0 / 48.0),
                    Step(dx: 0, dy: 1, weight: 7.0 / 48.0),
                    Step(dx: 1, dy: 1, weight: 5.0 / 48.0),
                    Step(dx: 2, dy: 1, weight: 3.0 / 48.0),
                    Step(dx: -2, dy: 2, weight: 1.0 / 48.0),
                    Step(dx: -1, dy: 2, weight: 3.0 / 48.0),
                    Step(dx: 0, dy: 2, weight: 5.0 / 48.0),
                    Step(dx: 1, dy: 2, weight: 3.0 / 48.0),
                    Step(dx: 2, dy: 2, weight: 1.0 / 48.0)]
        case .none:
            return []
        }
    }

    /// 输入：1200×1600×3 的 RGB（0…255，行优先，Float）。
    /// 输出：1200×1600 的调色板索引。
    ///
    /// 注意：这里刻意用 Double 做标量运算、Float 存缓冲，与 e6_convert.py
    /// 的 array('f') + Python float 完全同构，两边输出逐字节一致。
    static func quantize(rgb: [Float], mode: DitherMode, strength: Double) -> [UInt8] {
        let w = Spectrum6.width
        let h = Spectrum6.height
        precondition(rgb.count == w * h * 3, "RGB 缓冲大小不对")

        var buf = rgb
        var out = [UInt8](repeating: 0, count: w * h)

        let steps = kernel(mode)
        let serpentine = (mode == .fsSer)
        let thr = mode == .none ? 0.0 : strength
        let pal = Spectrum6.realPalette.map(Double.init)

        buf.withUnsafeMutableBufferPointer { b in
            out.withUnsafeMutableBufferPointer { o in
                var y = 0
                while y < h {
                    let base = y * w * 3
                    let reverse = serpentine && ((y & 1) == 1)

                    var k = 0
                    while k < w {
                        let x = reverse ? (w - 1 - k) : k
                        let i = base + x * 3

                        let r = Double(b[i])
                        let g = Double(b[i + 1])
                        let bl = Double(b[i + 2])

                        var best = 0
                        var bestDist = Double.greatestFiniteMagnitude
                        var c = 0
                        while c < Spectrum6.colorCount {
                            let dr = r - pal[c * 3]
                            let dg = g - pal[c * 3 + 1]
                            let db = bl - pal[c * 3 + 2]
                            let dist = dr * dr + dg * dg + db * db
                            if dist < bestDist {
                                bestDist = dist
                                best = c
                            }
                            c += 1
                        }

                        o[y * w + x] = UInt8(best)

                        let er = (r - pal[best * 3]) * thr
                        let eg = (g - pal[best * 3 + 1]) * thr
                        let eb = (bl - pal[best * 3 + 2]) * thr

                        /* 当前像素写成调色板色（Python 版同样处理） */
                        b[i] = Float(pal[best * 3])
                        b[i + 1] = Float(pal[best * 3 + 1])
                        b[i + 2] = Float(pal[best * 3 + 2])

                        for s in steps {
                            let sx = reverse ? (x - s.dx) : (x + s.dx)
                            let sy = y + s.dy
                            guard sx >= 0, sx < w, sy < h else { continue }

                            let j = (sy * w + sx) * 3
                            let v0 = Double(b[j]) + er * s.weight
                            b[j] = Float(v0 < 0 ? 0 : (v0 > 255 ? 255 : v0))
                            let v1 = Double(b[j + 1]) + eg * s.weight
                            b[j + 1] = Float(v1 < 0 ? 0 : (v1 > 255 ? 255 : v1))
                            let v2 = Double(b[j + 2]) + eb * s.weight
                            b[j + 2] = Float(v2 < 0 ? 0 : (v2 > 255 ? 255 : v2))
                        }

                        k += 1
                    }
                    y += 1
                }
            }
        }

        return out
    }
}

// MARK: - 打包

enum Packing {
    /// 索引 → 4bpp 字节流（高 nibble 在左）
    static func pack(_ indices: [UInt8]) -> [UInt8] {
        precondition(indices.count == Spectrum6.width * Spectrum6.height)
        var out = [UInt8](repeating: 0, count: indices.count / 2)
        let codes = Spectrum6.codes

        indices.withUnsafeBufferPointer { idx in
            out.withUnsafeMutableBufferPointer { o in
                var i = 0
                var j = 0
                while i < idx.count {
                    o[j] = (codes[Int(idx[i])] << 4) | codes[Int(idx[i + 1])]
                    i += 2
                    j += 1
                }
            }
        }
        return out
    }

    /// 拼 EPD6BIN：magic[8] + u16 w + u16 h + u16 frames + u32 interval + 帧数据（小端）
    static func makeBin(frame: [UInt8], intervalMs: UInt32 = 150) -> Data {
        var data = Data(capacity: 18 + frame.count)
        data.append(contentsOf: Spectrum6.magic)
        data.append(0)                                    // 第 8 字节补 0
        data.append(contentsOf: le16(UInt16(Spectrum6.width)))
        data.append(contentsOf: le16(UInt16(Spectrum6.height)))
        data.append(contentsOf: le16(1))
        data.append(contentsOf: [
            UInt8(intervalMs & 0xFF),
            UInt8((intervalMs >> 8) & 0xFF),
            UInt8((intervalMs >> 16) & 0xFF),
            UInt8((intervalMs >> 24) & 0xFF),
        ])
        data.append(contentsOf: frame)
        return data
    }

    static func le16(_ v: UInt16) -> [UInt8] {
        [UInt8(v & 0xFF), UInt8((v >> 8) & 0xFF)]
    }
}
