// M5GO v2.6 ESP32 WiFi clock: shows local time plus live Claude Code / Codex CLI
// working status and usage quota, polled from a small bridge service that
// runs on the developer's Mac (see ../bridge/bridge.py).
//
// Display: 320x240 SPI ILI9342C. TFT_eSPI supplies the M5Stack transport and
// coordinate model; the panel-specific init sequence below follows M5Stack's
// official M5GO v2.6/Core implementation. Pins are set in platformio.ini.

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiManager.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <AnimatedGIF.h>
#include "esp32-hal-dac.h"

#include "config.h"
#include "audio/construction_complete_pcm.h"
#include "img/claude_sprite.h"
#include "img/codex_sprite.h"
#include "img/claude_logo.h"
#include "img/codex_logo.h"

TFT_eSPI tft = TFT_eSPI();
WebServer webServer(80);

// M5GO v2.6 uses an ILI9342C panel. M5Stack's official Core library retains
// the ILI9341 coordinate model but selects this different register sequence
// for newer LCD hardware. Applying it after TFT_eSPI::init() lets the rest of
// this firmware keep the proven 320x240 rotation and drawing coordinates.
static void initM5GoV26Panel() {
  auto command = [](uint8_t value) { tft.writecommand(value); };
  auto data = [](uint8_t value) { tft.writedata(value); };

  command(0xC8);
  data(0xFF); data(0x93); data(0x42);

  command(0xC0); // Power control 1
  data(0x12); data(0x12);
  command(0xC1); // Power control 2
  data(0x03);
  command(0xB0);
  data(0xE0);
  command(0xF6);
  data(0x00); data(0x01); data(0x01);

  command(0x36); // Memory access control; M5Stack rotation 0 baseline
  data(0xA8);    // MY | MV, with TFT_BGR color order
  command(0x3A); // 16-bit RGB565
  data(0x55);
  command(0xB6); // Display function control
  data(0x08); data(0x82); data(0x27);

  command(0xE0); // Positive gamma
  const uint8_t positiveGamma[] = {
      0x00, 0x0C, 0x11, 0x04, 0x11, 0x08, 0x37, 0x89,
      0x4C, 0x06, 0x0C, 0x0A, 0x2E, 0x34, 0x0F};
  for (uint8_t value : positiveGamma) data(value);

  command(0xE1); // Negative gamma
  const uint8_t negativeGamma[] = {
      0x00, 0x0B, 0x11, 0x05, 0x13, 0x09, 0x33, 0x67,
      0x48, 0x07, 0x0E, 0x0B, 0x2E, 0x33, 0x0F};
  for (uint8_t value : negativeGamma) data(value);

  command(0x11); // Sleep out
  delay(120);
  command(0x29); // Display on
  tft.invertDisplay(true); // Official M5Stack ILI9342C path enables INVON
}

// ---------- custom sprite storage (LittleFS) ----------
// Custom uploads replace the compiled-in default animation without needing a
// firmware rebuild. You POST a raw .gif straight to /sprite/claude or
// /sprite/codex (the device serves its own upload page at "/"); the ESP8266
// decodes and rescales the GIF *on-device* (AnimatedGIF, line-by-line so it
// never needs a full-canvas buffer) into the wire format below, which the
// display path then reads back frame-by-frame:
//   [1 byte frame count][frame0 bytes][frame1 bytes]...
// Each frame is exactly CLAUDE_SPRITE_W x H (or CODEX_SPRITE_W x H) RGB565
// pixels, byte order matching tools/convert_sprites.py's to_rgb565() so the
// compiled-in defaults and custom uploads share one draw path.
const char *CLAUDE_SPRITE_FILE = "/c.bin";
const char *CODEX_SPRITE_FILE = "/x.bin";
const char *CLAUDE_IDLE_FILE = "/c_idle.bin";
const char *CODEX_IDLE_FILE = "/x_idle.bin";
const char *CLAUDE_GIF_FILE = "/c.gif"; // raw upload, decoded then removed
const char *CODEX_GIF_FILE = "/x.gif";
const int MAX_CUSTOM_FRAMES = 8;
const size_t CLAUDE_FRAME_BYTES = (size_t)CLAUDE_SPRITE_W * CLAUDE_SPRITE_H * 2;
const size_t CODEX_FRAME_BYTES = (size_t)CODEX_SPRITE_W * CODEX_SPRITE_H * 2;

// We never hold a whole sprite frame in RAM. Decoding a GIF needs ~24KB of
// heap for AnimatedGIF's own buffers, which wouldn't fit alongside a static
// full-frame buffer (a 120x120 frame is ~28KB) on the ESP8266's ~80KB. So both
// the display path and the decoder work one screen-row at a time through these
// two small scratch rows (SCREEN_W is the widest we ever need).
uint16_t rowBuf[SCREEN_W];     // current row being drawn / decoded
uint16_t prevRowBuf[SCREEN_W]; // decode only: same row from the previous frame
// ESP32 has enough heap for one decoded custom frame. Caching the active frame
// avoids opening/reading LittleFS once per scanline, which otherwise starves
// the HTTP server during the animation.
uint8_t spriteFrameCache[CODEX_FRAME_BYTES];
// A second frame buffer is used only at the loop seam to blend the final and
// first animation frames. It costs 28.8 KB on this ESP32 but removes the
// visible last-frame -> first-frame snap in non-seamless uploaded GIFs.
uint8_t spriteLoopBridgeCache[CODEX_FRAME_BYTES];

// Sprite/GIF/music payloads are stored in the byte-pre-swapped format emitted
// by tools/convert_sprites.py. TFT_eSPI's default ESP32 pushImage() path sends
// the uint16_t memory bytes as-is, so these rows must be passed unchanged.
inline uint16_t swap565(uint16_t c) { return (uint16_t)((c << 8) | (c >> 8)); }

bool claudeCustom = false;
int claudeCustomFrames = 0;
bool codexCustom = false;
int codexCustomFrames = 0;
bool claudeIdleCustom = false;
int claudeIdleFrames = 0;
bool codexIdleCustom = false;
int codexIdleFrames = 0;
uint32_t spriteRev = 0; // bumped on upload/reset so the Mac mirror re-fetches

const int SCREEN_CX = SCREEN_W / 2, SCREEN_CY = SCREEN_H / 2;
const int RING_MARGIN = 4;      // inset from screen edge
const int RING_THICKNESS = 10;  // ring bar thickness
const unsigned long ANIM_INTERVAL_MS = 90;   // sprite frame advance
const unsigned long ANIM_LOOP_BRIDGE_MS = 45; // short final->first blend
const unsigned long FLASH_INTERVAL_MS = 400; // "urgent" flash speed
const unsigned long SWITCH_BOTH_MS = 2000;   // both apps working: alternate fast
const unsigned long SWITCH_IDLE_MS = 6000;   // neither working: alternate slow

enum ActiveApp { APP_CLAUDE, APP_CODEX };
// First boot should introduce the device with Codex on screen.  A previously
// saved display-mode preference still takes precedence in loadDisplayMode().
ActiveApp currentApp = APP_CODEX;
unsigned long lastSwitchMs = 0;

// Display override, settable from the Mac app via POST /api/display:
// auto = follow working status, claude/codex = pin that app on screen,
// net/music = show Mac-side telemetry pages instead of the pet.
enum DisplayMode { MODE_AUTO, MODE_CLAUDE, MODE_CODEX, MODE_NET, MODE_MUSIC };
DisplayMode displayMode = MODE_AUTO;
bool appRedrawRequested = false; // defer expensive TFT work until after HTTP replies

// When AUTO and the Mac reports audio playing, the screen auto-switches to the
// music page and back when it stops — same spirit as the Claude/Codex auto
// switch. Only AUTO does this; a pinned mode is always honored as-is.
bool statusMusicPlaying = false;
DisplayMode lastEffectiveMode = MODE_AUTO;

// ---------- net speed mode state ----------
// Rendering is decoupled from the network: pollNet() fetches every 2s and
// only refills a queue of 250ms samples (the bridge samples at 4Hz and tags
// them with a running seq, so nothing is drawn twice or skipped). The sweep
// itself consumes exactly one queued sample every NET_DRAW_INTERVAL_MS, so
// the trace advances at a constant rate no matter how long HTTP takes.
const unsigned long NET_POLL_INTERVAL_MS = 2000; // queue refill cadence
const unsigned long NET_DRAW_INTERVAL_MS = 250;  // one chart step per bridge sample
const int NET_QUEUE = 32;
long netQRx[NET_QUEUE], netQTx[NET_QUEUE]; // ring buffer of pending samples
int netQHead = 0, netQCount = 0;
long netSeq = -1;                          // last bridge sample seq consumed into the queue
long netCurRx = 0, netCurTx = 0;           // smoothed readout for the header
unsigned long lastNetPollMs = 0;
unsigned long lastNetDrawMs = 0;
bool netChromeDrawn = false;
bool netHeaderDirty = false;

// Chart layout (task-manager style scrolling area chart, newest at the right)
const int NET_CHART_X = 8, NET_CHART_Y = 60, NET_CHART_W = 224, NET_CHART_H = 128;
long netHistRx[NET_CHART_W], netHistTx[NET_CHART_W]; // one 250ms sample per column
long netScale = 10240;    // current "nice" full-scale value (whole chart shares it)
String netLastDl, netLastUl, netLastScaleText; // change detection for partial redraws

// ---------- music mode state ----------
const int MUSIC_COVER_W = 128;
const int MUSIC_COVER_H = 128;
// Title/artist come as a Mac-rendered bitmap strip (232x44) because the
// panel fonts are ASCII-only and CJK titles would render as blanks.
const int MUSIC_TEXT_W = 232;
const int MUSIC_TEXT_H = 44;
const int MUSIC_TEXT_X = 4, MUSIC_TEXT_Y = 150;
const unsigned long MUSIC_POLL_INTERVAL_MS = 2000;
String musicTitle, musicArtist, musicAlbum;
bool musicPlaying = false;
int musicElapsed = 0, musicDuration = 0;
int musicArtworkRev = -1;
int musicTextRev = -1;
bool musicHasArtwork = false;
bool musicChromeDrawn = false;
unsigned long lastMusicPollMs = 0;

int claudeFrame = 0;
int codexFrame = 0;
bool claudeLoopBridgePending = false;
bool codexLoopBridgePending = false;
unsigned long lastAnimMs = 0;

bool flashOn = true;
unsigned long lastFlashMs = 0;

// Bridge host is not asked for during first-time WiFi setup: the Mac/Windows
// bridge discovers the device and pairs automatically (or set via /api/bridge).
String bridgeHost;

struct ClaudeStatus {
  String status = "unknown";
  long tokensToday = 0;
  int sessionMin = 0;
  int sessionWindowMin = 300;
  float fiveHourPct = -1; // real OAuth quota from the bridge, -1 = unknown
  int fiveHourResetMin = -1; // minutes until the 5h window resets
  float sevenDayPct = -1;
  int sevenDayResetMin = -1; // minutes until the 7-day window resets
  bool needsInput = false; // waiting on a permission/approval prompt
};

struct CodexStatus {
  String status = "unknown";
  long tokensToday = 0;
  float primaryPct = -1;
  int primaryResetMin = -1;
  float weeklyPct = -1;
  int weeklyResetMin = -1;
  bool needsInput = false;
};

ClaudeStatus claudeStatus;
CodexStatus codexStatus;

unsigned long lastPollMs = 0;
unsigned long lastSuccessMs = 0;
bool everPolled = false;
bool mainUiShown = false;      // false while the Wi-Fi configuration portal is visible
bool webServerStarted = false; // deferred because the portal also occupies port 80
uint8_t bridgeFailCount = 0;
unsigned long lastWiFiReconnectMs = 0;
bool statusBaselineReady = false;
unsigned long beepUntilMs = 0;
const int SPEAKER_CHANNEL = 1;

// The supplied WAV is downsampled at build time to unsigned 8-bit mono PCM
// (11,025 Hz) and compiled into flash. GPIO 25 is ESP32 DAC1 and feeds the
// M5GO's built-in speaker, so this needs no SD card or LittleFS upload.
constexpr uint32_t COMPLETION_SAMPLE_RATE = 11025;

void restoreSpeakerToneOutput() {
  dacWrite(SPEAKER_PIN, 0);
  ledcAttachPin(SPEAKER_PIN, SPEAKER_CHANNEL);
  ledcWriteTone(SPEAKER_CHANNEL, 0);
  ledcWrite(SPEAKER_CHANNEL, 0);
}

void playCompletionSound() {
  if (construction_complete_pcm_len == 0) return;

  // LEDC and the DAC cannot drive GPIO 25 at the same time.
  ledcWriteTone(SPEAKER_CHANNEL, 0);
  ledcWrite(SPEAKER_CHANNEL, 0);
  ledcDetachPin(SPEAKER_PIN);

  // Keep the real 11,025 Hz average interval (90 + fractional microseconds)
  // rather than relying on delayMicroseconds(), whose call overhead changes
  // the playback pitch.
  uint32_t nextSampleAt = micros();
  uint32_t fractionalUs = 0;
  constexpr uint32_t wholeUs = 1000000UL / COMPLETION_SAMPLE_RATE;
  constexpr uint32_t remainderUs = 1000000UL % COMPLETION_SAMPLE_RATE;
  for (size_t i = 0; i < construction_complete_pcm_len; ++i) {
    int sample = (int)pgm_read_byte(construction_complete_pcm + i) - 128;
    sample = sample * COMPLETION_SOUND_VOLUME_PERCENT / 100;
    dacWrite(SPEAKER_PIN, sample + 128);

    nextSampleAt += wholeUs;
    fractionalUs += remainderUs;
    if (fractionalUs >= COMPLETION_SAMPLE_RATE) {
      nextSampleAt++;
      fractionalUs -= COMPLETION_SAMPLE_RATE;
    }
    while ((int32_t)(micros() - nextSampleAt) < 0) {
      // Busy wait preserves the sample clock; Wi-Fi continues on the other core.
    }
  }
  restoreSpeakerToneOutput();
}

void triggerCompletionBeep() {
  if (construction_complete_pcm_len > 0) {
    playCompletionSound();
    return;
  }
  // Kept as a safe fallback if the asset is ever removed from a custom build.
  beepUntilMs = millis() + 120;
  ledcWriteTone(SPEAKER_CHANNEL, 520);
  ledcWrite(SPEAKER_CHANNEL, 24);
}

void updateBeep() {
  if (beepUntilMs != 0 && millis() >= beepUntilMs) {
    ledcWriteTone(SPEAKER_CHANNEL, 0);
    ledcWrite(SPEAKER_CHANNEL, 0);
    beepUntilMs = 0;
  }
}

// Quota values from the bridge are always "used" percentages. This preference
// only changes how they are rendered, not the exhausted-window countdown.
bool showQuotaRemaining = false;

// ---------- backlight brightness ----------
// The M5Stack Core panel backlight (TFT_BL, active HIGH) is PWM-dimmable.
// 0 = off, 100 = full. Persisted so it survives reboot.

int brightness = BRIGHTNESS_DEFAULT; // 0-100

void loadQuotaDisplay() {
  if (!LittleFS.exists(QUOTA_DISPLAY_FILE)) return;
  File f = LittleFS.open(QUOTA_DISPLAY_FILE, "r");
  if (!f) return;
  String value = f.readStringUntil('\n');
  value.trim();
  f.close();
  showQuotaRemaining = value == "remaining";
}

void saveQuotaDisplay() {
  File f = LittleFS.open(QUOTA_DISPLAY_FILE, "w");
  if (!f) return;
  f.println(showQuotaRemaining ? "remaining" : "used");
  f.close();
}

float displayedQuotaPct(float usedPct) {
  if (usedPct < 0) return usedPct;
  return showQuotaRemaining ? 100.0f - usedPct : usedPct;
}

void loadDisplayMode() {
  if (!LittleFS.exists(DISPLAY_MODE_FILE)) return;
  File f = LittleFS.open(DISPLAY_MODE_FILE, "r");
  if (!f) return;
  String value = f.readStringUntil('\n');
  value.trim();
  f.close();
  if (value == "claude") displayMode = MODE_CLAUDE;
  else if (value == "codex") displayMode = MODE_CODEX;
  else if (value == "net") displayMode = MODE_NET;
  else if (value == "music") displayMode = MODE_MUSIC;
  else displayMode = MODE_AUTO;
}

void saveDisplayMode() {
  const char *value = "auto";
  if (displayMode == MODE_CLAUDE) value = "claude";
  else if (displayMode == MODE_CODEX) value = "codex";
  else if (displayMode == MODE_NET) value = "net";
  else if (displayMode == MODE_MUSIC) value = "music";
  File f = LittleFS.open(DISPLAY_MODE_FILE, "w");
  if (!f) return;
  f.println(value);
  f.close();
}

void applyBrightness() {
  ledcWrite(0, map(brightness, 0, 100, 0, 255));
}

void loadBrightness() {
  if (!LittleFS.exists(BRIGHTNESS_FILE)) return;
  File f = LittleFS.open(BRIGHTNESS_FILE, "r");
  if (!f) return;
  int v = f.readStringUntil('\n').toInt();
  f.close();
  if (v >= 0 && v <= 100) brightness = v;
}

void saveBrightness() {
  File f = LittleFS.open(BRIGHTNESS_FILE, "w");
  if (!f) return;
  f.println(brightness);
  f.close();
}

// ---------- persistence for the bridge host ----------

void loadBridgeHost() {
  if (LittleFS.exists(WIFI_CONFIG_FILE)) {
    File f = LittleFS.open(WIFI_CONFIG_FILE, "r");
    bridgeHost = f.readStringUntil('\n');
    bridgeHost.trim();
    f.close();
  }
}

void saveBridgeHost(const String &host) {
  File f = LittleFS.open(WIFI_CONFIG_FILE, "w");
  f.println(host);
  f.close();
}

// ---------- custom sprite loading ----------

// Checks LittleFS for a previously-uploaded custom sprite and validates its
// size before trusting it (frame count byte + exact expected byte length).
void loadCustomSpriteState() {
  auto load = [](const char *path, size_t frameBytes, bool &ok, int &frames) {
    ok = false; frames = 0;
    File f = LittleFS.open(path, "r");
    if (f && f.size() >= 1) {
      uint8_t cnt = f.read();
      size_t expected = 1 + (size_t)cnt * frameBytes;
      if (cnt > 0 && cnt <= MAX_CUSTOM_FRAMES && (size_t)f.size() == expected) {
        ok = true; frames = cnt;
      }
    }
    if (f) f.close();
  };
  load(CLAUDE_SPRITE_FILE, CLAUDE_FRAME_BYTES, claudeCustom, claudeCustomFrames);
  load(CODEX_SPRITE_FILE, CODEX_FRAME_BYTES, codexCustom, codexCustomFrames);
  load(CLAUDE_IDLE_FILE, CLAUDE_FRAME_BYTES, claudeIdleCustom, claudeIdleFrames);
  load(CODEX_IDLE_FILE, CODEX_FRAME_BYTES, codexIdleCustom, codexIdleFrames);

  Serial.printf("[sprite] claude custom=%d frames=%d | codex custom=%d frames=%d\n", claudeCustom,
                claudeCustomFrames, codexCustom, codexCustomFrames);
}

int claudeFrameCount() { return claudeCustom ? claudeCustomFrames : CLAUDE_SPRITE_FRAMES; }
int codexFrameCount() { return codexCustom ? codexCustomFrames : CODEX_SPRITE_FRAMES; }
int claudeIdleFrameCount() { return claudeIdleCustom ? claudeIdleFrames : claudeFrameCount(); }
int codexIdleFrameCount() { return codexIdleCustom ? codexIdleFrames : codexFrameCount(); }

bool loadSpriteFrame(bool custom, const char *file, const uint16_t *const *progmemFrames, int frameIdx,
                     size_t frameBytes, uint8_t *out) {
  if (custom) {
    File f = LittleFS.open(file, "r");
    if (!f) return false;
    f.seek(1 + (size_t)frameIdx * frameBytes);
    bool ok = f.read(out, frameBytes) == (int)frameBytes;
    f.close();
    return ok;
  }
  const uint16_t *frame = progmemFrames[frameIdx];
  memcpy_P(out, frame, frameBytes);
  return true;
}

// Draws one sprite frame from a full-frame cache. A single pushImage call is
// substantially smoother than issuing one SPI transaction per scanline.
void drawSpriteFrame(bool custom, const char *file, const uint16_t *const *progmemFrames, int frameIdx, int w,
                     int h, size_t frameBytes) {
  if (!loadSpriteFrame(custom, file, progmemFrames, frameIdx, frameBytes, spriteFrameCache)) return;
  int x0 = SCREEN_CX - w / 2, y0 = SCREEN_CY - h / 2;
  tft.pushImage(x0, y0, w, h, reinterpret_cast<uint16_t *>(spriteFrameCache));
}

// A short cross-fade at the loop seam is only used while a pet is working.
// It makes GIFs whose final pose does not exactly meet their first pose loop
// continuously without adding a duplicate hold frame.
void drawSpriteLoopBridge(bool custom, const char *file, const uint16_t *const *progmemFrames, int count, int w,
                          int h, size_t frameBytes) {
  if (count < 2 ||
      !loadSpriteFrame(custom, file, progmemFrames, count - 1, frameBytes, spriteFrameCache) ||
      !loadSpriteFrame(custom, file, progmemFrames, 0, frameBytes, spriteLoopBridgeCache)) return;
  uint16_t *last = reinterpret_cast<uint16_t *>(spriteFrameCache);
  const uint16_t *first = reinterpret_cast<const uint16_t *>(spriteLoopBridgeCache);
  for (size_t i = 0; i < frameBytes / 2; ++i) {
    // Sprite frames are byte-normalized for TFT_eSPI, so swap to regular
    // RGB565 before averaging and swap back for pushImage().
    uint16_t a = swap565(last[i]);
    uint16_t b = swap565(first[i]);
    uint16_t blended = ((a & 0xF7DE) >> 1) + ((b & 0xF7DE) >> 1);
    last[i] = swap565(blended);
  }
  int x0 = SCREEN_CX - w / 2, y0 = SCREEN_CY - h / 2;
  tft.pushImage(x0, y0, w, h, last);
}

// ---------- helpers ----------

String formatTokens(long tokens) {
  if (tokens >= 1000000) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%.1fM", tokens / 1000000.0);
    return String(buf);
  }
  if (tokens >= 1000) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%.1fk", tokens / 1000.0);
    return String(buf);
  }
  return String(tokens);
}

// ---------- drawing ----------

void drawStaticChrome() {
  tft.fillScreen(TFT_BLACK);
}

// Bridge unreachable / data stale -> flashing red overrides everything else,
// matches the "urgent, look now" state from the reference signal-light design.
bool bridgeStale() {
  if (!everPolled) return true;
  return (millis() - lastSuccessMs) >= 5UL * BRIDGE_POLL_INTERVAL_MS;
}

// True when the app currently on screen is waiting on a permission/approval
// prompt — drives the red "look now, act" border flash.
bool currentAppNeedsInput() {
  return currentApp == APP_CLAUDE ? claudeStatus.needsInput : codexStatus.needsInput;
}

// Working vs idle is now conveyed by the sprite animation itself (moving vs
// still), not by ring color. The ring just stays steady green, except
// bridge-stale which flashes red ("check it now") and overrides everything.
uint16_t currentStatusColor() {
  if (bridgeStale()) return flashOn ? TFT_RED : TFT_BLACK;
  return TFT_GREEN;
}

// The ring is skipped when nothing changed (see drawSquareRing) so the 5s
// poll doesn't visibly blank-and-repaint it. Anything that paints over the
// ring area must invalidate this cache.
float ringLastPct = -1000;
uint16_t ringLastColor = 1;

// Paints the full square border in one color (all four sides), used for the
// attention flash so the whole edge blinks, not just the filled quota arc.
void drawFullBorder(uint16_t color) {
  ringLastPct = -1000; // ring got painted over; next ring draw must repaint
  int x0 = RING_MARGIN, y0 = RING_MARGIN;
  int horizontal = SCREEN_W - 2 * RING_MARGIN;
  int vertical = SCREEN_H - 2 * RING_MARGIN;
  tft.fillRect(x0, y0, horizontal, RING_THICKNESS, color); // top
  tft.fillRect(x0, SCREEN_H - RING_MARGIN - RING_THICKNESS,
               horizontal, RING_THICKNESS, color); // bottom
  tft.fillRect(x0, y0, RING_THICKNESS, vertical, color); // left
  tft.fillRect(SCREEN_W - RING_MARGIN - RING_THICKNESS, y0,
               RING_THICKNESS, vertical, color); // right
}

// Rectangular progress ring hugging the screen edge. `pct` of the perimeter
// is drawn clockwise: full top -> full right -> full bottom -> partial left.
// The unused portion is black; complete sides overlap at the corners so a
// later, partial side can never erase a corner that is already complete.
void drawSquareRing(float pct, uint16_t color) {
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  if (pct == ringLastPct && color == ringLastColor) return; // nothing changed
  ringLastPct = pct;
  ringLastColor = color;

  int x0 = RING_MARGIN, y0 = RING_MARGIN;
  int x1 = SCREEN_W - RING_MARGIN, y1 = SCREEN_H - RING_MARGIN;
  int horizontal = x1 - x0;
  int vertical = y1 - y0;
  float perimeter = 2.0f * (horizontal + vertical);

  // Clear all four complete sides first. Full side rectangles deliberately
  // overlap at the corners: once a side is complete, its corner must remain
  // filled instead of being reassigned to a later, still-incomplete side.
  tft.fillRect(x0, y0, horizontal, RING_THICKNESS, TFT_BLACK);
  tft.fillRect(x1 - RING_THICKNESS, y0, RING_THICKNESS, vertical, TFT_BLACK);
  tft.fillRect(x0, y1 - RING_THICKNESS, horizontal, RING_THICKNESS, TFT_BLACK);
  tft.fillRect(x0, y0, RING_THICKNESS, vertical, TFT_BLACK);

  // filled portion, clockwise: top -> right -> bottom -> left
  float remaining = perimeter * (pct / 100.0);
  if (remaining <= 0) return;

  float seg = min(remaining, (float)horizontal);
  tft.fillRect(x0, y0, (int)seg, RING_THICKNESS, color);
  remaining -= horizontal;
  if (remaining <= 0) return;

  seg = min(remaining, (float)vertical);
  tft.fillRect(x1 - RING_THICKNESS, y0,
               RING_THICKNESS, (int)seg, color);
  remaining -= vertical;
  if (remaining <= 0) return;

  seg = min(remaining, (float)horizontal);
  tft.fillRect(x1 - (int)seg, y1 - RING_THICKNESS,
               (int)seg, RING_THICKNESS, color);
  remaining -= horizontal;
  if (remaining <= 0) return;

  seg = min(remaining, (float)vertical);
  // The bottom-left corner is already painted by the completed bottom side.
  // If the first few pixels of the left side are drawn inside that same 10px
  // corner they become invisible (79% is only ~3px beyond three full sides).
  // Extend the left fill through its shared corner, so `visibleSeg` always
  // starts immediately above the bottom edge and remains perceptible.
  int visibleSeg = max(1, (int)ceilf(seg));
  int leftFillHeight = min(vertical, RING_THICKNESS + visibleSeg);
  tft.fillRect(x0, y1 - leftFillHeight, RING_THICKNESS, leftFillHeight, color);
}

void drawClaudeSprite(int frameIdx, bool working = true) {
  int count = working ? claudeFrameCount() : claudeIdleFrameCount();
  if (count > 0) frameIdx %= count;
  drawSpriteFrame(working ? claudeCustom : claudeIdleCustom,
                  working ? CLAUDE_SPRITE_FILE : CLAUDE_IDLE_FILE, claude_sprite_frames, frameIdx,
                  CLAUDE_SPRITE_W, CLAUDE_SPRITE_H, CLAUDE_FRAME_BYTES);
}

void drawClaudeLoopBridge() {
  drawSpriteLoopBridge(claudeCustom, CLAUDE_SPRITE_FILE, claude_sprite_frames, claudeFrameCount(),
                       CLAUDE_SPRITE_W, CLAUDE_SPRITE_H, CLAUDE_FRAME_BYTES);
}

void drawCodexSprite(int frameIdx, bool working = true) {
  int count = working ? codexFrameCount() : codexIdleFrameCount();
  if (count > 0) frameIdx %= count;
  drawSpriteFrame(working ? codexCustom : codexIdleCustom,
                  working ? CODEX_SPRITE_FILE : CODEX_IDLE_FILE, codex_sprite_frames, frameIdx,
                  CODEX_SPRITE_W, CODEX_SPRITE_H, CODEX_FRAME_BYTES);
}

void drawCodexLoopBridge() {
  drawSpriteLoopBridge(codexCustom, CODEX_SPRITE_FILE, codex_sprite_frames, codexFrameCount(),
                       CODEX_SPRITE_W, CODEX_SPRITE_H, CODEX_FRAME_BYTES);
}

String pctText(float pct) {
  return pct >= 0 ? String((int)pct) + "%" : "-";
}

// Quota readout below the sprite. Normally it has 5h / Wk columns; a
// weekly-only Codex plan uses one centered "Weekly 42%" line, matching the
// desktop mirror. Values repaint only when their text changes (force = after
// a full-screen clear), so status polling never flashes them.
const int QUOTA_LABEL_Y = 183, QUOTA_VALUE_Y = 199;
const int QUOTA_COL1_X = 70, QUOTA_COL2_X = 170;
const int QUOTA_CLEAR_X = RING_MARGIN + RING_THICKNESS;
const int QUOTA_CLEAR_W = SCREEN_W - 2 * QUOTA_CLEAR_X;
String lastQuota5h, lastQuotaWk;
bool lastQuotaHasHour = true;

// Faux-bold: the packed TFT_eSPI fonts have no bold face, so draw twice with
// a 1px x offset. Transparent draws - the caller must have cleared the region.
void drawBoldString(const String &s, int x, int y, int font, uint16_t color) {
  tft.setTextColor(color);
  tft.drawString(s, x, y, font);
  tft.drawString(s, x + 1, y, font);
}

void drawQuotaText(float hourPct, float weekPct, bool hasHourQuota, bool force) {
  tft.setTextDatum(TC_DATUM);
  if (force || hasHourQuota != lastQuotaHasHour) {
    // A layout switch must erase both the old labels and values (in
    // particular, the former 5h column) before drawing the new arrangement.
    // Never clear through the edge ring. At 85%, the left-side fill occupies
    // y=168..235; the former full-width clears at y=183..224 erased its middle
    // (and the matching right edge), leaving the isolated block seen on M5.
    tft.fillRect(QUOTA_CLEAR_X, QUOTA_LABEL_Y, QUOTA_CLEAR_W, 18, TFT_BLACK);
    tft.fillRect(QUOTA_CLEAR_X, QUOTA_VALUE_Y, QUOTA_CLEAR_W, 26, TFT_BLACK);
    lastQuota5h = "";
    lastQuotaWk = "";
    lastQuotaHasHour = hasHourQuota;
    if (hasHourQuota) {
      drawBoldString("5h", QUOTA_COL1_X, QUOTA_LABEL_Y, 2, TFT_LIGHTGREY);
      drawBoldString("Wk", QUOTA_COL2_X, QUOTA_LABEL_Y, 2, TFT_LIGHTGREY);
    }
  }
  String v1 = pctText(hourPct), v2 = pctText(weekPct);
  if (hasHourQuota && v1 != lastQuota5h) {
    lastQuota5h = v1;
    tft.fillRect(QUOTA_COL1_X - 50, QUOTA_VALUE_Y, 100, 26, TFT_BLACK);
    drawBoldString(v1, QUOTA_COL1_X, QUOTA_VALUE_Y, 4, TFT_WHITE);
  }
  if (v2 != lastQuotaWk) {
    lastQuotaWk = v2;
    if (hasHourQuota) {
      tft.fillRect(QUOTA_COL2_X - 50, QUOTA_VALUE_Y, 100, 26, TFT_BLACK);
      drawBoldString(v2, QUOTA_COL2_X, QUOTA_VALUE_Y, 4, TFT_WHITE);
    } else {
      // There is no 5h value to pair with it, so give the weekly value the
      // full width instead of retaining the old two-line mobile layout.
      tft.fillRect(QUOTA_CLEAR_X, QUOTA_VALUE_Y, QUOTA_CLEAR_W, 26, TFT_BLACK);
      drawBoldString("Weekly " + v2, SCREEN_CX, QUOTA_VALUE_Y, 4, TFT_WHITE);
    }
  }
}

// ---------- quota-exhausted countdown ----------
// When the current app's 5h or weekly window is used up, the pet is replaced
// by a countdown to that window's reset (bridge sends minutes-until-reset).
// A spent weekly window blocks usage even after the 5h one resets, so the
// weekly countdown takes priority when both are exhausted.

enum CdType { CD_NONE, CD_5H, CD_WEEK };

float currentHourPct() {
  return currentApp == APP_CLAUDE ? claudeStatus.fiveHourPct : codexStatus.primaryPct;
}

int currentHourResetMin() {
  return currentApp == APP_CLAUDE ? claudeStatus.fiveHourResetMin : codexStatus.primaryResetMin;
}

float currentWeekPct() {
  return currentApp == APP_CLAUDE ? claudeStatus.sevenDayPct : codexStatus.weeklyPct;
}

int currentWeekResetMin() {
  return currentApp == APP_CLAUDE ? claudeStatus.sevenDayResetMin : codexStatus.weeklyResetMin;
}

CdType desiredCountdown() {
  if (currentWeekPct() >= 99.9f && currentWeekResetMin() >= 0) return CD_WEEK;
  if (currentHourPct() >= 99.9f && currentHourResetMin() >= 0) return CD_5H;
  return CD_NONE;
}

CdType showingCd = CD_NONE; // what's on screen now (vs desiredCountdown())
String lastCountdown;

// The bridge only reports whole minutes, so the seconds tick locally against
// a deadline anchored at millis(). Re-anchor only when the bridge disagrees
// by more than ~a minute (new window, big clock drift), otherwise a poll
// landing mid-minute would make the seconds jump around.
unsigned long cdDeadlineMs = 0; // 0 = not anchored
ActiveApp cdApp = APP_CLAUDE;   // which app/window the anchor belongs to
CdType cdAnchorType = CD_NONE;

void syncCountdownDeadline() {
  int m = showingCd == CD_WEEK ? currentWeekResetMin() : currentHourResetMin();
  if (m < 0) {
    cdDeadlineMs = 0;
    return;
  }
  long bridgeSec = (long)m * 60 + 30; // bridge floors to minutes: assume mid-minute
  long ourSec = (long)(cdDeadlineMs - millis()) / 1000;
  if (cdDeadlineMs == 0 || cdApp != currentApp || cdAnchorType != showingCd || ourSec < 0 ||
      labs(ourSec - bridgeSec) > 90) {
    cdDeadlineMs = millis() + (unsigned long)bridgeSec * 1000UL;
    cdApp = currentApp;
    cdAnchorType = showingCd;
  }
}

void drawCountdown(bool force) {
  long remain = cdDeadlineMs ? (long)(cdDeadlineMs - millis()) / 1000
                             : (long)(showingCd == CD_WEEK ? currentWeekResetMin() : currentHourResetMin()) * 60;
  if (remain < 0) remain = 0;
  char buf[16];
  long hours = remain / 3600;
  if (hours >= 100) // weekly can be up to 168h: h:mm:ss wouldn't fit the ring
    snprintf(buf, sizeof(buf), "%ld:%02ld", hours, (remain % 3600) / 60);
  else
    snprintf(buf, sizeof(buf), "%ld:%02ld:%02ld", hours, (remain % 3600) / 60, remain % 60);
  String t(buf);
  if (!force && t == lastCountdown) return;
  // in-place glyph overwrite can't erase a shrinking string (h:mm:ss width is
  // constant, but 100:00 -> 99:59:59 changes layout once) - clear on any
  // length change
  if (t.length() != lastCountdown.length()) force = true;
  lastCountdown = t;
  tft.setTextDatum(TC_DATUM);
  if (force) {
    tft.fillRect(SCREEN_CX - 99, 66, 198, 84, TFT_BLACK);
    drawBoldString(showingCd == CD_WEEK ? "Wk RESET IN" : "5h RESET IN", SCREEN_CX, 72, 2, TFT_LIGHTGREY);
  }
  // Background-color draw overwrites glyphs in place (no clear-then-draw
  // flash between seconds).
  tft.setTextColor(TFT_ORANGE, TFT_BLACK);
  tft.drawString(t, SCREEN_CX, 102, 6);
}

// App logo in the top-left corner (inside the quota ring) so a glance tells
// which app the screen is currently showing. Drawn row-by-row from PROGMEM
// through rowBuf, same as the sprite path.
const int LOGO_X = 14, LOGO_Y = 18;

void drawAppLogo() {
  const uint16_t *logo = (currentApp == APP_CLAUDE) ? claude_logo_0 : codex_logo_0;
  int w = (currentApp == APP_CLAUDE) ? CLAUDE_LOGO_W : CODEX_LOGO_W;
  int h = (currentApp == APP_CLAUDE) ? CLAUDE_LOGO_H : CODEX_LOGO_H;
  for (int r = 0; r < h; r++) {
    memcpy_P(rowBuf, logo + (size_t)r * w, (size_t)w * 2);
    tft.pushImage(LOGO_X, LOGO_Y + r, w, 1, rowBuf);
  }
}

// Claude's ring percentage: real 5h OAuth quota from the bridge when known,
// otherwise fall back to elapsed session time as a rough stand-in.
float claudeRingPct() {
  if (claudeStatus.fiveHourPct >= 0) return claudeStatus.fiveHourPct;
  return claudeStatus.sessionWindowMin > 0
             ? (100.0 * claudeStatus.sessionMin / claudeStatus.sessionWindowMin)
             : 0;
}

// A Codex response can contain only the weekly window. In that case the
// weekly percentage becomes the main ring rather than a blank/zero 5h ring.
float codexRingPct() {
  return codexStatus.primaryPct >= 0 ? codexStatus.primaryPct : codexStatus.weeklyPct;
}

// Redraws whichever app is currently active, full screen: quota ring +
// sprite (or the reset countdown while the 5h window is exhausted).
// Full clear + repaint - only for real transitions (app switch, mode return,
// sprite change); steady-state data updates go through refreshActiveApp().
void drawActiveApp() {
  tft.fillScreen(TFT_BLACK);
  ringLastPct = -1000; // screen was cleared: force the ring repaint
  showingCd = desiredCountdown();
  if (showingCd != CD_NONE) syncCountdownDeadline();
  else cdDeadlineMs = 0;
  if (currentApp == APP_CLAUDE) {
    drawSquareRing(displayedQuotaPct(claudeRingPct()), currentStatusColor());
    if (showingCd == CD_NONE) drawClaudeSprite(claudeFrame, claudeStatus.status == "working");
    drawQuotaText(displayedQuotaPct(claudeRingPct()), displayedQuotaPct(claudeStatus.sevenDayPct), true, true);
  } else {
    drawSquareRing(max(displayedQuotaPct(codexRingPct()), 0.0f), currentStatusColor());
    if (showingCd == CD_NONE) drawCodexSprite(codexFrame, codexStatus.status == "working");
    drawQuotaText(displayedQuotaPct(codexStatus.primaryPct), displayedQuotaPct(codexStatus.weeklyPct),
                  codexStatus.primaryPct >= 0, true);
  }
  if (showingCd != CD_NONE) drawCountdown(true);
  drawAppLogo();
}

// In-place refresh after a bridge poll: ring repaint + only the text that
// actually changed. No fillScreen, so the 5s poll doesn't blank the screen.
void refreshActiveApp() {
  if (desiredCountdown() != showingCd) { // pet <-> countdown (or 5h <-> weekly) swap
    drawActiveApp();
    return;
  }
  if (currentApp == APP_CLAUDE) {
    drawSquareRing(displayedQuotaPct(claudeRingPct()), currentStatusColor());
    drawQuotaText(displayedQuotaPct(claudeRingPct()), displayedQuotaPct(claudeStatus.sevenDayPct), true, false);
  } else {
    drawSquareRing(max(displayedQuotaPct(codexRingPct()), 0.0f), currentStatusColor());
    drawQuotaText(displayedQuotaPct(codexStatus.primaryPct), displayedQuotaPct(codexStatus.weeklyPct),
                  codexStatus.primaryPct >= 0, false);
  }
  if (showingCd != CD_NONE) {
    syncCountdownDeadline();
    drawCountdown(false);
  }
}

// Redraws just the ring (cheap) - used for status color animation ticks
// between full redraws.
void redrawRingOnly() {
  if (currentApp == APP_CLAUDE) {
    drawSquareRing(displayedQuotaPct(claudeRingPct()), currentStatusColor());
  } else {
    drawSquareRing(max(displayedQuotaPct(codexRingPct()), 0.0f), currentStatusColor());
  }
}

// Who gets the screen:
//   - display mode pinned (Mac app) -> that app, always
//   - exactly one app working       -> that app, immediately
//   - both working                  -> alternate every SWITCH_BOTH_MS (2s)
//   - neither working               -> alternate slowly (SWITCH_IDLE_MS)
bool updateActiveApp() {
  ActiveApp desired = currentApp;

  if (displayMode == MODE_CLAUDE) {
    desired = APP_CLAUDE;
  } else if (displayMode == MODE_CODEX) {
    desired = APP_CODEX;
  } else if (claudeStatus.needsInput && !codexStatus.needsInput) {
    desired = APP_CLAUDE; // approval prompt wins the screen
  } else if (codexStatus.needsInput && !claudeStatus.needsInput) {
    desired = APP_CODEX;
  } else {
    bool claudeWorking = claudeStatus.status == "working";
    bool codexWorking = codexStatus.status == "working";
    if (claudeWorking && !codexWorking) {
      desired = APP_CLAUDE;
    } else if (codexWorking && !claudeWorking) {
      desired = APP_CODEX;
    } else {
      unsigned long interval = (claudeWorking && codexWorking) ? SWITCH_BOTH_MS : SWITCH_IDLE_MS;
      if (millis() - lastSwitchMs >= interval) {
        lastSwitchMs = millis();
        desired = (currentApp == APP_CLAUDE) ? APP_CODEX : APP_CLAUDE;
      }
    }
  }

  if (desired != currentApp) {
    currentApp = desired;
    lastSwitchMs = millis();
    return true;
  }
  return false;
}

// ---------- net speed screen ----------

String speedText(long bps) {
  char buf[16];
  if (bps >= 1000000) snprintf(buf, sizeof(buf), "%.1fM", bps / 1000000.0);
  else if (bps >= 1000) snprintf(buf, sizeof(buf), "%.0fK", bps / 1000.0);
  else snprintf(buf, sizeof(buf), "%ldB", bps);
  return String(buf);
}

void resetNetChart() {
  memset(netHistRx, 0, sizeof(netHistRx));
  memset(netHistTx, 0, sizeof(netHistTx));
  netScale = 10240;
  netLastDl = "";
  netLastUl = "";
  netLastScaleText = "";
  netQHead = 0;
  netQCount = 0;
  netSeq = -1;
}

// Full-scale steps: whole-chart shared scale snaps to the next "nice" value,
// so bar heights stay comparable and the axis label reads cleanly.
long niceNetScale(long maxV) {
  static const long steps[] = {10240,    20480,    51200,     102400,    204800,    512000,
                               1048576,  2097152,  5242880,   10485760,  20971520,  52428800,
                               104857600, 209715200, 524288000};
  for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
    if (maxV <= steps[i]) return steps[i];
  }
  return steps[sizeof(steps) / sizeof(steps[0]) - 1];
}

// Static chrome: labels that never change while in net mode.
void drawNetChrome() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(0x7BEF, TFT_BLACK);
  tft.drawString("DOWN", 14, 10, 1);
  tft.drawString("UP", 134, 10, 1);
  tft.setTextDatum(TC_DATUM);
  tft.drawString("MAC NET  -  56s", SCREEN_CX, 208, 1);
}

// Header readouts (1s-averaged), each repainted only when its text changes.
void drawNetHeaderIfChanged() {
  String dl = speedText(netCurRx) + "/s";
  String ul = speedText(netCurTx) + "/s";
  tft.setTextDatum(TL_DATUM);
  if (dl != netLastDl) {
    netLastDl = dl;
    tft.fillRect(12, 20, 116, 28, TFT_BLACK);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.drawString(dl, 12, 20, 4);
  }
  if (ul != netLastUl) {
    netLastUl = ul;
    tft.fillRect(132, 20, 108, 28, TFT_BLACK);
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.drawString(ul, 132, 20, 4);
  }
}

// Repaints the whole chart region from the sample ring, one row at a time
// through rowBuf (a single pushImage per row = no clear-then-draw flicker).
// Download is a dim-green filled area with a bright top edge; upload is a
// 2px yellow line on top; faint gridlines at 25/50/75%.
void drawNetChart() {
  static const uint16_t COL_GRID = swap565(0x2104);   // very dark grey
  static const uint16_t COL_FILL = swap565(0x02A0);   // dim green
  static const uint16_t COL_EDGE = swap565(TFT_GREEN);
  static const uint16_t COL_UL = swap565(TFT_YELLOW);
  static const uint16_t COL_BLACK = swap565(TFT_BLACK);

  long maxV = 0;
  for (int i = 0; i < NET_CHART_W; i++) {
    if (netHistRx[i] > maxV) maxV = netHistRx[i];
    if (netHistTx[i] > maxV) maxV = netHistTx[i];
  }
  netScale = niceNetScale(maxV);

  // Per-column heights (3-tap smoothed), then per-column line "bands": each
  // band spans from the previous column's height to this one's, so steep
  // rises/falls render as connected vertical strokes instead of detached
  // stair-step dots — that's what makes the undulation read as a continuous
  // line, like the Mac mirror's stroked polyline.
  static uint8_t hRx[NET_CHART_W], hTx[NET_CHART_W];
  static uint8_t dlLo[NET_CHART_W], dlHi[NET_CHART_W]; // DL edge band, incl. 3px weight
  static uint8_t ulLo[NET_CHART_W], ulHi[NET_CHART_W]; // UL line band
  const int LINE_T = 3; // stroke thickness in px
  for (int i = 0; i < NET_CHART_W; i++) {
    int lo = i > 0 ? i - 1 : 0, hi = i < NET_CHART_W - 1 ? i + 1 : NET_CHART_W - 1;
    long rx = (netHistRx[lo] + netHistRx[i] + netHistRx[hi]) / 3;
    long tx = (netHistTx[lo] + netHistTx[i] + netHistTx[hi]) / 3;
    int hr = (int)((float)rx / netScale * (NET_CHART_H - 2));
    int ht = (int)((float)tx / netScale * (NET_CHART_H - 2));
    hRx[i] = (uint8_t)constrain(hr, 0, NET_CHART_H - 1);
    hTx[i] = (uint8_t)constrain(ht, 0, NET_CHART_H - 1);
  }
  for (int i = 0; i < NET_CHART_W; i++) {
    int prevR = i > 0 ? hRx[i - 1] : hRx[0];
    int prevT = i > 0 ? hTx[i - 1] : hTx[0];
    dlHi[i] = (uint8_t)max((int)hRx[i], prevR);
    dlLo[i] = (uint8_t)max(0, min((int)hRx[i], prevR) - (LINE_T - 1));
    ulHi[i] = (uint8_t)max((int)hTx[i], prevT);
    ulLo[i] = (uint8_t)max(0, min((int)hTx[i], prevT) - (LINE_T - 1));
  }

  for (int row = 0; row < NET_CHART_H; row++) {
    int yFromBot = NET_CHART_H - 1 - row;
    bool gridRow = (row == NET_CHART_H / 4 || row == NET_CHART_H / 2 || row == 3 * NET_CHART_H / 4);
    for (int i = 0; i < NET_CHART_W; i++) {
      uint16_t c = gridRow ? COL_GRID : COL_BLACK;
      if (yFromBot <= dlHi[i] && yFromBot >= dlLo[i]) c = COL_EDGE;
      else if (yFromBot < dlLo[i]) c = COL_FILL;
      if (ulHi[i] > 0 && yFromBot <= ulHi[i] && yFromBot >= ulLo[i]) c = COL_UL;
      rowBuf[i] = c;
    }
    tft.pushImage(NET_CHART_X, NET_CHART_Y + row, NET_CHART_W, 1, rowBuf);
    if ((row & 31) == 31) yield();
  }

  // axis label (outside the chart, so it never gets repainted over)
  String scaleText = speedText(netScale);
  if (scaleText != netLastScaleText) {
    netLastScaleText = scaleText;
    tft.fillRect(120, 48, 112, 10, TFT_BLACK);
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(0x7BEF, TFT_BLACK);
    tft.drawString(scaleText, NET_CHART_X + NET_CHART_W, 48, 1);
    tft.setTextDatum(TL_DATUM);
  }
}

// Chart tick, every NET_DRAW_INTERVAL_MS: shift in queued sample(s), then
// one atomic repaint. If the queue backs up after a slow poll, it works off
// up to three samples per tick until it's back in step.
void netDrawTick() {
  if (!netChromeDrawn) {
    resetNetChart();
    drawNetChrome();
    netChromeDrawn = true;
    netHeaderDirty = true;
  }
  if (netHeaderDirty) {
    drawNetHeaderIfChanged();
    netHeaderDirty = false;
  }
  if (netQCount == 0) return;
  int steps = min(netQCount, netQCount > 16 ? 3 : 1);
  while (steps-- > 0 && netQCount > 0) {
    memmove(netHistRx, netHistRx + 1, sizeof(long) * (NET_CHART_W - 1));
    memmove(netHistTx, netHistTx + 1, sizeof(long) * (NET_CHART_W - 1));
    netHistRx[NET_CHART_W - 1] = netQRx[netQHead];
    netHistTx[NET_CHART_W - 1] = netQTx[netQHead];
    netQHead = (netQHead + 1) % NET_QUEUE;
    netQCount--;
  }
  drawNetChart();
}

// Ingests one /net payload from either HTTP polling or a USB serial frame.
bool handleNetPayload(const String &payload) {
  JsonDocument doc;
  if (deserializeJson(doc, payload)) return false;
  netCurRx = doc["rx_bps"] | 0L;
  netCurTx = doc["tx_bps"] | 0L;
  netHeaderDirty = true;
  long seq = doc["seq"] | -1L;
  JsonArray rx = doc["rx"], tx = doc["tx"];
  int n = min(rx.size(), tx.size());
  int fresh = (netSeq < 0) ? min(n, 8) : (int)min((long)n, seq - netSeq);
  if (fresh < 0) fresh = 0;
  for (int i = n - fresh; i < n; i++) {
    if (netQCount >= NET_QUEUE) break;
    int tail = (netQHead + netQCount) % NET_QUEUE;
    netQRx[tail] = rx[i].as<long>();
    netQTx[tail] = tx[i].as<long>();
    netQCount++;
  }
  if (seq >= 0) netSeq = seq;
  return true;
}

// Refills the sample queue from the bridge's /net endpoint. The seq field
// tells us which samples we've already queued, so overlapping tails are fine.
void pollNet() {
  if (WiFi.status() != WL_CONNECTED || bridgeHost.length() == 0) return;
  WiFiClient client;
  HTTPClient http;
  String url = "http://" + bridgeHost + "/net";
  http.setTimeout(BRIDGE_HTTP_TIMEOUT_MS);
  if (!http.begin(client, url)) return;
  int code = http.GET();
  if (code == HTTP_CODE_OK) handleNetPayload(http.getString());
  http.end();
}

String timeText(int sec) {
  if (sec < 0) sec = 0;
  char buf[12];
  snprintf(buf, sizeof(buf), "%d:%02d", sec / 60, sec % 60);
  return String(buf);
}

String fitText(String s, int maxPx, int font) {
  if (tft.textWidth(s, font) <= maxPx) return s;
  while (s.length() > 0 && tft.textWidth(s + "...", font) > maxPx) {
    s.remove(s.length() - 1);
  }
  return s + "...";
}

void drawMusicCoverPlaceholder() {
  const int x = (SCREEN_W - MUSIC_COVER_W) / 2;
  const int y = 14;
  tft.fillRect(x, y, MUSIC_COVER_W, MUSIC_COVER_H, TFT_DARKGREY);
  tft.drawRect(x, y, MUSIC_COVER_W, MUSIC_COVER_H, TFT_DARKGREY);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_LIGHTGREY, TFT_DARKGREY);
  tft.drawString("No Art", SCREEN_CX, y + MUSIC_COVER_H / 2, 2);
}

bool drawMusicCoverFromBridge() {
  if (WiFi.status() != WL_CONNECTED || bridgeHost.length() == 0 || !musicHasArtwork) return false;
  WiFiClient client;
  HTTPClient http;
  String url = "http://" + bridgeHost + "/music/cover.raw";
  http.setTimeout(BRIDGE_HTTP_TIMEOUT_MS);
  if (!http.begin(client, url)) return false;
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    return false;
  }
  WiFiClient *stream = http.getStreamPtr();
  const int x = (SCREEN_W - MUSIC_COVER_W) / 2;
  const int y = 14;
  const size_t rowBytes = (size_t)MUSIC_COVER_W * 2;
  bool ok = true;
  for (int r = 0; r < MUSIC_COVER_H; r++) {
    int got = stream->readBytes((uint8_t *)rowBuf, rowBytes);
    if (got != (int)rowBytes) {
      ok = false;
      break;
    }
    tft.pushImage(x, y + r, MUSIC_COVER_W, 1, rowBuf);
    yield();
  }
  http.end();
  return ok;
}

// Streams the Mac-rendered 232x44 title/artist strip and blits it row by
// row — the only way to get CJK on screen without shipping a font.
bool drawMusicTextFromBridge() {
  if (WiFi.status() != WL_CONNECTED || bridgeHost.length() == 0) return false;
  WiFiClient client;
  HTTPClient http;
  String url = "http://" + bridgeHost + "/music/text.raw";
  http.setTimeout(BRIDGE_HTTP_TIMEOUT_MS);
  if (!http.begin(client, url)) return false;
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    return false;
  }
  WiFiClient *stream = http.getStreamPtr();
  const size_t rowBytes = (size_t)MUSIC_TEXT_W * 2;
  bool ok = true;
  for (int r = 0; r < MUSIC_TEXT_H; r++) {
    int got = stream->readBytes((uint8_t *)rowBuf, rowBytes);
    if (got != (int)rowBytes) {
      ok = false;
      break;
    }
    tft.pushImage(MUSIC_TEXT_X, MUSIC_TEXT_Y + r, MUSIC_TEXT_W, 1, rowBuf);
    yield();
  }
  http.end();
  return ok;
}

// ASCII-only fallback if the strip fetch fails (CJK will stay blank, but at
// least latin titles show something).
void drawMusicTextFallback() {
  tft.fillRect(MUSIC_TEXT_X, MUSIC_TEXT_Y, MUSIC_TEXT_W, MUSIC_TEXT_H, TFT_BLACK);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  String title = musicTitle.length() ? musicTitle : "No Music";
  tft.drawString(fitText(title, 216, 2), SCREEN_CX, MUSIC_TEXT_Y + 4, 2);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString(fitText(musicArtist, 216, 2), SCREEN_CX, MUSIC_TEXT_Y + 24, 2);
}

// Regions repaint independently: cover / text strip only when their rev
// changes, progress bar + time on every poll (partial fill, no flicker
// elsewhere).
void drawMusicScreen(bool coverChanged, bool textChanged) {
  if (!musicChromeDrawn) {
    tft.fillScreen(TFT_BLACK);
    coverChanged = true;
    textChanged = true;
    musicChromeDrawn = true;
  }
  if (coverChanged) {
    if (!drawMusicCoverFromBridge()) drawMusicCoverPlaceholder();
  }
  if (textChanged) {
    if (!drawMusicTextFromBridge()) drawMusicTextFallback();
  }

  const int bx = 20, by = 204, bw = 200, bh = 8;
  tft.fillRect(0, by - 2, SCREEN_W, SCREEN_H - by + 2, TFT_BLACK);
  tft.fillRect(bx, by, bw, bh, TFT_DARKGREY);
  float progress = musicDuration > 0 ? (float)musicElapsed / (float)musicDuration : 0;
  if (progress < 0) progress = 0;
  if (progress > 1) progress = 1;
  uint16_t color = musicPlaying ? TFT_GREEN : TFT_LIGHTGREY;
  tft.fillRect(bx, by, (int)(bw * progress), bh, color);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString(timeText(musicElapsed) + " / " + timeText(musicDuration), SCREEN_CX, 220, 1);
}

void pollMusic() {
  if (WiFi.status() != WL_CONNECTED || bridgeHost.length() == 0) return;
  WiFiClient client;
  HTTPClient http;
  String url = "http://" + bridgeHost + "/music";
  http.setTimeout(BRIDGE_HTTP_TIMEOUT_MS);
  if (!http.begin(client, url)) return;
  int code = http.GET();
  if (code == HTTP_CODE_OK) {
    JsonDocument doc;
    if (!deserializeJson(doc, http.getString())) {
      musicTitle = doc["title"] | "";
      musicArtist = doc["artist"] | "";
      musicAlbum = doc["album"] | "";
      musicPlaying = doc["playing"] | false;
      statusMusicPlaying = musicPlaying; // fast stop-detection while music shows
      musicElapsed = doc["elapsed"] | 0;
      musicDuration = doc["duration"] | 0;
      musicHasArtwork = doc["has_artwork"] | false;
      int rev = doc["artwork_rev"] | -1;
      bool coverChanged = rev != musicArtworkRev;
      musicArtworkRev = rev;
      int tRev = doc["text_rev"] | -1;
      bool textChanged = tRev != musicTextRev;
      musicTextRev = tRev;
      drawMusicScreen(coverChanged, textChanged);
    }
  }
  http.end();
}

// ---------- WiFi / bridge polling ----------

WiFiManager wifiManager; // kept global so its non-blocking portal can run from loop()

void configModeCallback(WiFiManager *wm) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("WiFi setup needed", 8, 32, 2);
  tft.drawString("Connect phone to AP:", 8, 62, 2);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString(WIFI_PORTAL_AP_NAME, 8, 87, 2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("then open 192.168.4.1", 8, 117, 2);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString("Or plug into USB", 8, 155, 2);
  tft.drawString("for wired status", 8, 178, 2);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString("Firmware v" FW_VERSION, 8, 215, 2);
}

void setupWiFi() {
  wifiManager.setAPCallback(configModeCallback);
  wifiManager.setConfigPortalBlocking(false);

  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Connecting WiFi...", 8, 100, 2);

  Serial.println("[wifi] starting WiFiManager autoConnect (non-blocking portal)...");
  bool ok = wifiManager.autoConnect(WIFI_PORTAL_AP_NAME);
  // Keep the HTTP admin endpoint responsive on this battery-powered ESP32;
  // modem-sleep can otherwise add multi-second stalls while the TFT is busy.
  WiFi.setSleep(false);
  Serial.printf("[wifi] autoConnect result=%d ssid=%s ip=%s\n", ok, WiFi.SSID().c_str(),
                WiFi.localIP().toString().c_str());
  Serial.printf("[wifi] bridge host = '%s'\n", bridgeHost.c_str());
}

bool parseStatusJson(const String &payload) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) return false;

  JsonObject c = doc["claude"];
  if (!c.isNull()) {
    claudeStatus.status = c["status"] | "unknown";
    claudeStatus.tokensToday = c["tokens_today"] | 0;
    claudeStatus.sessionMin = c["session_min"] | 0;
    claudeStatus.sessionWindowMin = c["session_window_min"] | 300;
    claudeStatus.fiveHourPct = c["five_hour_pct"] | -1.0;
    claudeStatus.fiveHourResetMin = c["five_hour_reset_min"] | -1;
    claudeStatus.sevenDayPct = c["seven_day_pct"] | -1.0;
    claudeStatus.sevenDayResetMin = c["seven_day_reset_min"] | -1;
    claudeStatus.needsInput = c["needs_input"] | false;
  }

  JsonObject x = doc["codex"];
  if (!x.isNull()) {
    codexStatus.status = x["status"] | "unknown";
    codexStatus.tokensToday = x["tokens_today"] | 0;
    codexStatus.primaryPct = x["primary_pct"] | -1.0;
    codexStatus.primaryResetMin = x["primary_reset_min"] | -1;
    codexStatus.weeklyPct = x["weekly_pct"] | -1.0;
    codexStatus.weeklyResetMin = x["weekly_reset_min"] | -1;
    codexStatus.needsInput = x["needs_input"] | false;
  }
  statusMusicPlaying = doc["music_playing"] | false;
  return true;
}

// The mode actually rendered. In AUTO: a pending approval prompt wins (stay on
// the pet so its border can flash red at you), otherwise audio promotes to the
// music page.
DisplayMode effectiveMode() {
  if (displayMode == MODE_AUTO) {
    if (claudeStatus.needsInput || codexStatus.needsInput) return MODE_AUTO;
    if (statusMusicPlaying && WiFi.status() == WL_CONNECTED) return MODE_MUSIC;
  }
  return displayMode;
}

void pollBridge() {
  if (WiFi.status() != WL_CONNECTED || bridgeHost.length() == 0) {
    if (WiFi.status() != WL_CONNECTED && millis() - lastWiFiReconnectMs > 10000) {
      lastWiFiReconnectMs = millis();
      Serial.println("[wifi] disconnected; attempting reconnect");
      WiFi.reconnect();
    }
    Serial.printf("[bridge] skip poll: wifi=%d host='%s'\n", WiFi.status() == WL_CONNECTED, bridgeHost.c_str());
    return;
  }

  WiFiClient client;
  HTTPClient http;
  String url = "http://" + bridgeHost + BRIDGE_DEFAULT_PATH;
  http.setTimeout(BRIDGE_HTTP_TIMEOUT_MS);

  if (!http.begin(client, url)) {
    Serial.println("[bridge] http.begin() failed");
    return;
  }
  int code = http.GET();
  Serial.printf("[bridge] GET %s -> %d\n", url.c_str(), code);
  if (code == HTTP_CODE_OK) {
    String payload = http.getString();
    bool wasClaudeWorking = claudeStatus.status == "working";
    bool wasCodexWorking = codexStatus.status == "working";
    if (parseStatusJson(payload)) {
      bool completed = statusBaselineReady &&
                       ((wasClaudeWorking && claudeStatus.status != "working") ||
                        (wasCodexWorking && codexStatus.status != "working"));
      lastSuccessMs = millis();
      everPolled = true;
      bridgeFailCount = 0;
      statusBaselineReady = true;
      if (completed) triggerCompletionBeep();
      Serial.printf("[bridge] claude=%s tok=%ld | codex=%s tok=%ld primary=%.0f%%\n",
                    claudeStatus.status.c_str(), claudeStatus.tokensToday,
                    codexStatus.status.c_str(), codexStatus.tokensToday, codexStatus.primaryPct);
    } else {
      Serial.println("[bridge] JSON parse failed");
    }
  } else {
    // Treat an isolated timeout as a transient LAN hiccup. Keep the last
    // good state until several consecutive failures, avoiding visible
    // offline/working flicker when the Mac briefly wakes or changes Wi-Fi.
    if (bridgeFailCount < 255) bridgeFailCount++;
    if (bridgeFailCount >= 3) {
      claudeStatus.status = "offline";
      codexStatus.status = "offline";
    }
  }
  http.end();
  DisplayMode eff = effectiveMode();
  if (eff != MODE_NET && eff != MODE_MUSIC) {
    // Only a real app switch clears the screen; a plain data refresh paints
    // in place so the poll doesn't flash the whole display.
    if (updateActiveApp()) drawActiveApp();
    else refreshActiveApp();
  }
}

// ---------- wired USB serial bridge ----------
// When the clock is connected by USB, the macOS bridge pushes the same status
// and network JSON it normally serves over HTTP. This works without Wi-Fi and
// bypasses AP client isolation. Non-protocol serial logs are ignored.
unsigned long lastSerialFrameMs = 0;
bool wiredEverLinked = false;
char serialLine[1600];
size_t serialLineLen = 0;

bool wiredActive() { return wiredEverLinked && millis() - lastSerialFrameMs < 15000UL; }

void showMainUiIfNeeded() {
  if (mainUiShown) return;
  mainUiShown = true;
  drawStaticChrome();
  updateActiveApp();
  drawActiveApp();
}

void handleSerialFrame(char *line) {
  lastSerialFrameMs = millis();
  wiredEverLinked = true;
  if (!strncmp(line, "#HELLO", 6)) {
    Serial.printf("#DEVICE {\"name\":\"aiclock\",\"fw\":\"%s\"}\n", FW_VERSION);
    return;
  }
  if (!strncmp(line, "#STATUS ", 8)) {
    bool wasClaudeWorking = claudeStatus.status == "working";
    bool wasCodexWorking = codexStatus.status == "working";
    if (parseStatusJson(String(line + 8))) {
      bool completed = statusBaselineReady &&
                       ((wasClaudeWorking && claudeStatus.status != "working") ||
                        (wasCodexWorking && codexStatus.status != "working"));
      lastSuccessMs = millis();
      everPolled = true;
      bridgeFailCount = 0;
      statusBaselineReady = true;
      if (completed) triggerCompletionBeep();
      showMainUiIfNeeded();
      DisplayMode eff = effectiveMode();
      if (eff != MODE_NET && eff != MODE_MUSIC) {
        if (updateActiveApp()) drawActiveApp();
        else refreshActiveApp();
      }
    }
    return;
  }
  if (!strncmp(line, "#NET ", 5)) {
    handleNetPayload(String(line + 5));
    return;
  }
  if (!strncmp(line, "#CMD ", 5)) {
    JsonDocument doc;
    if (deserializeJson(doc, line + 5)) return;
    if (doc["brightness"].is<int>()) {
      brightness = constrain(doc["brightness"].as<int>(), 0, 100);
      applyBrightness();
      saveBrightness();
    }
    const char *mode = doc["display"] | (const char *)nullptr;
    if (mode) {
      String value(mode);
      if (value == "auto") displayMode = MODE_AUTO;
      else if (value == "claude") displayMode = MODE_CLAUDE;
      else if (value == "codex") displayMode = MODE_CODEX;
      else if (value == "net") displayMode = MODE_NET;
      else if (value == "music") displayMode = MODE_MUSIC;
      saveDisplayMode();
    }
  }
}

void pumpSerial() {
  while (Serial.available()) {
    char ch = (char)Serial.read();
    if (ch == '\n' || ch == '\r') {
      if (serialLineLen > 0 && serialLine[0] == '#') {
        serialLine[serialLineLen] = 0;
        handleSerialFrame(serialLine);
      }
      serialLineLen = 0;
    } else if (serialLineLen < sizeof(serialLine) - 1) {
      serialLine[serialLineLen++] = ch;
    } else {
      serialLineLen = 0;
    }
  }
}

// ---------- web admin ----------

String htmlEscape(const String &s) {
  String out = s;
  out.replace("&", "&amp;");
  out.replace("<", "&lt;");
  out.replace(">", "&gt;");
  out.replace("\"", "&quot;");
  return out;
}

void handleRoot() {
  String age = everPolled ? String((millis() - lastSuccessMs) / 1000) + "s ago" : "never";
  String html;
  html.reserve(3072);
  html += "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>AI Clock 设置</title>";
  html += "<style>body{font-family:-apple-system,sans-serif;max-width:480px;margin:24px "
          "auto;padding:0 16px;color:#222} h1{font-size:20px} label{display:block;margin-top:16px;font-weight:600}"
          "input{width:100%;box-sizing:border-box;padding:8px;font-size:16px;margin-top:4px}"
          "button{margin-top:16px;padding:10px 20px;font-size:16px;background:#2563eb;color:#fff;"
          "border:none;border-radius:6px}"
          "table{margin-top:20px;border-collapse:collapse;width:100%}"
          "td{padding:4px 8px;border-bottom:1px solid #eee;font-size:14px}"
          ".dot{display:inline-block;width:10px;height:10px;border-radius:50%;margin-right:6px}"
          "</style></head><body>";
  html += "<h1>AI Clock 设置</h1>";

  html += "<form method='POST' action='/save'>";
  html += "<label>Bridge host (ip:port)</label>";
  html += "<input name='bridge' value='" + htmlEscape(bridgeHost) + "' placeholder='192.168.1.181:8765'>";
  html += "<button type='submit'>保存</button>";
  html += "</form>";

  // Backlight brightness slider: applies live on release (PWM, persisted).
  html += "<h2 style='font-size:16px;margin-top:28px'>屏幕亮度</h2>";
  html += "<input type='range' min='0' max='100' value='" + String(brightness) + "' id='bri' "
          "oninput=\"document.getElementById('briv').textContent=this.value+'%'\" "
          "onchange=\"fetch('/api/brightness',{method:'POST',headers:{'Content-Type':"
          "'application/x-www-form-urlencoded'},body:'level='+this.value})\">";
  html += "<div style='font-size:13px;color:#555'>当前：<span id='briv'>" + String(brightness) +
          "%</span>（0 = 熄屏，设置立即生效并记住）</div>";

  html += "<h2 style='font-size:16px;margin-top:28px'>额度显示</h2>";
  html += "<select id='quota' onchange=\"fetch('/api/quota-display',{method:'POST',headers:{'Content-Type':"
          "'application/x-www-form-urlencoded'},body:'mode='+this.value})\">";
  html += "<option value='used'" + String(showQuotaRemaining ? "" : " selected") + ">显示已用</option>";
  html += "<option value='remaining'" + String(showQuotaRemaining ? " selected" : "") + ">显示剩余</option>";
  html += "</select><div style='font-size:13px;color:#555;margin-top:4px'>同时影响进度环和 5h / Wk 百分比，设置会保存。</div>";

  // On-device GIF upload: replaces a character's animation without reflashing.
  html += "<h2 style='font-size:16px;margin-top:28px'>桌宠动画（上传 GIF）</h2>";
  html += "<p style='font-size:13px;color:#555'>上传一个 .gif，设备会在板上解码并缩放到对应角色的尺寸，"
          "立刻替换动画，无需重新编译或烧录。GIF 太大可能因内存不足解码失败，换小一点的即可。</p>";
  html += "<form id='gifForm' method='POST' enctype='multipart/form-data' onsubmit='return setGifAction()'>";
  html += "<label>角色</label>";
  html += "<select id='gifTarget'><option value='codex'>Codex</option><option value='claude'>Claude</option></select>";
  html += "<label>GIF 文件</label><input type='file' name='file' accept='.gif' required>";
  html += "<button type='submit'>上传并应用</button>";
  html += "</form>";
  html += "<script>function setGifAction(){"
          "document.getElementById('gifForm').action='/sprite/'+document.getElementById('gifTarget').value;"
          "return true;}</script>";

  html += "<table>";
  html += "<tr><td>WiFi SSID</td><td>" + htmlEscape(WiFi.SSID()) + "</td></tr>";
  html += "<tr><td>设备 IP</td><td>" + WiFi.localIP().toString() + "</td></tr>";
  html += "<tr><td>上次桥接更新</td><td>" + age + "</td></tr>";
  html += "<tr><td>数据连接</td><td>" + String(wiredActive() ? "USB 串口直连" : "Wi-Fi 网络") + "</td></tr>";
  html += "<tr><td>Codex</td><td>" + htmlEscape(codexStatus.status) + ", " +
          formatTokens(codexStatus.tokensToday) + " tok, " +
          (codexStatus.primaryPct >= 0
              ? "5h " + String(codexStatus.primaryPct, 0) + "%"
              : "周 " + (codexStatus.weeklyPct >= 0 ? String(codexStatus.weeklyPct, 0) + "%" : "?")) +
          "</td></tr>";
  html += "<tr><td>Claude</td><td>" + htmlEscape(claudeStatus.status) + ", " +
          formatTokens(claudeStatus.tokensToday) + " tok</td></tr>";
  html += "</table>";

  html += "<form method='POST' action='/reset-wifi' onsubmit=\"return confirm('清除 WiFi "
          "设置并重启？设备会开启配网热点。');\">";
  html += "<button type='submit' style='background:#dc2626'>重置 WiFi</button>";
  html += "</form>";

  html += "</body></html>";
  webServer.send(200, "text/html", html);
}

void handleSave() {
  String newHost = webServer.arg("bridge");
  newHost.trim();
  bridgeHost = newHost;
  saveBridgeHost(bridgeHost);
  Serial.printf("[web] bridge host updated to '%s'\n", bridgeHost.c_str());
  webServer.sendHeader("Location", "/");
  webServer.send(303);
}

// ---------- JSON API for the Mac app ----------

const char *displayModeName(DisplayMode m) {
  if (m == MODE_CLAUDE) return "claude";
  if (m == MODE_CODEX) return "codex";
  if (m == MODE_NET) return "net";
  if (m == MODE_MUSIC) return "music";
  return "auto";
}

void handleApiInfo() {
  JsonDocument doc;
  doc["ip"] = WiFi.localIP().toString();
  doc["ssid"] = WiFi.SSID();
  doc["bridge"] = bridgeHost;
  doc["mode"] = displayModeName(displayMode);           // configured mode
  doc["effective"] = displayModeName(effectiveMode());   // what's on screen now
  doc["music_playing"] = statusMusicPlaying;
  doc["showing"] = (currentApp == APP_CLAUDE) ? "claude" : "codex";
  doc["last_update_s"] = everPolled ? (long)((millis() - lastSuccessMs) / 1000) : -1;
  doc["sprite_rev"] = spriteRev;
  doc["brightness"] = brightness;
  doc["quota_display"] = showQuotaRemaining ? "remaining" : "used";
  doc["wired"] = wiredActive();
  doc["fw"] = FW_VERSION;
  JsonObject c = doc["claude"].to<JsonObject>();
  c["status"] = claudeStatus.status;
  c["custom_sprite"] = claudeCustom;
  c["w"] = CLAUDE_SPRITE_W;
  c["h"] = CLAUDE_SPRITE_H;
  JsonObject x = doc["codex"].to<JsonObject>();
  x["status"] = codexStatus.status;
  x["custom_sprite"] = codexCustom;
  x["w"] = CODEX_SPRITE_W;
  x["h"] = CODEX_SPRITE_H;
  String out;
  serializeJson(doc, out);
  webServer.send(200, "application/json", out);
}

void handleApiDisplay() {
  String mode = webServer.arg("mode");
  if (mode == "auto") displayMode = MODE_AUTO;
  else if (mode == "claude") displayMode = MODE_CLAUDE;
  else if (mode == "codex") displayMode = MODE_CODEX;
  else if (mode == "net") displayMode = MODE_NET;
  else if (mode == "music") displayMode = MODE_MUSIC;
  else {
    webServer.send(400, "text/plain", "mode must be auto|claude|codex|net|music");
    return;
  }
  saveDisplayMode();
  Serial.printf("[api] display mode = %s\n", mode.c_str());
  if (displayMode == MODE_NET) {
    netChromeDrawn = false;
    lastNetPollMs = 0; // poll + draw on the next loop tick
  } else if (displayMode == MODE_MUSIC) {
    musicChromeDrawn = false;
    lastMusicPollMs = 0; // poll + draw on the next loop tick
  } else {
    appRedrawRequested = true;
  }
  webServer.send(200, "text/plain", "ok");
}

void handleApiBrightness() {
  String levelArg = webServer.arg("level");
  if (levelArg.length() == 0) {
    webServer.send(400, "text/plain", "missing level (0-100)");
    return;
  }
  int level = levelArg.toInt();
  if (level < 0) level = 0;
  if (level > 100) level = 100;
  brightness = level;
  applyBrightness();
  saveBrightness();
  Serial.printf("[api] brightness = %d\n", brightness);
  webServer.send(200, "text/plain", "ok");
}

void handleApiQuotaDisplay() {
  String mode = webServer.arg("mode");
  if (mode == "used") showQuotaRemaining = false;
  else if (mode == "remaining") showQuotaRemaining = true;
  else {
    webServer.send(400, "text/plain", "mode must be used|remaining");
    return;
  }
  saveQuotaDisplay();
  if (effectiveMode() != MODE_NET && effectiveMode() != MODE_MUSIC) appRedrawRequested = true;
  Serial.printf("[api] quota display = %s\n", mode.c_str());
  webServer.send(200, "text/plain", "ok");
}

void handleApiBridge() {
  String newHost = webServer.arg("host");
  newHost.trim();
  if (newHost.length() == 0) {
    webServer.send(400, "text/plain", "missing host");
    return;
  }
  bridgeHost = newHost;
  saveBridgeHost(bridgeHost);
  Serial.printf("[api] bridge host = '%s'\n", bridgeHost.c_str());
  webServer.send(200, "text/plain", "ok");
  lastPollMs = 0; // poll the new bridge on the next loop tick
}

// Streams the animation currently in use for a slot, in the same wire format
// as the custom .bin: [1 byte frame count][RGB565 frames...]. Lets the Mac
// app mirror exactly what the device is showing (custom upload or built-in).
void handleSpriteRaw(ActiveApp slot) {
  bool custom = (slot == APP_CLAUDE) ? claudeCustom : codexCustom;
  const char *binPath = (slot == APP_CLAUDE) ? CLAUDE_SPRITE_FILE : CODEX_SPRITE_FILE;
  if (custom) {
    File f = LittleFS.open(binPath, "r");
    if (f) {
      webServer.streamFile(f, "application/octet-stream");
      f.close();
      return;
    }
  }
  int frames = (slot == APP_CLAUDE) ? CLAUDE_SPRITE_FRAMES : CODEX_SPRITE_FRAMES;
  int w = (slot == APP_CLAUDE) ? CLAUDE_SPRITE_W : CODEX_SPRITE_W;
  int h = (slot == APP_CLAUDE) ? CLAUDE_SPRITE_H : CODEX_SPRITE_H;
  const uint16_t *const *arr = (slot == APP_CLAUDE) ? claude_sprite_frames : codex_sprite_frames;
  size_t frameBytes = (size_t)w * h * 2;
  webServer.setContentLength(1 + (size_t)frames * frameBytes);
  webServer.send(200, "application/octet-stream", "");
  uint8_t cnt = (uint8_t)frames;
  webServer.sendContent((const char *)&cnt, 1);
  for (int i = 0; i < frames; i++) {
    webServer.sendContent_P((PGM_P)arr[i], frameBytes);
    yield();
  }
}

// Removes a custom sprite so the compiled-in default animation comes back.
void handleSpriteReset(ActiveApp slot) {
  const char *binPath = (slot == APP_CLAUDE) ? CLAUDE_SPRITE_FILE : CODEX_SPRITE_FILE;
  LittleFS.remove(binPath);
  spriteRev++;
  loadCustomSpriteState();
  if (slot == APP_CLAUDE) claudeFrame = 0;
  else codexFrame = 0;
  if (currentApp == slot) drawActiveApp();
  webServer.send(200, "text/plain", "ok");
}

void handleResetWifi() {
  webServer.send(200, "text/html", "<html><body>Resetting WiFi, device will restart...</body></html>");
  delay(200);
  WiFiManager wm;
  wm.resetSettings();
  ESP.restart();
}

// ---------- on-device GIF decode (AnimatedGIF) ----------
// AnimatedGIF hands us the image one horizontal line at a time (via the draw
// callback) at the GIF's native resolution, so we never need a full-canvas
// buffer. We nearest-neighbour rescale into the target slot size and stream the
// result straight to the .bin one target row at a time. Because the .bin can't
// hold a whole frame in RAM to composite against, GIFs that only re-encode a
// changed sub-rectangle (the common optimizer output, disposal method 1) are
// composited by reading the *previous frame's* rows back out of the .bin we're
// writing. (Disposal method 2 "restore to background" isn't distinguished -
// uncovered pixels keep the previous frame instead of clearing; fine for the
// looping character animations this is for.)

struct GifDecodeCtx {
  int canvasW, canvasH; // GIF native size
  int targetW, targetH; // slot size we're rescaling down to
  size_t rowBytes;      // targetW * 2
  File out;             // output .bin, written sequentially
  File prevFile;        // previous frame in the .bin, read sequentially for compositing
  bool hasPrev;         // false for frame 0 (nothing to composite over -> black)
  int producedRow;      // next target row still owed for the current frame
};

static File gifReadFile; // one decode runs at a time, so a single handle is fine

void *gifOpenCB(const char *fname, int32_t *pSize) {
  gifReadFile = LittleFS.open(fname, "r");
  if (!gifReadFile) return nullptr;
  *pSize = (int32_t)gifReadFile.size();
  return (void *)&gifReadFile;
}

void gifCloseCB(void *) {
  if (gifReadFile) gifReadFile.close();
}

int32_t gifReadCB(GIFFILE *pFile, uint8_t *pBuf, int32_t iLen) {
  File *f = (File *)pFile->fHandle;
  // Read all remaining bytes. Dropping the final byte breaks GIFs whose
  // trailer/data boundary is significant.
  int32_t remaining = pFile->iSize - pFile->iPos;
  if (remaining < iLen) iLen = remaining;
  if (iLen <= 0) return 0;
  int32_t n = (int32_t)f->read(pBuf, iLen);
  pFile->iPos = (int32_t)f->position();
  return n;
}

int32_t gifSeekCB(GIFFILE *pFile, int32_t iPosition) {
  File *f = (File *)pFile->fHandle;
  f->seek(iPosition);
  pFile->iPos = iPosition;
  return iPosition;
}

// Loads the next previous-frame row into prevRowBuf (black if there's no
// previous frame). Reads are sequential and stay aligned with producedRow.
static void readPrevRow(GifDecodeCtx *ctx) {
  if (ctx->hasPrev)
    ctx->prevFile.read((uint8_t *)prevRowBuf, ctx->rowBytes);
  else
    memset(prevRowBuf, 0, ctx->rowBytes);
}

// Appends the current rowBuf as the next output row.
static void emitRow(GifDecodeCtx *ctx) {
  ctx->out.write((const uint8_t *)rowBuf, ctx->rowBytes);
  ctx->producedRow++;
}

// Emits a row that this frame doesn't touch: a straight copy of the previous
// frame (top/bottom gaps of a partial frame).
static void emitPrevRow(GifDecodeCtx *ctx) {
  readPrevRow(ctx);
  memcpy(rowBuf, prevRowBuf, ctx->rowBytes);
  emitRow(ctx);
}

// Rescales one decoded native line into target rows, compositing over the
// previous frame, and streams every target row it can now finalize.
void gifDrawCB(GIFDRAW *pDraw) {
  GifDecodeCtx *ctx = (GifDecodeCtx *)pDraw->pUser;
  int sy = pDraw->iY + pDraw->y; // absolute source line on the GIF canvas
  if (sy < 0 || sy >= ctx->canvasH) return;

  const uint8_t *pal = pDraw->pPalette24; // RGB888, 256 entries
  const uint8_t *src = pDraw->pPixels;    // palette indices, one per pixel of this line
  bool hasTrans = pDraw->ucHasTransparency;
  uint8_t transIdx = pDraw->ucTransparent;

  // Emit every target row whose nearest source line is <= sy and isn't done yet.
  while (ctx->producedRow < ctx->targetH) {
    int ty = ctx->producedRow;
    int srcRow = (int)((long)ty * ctx->canvasH / ctx->targetH);
    if (srcRow > sy) break;                       // needs a later source line
    if (srcRow < sy) { emitPrevRow(ctx); continue; } // source line was skipped -> previous frame

    // srcRow == sy: composite this source line over the previous frame's row.
    readPrevRow(ctx);
    memcpy(rowBuf, prevRowBuf, ctx->rowBytes);
    for (int tx = 0; tx < ctx->targetW; tx++) {
      int sx = (int)((long)tx * ctx->canvasW / ctx->targetW);
      int rel = sx - pDraw->iX;
      if (rel < 0 || rel >= pDraw->iWidth) continue; // outside this frame's rect: keep previous pixel
      uint8_t idx = src[rel];
      if (hasTrans && idx == transIdx) continue;     // transparent: keep previous pixel
      uint8_t r = pal[idx * 3 + 0], g = pal[idx * 3 + 1], b = pal[idx * 3 + 2];
      uint16_t val = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
      rowBuf[tx] = (uint16_t)(((val & 0xFF) << 8) | (val >> 8)); // byte-swap to match convert_sprites.py
    }
    emitRow(ctx);
  }
}

// Decodes gifPath into binPath in the [count][frames...] wire format the
// display path reads. Returns false on open/decode failure.
bool decodeGifToBin(const char *gifPath, const char *binPath, int targetW, int targetH) {
  // AnimatedGIF's internal state (~24KB of LZW/line/palette buffers) is big, so
  // allocate it on the heap only for the duration of a decode rather than
  // paying for it in .bss for the whole uptime.
  AnimatedGIF *gif = new AnimatedGIF();
  if (!gif) return false;
  gif->begin(GIF_PALETTE_RGB888);
  if (!gif->open(gifPath, gifOpenCB, gifCloseCB, gifReadCB, gifSeekCB, gifDrawCB)) {
    Serial.printf("[gif] open failed err=%d\n", gif->getLastError());
    delete gif;
    return false;
  }

  GifDecodeCtx ctx;
  ctx.canvasW = gif->getCanvasWidth();
  ctx.canvasH = gif->getCanvasHeight();
  ctx.targetW = targetW;
  ctx.targetH = targetH;
  ctx.rowBytes = (size_t)targetW * 2;
  ctx.hasPrev = false;
  size_t frameBytes = (size_t)targetW * targetH * 2;

  ctx.out = LittleFS.open(binPath, "w");
  if (!ctx.out) {
    gif->close();
    delete gif;
    return false;
  }
  ctx.out.write((uint8_t)0); // placeholder frame count, patched once we know the total

  uint8_t count = 0;
  int delayMs = 0, more = 1;
  while (count < MAX_CUSTOM_FRAMES) {
    ctx.producedRow = 0;
    ctx.hasPrev = false;
    if (count > 0) {
      ctx.out.flush(); // make the just-written previous frame visible to the read handle
      ctx.prevFile = LittleFS.open(binPath, "r");
      ctx.hasPrev = (bool)ctx.prevFile;
      if (ctx.hasPrev) ctx.prevFile.seek(1 + (size_t)(count - 1) * frameBytes);
    }

    more = gif->playFrame(false, &delayMs, &ctx);

    if (more >= 0) {
      // finalize any bottom rows this frame never touched
      while (ctx.producedRow < ctx.targetH) emitPrevRow(&ctx);
      count++;
    }
    if (ctx.prevFile) ctx.prevFile.close();
    if (more <= 0) break; // 0 = last frame, <0 = decode error
    yield();              // feed the WDT between frames
  }
  gif->close();
  delete gif;
  ctx.out.close();

  if (count == 0) {
    LittleFS.remove(binPath);
    return false;
  }
  File patch = LittleFS.open(binPath, "r+");
  if (patch) {
    patch.seek(0);
    patch.write(count);
    patch.close();
  }
  Serial.printf("[gif] decoded %d frame(s) %dx%d -> %dx%d\n", count, ctx.canvasW, ctx.canvasH, targetW, targetH);
  return true;
}

// ---------- sprite upload (raw .gif -> on-device decode) ----------
// ESP8266WebServer fully buffers a plain POST body into a heap String before
// the handler runs, which a whole GIF would blow RAM on - so we take the
// upload over its streaming multipart/HTTPUpload path, writing the raw .gif to
// LittleFS in small chunks, then decode it on the done callback.
File uploadFile;
bool spriteDecodePending = false;
ActiveApp spriteDecodeSlot = APP_CLAUDE;
bool spriteDecodeWorking = true;

void handleSpriteUploadChunk(const char *gifPath) {
  HTTPUpload &upload = webServer.upload();
  if (upload.status == UPLOAD_FILE_START) {
    uploadFile = LittleFS.open(gifPath, "w");
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadFile) uploadFile.write(upload.buf, upload.currentSize);
  } else if (upload.status == UPLOAD_FILE_END || upload.status == UPLOAD_FILE_ABORTED) {
    if (uploadFile) uploadFile.close();
  }
}

void handleSpriteUploadDone(ActiveApp slot, bool working = true) {
  const char *gifPath = (slot == APP_CLAUDE) ? (working ? "/c_work.gif" : "/c_idle.gif")
                                            : (working ? "/x_work.gif" : "/x_idle.gif");
  if (spriteDecodePending) {
    webServer.send(409, "text/plain", "another sprite is still decoding");
    return;
  }
  File f = LittleFS.open(gifPath, "r");
  bool uploaded = f && f.size() > 0;
  if (f) f.close();
  if (!uploaded) {
    webServer.send(400, "text/plain", "empty GIF upload");
    return;
  }
  // Do not hold the HTTP connection open during AnimatedGIF/LittleFS work.
  spriteDecodeSlot = slot;
  spriteDecodeWorking = working;
  spriteDecodePending = true;
  webServer.send(202, "text/plain", "accepted; GIF decoding in background");
  Serial.printf("[sprite] upload received (%s), queued for background decode\n", gifPath);
}

void processPendingSpriteDecode() {
  if (!spriteDecodePending) return;
  spriteDecodePending = false;
  ActiveApp slot = spriteDecodeSlot;
  bool working = spriteDecodeWorking;
  const char *gifPath = (slot == APP_CLAUDE) ? (working ? "/c_work.gif" : "/c_idle.gif")
                                            : (working ? "/x_work.gif" : "/x_idle.gif");
  const char *binPath = (slot == APP_CLAUDE) ? (working ? CLAUDE_SPRITE_FILE : CLAUDE_IDLE_FILE)
                                            : (working ? CODEX_SPRITE_FILE : CODEX_IDLE_FILE);
  int tw = (slot == APP_CLAUDE) ? CLAUDE_SPRITE_W : CODEX_SPRITE_W;
  int th = (slot == APP_CLAUDE) ? CLAUDE_SPRITE_H : CODEX_SPRITE_H;
  bool ok = decodeGifToBin(gifPath, binPath, tw, th);
  LittleFS.remove(gifPath);
  if (ok) {
    spriteRev++;
    loadCustomSpriteState();
    if (slot == APP_CLAUDE) claudeFrame = 0;
    else codexFrame = 0;
    if (currentApp == slot) drawActiveApp();
    Serial.println("[sprite] gif decoded & applied");
  } else {
    Serial.println("[sprite] gif decode FAILED");
  }
}

void setupWebServer() {
  webServer.on("/", HTTP_GET, handleRoot);
  webServer.on("/save", HTTP_POST, handleSave);
  webServer.on("/reset-wifi", HTTP_POST, handleResetWifi);
  webServer.on("/api/info", HTTP_GET, handleApiInfo);
  webServer.on("/api/display", HTTP_POST, handleApiDisplay);
  webServer.on("/api/bridge", HTTP_POST, handleApiBridge);
  webServer.on("/api/brightness", HTTP_POST, handleApiBrightness);
  webServer.on("/api/quota-display", HTTP_POST, handleApiQuotaDisplay);
  webServer.on("/sprite/claude/reset", HTTP_POST, []() { handleSpriteReset(APP_CLAUDE); });
  webServer.on("/sprite/codex/reset", HTTP_POST, []() { handleSpriteReset(APP_CODEX); });
  webServer.on("/sprite/claude/raw", HTTP_GET, []() { handleSpriteRaw(APP_CLAUDE); });
  webServer.on("/sprite/codex/raw", HTTP_GET, []() { handleSpriteRaw(APP_CODEX); });
  webServer.on(
      "/sprite/claude", HTTP_POST, []() { handleSpriteUploadDone(APP_CLAUDE); },
      []() { handleSpriteUploadChunk(CLAUDE_GIF_FILE); });
  webServer.on(
      "/sprite/codex", HTTP_POST, []() { handleSpriteUploadDone(APP_CODEX); },
      []() { handleSpriteUploadChunk(CODEX_GIF_FILE); });
  webServer.on("/sprite/claude/work", HTTP_POST, []() { handleSpriteUploadDone(APP_CLAUDE, true); },
               []() { handleSpriteUploadChunk("/c_work.gif"); });
  webServer.on("/sprite/claude/idle", HTTP_POST, []() { handleSpriteUploadDone(APP_CLAUDE, false); },
               []() { handleSpriteUploadChunk("/c_idle.gif"); });
  webServer.on("/sprite/codex/work", HTTP_POST, []() { handleSpriteUploadDone(APP_CODEX, true); },
               []() { handleSpriteUploadChunk("/x_work.gif"); });
  webServer.on("/sprite/codex/idle", HTTP_POST, []() { handleSpriteUploadDone(APP_CODEX, false); },
               []() { handleSpriteUploadChunk("/x_idle.gif"); });
  webServer.begin();
  Serial.printf("[web] admin server listening on http://%s/\n", WiFi.localIP().toString().c_str());
}

// Keep HTTP processing off the Arduino UI loop. ESP32 has FreeRTOS tasks
// rather than separate processes; pin the lightweight WebServer task to the
// other core so TFT SPI drawing cannot starve management requests.
void webServerTask(void *) {
  for (;;) {
    webServer.handleClient();
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

void startWebServerIfNeeded() {
  if (webServerStarted || WiFi.status() != WL_CONNECTED) return;
  setupWebServer();
  webServerStarted = true;
  xTaskCreatePinnedToCore(webServerTask, "http", 6144, nullptr, 1, nullptr, 0);
}

// ---------- Arduino entry points ----------

void setup() {
  Serial.setRxBufferSize(2048);
  Serial.begin(115200);
  // The M5 custom partition is explicitly named "littlefs". A freshly
  // flashed volume is blank, so format that exact partition once on failure
  // and then mount it again. Passing the label avoids Core-specific automatic
  // partition discovery that can miss the custom label.
  const char *littlefsLabel = "littlefs";
  bool fsMounted = LittleFS.begin(false, "/littlefs", 10, littlefsLabel);
  if (!fsMounted) {
    Serial.println("[fs] initial LittleFS mount failed; formatting partition");
    if (LittleFS.format()) fsMounted = LittleFS.begin(false, "/littlefs", 10, littlefsLabel);
  }
  if (!fsMounted) {
    Serial.println("[fs] LittleFS mount failed; persistent settings unavailable");
  } else {
    Serial.println("[fs] LittleFS mounted");
  }
  loadBridgeHost();
  loadBrightness();
  loadQuotaDisplay();
  loadDisplayMode();
  loadCustomSpriteState();

  tft.init();
  initM5GoV26Panel();
  // M5Stack's official Core1 display wrapper uses rotation 1 after selecting
  // the ILI9342C init path, yielding the expected 320x240 landscape canvas.
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  ledcSetup(0, BRIGHTNESS_PWM_FREQ, 8);
  ledcAttachPin(TFT_BL, 0);
  applyBrightness();
  ledcSetup(SPEAKER_CHANNEL, 2000, 8);
  ledcAttachPin(SPEAKER_PIN, SPEAKER_CHANNEL);
  ledcWriteTone(SPEAKER_CHANNEL, 0);

  setupWiFi();
  if (WiFi.status() == WL_CONNECTED) {
    startWebServerIfNeeded();
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("WiFi connected", 8, 70, 2);
    tft.drawString("Admin page:", 8, 100, 2);
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.drawString("http://" + WiFi.localIP().toString(), 8, 125, 2);
    delay(3000);
    showMainUiIfNeeded();
    pollBridge();
  }
}

void loop() {
  wifiManager.process();
  pumpSerial();
  if (!webServerStarted && WiFi.status() == WL_CONNECTED) {
    startWebServerIfNeeded();
    showMainUiIfNeeded();
    lastPollMs = 0;
  }
  if (!mainUiShown) return;

  updateBeep();
  processPendingSpriteDecode();
  unsigned long nowMs = millis();

  // Effective mode may differ from the configured one (AUTO -> music while
  // audio plays). On a transition, reset the incoming mode's chrome so it
  // repaints cleanly, and repaint the pet immediately when returning to it.
  DisplayMode eff = effectiveMode();
  if (appRedrawRequested && eff != MODE_NET && eff != MODE_MUSIC) {
    appRedrawRequested = false;
    updateActiveApp();
    drawActiveApp();
  }
  if (eff != lastEffectiveMode) {
    lastEffectiveMode = eff;
    if (eff == MODE_NET) {
      netChromeDrawn = false;
      lastNetPollMs = 0;
    } else if (eff == MODE_MUSIC) {
      musicChromeDrawn = false;
      lastMusicPollMs = 0;
    } else {
      updateActiveApp();
      drawActiveApp();
    }
  }

  if (eff == MODE_NET) {
    // net-speed mode: rendering (constant-rate sweep) is independent of the
    // bridge polls that refill its sample queue
    if (nowMs - lastNetDrawMs >= NET_DRAW_INTERVAL_MS) {
      lastNetDrawMs = nowMs;
      netDrawTick();
    }
    if (nowMs - lastNetPollMs >= NET_POLL_INTERVAL_MS) {
      lastNetPollMs = nowMs;
      pollNet();
    }
  } else if (eff == MODE_MUSIC) {
    // music now-playing mode: cover art + track metadata from the bridge
    if (nowMs - lastMusicPollMs >= MUSIC_POLL_INTERVAL_MS) {
      lastMusicPollMs = nowMs;
      pollMusic();
    }
  } else {
    // sprite walk-cycle animation (only advances while that app is showing)
    if (nowMs - lastAnimMs >= ANIM_INTERVAL_MS) {
      lastAnimMs = nowMs;
      bool claudeWorking = claudeStatus.status == "working";
      bool codexWorking = codexStatus.status == "working";
      if (!claudeWorking) claudeLoopBridgePending = false;
      if (!codexWorking) codexLoopBridgePending = false;
      bool drewLoopBridge = false;
      if (showingCd != CD_NONE) {
        // countdown owns the center area: no sprite frames over it
      } else if (currentApp == APP_CLAUDE && claudeWorking) {
        int count = claudeFrameCount();
        if (claudeLoopBridgePending) {
          claudeFrame = 0;
          claudeLoopBridgePending = false;
          drawClaudeSprite(claudeFrame);
        } else if (count > 1 && claudeFrame + 1 >= count) {
          drawClaudeLoopBridge();
          claudeLoopBridgePending = true;
          drewLoopBridge = true;
        } else {
          claudeFrame = (claudeFrame + 1) % count;
          drawClaudeSprite(claudeFrame);
        }
      } else if (currentApp == APP_CODEX && codexWorking) {
        int count = codexFrameCount();
        if (codexLoopBridgePending) {
          codexFrame = 0;
          codexLoopBridgePending = false;
          drawCodexSprite(codexFrame);
        } else if (count > 1 && codexFrame + 1 >= count) {
          drawCodexLoopBridge();
          codexLoopBridgePending = true;
          drewLoopBridge = true;
        } else {
          codexFrame = (codexFrame + 1) % count;
          drawCodexSprite(codexFrame);
        }
      } else if (currentApp == APP_CLAUDE && claudeIdleCustom) {
        claudeFrame = (claudeFrame + 1) % claudeIdleFrameCount();
        drawClaudeSprite(claudeFrame, false);
      } else if (currentApp == APP_CLAUDE && claudeFrame != 0) {
        claudeFrame = 0;
        drawClaudeSprite(claudeFrame, false);
      } else if (currentApp == APP_CODEX && codexIdleCustom) {
        codexFrame = (codexFrame + 1) % codexIdleFrameCount();
        drawCodexSprite(codexFrame, false);
      } else if (currentApp == APP_CODEX && codexFrame != 0) {
        codexFrame = 0;
        drawCodexSprite(codexFrame, false);
      }
      if (drewLoopBridge) {
        // The bridge is a transition, not another full-duration GIF frame.
        lastAnimMs = nowMs - (ANIM_INTERVAL_MS - ANIM_LOOP_BRIDGE_MS);
      }
    }

    // countdown seconds tick locally between bridge polls
    static unsigned long lastCdTickMs = 0;
    if (showingCd != CD_NONE && nowMs - lastCdTickMs >= 1000) {
      lastCdTickMs = nowMs;
      drawCountdown(false);
    }

    // "urgent" flash toggle (independent, faster cadence)
    if (nowMs - lastFlashMs >= FLASH_INTERVAL_MS) {
      lastFlashMs = nowMs;
      flashOn = !flashOn;
      if (bridgeStale()) {
        redrawRingOnly();
      } else if (currentAppNeedsInput()) {
        // approval needed: blink the whole border red, restore the quota ring
        // on the off-phase so it doesn't erase the normal chrome permanently
        if (flashOn) drawFullBorder(TFT_RED);
        else redrawRingOnly();
      }
    }

    // alternate which app is shown when neither/both are uniquely working
    if (updateActiveApp()) {
      drawActiveApp();
    }
  }

  // While serial frames are fresh, use the wired payloads exclusively to
  // avoid duplicate updates and make AP client isolation irrelevant.
  if (nowMs - lastPollMs >= BRIDGE_POLL_INTERVAL_MS) {
    lastPollMs = nowMs;
    if (!wiredActive()) pollBridge();
  }
}
