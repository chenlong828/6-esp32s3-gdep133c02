import AppKit
import Combine
import Foundation
import SwiftUI
import UniformTypeIdentifiers

struct LogEntry: Identifiable {
    enum Level {
        case info, success, warning, error
    }

    let id = UUID()
    let date = Date()
    let text: String
    let level: Level
}

@MainActor
final class AppModel: ObservableObject {

    // MARK: 设备

    @Published var devices: [DiscoveredDevice] = []
    @Published var selectedDeviceID: DiscoveredDevice.ID?
    @Published var scanning = false
    @Published var scanMessage = "准备扫描 ..."
    @Published var status: DeviceStatus?
    @Published var statusError: String?
    @Published var manualIP = ""

    // MARK: 图片 / 转换

    @Published var imageURL: URL?
    @Published var imageName = ""
    @Published var previewMode: PreviewMode = .crop
    @Published var editImage: CGImage?
    @Published var editImageSize = CGSize.zero
    /// 归一化裁切框（相对未旋转的源图；开启「允许超出原图」时可以超出 0…1，超出的部分填白）
    @Published var cropRect = CGRect(x: 0, y: 0, width: 1, height: 1)

    @Published var settings = ConvertSettings() {
        didSet {
            guard oldValue != settings else { return }
            persistSettings()
            if oldValue.rotate != settings.rotate
                || oldValue.allowOverscan != settings.allowOverscan {
                /* 换比例 / 换裁切范围后，把裁切框重置成当前模式的默认 */
                resetCropForRotation()
            }
            scheduleReconvert()
        }
    }
    @Published var converting = false
    @Published var conversionProgress: Double = 0
    @Published var preview: CGImage?
    @Published var shares: [Double] = []
    @Published var conversionInfo = ""
    @Published var packedFrame: [UInt8] = []
    @Published var compiledBin: Data?
    @Published var compiledCRC: UInt32 = 0

    // MARK: 上传

    @Published var uploading = false
    @Published var pipelineActive = false
    @Published var pipelineStage = ""
    @Published var pipelineProgress: Double = 0
    @Published var lastUpload: UploadResult?
    @Published var history: [HistoryFile] = []
    @Published var historyLoading = false
    @Published var showHistory = false

    // MARK: 日志 / 错误

    @Published var logs: [LogEntry] = []
    @Published var errorAlert: String?

    private let discovery = DiscoveryService()
    private let client = DeviceClient()

    private var sourceThumb: CGImage?
    private var editorGeneration = 0
    private var convertGeneration = 0
    private var reconvertWork: DispatchWorkItem?
    private var scanTimer: Timer?
    private var statusTimer: Timer?
    private var didStart = false

    var selectedDevice: DiscoveredDevice? {
        guard let id = selectedDeviceID else { return nil }
        return devices.first { $0.id == id }
    }

    var canUpload: Bool {
        selectedDevice != nil && compiledBin != nil && !uploading && !pipelineActive
    }

    /// 一键生成并上传：需要设备 + 已载入图片 + 没有别的流程在跑
    var canOneClick: Bool {
        selectedDevice != nil
            && imageURL != nil
            && editImageSize.width > 0
            && !pipelineActive
    }

    // MARK: 生命周期

    func onAppear() {
        guard !didStart else { return }
        didStart = true
        loadSettings()
        log("启动：开始局域网搜索", level: .info)

        scanTimer = Timer.scheduledTimer(withTimeInterval: 10, repeats: true) { [weak self] _ in
            guard let self else { return }
            Task { @MainActor in
                if !self.scanning { self.scan(auto: true) }
            }
        }
        statusTimer = Timer.scheduledTimer(withTimeInterval: 3, repeats: true) { [weak self] _ in
            guard let self else { return }
            Task { @MainActor in
                if self.selectedDevice != nil { self.refreshStatus() }
            }
        }
        scan()
    }

    // MARK: 发现

    func scan(auto: Bool = false) {
        guard !scanning else { return }
        scanning = true
        if !auto { scanMessage = "正在广播查询 UDP 3232 ..." }

        discovery.scan { [weak self] result in
            guard let self else { return }
            self.scanning = false

            switch result {
            case .success(let found):
                let previous = self.devices
                self.devices = found

                if found.isEmpty {
                    self.scanMessage = auto ? "未发现设备，继续搜索 ..." : "未发现设备"
                    if !auto {
                        self.log("扫描：未发现设备", level: .warning)
                    }
                    return
                }

                self.scanMessage = "发现 \(found.count) 台设备"
                if self.selectedDeviceID == nil
                    || !found.contains(where: { $0.id == self.selectedDeviceID }) {
                    self.selectedDeviceID = found.first?.id
                    self.refreshStatus()
                }
                if previous.isEmpty {
                    self.log("发现 \(found.count) 台设备：\(found.map(\.displayName).joined(separator: ", "))",
                             level: .success)
                }

            case .failure(let error):
                self.scanMessage = "扫描失败：\(error.localizedDescription)"
                if !auto {
                    self.log("扫描失败：\(error.localizedDescription)", level: .error)
                }
            }
        }
    }

    func select(device: DiscoveredDevice) {
        selectedDeviceID = device.id
        refreshStatus()
    }

    func addManualDevice() {
        let text = manualIP.trimmingCharacters(in: .whitespacesAndNewlines)
        guard isValidIPv4(text) else {
            errorAlert = "IP 格式不对，例如 192.168.1.50"
            return
        }

        if let existing = devices.first(where: { $0.ip == text }) {
            select(device: existing)
        } else {
            let device = DiscoveredDevice(ip: text,
                                          mac: "manual-\(text)",
                                          hostname: "手动添加",
                                          httpPort: 80)
            devices.append(device)
            select(device: device)
        }
        log("手动添加设备 \(text)", level: .info)
    }

    private func isValidIPv4(_ text: String) -> Bool {
        let parts = text.split(separator: ".", omittingEmptySubsequences: false)
        guard parts.count == 4 else { return false }
        return parts.allSatisfy {
            guard let value = Int($0), !$0.isEmpty, $0.count <= 3 else { return false }
            return value >= 0 && value <= 255
        }
    }

    // MARK: 状态

    func refreshStatus() {
        guard let device = selectedDevice else { return }
        Task {
            do {
                let value = try await client.status(for: device)
                self.status = value
                self.statusError = nil
            } catch {
                self.status = nil
                self.statusError = error.localizedDescription
            }
        }
    }

    // MARK: 选图

    func pickImage(url: URL) {
        imageURL = url
        imageName = url.lastPathComponent
        compiledBin = nil
        lastUpload = nil
        preview = nil
        shares = []
        previewMode = .crop
        sourceThumb = nil
        editImage = nil
        editImageSize = .zero

        editorGeneration += 1
        let generation = editorGeneration

        /* 换图时把正在跑的一键流程作废，避免旧图把结果盖回来 */
        convertGeneration += 1
        pipelineActive = false
        pipelineStage = ""

        log("选择图片：\(imageName)", level: .info)

        /* 缩略图只用来显示/裁切；真正转换仍读原图，画质不受影响 */
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            let thumb = try? ImageConverter.loadThumbnail(url: url)
            DispatchQueue.main.async {
                guard let self,
                      generation == self.editorGeneration,
                      self.imageURL == url else { return }

                if let thumb {
                    self.sourceThumb = thumb
                    self.prepareEditor()
                } else {
                    /* 缩略图都出不来，基本是文件损坏；给提示并退回成品视图 */
                    self.errorAlert = "无法读取图片预览：\(url.lastPathComponent)"
                    self.previewMode = .result
                }
                self.reconvert()
            }
        }
    }

    /// 裁切框在源图坐标系里的像素宽高比：0/180 是 3:4，90/270 是 4:3
    var frameAspect: CGFloat {
        CropMath.frameAspect(rotate: settings.rotate)
    }

    /// 编辑器显示原始方向的缩略图（旋转在转换时才应用）
    private func prepareEditor() {
        guard let thumb = sourceThumb else { return }
        editImage = thumb
        editImageSize = CGSize(width: thumb.width, height: thumb.height)
        resetCropForRotation()
    }

    private func resetCropForRotation() {
        guard editImageSize.width > 0 else { return }
        cropRect = CropMath.defaultCrop(imageSize: editImageSize,
                                        aspect: frameAspect,
                                        allowsOverscan: settings.allowOverscan)
    }

    func clearImage() {
        imageURL = nil
        imageName = ""
        preview = nil
        packedFrame = []
        compiledBin = nil
        shares = []
        conversionInfo = ""
        lastUpload = nil
        previewMode = .crop
        sourceThumb = nil
        editImage = nil
        editImageSize = .zero
        cropRect = CGRect(x: 0, y: 0, width: 1, height: 1)
        editorGeneration += 1
        convertGeneration += 1
        pipelineActive = false
        pipelineStage = ""
    }

    private func scheduleReconvert() {
        guard imageURL != nil else { return }
        reconvertWork?.cancel()
        let work = DispatchWorkItem { [weak self] in
            Task { @MainActor in self?.reconvert() }
        }
        reconvertWork = work
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.2, execute: work)
    }

    // MARK: 转换

    func reconvert() {
        guard let url = imageURL else { return }

        convertGeneration += 1
        let generation = convertGeneration
        let snapshot = settings
        /* 缩略图还没加载完时退回自动裁切，避免用到全图（比例不对）的框 */
        let crop: CGRect? = (snapshot.fit == .cover && editImageSize.width > 0)
            ? CropMath.rotated(cropRect, rotate: snapshot.rotate) : nil

        converting = true
        conversionProgress = 0
        compiledBin = nil

        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            do {
                let source = try ImageConverter.loadImage(url: url)
                let result = try ImageConverter.convert(cgImage: source,
                                                        settings: snapshot,
                                                        crop: crop) { value in
                    DispatchQueue.main.async {
                        guard let self, generation == self.convertGeneration else { return }
                        self.conversionProgress = value
                    }
                }
                DispatchQueue.main.async {
                    guard let self, generation == self.convertGeneration else { return }
                    self.apply(result)
                }
            } catch {
                DispatchQueue.main.async {
                    guard let self, generation == self.convertGeneration else { return }
                    self.converting = false
                    self.conversionProgress = 0
                    self.errorAlert = error.localizedDescription
                    self.log("转换失败：\(error.localizedDescription)", level: .error)
                }
            }
        }
    }

    private func apply(_ result: ConversionResult) {
        preview = result.preview
        shares = result.colorShares
        packedFrame = result.packed
        compiledBin = nil
        converting = false
        conversionProgress = 1
        applyConversionInfo(result)
        log("转换完成：\(result.packed.count) 字节，用时 \(String(format: "%.2f", result.elapsed)) s",
            level: .success)
    }

    private func applyConversionInfo(_ result: ConversionResult) {
        let pct = result.colorShares.enumerated()
            .map { "\(Spectrum6.colorNames[$0.offset]) \(Int(($0.element * 100).rounded()))%" }
            .joined(separator: "  ")
        conversionInfo = String(format: "转换用时 %.2f s ｜ %@", result.elapsed, pct)
    }

    // MARK: 裁切

    /// 拖完裁切框：重新算一次成品预览
    func commitCrop() {
        scheduleReconvert()
    }

    func resetCrop() {
        resetCropForRotation()
        scheduleReconvert()
    }

    /// 控制面板里的「转换并预览」：切到成品视图
    func convertNow() {
        previewMode = .result
        reconvert()
    }

    var cropSummary: String {
        guard editImageSize.width > 0 else { return "" }
        guard settings.fit == .cover else { return "整图（不裁切）" }

        let px = CropMath.pixelSize(cropRect, imageSize: editImageSize)
        let ratio = abs(frameAspect - CropMath.panelAspect) < 0.001 ? "3:4" : "4:3"
        let overflow = cropRect.minX < -0.0001 || cropRect.minY < -0.0001
            || cropRect.maxX > 1.0001 || cropRect.maxY > 1.0001
        let padding = overflow ? " · 白边" : ""
        let degrees = ((settings.rotate % 360) + 360) % 360
        let suffix = degrees == 0 ? "" : " · 上屏顺时针转 \(degrees)°"
        return "裁切框 \(ratio) · \(Int(px.width.rounded()))×\(Int(px.height.rounded()))\(padding)\(suffix)"
    }

    // MARK: 编译

    func compileBinary() {
        guard !packedFrame.isEmpty else {
            errorAlert = "请先转换图片"
            return
        }
        let bin = Packing.makeBin(frame: packedFrame)
        compiledBin = bin
        compiledCRC = CRC32.compute(bin)
        log("已编译 EPD6BIN：\(bin.count) 字节，CRC \(CRC32.hex(compiledCRC))", level: .success)
    }

    func exportBinary() {
        guard let bin = compiledBin else {
            errorAlert = "请先编译二进制"
            return
        }
        let panel = NSSavePanel()
        panel.nameFieldStringValue = archiveName()
        panel.allowedContentTypes = [UTType(filenameExtension: "bin") ?? .data]
        panel.canCreateDirectories = true
        guard panel.runModal() == .OK, let url = panel.url else { return }
        do {
            try bin.write(to: url)
            log("已导出 \(url.lastPathComponent)", level: .success)
        } catch {
            errorAlert = error.localizedDescription
        }
    }

    // MARK: 上传

    func upload() {
        guard let device = selectedDevice else {
            errorAlert = "还没有选中设备"
            return
        }
        guard let bin = compiledBin else {
            errorAlert = "请先点「编译二进制」"
            return
        }
        performUpload(bin: bin, device: device)
    }

    /// 一键：用当前（默认）参数转换 → 编译 → 上传
    func oneClickUpload() {
        guard !pipelineActive else { return }
        guard let device = selectedDevice else {
            errorAlert = "还没有选中设备"
            return
        }
        guard let url = imageURL else {
            errorAlert = "请先选一张照片"
            return
        }
        guard editImageSize.width > 0 else {
            errorAlert = "图片还在载入，稍等一下"
            return
        }

        pipelineActive = true
        pipelineStage = "转换中"
        pipelineProgress = 0
        lastUpload = nil

        convertGeneration += 1
        let generation = convertGeneration
        let snapshot = settings
        let crop: CGRect? = (snapshot.fit == .cover)
            ? CropMath.rotated(cropRect, rotate: snapshot.rotate) : nil

        log("一键生成：\(imageName) → \(device.baseDescription)", level: .info)

        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            do {
                let source = try ImageConverter.loadImage(url: url)
                let result = try ImageConverter.convert(cgImage: source,
                                                        settings: snapshot,
                                                        crop: crop) { value in
                    DispatchQueue.main.async {
                        guard let self,
                              generation == self.convertGeneration,
                              self.pipelineActive else { return }
                        self.pipelineProgress = value * 0.5
                    }
                }

                let bin = Packing.makeBin(frame: result.packed)
                let crc = CRC32.compute(bin)

                DispatchQueue.main.async {
                    guard let self,
                          generation == self.convertGeneration,
                          self.pipelineActive else { return }

                    /* 先把成品亮出来，再上传 */
                    self.preview = result.preview
                    self.shares = result.colorShares
                    self.packedFrame = result.packed
                    self.compiledBin = bin
                    self.compiledCRC = crc
                    self.converting = false
                    self.previewMode = .result
                    self.applyConversionInfo(result)

                    self.pipelineStage = "上传中"
                    self.pipelineProgress = 0.6
                    self.performUpload(bin: bin, device: device)
                }
            } catch {
                DispatchQueue.main.async {
                    guard let self, generation == self.convertGeneration else { return }
                    self.pipelineActive = false
                    self.pipelineStage = ""
                    self.pipelineProgress = 0
                    self.errorAlert = error.localizedDescription
                    self.log("一键生成失败：\(error.localizedDescription)", level: .error)
                }
            }
        }
    }

    private func performUpload(bin: Data, device: DiscoveredDevice) {
        uploading = true
        let name = archiveName()
        log("开始上传 \(name) → \(device.baseDescription)，\(bin.count) 字节", level: .info)

        Task {
            do {
                let result = try await client.upload(bin: bin,
                                                     imageName: name,
                                                     timestamp: Date(),
                                                     to: device)
                self.lastUpload = result
                self.uploading = false
                self.pipelineActive = false
                self.pipelineStage = ""
                self.pipelineProgress = 1
                self.log("上传成功：\(result.file)，设备端 \(result.receivedMs) ms，CRC \(CRC32.hex(result.crc32))",
                         level: .success)
                self.refreshStatus()
            } catch {
                self.uploading = false
                self.pipelineActive = false
                self.pipelineStage = ""
                self.pipelineProgress = 0
                self.errorAlert = error.localizedDescription
                self.log("上传失败：\(error.localizedDescription)", level: .error)
            }
        }
    }

    // MARK: 归档

    func loadHistory() {
        guard let device = selectedDevice else { return }
        historyLoading = true
        Task {
            do {
                let response = try await client.history(for: device)
                self.history = response.files
                self.historyLoading = false
            } catch {
                self.historyLoading = false
                self.errorAlert = error.localizedDescription
            }
        }
    }

    func showAgain(_ file: HistoryFile) {
        guard let device = selectedDevice else { return }
        Task {
            do {
                try await client.show(file: file.name, on: device)
                self.log("请求重新显示 \(file.name)", level: .info)
                self.refreshStatus()
            } catch {
                self.errorAlert = error.localizedDescription
            }
        }
    }

    func clearScreen(color: String) {
        guard let device = selectedDevice else { return }
        Task {
            do {
                try await client.clear(color: color, on: device)
                self.log("请求整屏刷 \(color)", level: .info)
            } catch {
                self.errorAlert = error.localizedDescription
            }
        }
    }

    // MARK: 工具

    func archiveName() -> String {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.dateFormat = "yyyyMMdd-HHmmss"
        let stamp = formatter.string(from: Date())

        let stem = (imageName as NSString).deletingPathExtension
        let allowed = Set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-")
        var slug = String(stem.map { allowed.contains($0) ? $0 : "-" }.prefix(24))
        while slug.hasPrefix("-") { slug.removeFirst() }
        while slug.hasSuffix("-") { slug.removeLast() }

        return slug.isEmpty ? "\(stamp).bin" : "\(stamp)-\(slug).bin"
    }

    func log(_ text: String, level: LogEntry.Level = .info) {
        logs.insert(LogEntry(text: text, level: level), at: 0)
        if logs.count > 200 {
            logs.removeLast(logs.count - 200)
        }
    }

    // MARK: 设置持久化

    private func persistSettings() {
        let defaults = UserDefaults.standard
        defaults.set(settings.rotate, forKey: "convert.rotate")
        defaults.set(settings.fit.rawValue, forKey: "convert.fit")
        defaults.set(settings.dither.rawValue, forKey: "convert.dither")
        defaults.set(settings.strength, forKey: "convert.strength")
        defaults.set(settings.allowOverscan, forKey: "convert.overscan")
    }

    private func loadSettings() {
        let defaults = UserDefaults.standard
        var loaded = ConvertSettings()
        if defaults.object(forKey: "convert.rotate") != nil {
            loaded.rotate = defaults.integer(forKey: "convert.rotate")
        }
        if let raw = defaults.string(forKey: "convert.fit"),
           let fit = FitMode(rawValue: raw) {
            loaded.fit = fit
        }
        if let raw = defaults.string(forKey: "convert.dither"),
           let dither = DitherMode(rawValue: raw) {
            loaded.dither = dither
        }
        if defaults.object(forKey: "convert.strength") != nil {
            loaded.strength = defaults.double(forKey: "convert.strength")
        }
        if defaults.object(forKey: "convert.overscan") != nil {
            loaded.allowOverscan = defaults.bool(forKey: "convert.overscan")
        }
        /* 直接写字段，不触发 didSet 里的自动重转 */
        settings = loaded
    }
}
