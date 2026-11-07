import AppKit
import SwiftUI

@main
enum Main {
    static func main() {
        let arguments = CommandLine.arguments
        if arguments.count > 1, arguments[1].hasPrefix("--") {
            CLI.run(arguments: Array(arguments.dropFirst()))
            return
        }
        EDPHostApp.main()
    }
}

struct EDPHostApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate
    @StateObject private var model = AppModel()

    var body: some Scene {
        WindowGroup("电子纸传图") {
            ContentView()
                .environmentObject(model)
        }
        .defaultSize(width: 1180, height: 820)
        .commands {
            CommandGroup(replacing: .newItem) {}
        }
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.regular)
        NSApp.activate(ignoringOtherApps: true)
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        true
    }
}
