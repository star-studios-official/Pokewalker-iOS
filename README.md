# PokeStride — PokéWalker Emulator for iOS

A full PokéWalker emulator for iPhone and iPad, based on [pokestride](https://github.com/edgarburgues/pokestride) (3DS) and [picowalker-core](https://github.com/h4lfheart/picowalker-core). Count your steps, walk Pokémon, and connect to your 3DS via WiFi — no IR hardware required.

![iOS 26+](https://img.shields.io/badge/iOS-26%2B-blue) ![Swift 6](https://img.shields.io/badge/Swift-6-orange) ![License](https://img.shields.io/badge/License-GPLv3-green)

---

## Features

### 🏃 Accurate Step Counting
- Reads your real step count from **Apple Health** (CMPedometer)
- Works in the **background** — counts steps even when the app is closed
- Respects the original 20-steps-per-watt formula
- Today's steps, lifetime steps, and watts all update in real time

### 📺 Authentic LCD Display
- 96×64 pixel display at 4-level grayscale (original PokéWalker feel)
- 4 color modes (toggle with the palette button):
  - **Grayscale** — faithful to the original LCD
  - **High Contrast** — better visibility in bright light
  - **Sepia** — warm vintage tone
  - **Full Color** — picowalker AMOLED-style palette

### 🔊 Real PokéWalker Audio
- Timer W square-wave beeper (matches original hardware)
- Sound effects from the EEPROM sound table (16 effects)
- Volume toggle in settings

### 📡 WiFi Transfer with 3DS (pkwbridge)
- Connect your 3DS running pkwbridge to transfer `pweep.rom` over WiFi
- **3DS acts as the server**, iPhone connects as a client
- Upload/download EEPROM wirelessly — no SD card swapping needed

### 📂 File Sharing
- Export/import EEPROM via the **iOS Files app**
- Copy `pweep.rom` from any source (emulator, hardware dump, etc.)
- Save automatically on exit

### 🎨 Color Mode System
Matches picowalker-core's `pw_color_mode` (0–3):
| Mode | Name | Description |
|------|------|-------------|
| 0 | Grayscale | Original PokéWalker LCD look |
| 1 | High Contrast | Enhanced contrast for outdoor use |
| 2 | Sepia | Warm vintage tone |
| 3 | Full Color | picowalker AMOLED-style colors |

### ⚡ Full H8/300H CPU Emulation
- All instruction groups: MOV, ADD, SUB, AND, OR, XOR, shifts, branches, MULXS, DIVXS
- Timer W (audio PWM), Timer B1, SCI3 (IrDA serial), EEPROM (64KB SPI)
- ROM hooks for factory test skip, battery check skip, input injection
- Compatible with original `pwflash.rom` firmware

---

## Requirements

- **iPhone or iPad** running iOS 26 or later
- **PokéWalker ROM** (`pwflash.rom`) — bundled in the app
- **Apple Health** access for step counting (optional but recommended)
- **pkwbridge** on 3DS for WiFi transfer (optional)

---

## Setup Guide

### 1. Install the App

**Via sideloading (AltStore/SideStore):**
1. Download the `.ipa` file from the [Releases](../../releases) page
2. Open in AltStore or SideStore
3. The app will install on your device

### 2. First Launch

1. Open PokeStride
2. **Grant Health access** when prompted — this enables real step counting
3. The emulator starts automatically with the bundled ROM
4. You'll see the PokéWalker splash screen on the LCD

### 3. Count Steps

- Walk around with your phone in your pocket or hand
- Steps are read from Apple Health every second
- The LCD updates in real time showing your PokéWalker screen
- Watts accumulate at 20 steps per watt

### 4. Transfer to 3DS (pkwbridge)

**On your 3DS:**
1. Install [pkwbridge](https://github.com/your-repo/pkwbridge) `.cia` or `.3dsx`
2. Load your HGSS `.sav` and `pweep.rom`
3. From the main menu, select **"WiFi server (iOS connect)"**
4. Note the IP address shown on the 3DS screen

**On your iPhone:**
1. Open PokeStride
2. Tap the **network icon** (🔗) in the header
3. Enter the 3DS IP address and port (default: 8000)
4. Tap **Connect**
5. Choose **Send EEPROM** or **Receive EEPROM**
6. The transfer completes automatically

### 5. Import EEPROM via Files App

1. Connect your iPhone to a computer or use iCloud Drive
2. Open the **Files** app
3. Navigate to **On My iPhone → PokeStride**
4. Copy your `pweep.rom` (64KB) into this folder
5. Open PokeStride — it will load the EEPROM automatically

### 6. Save Your Progress

- The app **auto-saves** when you close it
- You can also tap **Save** manually at any time
- The EEPROM is saved to `Documents/pweep.rom`
- Export this file to back up your walker data

---

## Color Modes

Toggle between 4 display modes by tapping the **palette button** in the stats bar:

- **Grayscale** (mode 0) — matches the original PokéWalker's 4-level grayscale LCD
- **High Contrast** (mode 1) — pure black/white for better outdoor visibility
- **Sepia** (mode 2) — warm tones like a vintage LCD
- **Full Color** (mode 3) — picowalker's AMOLED-style color palette

The selected mode is saved to EEPROM and persists across sessions.

---

## File Locations

| File | Location | Description |
|------|----------|-------------|
| ROM | `Resources/Data/pwflash.rom` | PokéWalker firmware (bundled) |
| EEPROM | `Documents/pweep.rom` | Walker save data (user-managed) |
| PokéWalker sprites | `Resources/Data/` | Font, icons, route graphics |

---

## Technical Details

### Architecture

```
┌─────────────────────────────────────────────┐
│  SwiftUI App                                │
│  ├── ContentView (LCD + buttons)            │
│  ├── SettingsView (color mode, audio)       │
│  └── NetworkView (WiFi transfer)            │
├─────────────────────────────────────────────┤
│  Emulator Core (C)                          │
│  ├── H8/300H CPU (all instruction groups)   │
│  ├── Timer W (audio PWM)                    │
│  ├── SCI3 (IrDA serial)                     │
│  ├── EEPROM (64KB SPI)                      │
│  └── LCD (96×64 2bpp)                       │
├─────────────────────────────────────────────┤
│  Swift Bridge                               │
│  ├── AVAudioEngine (real-time audio)        │
│  ├── CMPedometer (step counting)            │
│  ├── HealthKit (background delivery)        │
│  └── ColoredSprites (4bpp+BGR555 palette)   │
└─────────────────────────────────────────────┘
```

### WiFi Transfer Protocol

The iOS app connects to pkwbridge on the 3DS via HTTP:

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/transfer/info` | GET | Server info (device, version) |
| `/transfer/upload` | POST | Upload EEPROM to 3DS |
| `/transfer/download` | GET | Download EEPROM from 3DS |

Protocol mirrors PKSM's WirelessTransfer: simple HTTP REST on port 8000.

### HealthKit Integration

- Uses `CMPedometer` for real-time step updates
- Steps are injected into walker RAM at offset `0xF79C` (today) and `0xF780` (lifetime)
- Watts calculated as `steps ÷ 20` (clamped to 9999)
- Background step counting continues when the app is closed

---

## Building from Source

### Prerequisites
- Xcode 27+ (iOS 26 SDK)
- Swift 6+

### Build
```bash
cd PokeStride-iOS
open PokeStride.xcodeproj
# Select your device/target and hit Build (⌘B)
```

### Build IPA
```bash
xcodebuild build \
  -project PokeStride.xcodeproj \
  -scheme PokeStride \
  -destination "generic/platform=iOS" \
  -configuration Release \
  CODE_SIGNING_ALLOWED=NO \
  ONLY_ACTIVE_ARCH=NO

# Create IPA from build output
mkdir -p Payload
cp -r build/DerivedData/Build/Products/Release-iphoneos/PokeStride.app Payload/
zip -r PokeStride.ipa Payload/
```

### CI/CD
GitHub Actions workflow (`.github/workflows/build.yml`) builds both simulator and device targets, then exports an unsigned IPA.

---

## Building pkwbridge for 3DS

pkwbridge is a 3DS homebrew app that manages the EEPROM ↔ HGSS save bridge. The WiFi server is built-in.

### Prerequisites
- [devkitARM](https://devkitpro.org/) (3DS development toolchain)
- libctru, citro2d, citro3d

### Build
```bash
cd pkwbridge
make        # Builds .3dsx (homebrew launcher)
make cia    # Builds .cia (installable)
```

### Install
- **3DSX**: Copy `pkwbridge.3dsx` to your SD card's `/3ds/` folder
- **CIA**: Install `pkwbridge.cia` via FBI or similar CIA manager

---

## Credits

- **pokestride** — Original 3DS PokéWalker emulator by edgarburgues
- **picowalker-core** — Open-source PokéWalker firmware reimplementation
- **pkwbridge** — HGSS ↔ PokéWalker save bridge
- **PKSM** — Wireless transfer protocol reference
- **PokéWalker** by Nintendo — The original hardware that inspired this project

---

## License

This project is licensed under the GNU General Public License v3.0 — see the [LICENSE](LICENSE) file for details.

Pokémon and PokéWalker are trademarks of Nintendo/Game Freak. This project is not affiliated with Nintendo.
