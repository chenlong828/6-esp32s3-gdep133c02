import Foundation
import Darwin

/// UDP 广播发现（协议与固件 discovery.c、host/discover.py 一致）。
///
/// 查询 12 B：magic "EPD6DISC" + ver(1) + type(0x01) + nonce(u16 BE)
/// 应答     ：magic + ver(1) + type(0x02) + nonce + ip(4) + mac(6) + http_port(u16) + hostlen + hostname
enum DiscoveryError: LocalizedError {
    case socket(Int32)
    case bind(Int32)

    var errorDescription: String? {
        switch self {
        case .socket(let code): return "无法创建 UDP socket：\(String(cString: strerror(code)))"
        case .bind(let code): return "无法绑定 UDP socket：\(String(cString: strerror(code)))"
        }
    }
}

struct DiscoveryService {
    static let defaultPort: UInt16 = 3232
    static let magic = Array("EPD6DISC".utf8)

    /// 后台扫描一次，回调在主线程
    func scan(port: UInt16 = DiscoveryService.defaultPort,
              timeout: TimeInterval = 2.2,
              completion: @escaping (Result<[DiscoveredDevice], Error>) -> Void) {
        DispatchQueue.global(qos: .userInitiated).async {
            let result = Self.scanSync(port: port, timeout: timeout)
            DispatchQueue.main.async {
                completion(result)
            }
        }
    }

    static func scanSync(port: UInt16, timeout: TimeInterval) -> Result<[DiscoveredDevice], Error> {
        do {
            return .success(try scanThrowing(port: port, timeout: timeout))
        } catch {
            return .failure(error)
        }
    }

    private static func scanThrowing(port: UInt16,
                                     timeout: TimeInterval) throws -> [DiscoveredDevice] {
        let fd = socket(AF_INET, SOCK_DGRAM, 0)
        guard fd >= 0 else { throw DiscoveryError.socket(errno) }
        defer { close(fd) }

        var yes: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, socklen_t(MemoryLayout<Int32>.size))
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, socklen_t(MemoryLayout<Int32>.size))

        var tv = timeval(tv_sec: 0, tv_usec: 200_000)
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))

        var local = sockaddr_in()
        local.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        local.sin_family = sa_family_t(AF_INET)
        local.sin_port = 0
        local.sin_addr.s_addr = 0

        let bindResult = withUnsafePointer(to: &local) { ptr in
            ptr.withMemoryRebound(to: sockaddr.self, capacity: 1) { sa in
                bind(fd, sa, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
        guard bindResult == 0 else { throw DiscoveryError.bind(errno) }

        let nonce = UInt16.random(in: 1...UInt16.max)
        let probe = makeProbe(nonce: nonce)
        let targets = broadcastTargets()

        var found: [String: DiscoveredDevice] = [:]
        var buffer = [UInt8](repeating: 0, count: 2048)

        let deadline = Date().addingTimeInterval(timeout)
        var nextSend = Date()
        var sent = 0

        while Date() < deadline {
            if sent < 3 && Date() >= nextSend {
                for target in targets {
                    send(probe, to: target, port: port, fd: fd)
                }
                sent += 1
                nextSend = Date().addingTimeInterval(0.4)
            }

            var from = sockaddr_in()
            var fromLen = socklen_t(MemoryLayout<sockaddr_in>.size)

            let received = buffer.withUnsafeMutableBytes { raw -> Int in
                guard let base = raw.baseAddress else { return -1 }
                return withUnsafeMutablePointer(to: &from) { fp in
                    fp.withMemoryRebound(to: sockaddr.self, capacity: 1) { sa in
                        recvfrom(fd, base, raw.count, 0, sa, &fromLen)
                    }
                }
            }

            if received > 0 {
                let data = Data(buffer[0..<received])
                if let device = parseReply(data, nonce: nonce) {
                    let key = device.mac.isEmpty ? device.ip : device.mac
                    found[key] = device
                }
            } else if received < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR {
                break
            }
        }

        return found.values.sorted { $0.ip < $1.ip }
    }

    // MARK: 报文

    private static func makeProbe(nonce: UInt16) -> Data {
        var data = Data(magic)
        data.append(1)
        data.append(0x01)
        data.append(UInt8(nonce >> 8))
        data.append(UInt8(nonce & 0xFF))
        return data
    }

    static func parseReply(_ data: Data, nonce: UInt16? = nil) -> DiscoveredDevice? {
        let bytes = [UInt8](data)
        guard bytes.count >= 25 else { return nil }
        guard Array(bytes[0..<8]) == magic else { return nil }
        guard bytes[8] == 1, bytes[9] == 0x02 else { return nil }

        if let expected = nonce {
            let echoed = UInt16(bytes[10]) << 8 | UInt16(bytes[11])
            guard echoed == expected else { return nil }
        }

        let ip = "\(bytes[12]).\(bytes[13]).\(bytes[14]).\(bytes[15])"
        guard ip != "0.0.0.0" else { return nil }   // 设备在线但还没联网

        let mac = (16..<22).map { String(format: "%02x", bytes[$0]) }.joined(separator: ":")
        let httpPort = UInt16(bytes[22]) << 8 | UInt16(bytes[23])
        let hostLen = Int(bytes[24])
        guard bytes.count >= 25 + hostLen else { return nil }
        let hostname = String(decoding: bytes[25..<(25 + hostLen)], as: UTF8.self)

        return DiscoveredDevice(ip: ip,
                                mac: mac,
                                hostname: hostname,
                                httpPort: httpPort == 0 ? 80 : httpPort)
    }

    private static func send(_ data: Data, to target: String, port: UInt16, fd: Int32) {
        var addr = sockaddr_in()
        addr.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        addr.sin_family = sa_family_t(AF_INET)
        addr.sin_port = port.bigEndian
        guard inet_pton(AF_INET, target, &addr.sin_addr) == 1 else { return }

        data.withUnsafeBytes { raw in
            guard let base = raw.baseAddress else { return }
            _ = withUnsafePointer(to: &addr) { ptr in
                ptr.withMemoryRebound(to: sockaddr.self, capacity: 1) { sa in
                    sendto(fd, base, raw.count, 0, sa, socklen_t(MemoryLayout<sockaddr_in>.size))
                }
            }
        }
    }

    /// 255.255.255.255 + 各网卡的子网定向广播地址
    static func broadcastTargets() -> [String] {
        var targets: Set<String> = ["255.255.255.255"]

        var ifaddrPtr: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&ifaddrPtr) == 0 else { return Array(targets).sorted() }
        defer { freeifaddrs(ifaddrPtr) }

        var ptr = ifaddrPtr
        while let current = ptr {
            defer { ptr = current.pointee.ifa_next }
            let ifa = current.pointee

            guard let addr = ifa.ifa_addr, addr.pointee.sa_family == UInt8(AF_INET) else {
                continue
            }
            let flags = Int32(ifa.ifa_flags)
            guard (flags & IFF_UP) != 0, (flags & IFF_BROADCAST) != 0,
                  let dst = ifa.ifa_dstaddr else { continue }

            var host = [CChar](repeating: 0, count: Int(NI_MAXHOST))
            let rc = getnameinfo(dst,
                                 socklen_t(dst.pointee.sa_len),
                                 &host,
                                 socklen_t(host.count),
                                 nil,
                                 0,
                                 NI_NUMERICHOST)
            if rc == 0 {
                let text = String(cString: host)
                if !text.isEmpty, text != "0.0.0.0" {
                    targets.insert(text)
                }
            }
        }

        return Array(targets).sorted()
    }
}
