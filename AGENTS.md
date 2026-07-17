# Repository Guidelines

## Project Structure & Module Organization

- `firmware/` contains the M5GO v2.6 / M5Stack Core ESP32 Arduino/PlatformIO firmware. Main display, networking, and device HTTP endpoints live in `firmware/src/main.cpp`; bundled sprites and headers are in `firmware/include/`. See `firmware/README-M5STACK.md` for the supported hardware and flash layout.
- `mac-app/` is the native macOS menu-bar bridge, built with Swift Package Manager. Sources are in `Sources/AIClockBridge/`, resources in `Resources/`, and XCTest tests in `Tests/AIClockBridgeTests/`.
- `windows-app/AIClockBridge/` is the .NET 8 WinForms bridge. Keep behavior and the device protocol aligned with the macOS implementation.
- `tools/` holds asset-conversion utilities. Root `scripts/` contains maintained user-installable helper scripts; `scripts/codex-status-hook.sh` is installed outside the checkout at `~/.ai-clock/` and posts Codex lifecycle events to the local bridge.
- `docs/` and the bilingual root READMEs document hardware and user workflows. Keep `README.md` and `README.en.md` in sync; update `windows-app/README.md` when Windows behavior changes, and update `docs/DEVELOPMENT.md` for firmware architecture or API changes.

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

## Scripts and Documentation

- Keep shell scripts POSIX `sh` unless a platform-specific shell is required. Check shell syntax with `sh -n` and commit executable scripts with mode `755`.
- Do not embed a maintained script's implementation in a README. Link to its version-controlled source and document a copy/install command instead.
- Hook scripts must consume Codex's JSON stdin, return quickly, and never make a CLI session fail because the local bridge is unavailable. Do not put user paths, credentials, Wi-Fi settings, or device IP addresses in version-controlled hook configuration.

## Testing Guidelines

Add focused XCTest cases under `mac-app/Tests/AIClockBridgeTests/` using `*Tests.swift` filenames and descriptive test methods. Run `swift test` for macOS logic changes. For firmware changes, run PlatformIO build before flashing; test device APIs and display behavior on hardware when endpoints or rendering change.

## Commit & Pull Request Guidelines

Recent history uses concise Chinese summaries such as `优化：动画衔接优化`, plus scoped fixes such as `fix(mac): prevent network counter overflow crash`. Use an imperative, scoped subject when useful. PRs should explain the affected platform(s), include build/test commands run, link related issues, and attach screenshots or device photos for visual changes.

## Security & Configuration

Never commit CLI credentials, Wi-Fi settings, or local IP addresses. The bridge listens on LAN port 8765; test only on a trusted network and keep user-specific settings outside the repository. The hook endpoint is loopback-only (`127.0.0.1:8765/event`), while the bridge itself serves the device on the trusted LAN.
