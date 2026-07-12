## [LRN-20260712-001] correction

**Logged**: 2026-07-12T00:00:00+08:00
**Priority**: medium
**Status**: resolved
**Area**: config

### Summary
Building a Swift Package executable alone does not meet the macOS distribution requirement; package it as a drag-installable `.app` bundle.

### Details
The user requested a Mac app suitable for dragging into Applications. `swift build -c release` correctly produces an arm64 executable, but SwiftPM does not create an application bundle automatically.

### Suggested Action
Provide a reproducible packaging script that builds the release binary, embeds its SwiftPM resource bundle, creates `Contents/Info.plist`, and ad-hoc signs the `.app`.

### Metadata
- Source: user_feedback
- Related Files: mac-app/Package.swift
- Tags: macos, packaging, swiftpm

### Resolution
- **Resolved**: 2026-07-12T00:00:00+08:00
- **Notes**: Added an app-bundle packaging script and Info.plist template.

---

## [LRN-20260712-003] correction

**Logged**: 2026-07-12T00:00:00+08:00
**Priority**: high
**Status**: resolved
**Area**: frontend

### Summary
Do not byte-swap sprite rows twice on the ESP32 TFT_eSPI path.

### Details
`tools/convert_sprites.py` already emits RGB565 values in the byte-pre-swapped
format required by the default ESP32 `pushImage()` path. Applying a second
swap while drawing changes yellow artwork into blue/purple-looking colors.

### Suggested Action
Keep raw sprite/GIF/music rows unchanged at draw time; use `swap565()` only
when constructing firmware-side color constants.

### Metadata
- Source: user_feedback
- Related Files: firmware/src/main.cpp, tools/convert_sprites.py
- Tags: esp32, tft, rgb565, byte-order

### Resolution
- **Resolved**: 2026-07-12T00:00:00+08:00
- **Notes**: Removed the second swap from all raw pixel draw paths and flashed the M5.

---

## [LRN-20260712-002] correction

**Logged**: 2026-07-12T00:00:00+08:00
**Priority**: high
**Status**: resolved
**Area**: config

### Summary
ESP32 LittleFS must be initialized/formatted on first boot; silently ignoring a mount failure makes every persisted setting appear to save only until reboot.

### Details
The M5 API showed its bridge, quota preference, and mode reset after restart. The custom 16MB partition is blank after flashing, while firmware called `LittleFS.begin()` without checking its result. File writes consequently failed and settings only lived in RAM.

### Suggested Action
Use `LittleFS.begin(true)` at startup to format only an unmountable/blank volume, log the mount result, then load persisted settings.

### Metadata
- Source: user_feedback
- Related Files: firmware/src/main.cpp, firmware/partitions_m5stack_16mb.csv
- Tags: esp32, littlefs, persistence

### Resolution
- **Resolved**: 2026-07-12T00:00:00+08:00
- **Notes**: Added first-boot LittleFS formatting and will verify persistence across a hardware reset.

---
