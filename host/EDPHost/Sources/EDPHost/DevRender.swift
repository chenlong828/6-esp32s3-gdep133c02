import AppKit
import SwiftUI

/// 开发辅助：离屏渲染裁切编辑器、以及无界面地跑一遍“一键生成并上传”。
/// 只用于调试，不参与正常流程。
enum DevRender {

    enum SelftestError: LocalizedError {
        case timedOut(String)
        case failed(String)

        var errorDescription: String? {
            switch self {
            case .timedOut(let stage): return "自检超时：\(stage)"
            case .failed(let reason): return "自检失败：\(reason)"
            }
        }
    }

    /// 直接驱动 AppModel.oneClickUpload()，跑的就是界面按钮背后那条路径。
    @MainActor
    static func oneClickSelftest(imagePath: String, ip: String) throws {
        let model = AppModel()
        let device = DiscoveredDevice(ip: ip, mac: "selftest", hostname: "selftest", httpPort: 80)
        model.devices = [device]
        model.selectedDeviceID = device.id
        model.pickImage(url: URL(fileURLWithPath: imagePath))

        let loadDeadline = Date().addingTimeInterval(15)
        while model.editImageSize.width == 0 && Date() < loadDeadline {
            RunLoop.main.run(until: Date().addingTimeInterval(0.05))
        }
        guard model.editImageSize.width > 0 else {
            throw SelftestError.timedOut("缩略图载入")
        }
        print("缩略图已载入：\(Int(model.editImageSize.width))×\(Int(model.editImageSize.height))，\(model.cropSummary)")

        model.oneClickUpload()

        let deadline = Date().addingTimeInterval(90)
        while model.pipelineActive && Date() < deadline {
            RunLoop.main.run(until: Date().addingTimeInterval(0.1))
        }
        if model.pipelineActive {
            throw SelftestError.timedOut("转换/上传")
        }
        if let error = model.errorAlert {
            throw SelftestError.failed(error)
        }
        guard let result = model.lastUpload else {
            throw SelftestError.failed("没有拿到上传结果")
        }

        print("一键完成：\(result.file)，\(result.bytes) B，设备端 \(result.receivedMs) ms，"
              + "CRC \(CRC32.hex(result.crc32))，queued=\(result.queued)")
        print("转换信息：\(model.conversionInfo)")
        print("二进制：\(model.compiledBin?.count ?? 0) B，CRC \(CRC32.hex(model.compiledCRC))")
    }

    @MainActor
    static func renderCrop(imagePath: String, output: String,
                           width: CGFloat = 520, height: CGFloat = 640,
                           rotate: Int = 0) throws {
        let url = URL(fileURLWithPath: imagePath)
        let thumb = try ImageConverter.loadThumbnail(url: url, maxPixel: 2048)
        let size = CGSize(width: thumb.width, height: thumb.height)
        let aspect = CropMath.frameAspect(rotate: rotate)
        let crop = Binding.constant(CropMath.centeredMax(imageSize: size, aspect: aspect))

        let view = CropEditorView(image: thumb,
                                  imageSize: size,
                                  aspectRatio: aspect,
                                  crop: crop,
                                  enabled: true,
                                  onCommit: {})
            .frame(width: width, height: height)
            .background(Color(nsColor: .windowBackgroundColor))

        let renderer = ImageRenderer(content: view)
        renderer.scale = 2

        guard let cgImage = renderer.cgImage else {
            throw ImageConverterError.cannotRender
        }
        let rep = NSBitmapImageRep(cgImage: cgImage)
        guard let data = rep.representation(using: .png, properties: [:]) else {
            throw ImageConverterError.cannotRender
        }
        try data.write(to: URL(fileURLWithPath: output))
        print("wrote \(output) \(cgImage.width)x\(cgImage.height)")
    }
}
