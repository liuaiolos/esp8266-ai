<p align="center">
  <img src="docs/images/logo.svg" width="72" alt="logo">
</p>

<h1 align="center">AI Mac Mini Display</h1>

<p align="center">A tiny AI status computer for your desk — ESP32-S3 · Open Source Hardware · Desktop Companion</p>

<p align="center">
  <a href="README.md">中文</a> ·
  English
</p>

<p align="center">
  <a href="https://mac.qust.me">Website</a> ·
  <a href="https://github.com/pengchujin/esp8266-ai/releases/latest">Download</a>
</p>

<p align="center">
  <img src="docs/images/hero.jpg" width="640" alt="AI Mac Mini Display">
</p>

A retro mini-TV with a 240×240 screen that sits on your desk showing **what Claude Code / Codex CLI are doing right now and how much quota you have left**. No API key needed: everything comes from the CLI credentials and session logs already on your machine, served to the device over your LAN by the companion Mac / Windows bridge app.

## Features

| | |
|---|---|
| <img src="docs/images/feature1.jpg" width="360" alt="AI status"> | **AI status & quota**<br>Pet is walking = the AI is working. A square progress ring plus large digits show your real session / weekly quota usage; Codex accounts that expose only a weekly limit automatically show that limit alone. When a window is used up the pet becomes a reset countdown, and the border flashes red when the AI is waiting for your approval. |
| <img src="docs/images/feature2.jpg" width="360" alt="Network monitor"> | **Live network monitor**<br>Task-manager-style upload/download curves, 56-second rolling window, auto-scaling axis. |
| <img src="docs/images/music.jpg" width="360" alt="Now playing"> | **Now playing**<br>Album art, title, artist and progress bar in real time; switches in automatically when music starts, back when it stops. |
| <img src="docs/images/feature3.jpg" width="360" alt="Swappable pets"> | **Swappable pets**<br>Built-in [petdex.dev](https://petdex.dev) gallery with 3300+ open-source pets, or upload any GIF — decoded on the board itself, no reflashing needed. |

## Getting started

What you need: a **XiaoZhi-compatible Zhengchen 1.54-inch Wi-Fi board** (ESP32-S3 N16R8, 16 MB flash, 8 MB OPI PSRAM and a 240×240 ST7789 display) and a USB **data** cable. This firmware does not support the older ESP8266 SD2 mini-TV, M5Stack Core, Core2 or M5Stick.

### Step 1 · Build and flash the firmware

Install PlatformIO, then build and flash from the repository root:

```sh
cd firmware
pio run -e xiaozhi-s3-lcd154
pio run -e xiaozhi-s3-lcd154 -t upload --upload-port /dev/cu.usbmodem…
```

Without `--upload-port`, PlatformIO tries to detect the serial port automatically. The partition, LittleFS, OPI PSRAM and ST7789 settings are already in `firmware/platformio.ini`; see [firmware/README-XIAOZHI-S3-LCD154.md](firmware/README-XIAOZHI-S3-LCD154.md) for hardware and completion-sound details.

### Step 2 · Connect WiFi

On first boot the device opens a hotspot named **`AI-Clock-Setup`**: join it from your phone and the setup page pops up (or browse to `192.168.4.1`), pick your WiFi and enter the password. Done.

### Step 3 · Install the bridge app

Download from [Releases](https://github.com/pengchujin/esp8266-ai/releases/latest) and open:

- **macOS**: `AIClockBridge-*-macOS.dmg`, drag into Applications (ad-hoc signed; on first launch allow it in "System Settings → Privacy & Security" and grant local-network access)
- **Windows**: `AIClockBridge-*-Windows-x64.exe`, just double-click

The bridge lives in your menu bar / tray and **auto-discovers and pairs** with the device on the same LAN — at this point the screen comes alive.

<p align="center">
  <img src="docs/images/working.jpg" width="640" alt="In action">
</p>

Daily use is all on the tray icon: **left-click** opens a live mirror of the device screen (with a brightness slider at the bottom), **right-click** opens the full menu (quota details, screen switching, pet swapping, music/network pages, and more).

### Optional USB wired connection

If the Wi-Fi network isolates clients, or you do not want to configure Wi-Fi yet, keep the macOS bridge running and connect the clock by USB data cable. When it detects a compatible serial port, the bridge pushes status and network data to the device. Quit the bridge before flashing firmware so two programs do not compete for the serial port.

## FAQ

- **Screen border flashing red**: the device can't reach the bridge — make sure the app is running and on the same WiFi.
- **Quota shows `-` forever**: no Claude Code / Codex CLI login on this machine, so the bridge has no credentials to read.
- **Want a different pet**: right-click the tray icon → "Change pet animation…", pick one and upload.

### Codex remains working after it has stopped

The bridge scans `~/.codex/sessions` as a compatibility fallback, but a final transcript write does not mean that Codex is still running. Configure Codex hooks to post lifecycle events directly: the bridge treats those events as authoritative, while retaining log scanning for sessions without hooks.

The version-controlled script is [`scripts/codex-status-hook.sh`](scripts/codex-status-hook.sh). From the root of a cloned repository, install it to a stable location outside the repository:

```sh
mkdir -p ~/.ai-clock
install -m 755 scripts/codex-status-hook.sh ~/.ai-clock/codex-status-hook.sh
```

If your system does not have `install`, use:

```sh
mkdir -p ~/.ai-clock
cp scripts/codex-status-hook.sh ~/.ai-clock/codex-status-hook.sh
chmod 755 ~/.ai-clock/codex-status-hook.sh
```

In `~/.codex/hooks.json`, add one command hook for each of `SessionStart`, `UserPromptSubmit`, `PreToolUse`, `PermissionRequest`, `PostToolUse`, and `Stop`. Their commands are:

```text
~/.ai-clock/codex-status-hook.sh <event name>
```

For example, retain existing hooks and add this `Stop` entry:

```json
"Stop": [{
  "hooks": [{ "type": "command", "command": "~/.ai-clock/codex-status-hook.sh Stop", "timeout": 2 }]
}]
```

Approve the hook in Codex if prompted. `PermissionRequest` stops the working animation and raises the red attention alert; `Stop` ends the animation immediately instead of being overwritten by Codex's final JSONL write.

Ensure `[features]` in `~/.codex/config.toml` includes `hooks = true`; if Codex asks you to trust the new hook, run `/hooks` in the TUI and approve it. After updating the repository, rerun the install command above to update the installed script.

## Development

```
firmware/     XiaoZhi-compatible ESP32-S3 1.54-inch display firmware (PlatformIO + Arduino, with on-board GIF decoding)
mac-app/      macOS menu bar bridge (Swift/SPM, zero third-party dependencies)
windows-app/  Windows tray bridge (C# / .NET 8 WinForms)
tools/        GIF → RGB565 built-in sprite conversion script
scripts/      Version-controlled user helper scripts (including the Codex status hook)
docs/         Developer docs (pinout, HTTP API, architecture details)
```

### Build, test, and package

Install PlatformIO, the Xcode/Swift toolchain, and the .NET 8 SDK for the Windows bridge. Run these from the repository root. If `pio` is not on `PATH`, use `~/.platformio/penv/bin/pio` instead.

```bash
# Firmware (XiaoZhi-compatible ESP32-S3 1.54-inch display)
cd firmware && pio run -e xiaozhi-s3-lcd154
cd firmware && pio run -e xiaozhi-s3-lcd154 -t upload --upload-port /dev/cu.usbmodem…

# macOS bridge: run, test, and create a Release app bundle
cd mac-app && swift run
cd mac-app && swift test
cd mac-app && ./scripts/package-macos-app.sh release  # dist/AI Clock Bridge.app

# Windows bridge: build, run, and publish
cd windows-app && dotnet build AIClockBridge/AIClockBridge.csproj
cd windows-app && dotnet run --project AIClockBridge/AIClockBridge.csproj
cd windows-app && dotnet publish AIClockBridge/AIClockBridge.csproj -c Release -r win-x64 --self-contained false
```

Hardware pinout, display-driver gotchas, the device HTTP API and the on-board GIF decoding architecture are documented in **[docs/DEVELOPMENT.md](docs/DEVELOPMENT.md)** (Chinese).

Hardware, firmware and software are all open source — modify it, build it, even sell it.
