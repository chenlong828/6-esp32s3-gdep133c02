import SwiftUI
import UniformTypeIdentifiers

struct ContentView: View {
    @EnvironmentObject private var model: AppModel
    @State private var isImporting = false
    @State private var isDropTarget = false

    var body: some View {
        HSplitView {
            PreviewPane(isDropTarget: $isDropTarget, isImporting: $isImporting)
                .frame(minWidth: 420, idealWidth: 520)

            ControlPane(isImporting: $isImporting)
                .frame(minWidth: 400, idealWidth: 440, maxWidth: 560)
        }
        .frame(minWidth: 1000, minHeight: 760)
        .navigationTitle("电子纸传图 · GDEP133C02")
        .toolbar { toolbar }
        .fileImporter(isPresented: $isImporting,
                      allowedContentTypes: [.image],
                      allowsMultipleSelection: false) { result in
            switch result {
            case .success(let urls):
                if let url = urls.first { model.pickImage(url: url) }
            case .failure(let error):
                model.errorAlert = error.localizedDescription
            }
        }
        .sheet(isPresented: $model.showHistory) {
            HistoryView()
                .environmentObject(model)
        }
        .alert("出错了",
               isPresented: Binding(get: { model.errorAlert != nil },
                                    set: { if !$0 { model.errorAlert = nil } })) {
            Button("知道了", role: .cancel) { model.errorAlert = nil }
        } message: {
            Text(model.errorAlert ?? "")
        }
        .onAppear { model.onAppear() }
    }

    private var toolbar: some ToolbarContent {
        ToolbarItemGroup(placement: .automatic) {
            HStack(spacing: 6) {
                Circle()
                    .fill(connectionColor)
                    .frame(width: 8, height: 8)
                Text(connectionText)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Picker("设备", selection: $model.selectedDeviceID) {
                if model.devices.isEmpty {
                    Text("未发现设备").tag(Optional<String>.none)
                }
                ForEach(model.devices) { device in
                    Text(device.displayName).tag(Optional(device.id))
                }
            }
            .frame(width: 240)

            Button {
                model.scan()
            } label: {
                Label("重新扫描", systemImage: "antenna.radiowaves.left.and.right")
            }
            .disabled(model.scanning)
        }
    }

    private var connectionColor: Color {
        if model.status != nil { return .green }
        if model.selectedDevice != nil { return .orange }
        return .red
    }

    private var connectionText: String {
        if model.status != nil { return "已连接" }
        if model.selectedDevice != nil { return "连接中" }
        return model.scanning ? "搜索中" : "未连接"
    }
}
