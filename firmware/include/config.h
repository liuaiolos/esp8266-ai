#pragma once

// ---- Firmware version (shown on the first-time WiFi setup screen & /api/info) ----
#define FW_VERSION "0.5.3-m5go-v27"

// ---- Bridge polling ----
#define BRIDGE_DEFAULT_PORT 8765
#define BRIDGE_DEFAULT_PATH "/status"
#define BRIDGE_POLL_INTERVAL_MS 5000
#define BRIDGE_HTTP_TIMEOUT_MS 3000

// ---- WiFiManager ----
#define WIFI_PORTAL_AP_NAME "AI-Clock-Setup"
#define WIFI_CONFIG_FILE "/bridge_host.txt"

// ---- Backlight ----
#define BRIGHTNESS_FILE "/brightness.txt"
#define QUOTA_DISPLAY_FILE "/quota_display.txt"
#define DISPLAY_MODE_FILE "/display_mode.txt"
#define BRIGHTNESS_DEFAULT 100
#define BRIGHTNESS_PWM_FREQ 2000 // Hz; high enough to avoid visible flicker when dim
#define SPEAKER_PIN 25
#define COMPLETION_SOUND_VOLUME_PERCENT 42

// ---- Display layout (M5GO v2.7: 320x240 ILI9342C, landscape) ----
#define SCREEN_W 320
#define SCREEN_H 240
