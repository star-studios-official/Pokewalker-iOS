import SwiftUI
import AVFoundation
import CoreMotion
import HealthKit

/// Main emulator class — bridges the C H8/300H emulator to SwiftUI
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
    // nonisolated(unsafe) required: deinit is nonisolated in Swift 6 but must clean these up
    private nonisolated(unsafe) var statePointer: UnsafeMutablePointer<H8State>?
    private var renderTimer: Timer?
    private var subClockTimer: Timer?

    // MARK: - Audio
    private nonisolated(unsafe) var audioEngine: AVAudioEngine?
    private nonisolated(unsafe) var audioSourceNode: AVAudioSourceNode?

    // MARK: - Step Counter
    private let pedometer = CMPedometer()
    nonisolated(unsafe) private var lastStepCount: Int = 0
    private var stepTimer: Timer?
    private var stepCountingActive = false
    private let healthStore = HKHealthStore()
    private var observerQuery: HKObserverQuery?

    // MARK: - ROM & EEPROM
    private var romData: Data?
    private var eepromData: Data?

    // MARK: - Colored Sprites
    private let sprites = ColoredSprites.shared

    // MARK: - Picowalker Color Palettes
    static let colourMapNormal: [(r: UInt8, g: UInt8, b: UInt8)] = [
        (0xF7, 0xDE, 0xAD), (0x6F, 0xC0, 0x6B),
        (0x38, 0xC0, 0x73), (0x31, 0x82, 0x45),
    ]

    static let colourMapHSTX: [(r: UInt8, g: UInt8, b: UInt8)] = [
        (0xF7, 0xDE, 0xAD), (0x6C, 0xD8, 0x70),
        (0x18, 0xB8, 0x38), (0x25, 0x11, 0x24),
    ]

    private func paletteForMode(_ mode: UInt8) -> [(r: UInt8, g: UInt8, b: UInt8)] {
        switch mode {
        case 0: return Self.colourMapNormal
        case 1: return [(0xFF,0xFF,0xFF),(0xBB,0xBB,0xBB),(0x55,0x55,0x55),(0x00,0x00,0x00)]
        case 2: return [(0xF5,0xE6,0xC8),(0xC4,0xA8,0x7A),(0x7A,0x5E,0x3C),(0x2C,0x1A,0x0A)]
        case 3: return Self.colourMapHSTX
        default: return Self.colourMapNormal
        }
    }

    static let eepromHealthDataColorModeOffset = 0x016E

    init() {
        loadBundledAssets()
    }

    deinit {
        if let ptr = statePointer {
            ptr.deinitialize(count: 1)
            ptr.deallocate()
        }
        audioEngine?.stop()
    }

    // MARK: - Asset Loading

    private func loadBundledAssets() {
        if let url = Bundle.main.url(forResource: "pwflash", withExtension: "rom", subdirectory: "Data") {
            romData = try? Data(contentsOf: url)
        } else if let url = Bundle.main.url(forResource: "pwflash", withExtension: "rom") {
            romData = try? Data(contentsOf: url)
        }

        let eepromPath = documentsDirectory.appendingPathComponent("pweep.rom")
        if FileManager.default.fileExists(atPath: eepromPath.path) {
            eepromData = try? Data(contentsOf: eepromPath)
        } else if let url = Bundle.main.url(forResource: "pweep", withExtension: "rom", subdirectory: "Data") {
            eepromData = try? Data(contentsOf: url)
        } else if let url = Bundle.main.url(forResource: "pweep", withExtension: "rom") {
            eepromData = try? Data(contentsOf: url)
        } else {
            eepromData = Data(count: 65536)
            eepromData?.replaceSubrange(0..<8, with: "nintendo".data(using: .ascii)!)
        }

        if let eeprom = eepromData, eeprom.count >= 0x170 {
            colorMode = eeprom[Self.eepromHealthDataColorModeOffset]
            sprites.colorMode = Int(colorMode)
        }

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

        // Allocate state on heap — stable pointer for the audio render block
        let ptr = UnsafeMutablePointer<H8State>.allocate(capacity: 1)
        ptr.initialize(to: H8State())
        statePointer = ptr

        rom.withUnsafeBytes { romPtr in
            eepromData?.withUnsafeBytes { eepromPtr in
                h8_init(ptr,
                        romPtr.bindMemory(to: UInt8.self).baseAddress,
                        eepromPtr.bindMemory(to: UInt8.self).baseAddress)
            }
        }

        isRunning = true
        setupAudio()
        startTimers()
        startStepCounting()
    }

    func stop() {
        isRunning = false
        audioEngine?.stop()
        audioEngine = nil
        audioSourceNode = nil
        renderTimer?.invalidate()
        subClockTimer?.invalidate()
        stepTimer?.invalidate()
        renderTimer = nil
        subClockTimer = nil
        stepTimer = nil
        stopStepCounting()
        saveEEPROM()
    }

    private func startTimers() {
        subClockTimer = Timer.scheduledTimer(withTimeInterval: 1.0/32.0, repeats: true) { [weak self] _ in
            Task { @MainActor [weak self] in
                self?.runSubClockBatch()
            }
        }
        renderTimer = Timer.scheduledTimer(withTimeInterval: 1.0/60.0, repeats: true) { [weak self] _ in
            Task { @MainActor [weak self] in
                self?.renderFrame()
            }
        }
        stepTimer = Timer.scheduledTimer(withTimeInterval: 1.0, repeats: true) { [weak self] _ in
            Task { @MainActor [weak self] in
                self?.updateStepDisplay()
            }
        }
    }

    // MARK: - Emulation Loop

    private func runSubClockBatch() {
        guard isRunning, let state = statePointer else { return }
        let ticks = 1024
        h8_tick_subclock(state, Int32(ticks))
        h8_tick_sci3(state)
        var cycles: Int32 = 0
        let target = Int32(ticks) * (H8_SYSTEM_CLOCK / H8_SUB_CLOCK)
        while cycles < target && !h8_is_sleeping(state) {
            let r = h8_step(state)
            if r == 0 { break }
            cycles += Int32(r)
        }
        self.steps = h8_get_steps(state)
        self.lifetimeSteps = h8_get_lifetime_steps(state)
        self.watts = h8_get_watts(state)
        self.isSleeping = h8_is_sleeping(state)
    }

    private func renderFrame() {
        guard isRunning, let state = statePointer else { return }
        let w = Int(H8_LCD_WIDTH)
        let h = Int(H8_LCD_HEIGHT)
        var raw = [UInt32](repeating: 0, count: w * h)
        raw.withUnsafeMutableBufferPointer { h8_render_lcd(state, $0.baseAddress) }
        let palette = paletteForMode(colorMode)
        var color = [UInt32](repeating: 0, count: raw.count)
        for i in 0..<raw.count {
            let pixel = raw[i] & 0xFF
            let idx = pixel > 0xCC ? 0 : pixel > 0x88 ? 1 : pixel > 0x44 ? 2 : 3
            let c = palette[idx]
            let red = UInt32(c.r) << 16
            let grn = UInt32(c.g) << 8
            let blu = UInt32(c.b)
            color[i] = red | grn | blu | 0xFF000000
        }
        // Use Data copy + CGDataProvider so the CGImage owns its backing store
        let pixelData = Data(bytes: color, count: color.count * MemoryLayout<UInt32>.size)
        guard let provider = CGDataProvider(data: pixelData as CFData) else { return }
        let cs = CGColorSpaceCreateDeviceRGB()
        let img = CGImage(
            width: w, height: h,
            bitsPerComponent: 8, bitsPerPixel: 32,
            bytesPerRow: w * 4, space: cs,
            bitmapInfo: CGBitmapInfo(rawValue: CGBitmapInfo.byteOrder32Little.rawValue | CGImageAlphaInfo.noneSkipFirst.rawValue),
            provider: provider, decode: nil,
            shouldInterpolate: false, intent: .defaultIntent)
        if let img = img { self.lcdFrame = img }
    }

    private func updateStepDisplay() {}

    // MARK: - Button Input

    func pressButton(_ button: UInt8) {
        guard let s = statePointer else { return }
        h8_set_keys(s, button)
    }
    func pressEnter() { pressButton(UInt8(H8_BTN_ENTER)) }
    func pressLeft()  { pressButton(UInt8(H8_BTN_LEFT)) }
    func pressRight() { pressButton(UInt8(H8_BTN_RIGHT)) }

    // MARK: - Audio (called from start() so statePointer is ready)

    nonisolated private func setupAudio() {
        guard let statePtr = statePointer else { return }

        // Snapshot the raw C function pointers outside any actor context.
        // The render block runs on the audio IO thread and must not
        // trigger Swift 6 actor-isolation checks.
        let activeFunc  = h8_is_timer_w_active
        let graFunc     = h8_get_timer_w_gra
        let volFunc     = h8_get_volume
        let subClock    = Double(H8_SUB_CLOCK)

        let engine = AVAudioEngine()
        guard let format = AVAudioFormat(commonFormat: .pcmFormatInt16, sampleRate: 22050, channels: 1, interleaved: true) else { return }

        // The render block is @Sendable and runs on the audio IO thread.
        // We capture only raw pointers / C function values — no actor state.
        let sourceNode = AVAudioSourceNode(renderBlock: { _, _, frameCount, audioBufferList -> OSStatus in
            let abl = UnsafeMutableAudioBufferListPointer(audioBufferList)
            let buf = abl[0]
            let ptr = buf.mData!.assumingMemoryBound(to: Int16.self)
            let count = Int(frameCount)

            let active = activeFunc(statePtr)
            let gra    = graFunc(statePtr)
            let vol    = volFunc(statePtr)

            if active && gra > 0 && vol > 0 {
                let freq = subClock / (2.0 * Double(gra))
                let amp  = Int16(16000 * Double(vol) / 2.0)
                for i in 0..<count {
                    let t = Double(i) / 22050.0
                    ptr[i] = (sin(2.0 * .pi * freq * t) > 0) ? amp : -amp
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
            print("Audio engine failed: \(error)")
        }
    }

    // MARK: - Step Counter

    func startStepCounting() {
        guard !stepCountingActive else { return }
        stepCountingActive = true

        // Request HealthKit authorization
        let stepType = HKQuantityType.quantityType(forIdentifier: .stepCount)!
        healthStore.requestAuthorization(toShare: nil, read: [stepType]) { [weak self] granted, error in
            guard granted, error == nil else {
                print("HealthKit auth denied, falling back to CMPedometer")
                Task { @MainActor in self?.startCMPedometerFallback() }
                return
            }
            Task { @MainActor in self?.setupHealthKitObserver() }
        }
    }

    private func setupHealthKitObserver() {
        let stepType = HKQuantityType.quantityType(forIdentifier: .stepCount)!
        let startOfDay = Calendar.current.startOfDay(for: Date())
        let predicate = HKQuery.predicateForSamples(withStart: startOfDay, end: nil)

        // Get initial count (capture lastStepCount before closure to avoid actor crossing)
        let statsQuery = HKStatisticsQuery(quantityType: stepType, quantitySamplePredicate: predicate, options: .cumulativeSum) { [weak self] _, result, _ in
            guard let sum = result?.sumQuantity() else { return }
            let count = Int(sum.doubleValue(for: .count()))
            Task { @MainActor in self?.lastStepCount = count }
        }
        healthStore.execute(statsQuery)

        // Observer for real-time updates (works in background)
        // Nonisolated(unsafe) copies avoid actor-crossing warnings in closures
        let store = healthStore
        let stepTypeCapture = stepType
        observerQuery = HKObserverQuery(sampleType: stepType, predicate: predicate) { [weak self] _, completionHandler, error in
            guard error == nil else { completionHandler(); return }
            // Re-query the total and compute delta
            let innerQuery = HKStatisticsQuery(quantityType: stepTypeCapture, quantitySamplePredicate: predicate, options: .cumulativeSum) { _, innerResult, _ in
                guard let sum = innerResult?.sumQuantity() else { completionHandler(); return }
                let total = Int(sum.doubleValue(for: .count()))
                Task { @MainActor [weak self] in
                    guard let self = self else { return }
                    let delta = total - self.lastStepCount
                    if delta > 0 {
                        self.lastStepCount = total
                        if let s = self.statePointer {
                            h8_inject_steps(s, UInt32(delta))
                            self.steps = h8_get_steps(s)
                            self.lifetimeSteps = h8_get_lifetime_steps(s)
                        }
                    }
                }
                completionHandler()
            }
            store.execute(innerQuery)
        }
        if let q = observerQuery {
            healthStore.execute(q)
        }

        // Enable background delivery — triggers even when app is closed
        healthStore.enableBackgroundDelivery(for: stepType, frequency: .immediate) { success, error in
            if success {
                print("HealthKit background delivery enabled")
            } else if let error = error {
                print("Background delivery error: \(error)")
            }
        }
    }

    private func startCMPedometerFallback() {
        guard CMPedometer.isStepCountingAvailable() else { return }
        let startOfDay = Calendar.current.startOfDay(for: Date())
        pedometer.startUpdates(from: startOfDay) { [weak self] data, error in
            guard let data = data, error == nil else { return }
            let delta = data.numberOfSteps.intValue - (self?.lastStepCount ?? 0)
            if delta > 0, let self = self {
                self.lastStepCount = data.numberOfSteps.intValue
                Task { @MainActor in
                    guard let s = self.statePointer else { return }
                    h8_inject_steps(s, UInt32(delta))
                    self.steps = h8_get_steps(s)
                    self.lifetimeSteps = h8_get_lifetime_steps(s)
                }
            }
        }
    }

    func stopStepCounting() {
        stepCountingActive = false
        pedometer.stopUpdates()
        if let q = observerQuery {
            healthStore.stop(q)
            observerQuery = nil
        }
    }

    // MARK: - Color Mode

    func setColorMode(_ mode: Int) {
        colorMode = UInt8(mode)
        sprites.colorMode = mode
        if var eeprom = eepromData, eeprom.count > Self.eepromHealthDataColorModeOffset {
            eeprom[Self.eepromHealthDataColorModeOffset] = UInt8(mode)
            eepromData = eeprom
        }
    }

    func getColorMode() -> Int { Int(colorMode) }
    func setLCDBrightness(_ b: Double) { UserDefaults.standard.set(b, forKey: "lcdBrightness") }

    func setAudioEnabled(_ enabled: Bool) {
        if enabled { audioEngine?.mainMixerNode.outputVolume = 1.0 }
        else { audioEngine?.mainMixerNode.outputVolume = 0.0 }
    }

    // MARK: - EEPROM

    func saveEEPROM() {
        guard let state = statePointer else { return }
        var buf = [UInt8](repeating: 0, count: Int(H8_EEPROM_SIZE))
        _ = buf.withUnsafeMutableBufferPointer { h8_save_eeprom(state, $0.baseAddress) }
        try? Data(buf).write(to: documentsDirectory.appendingPathComponent("pweep.rom"))
    }

    func loadEEPROM(from url: URL) {
        guard let data = try? Data(contentsOf: url), data.count >= Int(H8_EEPROM_SIZE),
              let state = statePointer else { return }
        eepromData = data
        data.withUnsafeBytes { h8_load_eeprom(state, $0.bindMemory(to: UInt8.self).baseAddress) }
        if data.count > Self.eepromHealthDataColorModeOffset { colorMode = data[Self.eepromHealthDataColorModeOffset] }
    }

    func resetEEPROM() {
        eepromData = Data(count: 65536)
        eepromData?.replaceSubrange(0..<8, with: "nintendo".data(using: .ascii)!)
        if isRunning, let s = statePointer {
            eepromData?.withUnsafeBytes { h8_load_eeprom(s, $0.bindMemory(to: UInt8.self).baseAddress) }
        }
    }

    // MARK: - Network Transfer (matches PKSM WirelessTransfer protocol)

    func connectToPKWBridge(address: String, port: Int = 8000) async -> Bool {
        guard let url = URL(string: "http://\(address):\(port)/transfer/info") else { return false }
        do {
            let (data, response) = try await URLSession.shared.data(from: url)
            guard let httpResponse = response as? HTTPURLResponse,
                  httpResponse.statusCode == 200 else {
                print("Connection failed: bad status")
                return false
            }
            let json = try JSONSerialization.jsonObject(with: data) as? [String: Any]
            print("Connected to: \(json?["device"] ?? "unknown")")
            return true
        } catch { print("Connection failed: \(error)"); return false }
    }

    func sendEEPROMToPKWBridge(address: String, port: Int) async -> Bool {
        guard let url = URL(string: "http://\(address):\(port)/transfer/upload"),
              let eeprom = eepromData else { return false }

        let boundary = "----pokestride-\(UUID().uuidString)"
        var req = URLRequest(url: url)
        req.httpMethod = "POST"
        req.setValue("multipart/form-data; boundary=\(boundary)", forHTTPHeaderField: "Content-Type")
        req.timeoutInterval = 30

        // Build multipart body matching what the 3DS server expects
        var body = Data()
        body.append(Data("--\(boundary)\r\n".utf8))
        body.append(Data("Content-Disposition: form-data; name=\"meta\"\r\n".utf8))
        body.append(Data("Content-Type: application/json\r\n\r\n".utf8))
        let meta = "{\"titleId\":\"PokeStride\",\"dataType\":\"eeprom\",\"fileName\":\"pweep.rom\",\"fileBytesTotal\":\(eeprom.count)}"
        body.append(Data(meta.utf8))
        body.append(Data("\r\n".utf8))

        body.append(Data("--\(boundary)\r\n".utf8))
        body.append(Data("Content-Disposition: form-data; name=\"file\"; filename=\"pweep.rom\"\r\n".utf8))
        body.append(Data("Content-Type: application/octet-stream\r\n\r\n".utf8))
        body.append(eeprom)
        body.append(Data("\r\n--\(boundary)--\r\n".utf8))

        req.httpBody = body
        do {
            let (data, resp) = try await URLSession.shared.data(for: req)
            let code = (resp as? HTTPURLResponse)?.statusCode ?? -1
            if code == 200 {
                let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any]
                if json?["ok"] as? Bool == true { return true }
            }
            print("Upload failed: HTTP \(code)")
            return false
        } catch { print("Upload failed: \(error)"); return false }
    }

    func receiveEEPROMFromPKWBridge(address: String, port: Int) async -> Bool {
        guard let url = URL(string: "http://\(address):\(port)/transfer/download") else { return false }
        do {
            let (data, response) = try await URLSession.shared.data(from: url)
            guard let httpResponse = response as? HTTPURLResponse,
                  httpResponse.statusCode == 200 else {
                print("Download failed: bad status")
                return false
            }
            guard data.count >= Int(H8_EEPROM_SIZE) else {
                print("Download failed: too small (\(data.count) bytes)")
                return false
            }
            let eeprom = data.prefix(Int(H8_EEPROM_SIZE))
            eepromData = eeprom
            // Save to Documents for Files app access and persistence
            try? eeprom.write(to: documentsDirectory.appendingPathComponent("pweep.rom"))
            // Hot-reload into running emulator
            if isRunning, let s = statePointer {
                eeprom.withUnsafeBytes { h8_load_eeprom(s, $0.bindMemory(to: UInt8.self).baseAddress) }
            }
            return true
        } catch { print("Download failed: \(error)"); return false }
    }
}
