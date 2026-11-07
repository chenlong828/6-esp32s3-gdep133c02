import Foundation

// MARK: - 局域网里发现到的设备

struct DiscoveredDevice: Identifiable, Hashable {
    let ip: String
    let mac: String
    let hostname: String
    let httpPort: UInt16

    var id: String { mac.isEmpty ? "\(ip):\(httpPort)" : mac }

    var displayName: String {
        hostname.isEmpty ? ip : "\(hostname) · \(ip)"
    }

    var baseDescription: String {
        "\(ip):\(httpPort)"
    }
}

// MARK: - /api/v1/status

struct DeviceStatus: Decodable {
    struct Panel: Decodable {
        let width: Int
        let height: Int
        let colors: Int
        let frameBytes: Int
        let ready: Bool
    }

    struct Storage: Decodable {
        let mounted: Bool
        let totalBytes: UInt64
        let freeBytes: UInt64
    }

    struct Display: Decodable {
        let busy: Bool
        let refreshCount: Int
        let lastRefreshMs: Int
        let lastRefreshUptimeS: Int
        let lastError: String
        let currentName: String
        let currentCrc: UInt32
        let currentSavedAt: UInt32
        let currentBytes: Int
    }

    struct Upload: Decodable {
        let active: Bool
        let received: Int
        let total: Int
        let okCount: Int
        let failCount: Int
        let lastBytes: Int
        let lastMs: Int
    }

    let device: String
    let firmware: String
    let hostname: String
    let ip: String
    let mac: String
    let uptimeS: Int
    let httpPort: Int
    let discoveryPort: Int
    let panel: Panel
    let sd: Storage
    let display: Display
    let upload: Upload
}

// MARK: - 上传 / 归档

struct UploadResult: Decodable {
    let ok: Bool
    let file: String
    let bytes: UInt32
    let crc32: UInt32
    let savedAt: UInt32
    let receivedMs: Int
    let queued: Bool
}

struct HistoryFile: Decodable, Identifiable {
    let name: String
    let bytes: UInt32
    var id: String { name }

    var sizeText: String {
        ByteCountFormatter.string(fromByteCount: Int64(bytes), countStyle: .file)
    }
}

struct HistoryResponse: Decodable {
    let ok: Bool
    let files: [HistoryFile]
    let count: Int
    let current: String
}

struct APIErrorBody: Decodable {
    let ok: Bool?
    let error: String?
    let message: String?
}

enum DeviceError: LocalizedError {
    case badURL
    case http(Int)
    case server(String)
    case network(String)

    var errorDescription: String? {
        switch self {
        case .badURL: return "设备地址不合法"
        case .http(let code): return "设备返回 HTTP \(code)"
        case .server(let message): return message
        case .network(let message): return message
        }
    }
}
