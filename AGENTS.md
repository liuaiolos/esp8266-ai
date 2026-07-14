# Repository Guidelines

## Project Structure & Module Organization

- `firmware/` contains the M5Stack Core ESP32 Arduino/PlatformIO firmware. Main display, networking, and device HTTP endpoints live in `firmware/src/main.cpp`; bundled sprites and headers are in `firmware/include/`.
- `mac-app/` is the native macOS menu-bar bridge, built with Swift Package Manager. Sources are in `Sources/AIClockBridge/`, resources in `Resources/`, and XCTest tests in `Tests/AIClockBridgeTests/`.
- `windows-app/AIClockBridge/` is the .NET 8 WinForms bridge. Keep behavior and the device protocol aligned with the macOS implementation.
- `tools/` holds asset-conversion utilities. `docs/` and the bilingual READMEs document hardware and user workflows.

## Build, Test, and Development Commands

Run commands from the indicated module directory:

```sh
cd firmware && pio run -e m5stack-core-esp32     # build ESP32 firmware
cd firmware && pio run -e m5stack-core-esp32 -t upload --upload-port /dev/cu.usbserial-…
cd mac-app && swift run                           # run the macOS bridge
cd mac-app && swift test                          # run XCTest suite
cd mac-app && ./scripts/package-macos-app.sh release  # create dist/*.app
cd windows-app && dotnet build AIClockBridge/AIClockBridge.csproj
```

The firmware target uses 16 MB flash and LittleFS; preserve its partition and upload settings unless the hardware changes.

## Coding Style & Naming Conventions

Follow existing local style: four-space indentation in Swift and C#, two-space indentation in C++ firmware code, and braces on the declaration line. Use `camelCase` for Swift/C++ variables and methods; C# public members use `PascalCase`. Keep platform-parity changes deliberate: update macOS and Windows counterparts when their behavior or API interpretation must match. Do not hand-edit generated sprite headers; use the relevant script in `tools/`.

## Testing Guidelines

Add focused XCTest cases under `mac-app/Tests/AIClockBridgeTests/` using `*Tests.swift` filenames and descriptive test methods. Run `swift test` for macOS logic changes. For firmware changes, run PlatformIO build before flashing; test device APIs and display behavior on hardware when endpoints or rendering change.

## Commit & Pull Request Guidelines

Recent history uses concise Chinese summaries such as `优化：动画衔接优化`, plus scoped fixes such as `fix(mac): prevent network counter overflow crash`. Use an imperative, scoped subject when useful. PRs should explain the affected platform(s), include build/test commands run, link related issues, and attach screenshots or device photos for visual changes.

## Security & Configuration

Never commit CLI credentials, Wi-Fi settings, or local IP addresses. The bridge listens on LAN port 8765; test only on a trusted network and keep user-specific settings outside the repository.
