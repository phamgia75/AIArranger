// P2.6 iPadOS host app entry. Lifecycle: ScenePhase -> HostEngine.handle(phase:).
import SwiftUI

@main
struct AIArrangerApp: App {
    @StateObject private var model = LiveModel()
    @Environment(\.scenePhase) private var scenePhase

    var body: some Scene {
        WindowGroup {
            LiveView(model: model)
        }
        .onChange(of: scenePhase) { _, phase in model.phase(phase) }
    }
}
