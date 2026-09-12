import SwiftUI

@main
struct PokeStrideApp: App {
    @StateObject private var emulator = PokeWalkerEmulator()
    
    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(emulator)
                .preferredColorScheme(.dark)
        }
    }
}
