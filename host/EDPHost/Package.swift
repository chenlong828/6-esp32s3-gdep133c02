// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "EDPHost",
    platforms: [
        .macOS(.v13)
    ],
    products: [
        .executable(name: "EDPHost", targets: ["EDPHost"])
    ],
    targets: [
        .executableTarget(
            name: "EDPHost",
            path: "Sources/EDPHost"
        )
    ]
)
