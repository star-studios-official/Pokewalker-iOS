import SwiftUI
import AVFoundation
import CoreMotion
import HealthKit

/// Main emulator class — bridges the C H8/300H emulator to SwiftUI
/// Color mode system matches picowalker-core: pw_color_mode (0-3)
/// stored in health_data_cache.color_mode at EEPROM offset 0x16
@MainActor
class PokeWalkerEmulator: ObservableObject {
    // MARK: - Published State
    @Published var lcdFrame: CGImage?
    @Published var steps: UInt32 = 0
    @Published var lifetimeSteps: UInt32 = 0
    @Published var watts: UInt16 = 0
    @Published var isRunning = false
    @Published var isSleeping = false
    @Published var pokemonName: String = ""
    @Published var pokemonLevel: UInt8 = 0
    @Published var pokemonSpecies: UInt16 = 0
    @Published var colorMode: UInt8 = 0

    // MARK: - C Emulator State
    private var state = H8State()
    private var renderTimer: Timer?
    private var subClockTimer: Timer?

    // MARK: - Audio
    private var audioEngine: AVAudioEngine?
    private var audioSourceNode: AVAudioSourceNode?

    // MARK: - Step Counter
    private let pedometer = CMPedometer()
    private var lastStepCount: Int = 0
    private var stepTimer: Timer?

    // MARK: - HealthKit
    private let healthStore = HKHealthStore()

    // MARK: - ROM & EEPROM
    private var romData: Data?
    private var eepromData: Data?

    // MARK: - Colored Sprites
    private let sprites = ColoredSprites.shared

    // MARK: - Picowalker Color Palettes
    // From picowalker/src/drivers/screen/sh8601z_rp2xxx_qspi_pio.h
    // RGB565 -> RGB888 conversion for the 4 grayscale levels
    static let colourMapNormal: [(r: UInt8, g: UInt8, b: UInt8)] = [
        (0xF7, 0xDE, 0xAD),  // white  (0x7ead -> 0b0111_1110_1010_1101)
        (0x6F, 0xC0, 0x6B),  // light grey (0xd786)
        (0x38, 0xC0, 0x73),  // dark grey (0xe307)
        (0x31, 0x82, 0x45),  // black  (0xa419)
    ]

    static let colourMapHSTX: [(r: UInt8, g: UInt8, b: UInt8)] = [
        (0xF7, 0xDE, 0xAD),  // white  (0xe75b)
        (0x6C, 0xD8, 0x70),  // light grey (0xbe16)
        (0x18, 0xB8, 0x38),  // dark grey (0x7c0e)
        (0x25, 0x11, 0x24),  // black  (0x5289)
    ]

    // N_COLOR_MODES = 4 from picowalker-core/src/apps/app_picowalker.c
    // Mode 0: Normal grayscale (original PokéWalker LCD feel)
    // Mode 1: High contrast grayscale
    // Mode 2: Sepia/warm tone
    // Mode 3: Full color (picowalker "color_fancy" mode)

    /// RGB565 to RGB888 helper
    private func rgb555toRGB(_ val: UInt16) -> (r: UInt8, g: UInt8, b: UInt8) {
        let r5 = Int(val & 0x1F)
        let g5 = Int((val >> 5) & 0x1F)
        let b5 = Int((val >> 10) & 0x1F)
        return (UInt8((r5 << 3) | (r5 >> 2)),
                UInt8((g5 << 3) | (g5 >> 2)),
                UInt8((b5 << 3) | (b5 >> 2)))
    }

    /// Get the 4-color palette for the current color mode
    private func paletteForMode(_ mode: UInt8) -> [(r: UInt8, g: UInt8, b: UInt8)] {
        switch mode {
        case 0: // Normal grayscale (picowalker default)
            return Self.colourMapNormal
        case 1: // High contrast
            return [
                (0xFF, 0xFF, 0xFF),  // white
                (0xBB, 0xBB, 0xBB),  // light grey
                (0x55, 0x55, 0x55),  // dark grey
                (0x00, 0x00, 0x00),  // black
            ]
        case 2: // Sepia/warm
            return [
                (0xF5, 0xE6, 0xC8),  // warm white
                (0xC4, 0xA8, 0x7A),  // light sepia
                (0x7A, 0x5E, 0x3C),  // dark sepia
                (0x2C, 0x1A, 0x0A),  // dark brown
            ]
        case 3: // Full color (picowalker "color_fancy" mode)
            // This mode uses the colored sprite atlas
            // For LCD rendering, use enhanced palette
            return Self.colourMapHSTX
        default:
            return Self.colourMapNormal
        }
    }

    /// EEPROM HealthData offset for color_mode (from picowalker-core/types.h)
    static let eepromHealthDataColorModeOffset = 0x016E  // 0x0156 (health_data_1) + 0x16 (color_mode field)

    init() {
        loadBundledAssets()
        setupAudio()
        setupStepCounter()
    }

    deinit {
        stop()
        audioEngine?.stop()
    }

    // MARK: - Asset Loading

    private func loadBundledAssets() {
        // Load ROM from app bundle (Data/pwflash.rom)
        if let romURL = Bundle.main.url(forResource: "pwflash", withExtension: "rom", subdirectory: "Data") {
            romData = try? Data(contentsOf: romURL)
        } else if let romURL = Bundle.main.url(forResource: "pwflash", withExtension: "rom") {
            romData = try? Data(contentsOf: romURL)
        }

        // Load EEPROM from Documents (user can copy via Files app)
        let eepromPath = documentsDirectory.appendingPathComponent("pweep.rom")
        if FileManager.default.fileExists(atPath: eepromPath.path) {
            eepromData = try? Data(contentsOf: eepromPath)
        } else {
            // Try bundle default
            if let bundleEEPROM = Bundle.main.url(forResource: "pweep", withExtension: "rom", subdirectory: "Data") {
                eepromData = try? Data(contentsOf: bundleEEPROM)
            } else if let bundleEEPROM = Bundle.main.url(forResource: "pweep", withExtension: "rom") {
                eepromData = try? Data(contentsOf: bundleEEPROM)
            } else {
                // Create fresh EEPROM with "nintendo" magic
                eepromData = Data(count: 65536)
                eepromData?.replaceSubrange(0..<8, with: "nintendo".data(using: .ascii)!)
            }
        }

        // Read color mode from EEPROM health data
        if let eeprom = eepromData, eeprom.count >= 0x170 {
            colorMode = eeprom[Self.eepromHealthDataColorModeOffset]
            sprites.colorMode = Int(colorMode)
        }

        // Load pokeicon sprite data if available
        sprites.loadFromBundle()
    }

    private var documentsDirectory: URL {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
    }

    // MARK: - Emulation Control

    func start() {
        guard !isRunning else { return }

        guard let rom = romData else {
            print("ERROR: No ROM loaded (pwflash.rom not found in bundle)")
            return
        }

        // Initialize the C emulator
        rom.withUnsafeBytes { romPtr in
            eepromData?.withUnsafeBytes { eepromPtr in
                h8_init(&state, romPtr.bindMemory(to: UInt8.self).baseAddress,
                         eepromPtr.bindMemory(to: UInt8.self).baseAddress)
            }
        }

        // Set up callbacks
        let unmanagedSelf = Unmanaged.passUnretained(self).toOpaque()
        h8_set_callbacks(&state,
                         lcdCallback,
                         audioCallback,
                         stepCallback,
                         unmanagedSelf)

        isRunning = true

        // Start sub-clock timer (32768 Hz, batch ~1024 ticks)
        subClockTimer = Timer.scheduledTimer(withTimeInterval: 1.0/32.0, repeats: true) { [weak self] _ in
            self?.runSubClockBatch()
        }

        // Start LCD render timer (60 FPS)
        renderTimer = Timer.scheduledTimer(withTimeInterval: 1.0/60.0, repeats: true) { [weak self] _ in
            self?.renderFrame()
        }

        // Start step injection timer
        stepTimer = Timer.scheduledTimer(withTimeInterval: 1.0, repeats: true) { [weak self] _ in
            self?.injectHealthKitSteps()
        }
    }

    func stop() {
        isRunning = false
        renderTimer?.invalidate()
        subClockTimer?.invalidate()
        stepTimer?.invalidate()
        renderTimer = nil
        subClockTimer = nil
        stepTimer = nil
        saveEEPROM()
    }

    // MARK: - Emulation Loop

    private func runSubClockBatch() {
        guard isRunning else { return }

        let ticksPerBatch = 1024
        h8_tick_subclock(&state, Int32(ticksPerBatch))
        h8_tick_sci3(&state)

        var cyclesRun: Int32 = 0
        let targetCycles = Int32(ticksPerBatch * (H8_SYSTEM_CLOCK / H8_SUB_CLOCK))

        while cyclesRun < targetCycles && !h8_is_sleeping(&state) {
            let result = h8_step(&state)
            if result == 0 { break }
            cyclesRun += Int32(result)
        }

        DispatchQueue.main.async { [weak self] in
            guard let self = self else { return }
            self.steps = h8_get_steps(&self.state)
            self.lifetimeSteps = h8_get_lifetime_steps(&self.state)
            self.watts = h8_get_watts(&self.state)
            self.isSleeping = h8_is_sleeping(&self.state)
        }
    }

    private func renderFrame() {
        guard isRunning else { return }

        // Render raw LCD pixels (2bpp grayscale, values 0-3)
        var rawPixels = [UInt32](repeating: 0, count: H8_LCD_WIDTH * H8_LCD_HEIGHT)
        rawPixels.withUnsafeMutableBufferPointer { ptr in
            h8_render_lcd(&state, ptr.baseAddress)
        }

        // Apply color palette based on pw_color_mode
        let palette = paletteForMode(colorMode)

        // Convert raw 2bpp grayscale to colored pixels
        var colorPixels = [UInt32](repeating: 0, count: H8_LCD_WIDTH * H8_LCD_HEIGHT)
        for i in 0..<rawPixels.count {
            // Extract the 2-bit pixel value (0-3) from the raw BGRA pixel
            // h8_render_lcd produces BGRA8 with grayscale in all channels
            let raw = rawPixels[i]
            // The LCD renderer puts gray value in the blue channel
            let gray = Int(raw & 0xFF)
            // Map to palette index: 0xFF->0, 0xAA->1, 0x55->2, 0x00->3
            let idx: Int
            if gray > 0xCC { idx = 0 }
            else if gray > 0x88 { idx = 1 }
            else if gray > 0x44 { idx = 2 }
            else { idx = 3 }

            let c = palette[idx]
            // BGRA format: Blue, Green, Red, Alpha
            colorPixels[i] = UInt32(c.b) | (UInt32(c.g) << 8) | (UInt32(c.r) << 16) | 0xFF000000
        }

        // Convert BGRA pixels to CGImage
        let colorSpace = CGColorSpaceCreateDeviceRGB()
        guard let context = CGContext(
            data: &colorPixels,
            width: H8_LCD_WIDTH,
            height: H8_LCD_HEIGHT,
            bitsPerComponent: 8,
            bytesPerRow: H8_LCD_WIDTH * 4,
            space: colorSpace,
            bitmapInfo: CGBitmapInfo.byteOrder32Little.rawValue | CGImageAlphaInfo.noneSkipFirst.rawValue
        ) else { return }

        if let cgImage = context.makeImage() {
            DispatchQueue.main.async {
                self.lcdFrame = cgImage
            }
        }
    }

    // MARK: - Button Input

    func pressButton(_ button: UInt8) {
        h8_set_keys(&state, button)
    }

    func pressEnter() { pressButton(UInt8(H8_BTN_ENTER)) }
    func pressLeft()  { pressButton(UInt8(H8_BTN_LEFT)) }
    func pressRight() { pressButton(UInt8(H8_BTN_RIGHT)) }

    // MARK: - Audio Setup

    private func setupAudio() {
        let engine = AVAudioEngine()
        let format = AVAudioFormat(commonFormat: .pcmFormatInt16,
                                    sampleRate: 22050,
                                    channels: 1,
                                    interleaved: true)!

        let sourceNode = AVAudioSourceNode(renderBlock: { [weak self] _, _, frameCount, audioBufferList -> OSStatus in
            guard let self = self else { return noErr }

            let ablPointer = UnsafeMutableAudioBufferListPointer(audioBufferList)
            let buffer = ablPointer[0]
            let ptr = buffer.mData!.assumingMemoryBound(to: Int16.self)
            let count = Int(frameCount)

            if h8_is_timer_w_active(&self.state) {
                let gra = h8_get_timer_w_gra(&self.state)
                let volume = h8_get_volume(&self.state)

                if gra > 0 && volume > 0 {
                    let frequency = Double(H8_SUB_CLOCK) / (2.0 * Double(gra))
                    let sampleRate = 22050.0
                    let amplitude = Int16(16000 * Double(volume) / 2.0)

                    for i in 0..<count {
                        let t = Double(i) / sampleRate
                        let sample = (sin(2.0 * .pi * frequency * t) > 0) ? amplitude : -amplitude
                        ptr[i] = sample
                    }
                } else {
                    memset(ptr, 0, count * MemoryLayout<Int16>.size)
                }
            } else {
                memset(ptr, 0, count * MemoryLayout<Int16>.size)
            }

            return noErr
        })

        engine.attach(sourceNode)
        engine.connect(sourceNode, to: engine.mainMixerNode, format: format)

        do {
            try engine.start()
            self.audioEngine = engine
            self.audioSourceNode = sourceNode
        } catch {
            print("Audio engine failed to start: \(error)")
        }
    }

    // MARK: - Step Counter

    func setupStepCounter() {
        guard CMPedometer.isStepCountingAvailable() else {
            print("Step counting not available on this device")
            return
        }

        let calendar = Calendar.current
        let now = Date()
        let startOfDay = calendar.startOfDay(for: now)

        pedometer.startUpdates(from: startOfDay) { [weak self] data, error in
            guard let data = data, error == nil else { return }
            let newSteps = data.numberOfSteps.intValue
            let delta = newSteps - (self?.lastStepCount ?? 0)
            if delta > 0 {
                self?.lastStepCount = newSteps
                Task { @MainActor in
                    h8_inject_steps(&self!.state, UInt32(delta))
                }
            }
        }
    }

    func startStepCounting() {
        setupStepCounter()
    }

    func stopStepCounting() {
        pedometer.stopUpdates()
    }

    private func injectHealthKitSteps() {
        // CMPedometer handles real-time updates via the callback above
    }

    // MARK: - Color Mode (picowalker pw_color_mode)

    func setColorMode(_ mode: Int) {
        colorMode = UInt8(mode)
        sprites.colorMode = mode

        // Write to EEPROM health_data.color_mode (offset 0x16 in health_data)
        let eepromAddr = Self.eepromHealthDataColorModeOffset
        if var eeprom = eepromData, eeprom.count > eepromAddr {
            eeprom[eepromAddr] = UInt8(mode)
            eepromData = eeprom
        }
    }

    func getColorMode() -> Int {
        return Int(colorMode)
    }

    // MARK: - LCD Brightness

    func setLCDBrightness(_ brightness: Double) {
        // Store for future use - the actual LCD doesn't have brightness control
        UserDefaults.standard.set(brightness, forKey: "lcdBrightness")
    }

    // MARK: - Audio Toggle

    func setAudioEnabled(_ enabled: Bool) {
        if enabled {
            audioEngine?.mainMixerNode.outputVolume = 1.0
        } else {
            audioEngine?.mainMixerNode.outputVolume = 0.0
        }
    }

    // MARK: - EEPROM Persistence

    func saveEEPROM() {
        let path = documentsDirectory.appendingPathComponent("pweep.rom")
        var buffer = [UInt8](repeating: 0, count: H8_EEPROM_SIZE)
        buffer.withUnsafeMutableBufferPointer { ptr in
            h8_save_eeprom(&state, ptr.baseAddress)
        }
        try? Data(buffer).write(to: path)
    }

    func loadEEPROM(from url: URL) {
        guard let data = try? Data(contentsOf: url), data.count >= H8_EEPROM_SIZE else {
            print("Invalid EEPROM file")
            return
        }
        eepromData = data
        data.withUnsafeBytes { ptr in
            h8_load_eeprom(&state, ptr.bindMemory(to: UInt8.self).baseAddress)
        }
        // Re-read color mode
        if data.count > Self.eepromHealthDataColorModeOffset {
            colorMode = data[Self.eepromHealthDataColorModeOffset]
        }
    }

    func resetEEPROM() {
        // Create fresh EEPROM with "nintendo" magic
        eepromData = Data(count: 65536)
        eepromData?.replaceSubrange(0..<8, with: "nintendo".data(using: .ascii)!)
        if isRunning {
            eepromData?.withUnsafeBytes { ptr in
                h8_load_eeprom(&state, ptr.bindMemory(to: UInt8.self).baseAddress)
            }
        }
    }

    // MARK: - Network Transfer

    func connectToPKWBridge(address: String, port: Int = 8000) async -> Bool {
        guard let url = URL(string: "http://\(address):\(port)/transfer/info") else {
            return false
        }

        do {
            let (data, _) = try await URLSession.shared.data(from: url)
            let json = try JSONSerialization.jsonObject(with: data) as? [String: Any]
            print("Connected to: \(json?["device"] ?? "unknown")")
            return true
        } catch {
            print("Connection failed: \(error)")
            return false
        }
    }

    func sendEEPROMToPKWBridge(address: String, port: Int) async -> Bool {
        guard let url = URL(string: "http://\(address):\(port)/transfer/upload"),
              let eeprom = eepromData else { return false }

        var request = URLRequest(url: url)
        request.httpMethod = "POST"
        request.setValue("application/octet-stream", forHTTPHeaderField: "Content-Type")
        request.httpBody = eeprom

        do {
            let (_, response) = try await URLSession.shared.data(for: request)
            return (response as? HTTPURLResponse)?.statusCode == 200
        } catch {
            print("Upload failed: \(error)")
            return false
        }
    }

    func receiveEEPROMFromPKWBridge(address: String, port: Int) async -> Bool {
        guard let url = URL(string: "http://\(address):\(port)/transfer/download") else { return false }

        do {
            let (data, _) = try await URLSession.shared.data(from: url)
            guard data.count >= H8_EEPROM_SIZE else { return false }
            eepromData = data.prefix(H8_EEPROM_SIZE)
            if isRunning {
                eepromData?.withUnsafeBytes { ptr in
                    h8_load_eeprom(&state, ptr.bindMemory(to: UInt8.self).baseAddress)
                }
            }
            return true
        } catch {
            print("Download failed: \(error)")
            return false
        }
    }

    func receiveSaveFromPKWBridge(address: String, pin: String) async -> Bool {
        return false
    }
}

// MARK: - C Callbacks

private func lcdCallback(_ videoBuffer: UnsafePointer<UInt32>?, _ userdata: UnsafeMutableRawPointer?) {
    // Handled in Swift timer
}

private func audioCallback(_ graValue: UInt16, _ volume: UInt8, _ timerActive: Bool, _ userdata: UnsafeMutableRawPointer?) {
    // Handled by AVAudioSourceNode render block
}

private func stepCallback(_ userdata: UnsafeMutableRawPointer?) -> UInt32 {
    return 0
}
