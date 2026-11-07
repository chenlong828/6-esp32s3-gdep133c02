import SwiftUI

/// 右侧：设备、照片、画面参数、一键生成并上传、日志。
struct ControlPane: View {
    @EnvironmentObject private var model: AppModel
    @Binding var isImporting: Bool

    var body: some View {
        ScrollView {
            VStack(spacing: 14) {
                deviceCard
                imageCard
                lookCard
                generateCard
                logCard
            }
            .padding(16)
        }
        .background(Color(nsColor: .windowBackgroundColor))
    }

    // MARK: ① 设备

    private var deviceCard: some View {
        SectionCard("① 设备", subtitle: model.scanMessage) {
            if let status = model.status {
                VStack(alignment: .leading, spacing: 6) {
                    detailRow("地址", "\(status.ip):\(status.httpPort)")
                    detailRow("MAC", status.mac)
                    detailRow("固件", "\(status.firmware) · \(status.device)")
                    detailRow("SD 卡", status.sd.mounted
                              ? "已挂载 · 剩余 \(formatBytes(status.sd.freeBytes))"
                              : "未挂载（无法上传）")
                    detailRow("屏幕", status.display.busy
                              ? "刷新中 ..."
                              : (status.display.currentName.isEmpty ? "暂无画面" : status.display.currentName))
                    detailRow("刷新", "累计 \(status.display.refreshCount) 次 · 上次 \(status.display.lastRefreshMs) ms")
                }
            } else if let error = model.statusError {
                Label(error, systemImage: "exclamationmark.triangle")
                    .font(.caption)
                    .foregroundStyle(.orange)
            } else {
                Text("正在连接设备 ...")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Divider()

            HStack(spacing: 8) {
                TextField("手动输入设备 IP，例如 192.168.1.50", text: $model.manualIP)
                    .textFieldStyle(.roundedBorder)
                    .onSubmit { model.addManualDevice() }
                Button("添加") { model.addManualDevice() }
            }

            HStack(spacing: 10) {
                Button("刷新状态") { model.refreshStatus() }
                    .disabled(model.selectedDevice == nil)
                Button("查看归档") {
                    model.showHistory = true
                    model.loadHistory()
                }
                .disabled(model.selectedDevice == nil)
                Menu("整屏清屏") {
                    ForEach(["white", "black", "red", "yellow", "blue", "green"], id: \.self) { color in
                        Button(colorLabel(color)) { model.clearScreen(color: color) }
                    }
                }
                .disabled(model.selectedDevice == nil)
                .frame(width: 110)
            }
            .font(.caption)
        }
    }

    // MARK: ② 照片

    private var imageCard: some View {
        SectionCard("② 照片", subtitle: model.imageURL == nil ? "未选择" : "已载入") {
            Button {
                isImporting = true
            } label: {
                Label("选择照片…", systemImage: "photo")
            }

            if !model.imageName.isEmpty {
                Text(model.imageName)
                    .font(.caption.monospaced())
                    .lineLimit(2)
                    .textSelection(.enabled)
            } else {
                Text("支持 JPEG / PNG / HEIC，也可以把图片拖到左侧预览区")
                    .font(.caption2)
                    .foregroundStyle(.tertiary)
            }
        }
    }

    // MARK: ③ 画面

    private var lookCard: some View {
        SectionCard("③ 画面", subtitle: "裁切框随旋转切换 3:4 / 4:3") {
            HStack(spacing: 8) {
                Text("旋转")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .frame(width: 44, alignment: .leading)
                Picker("旋转", selection: $model.settings.rotate) {
                    Text("0°").tag(0)
                    Text("90°").tag(90)
                    Text("180°").tag(180)
                    Text("270°").tag(270)
                }
                .pickerStyle(.segmented)
                .labelsHidden()
                Text("顺时针")
                    .font(.caption2)
                    .foregroundStyle(.tertiary)
            }

            HStack(spacing: 8) {
                Text("适配")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .frame(width: 44, alignment: .leading)
                Picker("适配", selection: $model.settings.fit) {
                    ForEach(FitMode.allCases) { mode in
                        Text(mode.label).tag(mode)
                    }
                }
                .pickerStyle(.menu)
                .labelsHidden()
            }

            Text(model.settings.fit.hint)
                .font(.caption2)
                .foregroundStyle(.tertiary)

            if model.settings.fit == .cover {
                Text("在左侧拖框内移动、拖四角缩放；框固定为面板比例，旋转后自动重置。")
                    .font(.caption2)
                    .foregroundStyle(.tertiary)
            }

            HStack(spacing: 8) {
                Text("抖动")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .frame(width: 44, alignment: .leading)
                Picker("抖动", selection: $model.settings.dither) {
                    ForEach(DitherMode.allCases) { mode in
                        Text(mode.label).tag(mode)
                    }
                }
                .pickerStyle(.menu)
                .labelsHidden()
            }

            HStack(spacing: 8) {
                Text("强度")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .frame(width: 44, alignment: .leading)
                Slider(value: $model.settings.strength, in: 0...1)
                Text(String(format: "%.2f", model.settings.strength))
                    .font(.caption.monospacedDigit())
                    .foregroundStyle(.secondary)
                    .frame(width: 38, alignment: .trailing)
            }

            if !model.conversionInfo.isEmpty {
                Text(model.conversionInfo)
                    .font(.caption2)
                    .foregroundStyle(.secondary)
            }
        }
    }

    // MARK: ④ 生成并上传

    private var generateCard: some View {
        SectionCard("④ 生成并上传",
                    subtitle: model.selectedDevice?.baseDescription ?? "未选择设备") {

            Button {
                model.oneClickUpload()
            } label: {
                HStack(spacing: 8) {
                    if model.pipelineActive {
                        ProgressView().controlSize(.small)
                    }
                    Text(model.pipelineActive
                         ? (model.pipelineStage.isEmpty ? "处理中 ..." : model.pipelineStage)
                         : "一键生成并上传")
                        .fontWeight(.semibold)
                    Spacer()
                    Image(systemName: "paperplane.fill")
                }
                .frame(maxWidth: .infinity)
            }
            .buttonStyle(.borderedProminent)
            .controlSize(.large)
            .keyboardShortcut(.return, modifiers: .command)
            .disabled(!model.canOneClick)

            if model.pipelineActive {
                if model.pipelineStage == "上传中" {
                    ProgressView()
                        .controlSize(.small)
                        .frame(maxWidth: .infinity, alignment: .leading)
                } else {
                    ProgressView(value: model.pipelineProgress)
                }
            } else {
                Text("用当前参数自动完成：转换 → 编译 → 上传。⌘↩ 随时可用。")
                    .font(.caption2)
                    .foregroundStyle(.tertiary)
            }

            HStack(spacing: 12) {
                Button("仅转换预览") { model.convertNow() }
                    .disabled(model.imageURL == nil || model.converting || model.pipelineActive)
                Button("仅编译") { model.compileBinary() }
                    .disabled(model.packedFrame.isEmpty || model.pipelineActive)
                Button("导出 .bin") { model.exportBinary() }
                    .disabled(model.compiledBin == nil || model.pipelineActive)
            }
            .font(.caption)

            if let bin = model.compiledBin {
                detailRow("二进制", "\(bin.count) 字节 · CRC \(CRC32.hex(model.compiledCRC))")
            }

            if let result = model.lastUpload {
                VStack(alignment: .leading, spacing: 6) {
                    detailRow("归档", result.file)
                    detailRow("设备端", "\(result.receivedMs) ms · CRC \(CRC32.hex(result.crc32))")
                    detailRow("刷屏", result.queued ? "已排队（约 30 s）" : "未排队（队列满）")
                }
                Label("已保存到 SD，屏幕正在刷新", systemImage: "checkmark.circle.fill")
                    .font(.caption)
                    .foregroundStyle(.green)
            }

            if let status = model.status, status.display.busy {
                HStack(spacing: 8) {
                    ProgressView().controlSize(.small)
                    Text("屏幕刷新中，期间请勿断电")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }

            if model.selectedDevice == nil {
                Text("先在工具栏或上方选择设备；找不到就手动输入 IP")
                    .font(.caption2)
                    .foregroundStyle(.tertiary)
            }
            if model.imageURL == nil {
                Text("先选一张照片")
                    .font(.caption2)
                    .foregroundStyle(.tertiary)
            }
        }
    }

    // MARK: 日志

    private var logCard: some View {
        SectionCard("运行日志", subtitle: "最近 \(min(model.logs.count, 8)) 条") {
            if model.logs.isEmpty {
                Text("暂无日志").font(.caption2).foregroundStyle(.tertiary)
            } else {
                VStack(alignment: .leading, spacing: 4) {
                    ForEach(model.logs.prefix(8)) { entry in
                        HStack(alignment: .firstTextBaseline, spacing: 6) {
                            Circle()
                                .fill(color(for: entry.level))
                                .frame(width: 6, height: 6)
                            Text(timeText(entry.date))
                                .font(.caption2.monospacedDigit())
                                .foregroundStyle(.tertiary)
                            Text(entry.text)
                                .font(.caption2)
                                .lineLimit(2)
                            Spacer(minLength: 0)
                        }
                    }
                }
            }
        }
    }

    // MARK: 小工具

    private func detailRow(_ key: String, _ value: String) -> some View {
        HStack(alignment: .firstTextBaseline, spacing: 8) {
            Text(key)
                .font(.caption)
                .foregroundStyle(.secondary)
                .frame(width: 52, alignment: .leading)
            Text(value)
                .font(.caption.monospaced())
                .textSelection(.enabled)
                .lineLimit(2)
            Spacer(minLength: 0)
        }
    }

    private func formatBytes(_ bytes: UInt64) -> String {
        ByteCountFormatter.string(fromByteCount: Int64(bytes), countStyle: .file)
    }

    private func colorLabel(_ name: String) -> String {
        switch name {
        case "white": return "白色"
        case "black": return "黑色"
        case "red": return "红色"
        case "yellow": return "黄色"
        case "blue": return "蓝色"
        default: return "绿色"
        }
    }

    private func timeText(_ date: Date) -> String {
        let formatter = DateFormatter()
        formatter.dateFormat = "HH:mm:ss"
        return formatter.string(from: date)
    }

    private func color(for level: LogEntry.Level) -> Color {
        switch level {
        case .info: return .secondary
        case .success: return .green
        case .warning: return .orange
        case .error: return .red
        }
    }
}

// MARK: - 卡片容器

struct SectionCard<Content: View>: View {
    let title: String
    let subtitle: String?
    let content: Content

    init(_ title: String, subtitle: String? = nil, @ViewBuilder content: () -> Content) {
        self.title = title
        self.subtitle = subtitle
        self.content = content()
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(alignment: .firstTextBaseline) {
                Text(title).font(.headline)
                Spacer(minLength: 8)
                if let subtitle {
                    Text(subtitle)
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                }
            }
            content
        }
        .padding(14)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(RoundedRectangle(cornerRadius: 12)
            .fill(Color(nsColor: .controlBackgroundColor)))
        .overlay(RoundedRectangle(cornerRadius: 12)
            .strokeBorder(Color.primary.opacity(0.06)))
    }
}
