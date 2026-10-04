// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "TalkbotCore",
    platforms: [.iOS(.v16), .macOS(.v13)],
    products: [.library(name: "TalkbotCore", targets: ["TalkbotCore"])],
    targets: [
        .target(name: "TalkbotCore"),
        .testTarget(name: "TalkbotCoreTests", dependencies: ["TalkbotCore"])
    ]
)
