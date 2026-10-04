import SwiftUI

@main
@MainActor
struct TalkbotApp: App {
    @StateObject private var model = TalkbotModel()
    var body: some Scene {
        WindowGroup { CompanionView(model: model, tracker: model.tracker) }
    }
}
