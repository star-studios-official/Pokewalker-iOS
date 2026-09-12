import SwiftUI

struct ContentView: View {
    @EnvironmentObject var emulator: PokeWalkerEmulator
    @State private var showSettings = false
    @State private var showNetwork = false
    
    var body: some View {
        ZStack {
            // Background gradient — deep PokéWalker blue
            LinearGradient(
                gradient: Gradient(colors: [
                    Color(red: 0.05, green: 0.08, blue: 0.15),
                    Color(red: 0.02, green: 0.04, blue: 0.08)
                ]),
                startPoint: .top,
                endPoint: .bottom
            )
            .ignoresSafeArea()
            
            VStack(spacing: 24) {
                // Header
                headerView
                
                // LCD Display
                lcdDisplay
                
                // Stats bar
                statsBar
                
                // Virtual buttons
                buttonPad
                
                // Bottom controls
                bottomControls
            }
            .padding(.horizontal, 20)
            .padding(.top, 10)
        }
        .sheet(isPresented: $showSettings) {
            SettingsView()
                .environmentObject(emulator)
        }
        .sheet(isPresented: $showNetwork) {
            NetworkView()
                .environmentObject(emulator)
        }
        .onAppear {
            if !emulator.isRunning {
                emulator.start()
            }
        }
        .onDisappear {
            emulator.saveEEPROM()
        }
    }
    
    // MARK: - Header
    
    private var headerView: some View {
        HStack {
            VStack(alignment: .leading) {
                Text("PokéWalker")
                    .font(.title2.weight(.semibold).design(.rounded))
                    .foregroundStyle(.white)
                Text("HeartGold · SoulSilver")
                    .font(.system(.caption, design: .rounded))
                    .foregroundStyle(.white.opacity(0.5))
            }
            
            Spacer()
            
            HStack(spacing: 12) {
                Button {
                    showNetwork = true
                } label: {
                    Image(systemName: "network")
                        .font(.system(.body, weight: .medium))
                        .foregroundStyle(.white.opacity(0.7))
                }
                
                Button {
                    showSettings = true
                } label: {
                    Image(systemName: "gearshape.fill")
                        .font(.system(.body, weight: .medium))
                        .foregroundStyle(.white.opacity(0.7))
                }
            }
        }
    }
    
    // MARK: - LCD Display
    
    private var lcdDisplay: some View {
        VStack(spacing: 0) {
            // LCD frame with Liquid Glass effect
            ZStack {
                // Outer bezel
                RoundedRectangle(cornerRadius: 16)
                    .fill(.ultraThinMaterial)
                    .background(.ultraThinMaterial)
                    .frame(height: 280)
                
                // LCD screen
                if let frame = emulator.lcdFrame {
                    Image(frame, scale: 1, orientation: .up, label: Text("LCD"))
                        .resizable()
                        .interpolation(.none)
                        .aspectRatio(contentMode: .fit)
                        .frame(width: 288, height: 192)
                        .clipShape(RoundedRectangle(cornerRadius: 8))
                        .padding(8)
                } else {
                    // Placeholder when emulator isn't running
                    ZStack {
                        Color(red: 0.7, green: 0.75, blue: 0.6)
                        Text("No ROM")
                            .font(.system(.caption, design: .monospaced))
                            .foregroundStyle(.black.opacity(0.5))
                    }
                    .clipShape(RoundedRectangle(cornerRadius: 8))
                    .padding(8)
                }
            }
        }
        .background(.ultraThinMaterial)
    }    // MARK: - Stats Bar

    private var statsBar: some View {
        HStack(spacing: 12) {
            // Color mode toggle (picowalker pw_color_mode)
            Button {
                withAnimation(.easeOut(duration: 0.2)) {
                    let next = (emulator.colorMode + 1) % 4
                    emulator.setColorMode(Int(next))
                }
            } label: {
                HStack(spacing: 6) {
                    Image(systemName: "paintpalette.fill")
                        .font(.system(.caption, weight: .medium))
                    Text(colorModeName)
                        .font(.system(.caption2, design: .rounded, weight: .medium))
                }
                .foregroundStyle(.white)
                .padding(.horizontal, 10)
                .padding(.vertical, 6)
                .background(.ultraThinMaterial)
            }
            .buttonStyle(.plain)

            StatBadge(icon: "figure.walk", value: "\(emulator.steps)", label: "Steps")
            StatBadge(icon: "bolt.fill", value: "\(emulator.watts)W", label: "Watts")
            
            if emulator.isSleeping {
                StatBadge(icon: "moon.fill", value: "Zzz", label: "Sleep")
            }
        }
        .padding(.horizontal, 8)
    }

    private var colorModeName: String {
        switch emulator.colorMode {
        case 0: return "Grayscale"
        case 1: return "Contrast"
        case 2: return "Sepia"
        case 3: return "Color"
        default: return "Mode \(emulator.colorMode)"
        }
    }
    
    // MARK: - Button Pad
    
    private var buttonPad: some View {
        HStack(spacing: 40) {
            // LEFT button
            Button {
                withAnimation(.easeOut(duration: 0.1)) {
                    emulator.pressLeft()
                }
            } label: {
                Image(systemName: "chevron.left")
                    .font(.title2.bold())
                    .foregroundStyle(.white)
                    .frame(width: 56, height: 56)
                    .background(.ultraThinMaterial)
            }
            .buttonStyle(.plain)
            
            // ENTER button (center)
            Button {
                withAnimation(.easeOut(duration: 0.1)) {
                    emulator.pressEnter()
                }
            } label: {
                Text("A")
                    .font(.title2.bold())
                    .foregroundStyle(.white)
                    .frame(width: 72, height: 72)
                    .background(.ultraThinMaterial)
            }
            .buttonStyle(.plain)
            
            // RIGHT button
            Button {
                withAnimation(.easeOut(duration: 0.1)) {
                    emulator.pressRight()
                }
            } label: {
                Image(systemName: "chevron.right")
                    .font(.title2.bold())
                    .foregroundStyle(.white)
                    .frame(width: 56, height: 56)
                    .background(.ultraThinMaterial)
            }
            .buttonStyle(.plain)
        }
        .padding(.vertical, 8)
    }
    
    // MARK: - Bottom Controls
    
    private var bottomControls: some View {
        HStack(spacing: 20) {
            Button {
                emulator.saveEEPROM()
            } label: {
                Label("Save", systemImage: "square.and.arrow.down")
                    .font(.system(.caption, design: .rounded, weight: .medium))
                    .foregroundStyle(.white.opacity(0.7))
                    .padding(.horizontal, 16)
                    .padding(.vertical, 8)
                    .background(.ultraThinMaterial)
            }
            .buttonStyle(.plain)
            
            if emulator.isRunning {
                Button {
                    emulator.stop()
                } label: {
                    Label("Pause", systemImage: "pause.fill")
                        .font(.system(.caption, design: .rounded, weight: .medium))
                        .foregroundStyle(.white.opacity(0.7))
                        .padding(.horizontal, 16)
                        .padding(.vertical, 8)
                        .background(.ultraThinMaterial)
                }
                .buttonStyle(.plain)
            } else {
                Button {
                    emulator.start()
                } label: {
                    Label("Resume", systemImage: "play.fill")
                        .font(.system(.caption, design: .rounded, weight: .medium))
                        .foregroundStyle(.white.opacity(0.7))
                        .padding(.horizontal, 16)
                        .padding(.vertical, 8)
                        .background(.ultraThinMaterial)
                }
                .buttonStyle(.plain)
            }
            
            Spacer()
            
            Text("v1.0")
                .font(.system(.caption2, design: .monospaced))
                .foregroundStyle(.white.opacity(0.3))
        }
    }
}

// MARK: - Stat Badge

struct StatBadge: View {
    let icon: String
    let value: String
    let label: String
    
    var body: some View {
        VStack(spacing: 4) {
            Image(systemName: icon)
                .font(.system(.caption, weight: .medium))
                .foregroundStyle(.cyan)
            Text(value)
                .font(.system(.caption, design: .monospaced, weight: .semibold))
                .foregroundStyle(.white)
            Text(label)
                .font(.system(.caption2, design: .rounded))
                .foregroundStyle(.white.opacity(0.4))
        }
        .frame(minWidth: 70)
        .padding(.vertical, 10)
        .padding(.horizontal, 12)
        .background(.ultraThinMaterial)
    }
}

#Preview {
    ContentView()
        .environmentObject(PokeWalkerEmulator())
}
