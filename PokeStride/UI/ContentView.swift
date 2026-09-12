import SwiftUI
import UniformTypeIdentifiers

struct ContentView: View {
    @EnvironmentObject var emulator: PokeWalkerEmulator
    @State private var showSettings = false
    @State private var showNetwork = false
    @State private var showFileImporter = false
    @State private var importError: String?
    
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
            
            if emulator.needsEEPROMImport {
                importScreen
            } else {
                mainUI
            }
        }
        .sheet(isPresented: $showSettings) {
            SettingsView()
                .environmentObject(emulator)
        }
        .sheet(isPresented: $showNetwork) {
            NetworkView()
                .environmentObject(emulator)
        }
        .fileImporter(
            isPresented: $showFileImporter,
            allowedContentTypes: [
                UTType(filenameExtension: "bin") ?? .data,
                UTType(filenameExtension: "rom") ?? .data,
                .data
            ],
            allowsMultipleSelection: false
        ) { result in
            handleImport(result)
        }
        .onAppear {
            if emulator.hasEEPROM && !emulator.isRunning {
                emulator.start()
            }
        }
        .onDisappear {
            emulator.saveEEPROM()
        }
    }
    
    // MARK: - Import Screen
    
    private var importScreen: some View {
        VStack(spacing: 32) {
            Spacer()
            
            // Icon
            Image(systemName: "externaldrive.badge.checkmark")
                .font(.system(size: 64))
                .foregroundStyle(.cyan.opacity(0.8))
            
            VStack(spacing: 12) {
                Text("Import Your Save")
                    .font(.title2.bold())
                    .foregroundStyle(.white)
                
                Text("PokéStride needs your PokéWalker EEPROM save file to run. Import your `pweep.rom` or `eeprom.bin` — it's the save data from the PokéWalker accessory in HeartGold/SoulSilver.")
                    .font(.system(.caption, design: .rounded))
                    .foregroundStyle(.white.opacity(0.6))
                    .multilineTextAlignment(.center)
                    .padding(.horizontal, 40)
            }
            
            if let err = importError {
                Text(err)
                    .font(.system(.caption, design: .monospaced))
                    .foregroundStyle(.red.opacity(0.8))
                    .padding(.horizontal, 40)
                    .multilineTextAlignment(.center)
            }
            
            Button {
                showFileImporter = true
            } label: {
                Label("Import eeprom.bin", systemImage: "doc.badge.plus")
                    .font(.system(.body, design: .rounded).weight(.semibold))
                    .foregroundStyle(.white)
                    .padding(.horizontal, 32)
                    .padding(.vertical, 14)
                    .background(.cyan.opacity(0.8))
                    .clipShape(Capsule())
            }
            .buttonStyle(.plain)
            
            Spacer()
            
            // Network transfer option
            VStack(spacing: 8) {
                Text("— or —")
                    .font(.system(.caption, design: .rounded))
                    .foregroundStyle(.white.opacity(0.3))
                
                Button {
                    showNetwork = true
                } label: {
                    Label("Transfer from 3DS via WiFi", systemImage: "wifi")
                        .font(.system(.caption, design: .rounded, weight: .medium))
                        .foregroundStyle(.white.opacity(0.5))
                }
                .buttonStyle(.plain)
            }
            .padding(.bottom, 40)
        }
    }
    
    // MARK: - Main UI
    
    private var mainUI: some View {
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
    
    // MARK: - Header
    
    private var headerView: some View {
        HStack {
            VStack(alignment: .leading) {
                Text("PokéWalker")
                    .font(.title2.weight(.semibold))
                    .fontDesign(.rounded)
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
        GeometryReader { geo in
            let availableWidth = geo.size.width - 24  // padding
            let aspectRatio = Double(H8_LCD_WIDTH) / Double(H8_LCD_HEIGHT)  // 96/64 = 1.5
            let displayHeight = availableWidth / aspectRatio
            
            ZStack {
                RoundedRectangle(cornerRadius: 16)
                    .fill(.ultraThinMaterial)
                
                if let frame = emulator.lcdFrame {
                    Image(frame, scale: 1, orientation: .up, label: Text("LCD"))
                        .resizable()
                        .interpolation(.none)
                        .frame(
                            width: min(availableWidth, displayHeight * aspectRatio),
                            height: min(displayHeight, availableWidth / aspectRatio)
                        )
                        .clipShape(RoundedRectangle(cornerRadius: 8))
                } else {
                    ZStack {
                        Color(red: 0.7, green: 0.75, blue: 0.6)
                        Text("Loading…")
                            .font(.system(.caption, design: .monospaced))
                            .foregroundStyle(.black.opacity(0.5))
                    }
                    .frame(
                        width: min(availableWidth, displayHeight * aspectRatio),
                        height: min(displayHeight, availableWidth / aspectRatio)
                    )
                    .clipShape(RoundedRectangle(cornerRadius: 8))
                }
            }
        }
        .frame(height: 220)
    }
    
    // MARK: - Stats Bar
    
    private var statsBar: some View {
        HStack(spacing: 12) {
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
            
            Button {
                showFileImporter = true
            } label: {
                Label("Import", systemImage: "arrow.triangle.2.circlepath")
                    .font(.system(.caption, design: .rounded, weight: .medium))
                    .foregroundStyle(.white.opacity(0.7))
                    .padding(.horizontal, 16)
                    .padding(.vertical, 8)
                    .background(.ultraThinMaterial)
            }
            .buttonStyle(.plain)
            
            Spacer()
            
            Text("v1.0")
                .font(.system(.caption2, design: .monospaced))
                .foregroundStyle(.white.opacity(0.3))
        }
    }
    
    // MARK: - Import Handler
    
    private func handleImport(_ result: Result<[URL], Error>) {
        switch result {
        case .success(let urls):
            guard let url = urls.first else { return }
            emulator.log("File selected: \(url.lastPathComponent)")
            // Security-scoped access
            let accessing = url.startAccessingSecurityScopedResource()
            defer { if accessing { url.stopAccessingSecurityScopedResource() } }
            emulator.importEEPROM(from: url)
            // Auto-start after import
            if emulator.hasEEPROM && !emulator.isRunning {
                emulator.start()
            }
        case .failure(let error):
            importError = "Import failed: \(error.localizedDescription)"
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
