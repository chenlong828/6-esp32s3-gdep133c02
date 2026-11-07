import Foundation

/// CRC32（IEEE / zlib），与固件 crc32.c、Python zlib.crc32 完全一致。
enum CRC32 {
    private static let table: [UInt32] = {
        var lookup = [UInt32](repeating: 0, count: 256)
        for i in 0..<256 {
            var c = UInt32(i)
            for _ in 0..<8 {
                c = (c & 1) != 0 ? (0xEDB8_8320 ^ (c >> 1)) : (c >> 1)
            }
            lookup[i] = c
        }
        return lookup
    }()

    static func compute(_ data: Data) -> UInt32 {
        var crc: UInt32 = 0xFFFF_FFFF
        data.withUnsafeBytes { raw in
            let bytes = raw.bindMemory(to: UInt8.self)
            for b in bytes {
                crc = (crc >> 8) ^ table[Int((crc ^ UInt32(b)) & 0xFF)]
            }
        }
        return ~crc
    }

    static func hex(_ value: UInt32) -> String {
        String(format: "%08x", value)
    }
}
