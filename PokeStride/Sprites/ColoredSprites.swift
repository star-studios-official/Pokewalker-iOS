import SwiftUI

/// Colored sprite system — matches picowalker's pw_color_mode behavior.
///
/// The picowalker-core stores pw_color_mode (0-3) in health_data_cache.color_mode
/// at EEPROM offset 0x16. The picowalker hardware driver uses colour_map[4]
/// to convert 2bpp pixel values to RGB565/RGB888 colors on the AMOLED display.
///
/// 4 color modes:
///   0: Normal grayscale (original PokéWalker LCD feel)
///   1: High contrast grayscale
///   2: Sepia/warm tone
///   3: Full color (picowalker "color_fancy" mode)
@MainActor
class ColoredSprites: ObservableObject {
    static let shared = ColoredSprites()

    @Published var colorMode: Int = 0
    @Published var isLoaded = false

    /// Color mode names matching picowalker's N_COLOR_MODES = 4
    let colorModeNames = ["Grayscale", "High Contrast", "Sepia", "Full Color"]

    // MARK: - Picowalker colour_map palettes (RGB565 -> RGB888)
    // From picowalker/src/drivers/screen/sh8601z_rp2xxx_qspi_pio.h

    /// RGB565 -> RGB888 helper
    private func rgb565toRGB(_ val: UInt16) -> (UInt8, UInt8, UInt8) {
        let r5 = Int(val & 0x1F)
        let g5 = Int((val >> 5) & 0x1F)
        let b5 = Int((val >> 10) & 0x1F)
        return (UInt8((r5 << 3) | (r5 >> 2)),
                UInt8((g5 << 3) | (g5 >> 2)),
                UInt8((b5 << 3) | (b5 >> 2)))
    }

    /// Get 4-color palette for current mode (matches picowalker colour_map)
    var currentPalette: [(r: UInt8, g: UInt8, b: UInt8)] {
        let white = rgb565toRGB(0x7ead)
        let lightGrey = rgb565toRGB(0xd786)
        let darkGrey = rgb565toRGB(0xe307)
        let black = rgb565toRGB(0xa419)
        switch colorMode {
        case 0: // Normal grayscale
            return [white, lightGrey, darkGrey, black]
        case 1: // High contrast
            return [
                (0xFF, 0xFF, 0xFF),
                (0xBB, 0xBB, 0xBB),
                (0x55, 0x55, 0x55),
                (0x00, 0x00, 0x00),
            ]
        case 2: // Sepia
            return [
                (0xF5, 0xE6, 0xC8),
                (0xC4, 0xA8, 0x7A),
                (0x7A, 0x5E, 0x3C),
                (0x2C, 0x1A, 0x0A),
            ]
        case 3: // Full color (picowalker HSTX palette)
            return [
                rgb565toRGB(0xe75b), // white
                rgb565toRGB(0xbe16), // light grey
                rgb565toRGB(0x7c0e), // dark grey
                rgb565toRGB(0x5289), // black
            ]
        default:
            return [white, lightGrey, darkGrey, black]
        }
    }

    // MARK: - Data Files
    // pokestride/data contains:
    //   font_sys_glyphs.bin - System font glyphs
    //   font_sys_widths.bin - Font character widths
    //   course_bitmaps.bin  - Course/map bitmaps
    //   phcgra_data.bin     - Route graphics (1019904 bytes)
    //   phcicon_data.bin    - Pokemon/item icons (207360 bytes)

    private var fontGlyphs: Data?
    private var fontWidths: Data?
    private var courseBitmaps: Data?
    private var phcgraData: Data?
    private var phciconData: Data?

    func loadFromBundle() {
        // Load pokestride data files from bundle
        fontGlyphs = loadFromBundle("font_sys_glyphs", ext: "bin")
        fontWidths = loadFromBundle("font_sys_widths", ext: "bin")
        courseBitmaps = loadFromBundle("course_bitmaps", ext: "bin")
        phcgraData = loadFromBundle("phcgra_data", ext: "bin")
        phciconData = loadFromBundle("phcicon_data", ext: "bin")

        isLoaded = (fontGlyphs != nil && fontWidths != nil)
        print("ColoredSprites: Loaded data files - font: \(isLoaded), phcgra: \(phcgraData != nil), phcicon: \(phciconData != nil)")
    }

    private func loadFromBundle(_ name: String, ext: String) -> Data? {
        if let url = Bundle.main.url(forResource: name, withExtension: ext, subdirectory: "Data") {
            return try? Data(contentsOf: url)
        }
        if let path = Bundle.main.path(forResource: name, ofType: ext) {
            return try? Data(contentsOf: URL(fileURLWithPath: path))
        }
        return nil
    }

    // MARK: - Palette Preview (for SettingsView)

    func paletteColor(forLevel level: Int) -> Color {
        let palette = currentPalette
        guard level >= 0 && level < palette.count else { return .gray }
        let c = palette[level]
        return Color(red: Double(c.r) / 255.0,
                     green: Double(c.g) / 255.0,
                     blue: Double(c.b) / 255.0)
    }
}
