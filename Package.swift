// swift-tools-version: 6.0

import PackageDescription

let package = Package(
    name: "IPhoneScreenBridge",
    platforms: [
        .macOS(.v14)
    ],
    products: [
        .executable(name: "iphone-screen-bridge", targets: ["IPhoneScreenBridge"])
    ],
    targets: [
        .executableTarget(
            name: "IPhoneScreenBridge",
            path: "Sources/IPhoneScreenBridge"
        )
    ],
    swiftLanguageModes: [.v5]
)
