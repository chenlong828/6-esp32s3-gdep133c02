import SwiftUI
import UniformTypeIdentifiers

/// 左侧：裁切编辑器 / 成品预览 + 六色占比。
struct PreviewPane: View {
    @EnvironmentObject private var model: AppModel
    @Binding var isDropTarget: Bool
    @Binding var isImporting: Bool

    var body: some View {
        VStack(spacing: 0) {
            header
            Divider()

            ZStack {
                content

                if isDropTarget {
                    RoundedRectangle(cornerRadius: 12)
                        .strokeBorder(Color.accentColor,
                                      style: StrokeStyle(lineWidth: 3, dash: [8, 6]))
                        .padding(16)
                    Text("松开导入")
                        .font(.title3.bold())
                        .foregroundStyle(Color.accentColor)
                }

                if model.converting && model.previewMode == .result {
                    VStack(spacing: 10) {
                        ProgressView(value: model.conversionProgress)
                            .frame(width: 220)
                        Text("正在抖动 / 打包 ...")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                    .padding(16)
                    .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 12))
                }
            }
            .onDrop(of: [UTType.fileURL.identifier], isTargeted: $isDropTarget) { providers in
                guard let provider = providers.first else { return false }
                _ = provider.loadObject(ofClass: URL.self) { url, _ in
                    guard let url else { return }
                    DispatchQueue.main.async {
                        model.pickImage(url: url)
                    }
                }
                return true
            }

            if !model.shares.isEmpty {
                Divider()
                colorShareList
                    .padding(.horizontal, 16)
                    .padding(.vertical, 10)
            }
        }
    }

    // MARK: 主体

    @ViewBuilder
    private var content: some View {
        if model.previewMode == .crop {
            if let edit = model.editImage {
                CropEditorView(image: edit,
                               imageSize: model.editImageSize,
                               aspectRatio: model.frameAspect,
                               crop: $model.cropRect,
                               enabled: model.settings.fit == .cover,
                               onCommit: { model.commitCrop() })
            } else if model.imageURL != nil {
                VStack(spacing: 10) {
                    ProgressView()
                    Text("正在读取图片 ...")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            } else {
                placeholder
            }
        } else if let image = model.preview {
            ZStack {
                Color(nsColor: .underPageBackgroundColor)
                Image(decorative: image, scale: 1)
                    .resizable()
                    .interpolation(.high)
                    .aspectRatio(contentMode: .fit)
                    .padding(20)
                    .shadow(radius: 12, y: 4)
            }
        } else {
            placeholder
        }
    }

    // MARK: 顶部

    private var header: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 10) {
                VStack(alignment: .leading, spacing: 2) {
                    Text(model.imageName.isEmpty ? "尚未选择照片" : model.imageName)
                        .font(.headline)
                        .lineLimit(1)
                    Text("面板 1200 × 1600 · 6 色 · 4bpp · 单帧 960 000 B")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }

                Spacer()

                if model.imageURL != nil {
                    Button {
                        model.clearImage()
                    } label: {
                        Image(systemName: "xmark.circle")
                    }
                    .buttonStyle(.plain)
                    .help("清除当前图片")
                }

                Button("选择照片…") { isImporting = true }
                    .keyboardShortcut("o", modifiers: .command)
            }

            HStack(spacing: 10) {
                Picker("视图", selection: $model.previewMode) {
                    ForEach(PreviewMode.allCases) { mode in
                        Text(mode.label).tag(mode)
                    }
                }
                .pickerStyle(.segmented)
                .labelsHidden()
                .frame(width: 128)
                .disabled(model.editImage == nil)

                Text(statusText)
                    .font(.caption2)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)

                Spacer()

                if model.previewMode == .crop && model.imageURL != nil {
                    Button("重置裁切") { model.resetCrop() }
                        .font(.caption)
                        .disabled(model.settings.fit != .cover)
                }

                if model.converting {
                    ProgressView()
                        .controlSize(.small)
                }
            }
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 12)
    }

    private var statusText: String {
        if model.previewMode == .result {
            return model.preview == nil ? "尚未生成成品" : "成品预览（就是真会刷到屏幕上的画面）"
        }
        if model.settings.fit != .cover {
            return "整图模式：不裁切"
        }
        var text = model.cropSummary
        if model.editImageSize.width > model.editImageSize.height
            && model.settings.rotate % 180 == 0 {
            text += " · 横图可旋转 90° 换成 4:3 框"
        }
        return text
    }

    // MARK: 占位

    private var placeholder: some View {
        ZStack {
            Color(nsColor: .underPageBackgroundColor)
            VStack(spacing: 14) {
                Image(systemName: "photo.on.rectangle.angled")
                    .font(.system(size: 46, weight: .light))
                    .foregroundStyle(.secondary)
                Text("把照片拖到这里，或点「选择照片…」")
                    .foregroundStyle(.secondary)
                Text("载入后可以在裁切视图里拖动、缩放裁切框")
                    .font(.caption)
                    .foregroundStyle(.tertiary)
            }
        }
    }

    // MARK: 六色占比

    private var colorShareList: some View {
        VStack(spacing: 6) {
            ForEach(0..<Spectrum6.colorCount, id: \.self) { index in
                HStack(spacing: 8) {
                    RoundedRectangle(cornerRadius: 3)
                        .fill(Self.color(at: index))
                        .frame(width: 12, height: 12)
                        .overlay(RoundedRectangle(cornerRadius: 3)
                            .strokeBorder(Color.primary.opacity(0.15), lineWidth: 0.5))
                    Text(Spectrum6.colorNames[index])
                        .font(.caption)
                        .frame(width: 22, alignment: .leading)
                    ProgressView(value: share(at: index))
                        .tint(Self.color(at: index))
                    Text(percentText(at: index))
                        .font(.caption.monospacedDigit())
                        .foregroundStyle(.secondary)
                        .frame(width: 46, alignment: .trailing)
                }
            }
        }
    }

    private func share(at index: Int) -> Double {
        model.shares.indices.contains(index) ? model.shares[index] : 0
    }

    private func percentText(at index: Int) -> String {
        String(format: "%.1f%%", share(at: index) * 100)
    }

    static func color(at index: Int) -> Color {
        let base = index * 3
        let palette = Spectrum6.realPalette
        guard palette.indices.contains(base + 2) else { return .gray }
        return Color(red: Double(palette[base]) / 255,
                     green: Double(palette[base + 1]) / 255,
                     blue: Double(palette[base + 2]) / 255)
    }
}
