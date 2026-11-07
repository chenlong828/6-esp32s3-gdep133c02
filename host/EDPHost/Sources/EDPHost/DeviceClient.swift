import Foundation

/// 与固件 HTTP API 对话。所有请求都是短连接，失败交给上层展示。
struct DeviceClient {

    private let session: URLSession
    private let decoder: JSONDecoder

    init() {
        let cfg = URLSessionConfiguration.ephemeral
        cfg.timeoutIntervalForRequest = 20
        cfg.timeoutIntervalForResource = 180
        cfg.waitsForConnectivity = false
        cfg.allowsExpensiveNetworkAccess = true
        cfg.allowsConstrainedNetworkAccess = true
        session = URLSession(configuration: cfg)

        let d = JSONDecoder()
        d.keyDecodingStrategy = .convertFromSnakeCase
        decoder = d
    }

    // MARK: URL

    private func url(_ device: DiscoveredDevice,
                     path: String,
                     query: [URLQueryItem] = []) -> URL? {
        var comps = URLComponents()
        comps.scheme = "http"
        comps.host = device.ip
        comps.port = Int(device.httpPort)
        comps.path = path
        if !query.isEmpty {
            comps.queryItems = query
        }
        return comps.url
    }

    private func check(_ response: URLResponse, _ data: Data) throws {
        guard let http = response as? HTTPURLResponse else {
            throw DeviceError.network("没有收到 HTTP 响应")
        }
        guard (200..<300).contains(http.statusCode) else {
            if let body = try? decoder.decode(APIErrorBody.self, from: data),
               let message = body.message, !message.isEmpty {
                throw DeviceError.server(message)
            }
            throw DeviceError.http(http.statusCode)
        }
    }

    // MARK: 状态

    func status(for device: DiscoveredDevice) async throws -> DeviceStatus {
        guard let url = url(device, path: "/api/v1/status") else { throw DeviceError.badURL }
        var request = URLRequest(url: url)
        request.timeoutInterval = 6
        let (data, response) = try await session.data(for: request)
        try check(response, data)
        return try decoder.decode(DeviceStatus.self, from: data)
    }

    // MARK: 上传

    func upload(bin: Data,
                imageName: String,
                timestamp: Date,
                to device: DiscoveredDevice) async throws -> UploadResult {
        guard let url = url(device, path: "/api/v1/image") else { throw DeviceError.badURL }

        var request = URLRequest(url: url)
        request.httpMethod = "POST"
        request.timeoutInterval = 120
        request.setValue("application/octet-stream", forHTTPHeaderField: "Content-Type")
        request.setValue(imageName, forHTTPHeaderField: "X-Image-Name")
        request.setValue(String(Int(timestamp.timeIntervalSince1970)),
                         forHTTPHeaderField: "X-Timestamp")
        request.setValue(CRC32.hex(CRC32.compute(bin)), forHTTPHeaderField: "X-Crc32")

        let (data, response) = try await session.upload(for: request, from: bin)
        try check(response, data)
        return try decoder.decode(UploadResult.self, from: data)
    }

    // MARK: 归档

    func history(for device: DiscoveredDevice) async throws -> HistoryResponse {
        guard let url = url(device, path: "/api/v1/history") else { throw DeviceError.badURL }
        var request = URLRequest(url: url)
        request.timeoutInterval = 8
        let (data, response) = try await session.data(for: request)
        try check(response, data)
        return try decoder.decode(HistoryResponse.self, from: data)
    }

    func show(file: String, on device: DiscoveredDevice) async throws {
        guard let url = url(device,
                            path: "/api/v1/display",
                            query: [URLQueryItem(name: "file", value: file)])
        else { throw DeviceError.badURL }

        var request = URLRequest(url: url)
        request.httpMethod = "POST"
        request.timeoutInterval = 10
        let (data, response) = try await session.data(for: request)
        try check(response, data)
    }

    func clear(color: String, on device: DiscoveredDevice) async throws {
        guard let url = url(device,
                            path: "/api/v1/clear",
                            query: [URLQueryItem(name: "color", value: color)])
        else { throw DeviceError.badURL }

        var request = URLRequest(url: url)
        request.httpMethod = "POST"
        request.timeoutInterval = 10
        let (data, response) = try await session.data(for: request)
        try check(response, data)
    }
}
