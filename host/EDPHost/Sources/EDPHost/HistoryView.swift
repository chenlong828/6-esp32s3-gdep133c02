import SwiftUI

/// 设备端归档列表：每次上传都留一份，可以点“重新显示”回滚。
struct HistoryView: View {
    @EnvironmentObject private var model: AppModel
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                VStack(alignment: .leading, spacing: 2) {
                    Text("设备归档").font(.title3.bold())
                    Text(model.selectedDevice?.baseDescription ?? "未选择设备")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                Spacer()
                Button {
                    model.loadHistory()
                } label: {
                    Label("刷新", systemImage: "arrow.clockwise")
                }
                .disabled(model.historyLoading)
                Button("关闭") { dismiss() }
            }

            if model.historyLoading && model.history.isEmpty {
                ProgressView().frame(maxWidth: .infinity, maxHeight: .infinity)
            } else if model.history.isEmpty {
                VStack(spacing: 8) {
                    Image(systemName: "archivebox")
                        .font(.system(size: 36, weight: .light))
                        .foregroundStyle(.secondary)
                    Text("设备上还没有归档图片")
                        .foregroundStyle(.secondary)
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                List(model.history) { file in
                    HStack(spacing: 12) {
                        VStack(alignment: .leading, spacing: 3) {
                            Text(file.name)
                                .font(.body.monospaced())
                            Text(file.sizeText)
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        }
                        Spacer()
                        if file.name == model.status?.display.currentName {
                            Text("当前")
                                .font(.caption2)
                                .padding(.horizontal, 7)
                                .padding(.vertical, 2)
                                .background(Capsule().fill(Color.accentColor.opacity(0.15)))
                        }
                        Button("重新显示") { model.showAgain(file) }
                            .buttonStyle(.bordered)
                    }
                    .padding(.vertical, 4)
                }
                .listStyle(.inset)
            }
        }
        .padding(20)
        .frame(minWidth: 660, minHeight: 480)
        .onAppear { model.loadHistory() }
    }
}
