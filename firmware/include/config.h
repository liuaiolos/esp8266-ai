#pragma once

// ---- Firmware version (shown on the first-time WiFi setup screen & /api/info) ----
#if defined(XIAOZHI_S3_LCD154)
#define FW_VERSION "0.5.5-xiaozhi-lcd154"
#elif defined(T_EMBED_CC1101)
#define FW_VERSION "0.5.5-t-embed-cc1101"
#else
#define FW_VERSION "0.5.5-m5go-v26"
#endif

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
#define COMPLETION_VOLUME_FILE "/completion_volume.txt"
#define QUOTA_DISPLAY_FILE "/quota_display.txt"
#define DISPLAY_MODE_FILE "/display_mode.txt"
#define BRIGHTNESS_DEFAULT 100
#define BRIGHTNESS_PWM_FREQ 2000 // Hz; high enough to avoid visible flicker when dim
#if defined(T_EMBED_CC1101)
// T-Embed's speaker is driven by an I2S amplifier, not the ESP32 DAC used by
// the M5GO. The current completion PCM path is disabled on this target until
// an I2S playback implementation is added.
#define SPEAKER_PIN 7
#elif defined(XIAOZHI_S3_LCD154)
// Zhengchen 1.54 TFT Wi-Fi has a raw I2S speaker amplifier.
#define SPEAKER_I2S_DOUT 7
#define SPEAKER_I2S_BCLK 15
#define SPEAKER_I2S_LRCK 16
#else
#define SPEAKER_PIN 25
#endif
#define COMPLETION_SOUND_VOLUME_DEFAULT 72

// ---- Display layout ----
#if defined(XIAOZHI_S3_LCD154)
// XiaoZhi AI voice box: 1.54-inch ST7789 square panel.
#define SCREEN_W 240
#define SCREEN_H 240
#elif defined(T_EMBED_CC1101)
// T-Embed's ST7789 panel is physically upright (170x320). Keeping that
// orientation gives the status view enough room for the sprite and quotas.
#define SCREEN_W 170
#define SCREEN_H 320
#else
// M5GO v2.6: 320x240 ILI9342C, landscape.
#define SCREEN_W 320
#define SCREEN_H 240
#endif
