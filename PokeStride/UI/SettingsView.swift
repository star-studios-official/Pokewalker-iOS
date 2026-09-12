import SwiftUI
import HealthKit

struct SettingsView: View {
    @EnvironmentObject var emulator: PokeWalkerEmulator
    @StateObject private var sprites = ColoredSprites.shared
    @Environment(\.dismiss) private var dismiss

    @AppStorage("colorMode") private var colorMode = 0
    @AppStorage("audioEnabled") private var audioEnabled = true
    @AppStorage("backgroundSteps") private var backgroundSteps = true
    @AppStorage("lcdBrightness") private var lcdBrightness = 1.0

    var body: some View {
        NavigationView {
            ZStack {
                // Deep background
                LinearGradient(
                    gradient: Gradient(colors: [
                        Color(red: 0.05, green: 0.08, blue: 0.15),
                        Color(red: 0.02, green: 0.04, blue: 0.08)
                    ]),
                    startPoint: .top,
                    endPoint: .bottom
                )
                .ignoresSafeArea()

                Form {
                    // Display Section
                    Section {
                        // Color Mode Picker
                        VStack(alignment: .leading, spacing: 8) {
                            Label("Display Mode", systemImage: "paintbrush.fill")
                                .font(.system(.body, design: .rounded, weight: .medium))
                                .foregroundStyle(.white)

                            Picker("Color Mode", selection: $colorMode) {
                                ForEach(0..<sprites.colorModeNames.count, id: \.self) { idx in
                                    Text(sprites.colorModeNames[idx])
                                        .tag(idx)
                                }
                            }
                            .pickerStyle(.segmented)
                            .onChange(of: colorMode) { _, newValue in
                                sprites.colorMode = newValue
                                emulator.setColorMode(newValue)
                            }

                            // Preview of current color mode
                            HStack {
                                ForEach(0..<4, id: \.self) { level in
                                    RoundedRectangle(cornerRadius: 4)
                                        .fill(colorForLevel(level))
                                        .frame(width: 40, height: 40)
                                        .overlay(
                                            Text("\(level)")
                                                .font(.system(.caption2, design: .monospaced))
                                                .foregroundStyle(.white.opacity(0.7))
                                        )
                                }
                            }
                            .padding(.vertical, 4)
                        }
                        .listRowBackground(.ultraThinMaterial)

                        // LCD Brightness
                        VStack(alignment: .leading, spacing: 8) {
                            Label("LCD Brightness", systemImage: "sun.max.fill")
                                .font(.system(.body, design: .rounded, weight: .medium))
                                .foregroundStyle(.white)

                            Slider(value: $lcdBrightness, in: 0.3...1.0, step: 0.1)
                                .tint(.cyan)
                                .onChange(of: lcdBrightness) { _, newValue in
                                    emulator.setLCDBrightness(newValue)
                                }
                        }
                        .listRowBackground(.ultraThinMaterial)
                    } header: {
                        Text("Display")
                            .font(.system(.headline, design: .rounded))
                            .foregroundStyle(.cyan)
                    }

                    // Audio Section
                    Section {
                        Toggle(isOn: $audioEnabled) {
                            Label("Sound Effects", systemImage: "speaker.wave.2.fill")
                                .font(.system(.body, design: .rounded, weight: .medium))
                                .foregroundStyle(.white)
                        }
                        .tint(.cyan)
                        .onChange(of: audioEnabled) { _, newValue in
                            emulator.setAudioEnabled(newValue)
                        }
                        .listRowBackground(.ultraThinMaterial)

                        // Volume indicator
                        if audioEnabled {
                            HStack {
                                Image(systemName: "speaker.fill")
                                    .foregroundStyle(.white.opacity(0.5))
                                ProgressView(value: 1.0)
                                    .tint(.cyan)
                                Image(systemName: "speaker.wave.3.fill")
                                    .foregroundStyle(.white.opacity(0.5))
                            }
                            .padding(.vertical, 4)
                            .listRowBackground(.ultraThinMaterial)
                        }
                    } header: {
                        Text("Audio")
                            .font(.system(.headline, design: .rounded))
                            .foregroundStyle(.cyan)
                    }

                    // Steps Section
                    Section {
                        Toggle(isOn: $backgroundSteps) {
                            Label("Background Step Counting", systemImage: "figure.walk")
                                .font(.system(.body, design: .rounded, weight: .medium))
                                .foregroundStyle(.white)
                        }
                        .tint(.cyan)
                        .onChange(of: backgroundSteps) { _, newValue in
                            if newValue {
                                emulator.startStepCounting()
                            } else {
                                emulator.stopStepCounting()
                            }
                        }
                        .listRowBackground(.ultraThinMaterial)

                        if backgroundSteps {
                            VStack(alignment: .leading, spacing: 6) {
                                HStack {
                                    Text("Today's Steps")
                                        .foregroundStyle(.white.opacity(0.6))
                                    Spacer()
                                    Text("\(emulator.steps)")
                                        .font(.system(.body, design: .monospaced, weight: .semibold))
                                        .foregroundStyle(.white)
                                }
                                HStack {
                                    Text("Lifetime Steps")
                                        .foregroundStyle(.white.opacity(0.6))
                                    Spacer()
                                    Text("\(emulator.lifetimeSteps)")
                                        .font(.system(.body, design: .monospaced, weight: .semibold))
                                        .foregroundStyle(.white)
                                }
                            }
                            .font(.system(.caption, design: .rounded))
                            .listRowBackground(.ultraThinMaterial)
                        }
                    } header: {
                        Text("Health")
                            .font(.system(.headline, design: .rounded))
                            .foregroundStyle(.cyan)
                    }

                    // Data Section
                    Section {
                        Button {
                            emulator.saveEEPROM()
                        } label: {
                            Label("Save EEPROM", systemImage: "square.and.arrow.down")
                                .font(.system(.body, design: .rounded, weight: .medium))
                                .foregroundStyle(.white)
                        }
                        .listRowBackground(.ultraThinMaterial)

                        Button {
                            // Reset EEPROM to fresh state
                            emulator.resetEEPROM()
                        } label: {
                            Label("Reset EEPROM", systemImage: "arrow.counterclockwise")
                                .font(.system(.body, design: .rounded, weight: .medium))
                                .foregroundStyle(.red)
                        }
                        .listRowBackground(.ultraThinMaterial)

                        // File import
                        Button {
                            // Would open document picker for pweep.rom import
                        } label: {
                            Label("Import EEPROM", systemImage: "arrow.up.doc")
                                .font(.system(.body, design: .rounded, weight: .medium))
                                .foregroundStyle(.white)
                        }
                        .listRowBackground(.ultraThinMaterial)
                    } header: {
                        Text("Data")
                            .font(.system(.headline, design: .rounded))
                            .foregroundStyle(.cyan)
                    }

                    // About Section
                    Section {
                        HStack {
                            Text("Version")
                                .foregroundStyle(.white.opacity(0.6))
                            Spacer()
                            Text("1.0.0")
                                .font(.system(.caption, design: .monospaced))
                                .foregroundStyle(.white.opacity(0.4))
                        }
                        .listRowBackground(.ultraThinMaterial)

                        HStack {
                            Text("Based on")
                                .foregroundStyle(.white.opacity(0.6))
                            Spacer()
                            Text("pokestride / picowalker-core")
                                .font(.system(.caption, design: .monospaced))
                                .foregroundStyle(.white.opacity(0.4))
                        }
                        .listRowBackground(.ultraThinMaterial)
                    } header: {
                        Text("About")
                            .font(.system(.headline, design: .rounded))
                            .foregroundStyle(.cyan)
                    }
                }
                .scrollContentBackground(.hidden)
            }
            .navigationTitle("Settings")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .navigationBarTrailing) {
                    Button("Done") { dismiss() }
                        .font(.system(.body, design: .rounded, weight: .medium))
                }
            }
        }
    }

    private func colorForLevel(_ level: Int) -> Color {
        return sprites.paletteColor(forLevel: level)
    }
}
