#include <M5Unified.h>
#include <WiFi.h>
#include <HTTPClient.h>

#include "secrets.h"

// ============================================================================
// Donald Boy — Stage 2
// Press BtnA → record 3s → POST raw PCM to laptop server → play the response.
// Server is just an echo for now; later it will Donald-ify the audio.
// ============================================================================

// ----- Audio config ---------------------------------------------------------

constexpr int    SAMPLE_RATE      = 16000;
constexpr int    RECORD_SECONDS   = 5;    // how long the user can talk
constexpr int    MAX_PLAY_SECONDS = 15;   // how long Trump can reply
constexpr size_t RECORD_SAMPLES   = SAMPLE_RATE * RECORD_SECONDS;
constexpr size_t RECORD_BYTES     = RECORD_SAMPLES * sizeof(int16_t);
constexpr size_t MAX_AUDIO_SAMPLES = SAMPLE_RATE * MAX_PLAY_SECONDS;
constexpr size_t MAX_AUDIO_BYTES   = MAX_AUDIO_SAMPLES * sizeof(int16_t);

// Audio buffer lives in 8 MB PSRAM (allocated in setup) — frees the precious
// internal DRAM for WiFi/HTTP and lets us afford 12s playback windows.
int16_t* audioBuffer = nullptr;
size_t   audioLen    = 0;  // valid samples currently in the buffer

// Last received dialog text (rendered under the face during ST_TALKING).
static char lastUserSaid[256]  = "";
static char lastTrumpSaid[256] = "";

// Hardware volume cycle (BtnB). Order is "press to lower" — wraps from mute
// back to loudest. Stays in RAM only; resets to LOUDEST on every boot.
constexpr uint8_t VOLUME_LEVELS[] = { 255, 192, 128, 64, 0 };  // 100/75/50/25/mute
constexpr size_t  NUM_VOL_LEVELS  = sizeof(VOLUME_LEVELS) / sizeof(VOLUME_LEVELS[0]);
uint8_t           volumeIndex     = 0;  // start at loudest

// Decode an URL-encoded String into a fixed-size C buffer.
void urlDecodeInto(const String& src, char* dst, size_t maxLen) {
  size_t di = 0;
  size_t srcLen = src.length();
  for (size_t si = 0; si < srcLen && di < maxLen - 1; ) {
    char c = src.charAt(si);
    if (c == '%' && si + 2 < srcLen) {
      char hex[3] = { src.charAt(si + 1), src.charAt(si + 2), 0 };
      dst[di++] = (char)strtol(hex, nullptr, 16);
      si += 3;
    } else if (c == '+') {
      dst[di++] = ' ';
      si++;
    } else {
      dst[di++] = c;
      si++;
    }
  }
  dst[di] = '\0';
}

// ----- Animation engine -----------------------------------------------------

struct Frame {
  const char* hair;
  const char* body;
  uint32_t    durationMs;
};

struct Animation {
  const Frame* frames;
  size_t       count;
  const char*  label;  // status text shown beneath the face (nullptr = none)
};

const Frame IDLE_FRAMES[] = {
  { "〜〜", "¯＼_(ツ)_/¯",  2500 },
  { "〜〜", "¯＼_(--)_/¯",  180  },
  { "〜〜", "¯＼_(ツ)_/¯",  1800 },
};

const Frame LISTENING_FRAMES[] = {
  { "〜〜", "¯＼_(O_O)_/¯", 220 },
  { "〜〜", "¯＼_(o_o)_/¯", 220 },
};

const Frame THINKING_FRAMES[] = {
  { "〜〜", "¯＼_(?_?)_/¯", 300 },  // both ? — puzzled
  { "〜〜", "¯＼_(?_o)_/¯", 250 },  // glance right
  { "〜〜", "¯＼_(?_?)_/¯", 300 },
  { "〜〜", "¯＼_(o_?)_/¯", 250 },  // glance left
};

const Frame TALKING_FRAMES[] = {
  { "〜〜", "¯＼_(-o-)_/¯", 130 },
  { "〜〜", "¯＼_(-.-)_/¯", 130 },
  { "〜〜", "¯＼_(-O-)_/¯", 130 },
  { "〜〜", "¯＼_(._.)_/¯", 130 },
};

const Animation IDLE_ANIM      = { IDLE_FRAMES,      sizeof(IDLE_FRAMES)      / sizeof(Frame), nullptr           };
const Animation LISTENING_ANIM = { LISTENING_FRAMES, sizeof(LISTENING_FRAMES) / sizeof(Frame), "listening..."    };
const Animation THINKING_ANIM  = { THINKING_FRAMES,  sizeof(THINKING_FRAMES)  / sizeof(Frame), "donaldifying..." };
const Animation TALKING_ANIM   = { TALKING_FRAMES,   sizeof(TALKING_FRAMES)   / sizeof(Frame), "speaking..."     };

void drawAsciiFrame(const Frame& f,
                    const char* label     = nullptr,
                    const char* userText  = nullptr,
                    const char* trumpText = nullptr) {
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextSize(1);

  const bool hasDialog = (userText && *userText) || (trumpText && *trumpText);

  // Big-font metrics for hair + body
  M5.Display.setTextDatum(middle_center);
  M5.Display.setFont(&fonts::lgfxJapanGothic_24);
  const int bigH = M5.Display.fontHeight();

  int bodyLines = 1;
  for (const char* q = f.body; *q; ++q) if (*q == '\n') bodyLines++;
  const int faceH = (1 + bodyLines) * bigH;

  const int smallH = 16;
  const int gapH   = 6;
  const int labelStackH = label ? (gapH + smallH) : 0;

  const int cx = M5.Display.width() / 2;
  // When dialog is present, push face to the top and let dialog have the bottom.
  // Otherwise, vertically centre the whole face+label block.
  const int yTop = hasDialog
      ? 4
      : (M5.Display.height() - (faceH + labelStackH)) / 2;

  // 1) Orange hair tuft
  int y = yTop + bigH / 2;
  M5.Display.setTextColor(TFT_ORANGE, TFT_BLACK);
  M5.Display.drawString(f.hair, cx, y);
  y += bigH;

  // 2) Green body
  M5.Display.setTextColor(TFT_GREEN, TFT_BLACK);
  const char* p = f.body;
  char line[128];
  while (*p) {
    int i = 0;
    while (*p && *p != '\n' && i < (int)sizeof(line) - 1) line[i++] = *p++;
    line[i] = '\0';
    if (*p == '\n') p++;
    M5.Display.drawString(line, cx, y);
    y += bigH;
  }

  // 3) Status label
  int blockBottom = yTop + faceH;
  if (label) {
    M5.Display.setFont(&fonts::lgfxJapanGothic_16);
    M5.Display.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    int labelY = blockBottom + gapH + M5.Display.fontHeight() / 2;
    M5.Display.drawString(label, cx, labelY);
    blockBottom = labelY + M5.Display.fontHeight() / 2;
  }

  // 4) Dialog block — small font, word-wrapped, left-anchored
  if (hasDialog) {
    M5.Display.setFont(&fonts::lgfxJapanGothic_12);
    M5.Display.setTextDatum(top_left);
    M5.Display.setTextWrap(true, false);

    int dialogY = blockBottom + 8;

    if (userText && *userText) {
      M5.Display.setTextColor(TFT_CYAN, TFT_BLACK);
      M5.Display.setCursor(3, dialogY);
      M5.Display.printf("you: %s", userText);
      dialogY = M5.Display.getCursorY() + 6;
    }
    if (trumpText && *trumpText) {
      M5.Display.setTextColor(TFT_YELLOW, TFT_BLACK);
      M5.Display.setCursor(3, dialogY);
      M5.Display.printf("him: %s", trumpText);
    }
  }
}

// Plain status text shown during boot / errors (no fancy face).
void drawStatus(const char* line1, const char* line2 = nullptr) {
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setFont(&fonts::lgfxJapanGothic_16);
  M5.Display.setTextSize(1);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextColor(TFT_GREEN, TFT_BLACK);

  int cx = M5.Display.width()  / 2;
  int cy = M5.Display.height() / 2;
  int lineH = M5.Display.fontHeight();

  if (line2) {
    M5.Display.drawString(line1, cx, cy - lineH / 2);
    M5.Display.drawString(line2, cx, cy + lineH / 2);
  } else {
    M5.Display.drawString(line1, cx, cy);
  }
}

// ----- State machine --------------------------------------------------------

enum AppState { ST_IDLE, ST_LISTENING, ST_THINKING, ST_TALKING };

AppState         appState       = ST_IDLE;
const Animation* currentAnim    = &IDLE_ANIM;
size_t           currentFrame   = 0;
uint32_t         frameStartedAt = 0;

// One-shot full-screen "VOLUME ▮▮▮▯▯ 60%" flash, used when BtnB cycles volume.
void drawVolumeFlash() {
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextSize(1);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setFont(&fonts::lgfxJapanGothic_24);

  const int cx = M5.Display.width()  / 2;
  const int cy = M5.Display.height() / 2;

  M5.Display.setTextColor(TFT_ORANGE, TFT_BLACK);
  M5.Display.drawString("VOLUME", cx, cy - 36);

  // Bar: outline + fill scaled to current level
  const int barW = 90;
  const int barH = 14;
  const int barX = cx - barW / 2;
  const int barY = cy - barH / 2;
  M5.Display.drawRect(barX, barY, barW, barH, TFT_GREEN);
  const int filled = (VOLUME_LEVELS[volumeIndex] * (barW - 4)) / 255;
  if (filled > 0) {
    M5.Display.fillRect(barX + 2, barY + 2, filled, barH - 4, TFT_GREEN);
  }

  // Numeric label below bar (or "MUTE" at zero)
  char buf[16];
  uint8_t pct = (VOLUME_LEVELS[volumeIndex] * 100) / 255;
  if (pct == 0) snprintf(buf, sizeof(buf), "MUTE");
  else          snprintf(buf, sizeof(buf), "%u%%", pct);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.drawString(buf, cx, cy + 32);
}

void switchAnim(const Animation* a) {
  currentAnim    = a;
  currentFrame   = 0;
  frameStartedAt = millis();
  const bool talking = (appState == ST_TALKING);
  drawAsciiFrame(currentAnim->frames[0], currentAnim->label,
                 talking ? lastUserSaid  : nullptr,
                 talking ? lastTrumpSaid : nullptr);
}

void tickAnim() {
  uint32_t now = millis();
  const Frame& f = currentAnim->frames[currentFrame];
  if (now - frameStartedAt >= f.durationMs) {
    currentFrame   = (currentFrame + 1) % currentAnim->count;
    frameStartedAt = now;
    const bool talking = (appState == ST_TALKING);
    drawAsciiFrame(currentAnim->frames[currentFrame], currentAnim->label,
                   talking ? lastUserSaid  : nullptr,
                   talking ? lastTrumpSaid : nullptr);
  }
}

// ----- WiFi -----------------------------------------------------------------

bool connectWiFi(uint32_t timeoutMs = 15000) {
  drawStatus("WiFi...", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeoutMs) {
    delay(200);
  }
  return WiFi.status() == WL_CONNECTED;
}

// ----- HTTP round-trip ------------------------------------------------------
// Sends `audioBuffer` (AUDIO_BYTES) to the server, overwrites it with the
// response, sets `audioLen` to the number of int16 samples received.
// Returns true on success.
bool postAudio() {
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(15000);

  if (!http.begin(client, SERVER_URL)) return false;
  http.addHeader("Content-Type", "application/octet-stream");

  // Ask HTTPClient to capture these response headers (default: discards them).
  static const char* dialogHeaders[] = { "X-User-Said", "X-Trump-Said" };
  http.collectHeaders(dialogHeaders, 2);

  int status = http.POST((uint8_t*)audioBuffer, RECORD_BYTES);
  int contentLen = http.getSize();
  Serial.printf("[postAudio] sent=%u status=%d Content-Length=%d\n",
                (unsigned)RECORD_BYTES, status, contentLen);
  if (status != 200) {
    http.end();
    return false;
  }

  // Pull the dialog headers (URL-encoded). Empty string is safe.
  urlDecodeInto(http.header("X-User-Said"),  lastUserSaid,  sizeof(lastUserSaid));
  urlDecodeInto(http.header("X-Trump-Said"), lastTrumpSaid, sizeof(lastTrumpSaid));

  // Stream response body back into the buffer (overwriting our request).
  WiFiClient* stream = http.getStreamPtr();
  size_t totalRead = 0;
  uint32_t lastByteAt = millis();

  while (totalRead < MAX_AUDIO_BYTES) {
    int avail = stream->available();
    if (avail > 0) {
      size_t toRead = min((size_t)avail, MAX_AUDIO_BYTES - totalRead);
      int n = stream->readBytes((uint8_t*)audioBuffer + totalRead, toRead);
      if (n > 0) {
        totalRead += n;
        lastByteAt = millis();
      }
    } else if (!http.connected() && totalRead > 0) {
      break;  // server closed cleanly after sending all it had
    } else if ((millis() - lastByteAt) > 5000) {
      break;  // 5s with no progress — give up
    } else {
      delay(1);
    }
  }

  http.end();
  audioLen = totalRead / sizeof(int16_t);
  Serial.printf("[postAudio] read=%u bytes -> audioLen=%u samples (%.2fs)\n",
                (unsigned)totalRead, (unsigned)audioLen,
                (float)audioLen / SAMPLE_RATE);
  return audioLen > 0;
}

// ----- Audio transitions ----------------------------------------------------

void enterIdle() {
  appState = ST_IDLE;
  switchAnim(&IDLE_ANIM);
}

// Bumps to the next preset, applies it immediately to the codec, flashes the
// indicator on screen, then re-renders the current face.
void cycleVolume() {
  volumeIndex = (volumeIndex + 1) % NUM_VOL_LEVELS;
  M5.Speaker.setVolume(VOLUME_LEVELS[volumeIndex]);
  Serial.printf("[volume] level=%u (%u/255)\n",
                volumeIndex, VOLUME_LEVELS[volumeIndex]);

  drawVolumeFlash();
  delay(900);  // hold the flash visible — DMA audio keeps playing in background

  // Restore current face after the flash.
  const bool talking = (appState == ST_TALKING);
  drawAsciiFrame(currentAnim->frames[currentFrame], currentAnim->label,
                 talking ? lastUserSaid  : nullptr,
                 talking ? lastTrumpSaid : nullptr);
  frameStartedAt = millis();  // don't immediately advance after the delay
}

void enterListening() {
  appState = ST_LISTENING;
  switchAnim(&LISTENING_ANIM);

  if (M5.Speaker.isEnabled()) M5.Speaker.end();
  M5.Mic.begin();
  M5.Mic.record(audioBuffer, RECORD_SAMPLES, SAMPLE_RATE);
}

void enterThinking() {
  appState = ST_THINKING;
  switchAnim(&THINKING_ANIM);

  if (M5.Mic.isEnabled()) M5.Mic.end();

  // Blocking HTTP round-trip. The thinking face is already drawn; animation
  // won't tick during this call, which is fine — face stays static.
  bool ok = postAudio();

  if (!ok) {
    drawStatus("Network error", "back to idle");
    delay(1200);
    enterIdle();
    return;
  }

  // Audio data now in audioBuffer with audioLen samples → start playback.
  appState = ST_TALKING;
  switchAnim(&TALKING_ANIM);
  M5.Speaker.begin();
  M5.Speaker.playRaw(audioBuffer, audioLen, SAMPLE_RATE);
}

// ----- Arduino lifecycle ----------------------------------------------------

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setRotation(0);
  M5.Speaker.setVolume(VOLUME_LEVELS[volumeIndex]);
  Serial.begin(115200);
  delay(50);
  Serial.println("\n[boot] Donald Boy starting");
  Serial.printf("[boot] free DRAM heap: %u bytes\n", ESP.getFreeHeap());
  Serial.printf("[boot] free PSRAM:     %u bytes\n", (unsigned)ESP.getFreePsram());

  // Allocate audio buffer in PSRAM (DRAM stays free for WiFi, etc.)
  audioBuffer = (int16_t*)ps_malloc(MAX_AUDIO_BYTES);
  if (!audioBuffer) {
    Serial.println("[boot] FATAL: ps_malloc failed for audio buffer");
    drawStatus("PSRAM alloc", "fail");
    while (true) delay(1000);
  }
  Serial.printf("[boot] audio buffer: %u bytes in PSRAM @ %p\n",
                (unsigned)MAX_AUDIO_BYTES, audioBuffer);

  if (connectWiFi()) {
    drawStatus("connected!", WiFi.localIP().toString().c_str());
    delay(1200);
  } else {
    drawStatus("WiFi failed", "press to retry");
    delay(1500);
    // Continue anyway — postAudio() will fail and we'll show network error.
  }

  enterIdle();
}

void loop() {
  M5.update();

  // BtnB cycles volume from any state.
  if (M5.BtnB.wasPressed()) cycleVolume();

  tickAnim();

  switch (appState) {
    case ST_IDLE:
      if (M5.BtnA.wasPressed()) enterListening();
      break;
    case ST_LISTENING:
      if (!M5.Mic.isRecording()) enterThinking();
      break;
    case ST_THINKING:
      // postAudio() was synchronous inside enterThinking(), so by the time we
      // get here the state has already advanced to ST_TALKING (or ST_IDLE on
      // failure). This case is intentionally empty.
      break;
    case ST_TALKING:
      if (!M5.Speaker.isPlaying()) enterIdle();
      break;
  }

  delay(10);
}
