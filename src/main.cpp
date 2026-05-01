#include <M5Unified.h>
#include <WiFi.h>
#include <HTTPClient.h>

#include "secrets.h"
#include "config.h"

// ============================================================================
// Donald Boy — Stage 2
// Press BtnA → record 3s → POST raw PCM to laptop server → play the response.
// Server is just an echo for now; later it will Donald-ify the audio.
// ============================================================================

// ----- Audio config ---------------------------------------------------------

// Architectural constant — must match the server. Tunables (RECORD_SECONDS,
// MAX_PLAY_SECONDS, etc.) live in include/config.h.
constexpr int    SAMPLE_RATE       = 16000;
constexpr size_t RECORD_SAMPLES    = SAMPLE_RATE * RECORD_SECONDS;
constexpr size_t RECORD_BYTES      = RECORD_SAMPLES * sizeof(int16_t);
constexpr size_t MAX_AUDIO_SAMPLES = SAMPLE_RATE * MAX_PLAY_SECONDS;
constexpr size_t MAX_AUDIO_BYTES   = MAX_AUDIO_SAMPLES * sizeof(int16_t);

// Audio buffer lives in 8 MB PSRAM (allocated in setup) — frees the precious
// internal DRAM for WiFi/HTTP and lets us afford 12s playback windows.
int16_t* audioBuffer = nullptr;
size_t   audioLen    = 0;  // valid samples currently in the buffer

// Last received dialog text (rendered under the face during ST_TALKING).
static char lastUserSaid[256]  = "";
static char lastTrumpSaid[256] = "";

// App state machine. Defined here (early) so drawMarquee() — which is defined
// before the rest of the state-machine code — can refer to ST_TALKING.
enum AppState { ST_IDLE, ST_LISTENING, ST_THINKING, ST_TALKING };
AppState appState = ST_IDLE;

// Hardware volume cycle (BtnB). Levels are defined in config.h.
// Index resets to 0 (loudest) on every boot — see config.h note.
uint8_t  volumeIndex   = 0;

// Marquee runtime state (constants live in config.h).
// marqueeStripY is set by drawAsciiFrame() so the strip sits right below the
// "you:" line (closes the visual gap). Default placement covers the
// "no user text yet" case before the first dialog arrives.
int      marqueeOffset = 0;
uint32_t lastMarqueeMs = 0;
int      marqueeStripY = 140;

// Word-wrapped reply for the vertical-scroll marquee.
// wrapTrumpText() populates this from lastTrumpSaid on each new reply.
constexpr int MAX_WRAPPED_LINES = 16;
constexpr int MAX_LINE_LEN      = 80;
char wrappedLines[MAX_WRAPPED_LINES][MAX_LINE_LEN];
int  numWrappedLines = 0;

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
      // Park the marquee strip right below the user text. Clamp to keep it
      // on screen even if user text wraps a lot.
      const int maxY = M5.Display.height() - MARQUEE_STRIP_H - 4;
      marqueeStripY  = M5.Display.getCursorY() + 10;
      if (marqueeStripY > maxY) marqueeStripY = maxY;
      // trumpText is rendered by drawMarquee() in that strip — not here.
    }
  }
}

// Word-wrap lastTrumpSaid into wrappedLines[] for the marquee. Greedy fit:
// keep adding words to the current line until the next would exceed maxW.
// Called once per reply, from postAudio() after the X-Trump-Said header
// is decoded into lastTrumpSaid.
void wrapTrumpText() {
  numWrappedLines = 0;
  if (lastTrumpSaid[0] == '\0') return;

  M5.Display.setFont(&fonts::lgfxJapanGothic_20);
  M5.Display.setTextSize(1);

  const int maxW = M5.Display.width() - 8;  // 4 px gutter each side

  // strtok_r mutates the buffer; copy first.
  char buf[sizeof(lastTrumpSaid)];
  strncpy(buf, lastTrumpSaid, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  char  current[MAX_LINE_LEN] = "";
  char* save = nullptr;
  for (char* word = strtok_r(buf, " ", &save);
       word && numWrappedLines < MAX_WRAPPED_LINES;
       word = strtok_r(nullptr, " ", &save)) {
    char attempt[MAX_LINE_LEN];
    if (current[0]) snprintf(attempt, sizeof(attempt), "%s %s", current, word);
    else            snprintf(attempt, sizeof(attempt), "%s",     word);

    if ((int)M5.Display.textWidth(attempt) > maxW && current[0]) {
      // Would overflow — commit current line, start fresh with this word.
      strncpy(wrappedLines[numWrappedLines], current, MAX_LINE_LEN - 1);
      wrappedLines[numWrappedLines][MAX_LINE_LEN - 1] = '\0';
      numWrappedLines++;
      strncpy(current, word, MAX_LINE_LEN - 1);
      current[MAX_LINE_LEN - 1] = '\0';
    } else {
      strncpy(current, attempt, MAX_LINE_LEN - 1);
      current[MAX_LINE_LEN - 1] = '\0';
    }
  }
  if (current[0] && numWrappedLines < MAX_WRAPPED_LINES) {
    strncpy(wrappedLines[numWrappedLines], current, MAX_LINE_LEN - 1);
    wrappedLines[numWrappedLines][MAX_LINE_LEN - 1] = '\0';
    numWrappedLines++;
  }
}

// Vertical-scroll marquee: shows ~3 wrapped lines visible at a time, the whole
// block scrolls upward continuously while ST_TALKING. When the last line exits
// the top of the strip, scroll resets so it enters from below again.
void drawMarquee() {
  if (appState != ST_TALKING)  return;
  if (numWrappedLines == 0)    return;

  uint32_t now = millis();
  if (now - lastMarqueeMs < MARQUEE_TICK_MS) return;
  lastMarqueeMs = now;

  const int screenW = M5.Display.width();
  const int screenH = M5.Display.height();
  const int stripY  = marqueeStripY;
  const int stripH  = screenH - stripY - 4;
  if (stripH < 16) return;

  M5.Display.fillRect(0, stripY, screenW, stripH, TFT_BLACK);

  M5.Display.setFont(&fonts::lgfxJapanGothic_20);
  M5.Display.setTextSize(1);
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextWrap(false, false);
  M5.Display.setTextColor(TFT_YELLOW, TFT_BLACK);

  const int lineH  = M5.Display.fontHeight() + 4;  // small inter-line padding
  const int totalH = numWrappedLines * lineH;

  // Short replies that fit entirely in the strip → static, no scroll.
  if (totalH <= stripH) {
    for (int i = 0; i < numWrappedLines; ++i) {
      M5.Display.setCursor(4, stripY + i * lineH + 2);
      M5.Display.print(wrappedLines[i]);
    }
    return;
  }

  // Long replies → scroll. Clip drawing to the strip so partial top/bottom
  // lines don't bleed into the face area above.
  M5.Display.setClipRect(0, stripY, screenW, stripH);

  for (int i = 0; i < numWrappedLines; ++i) {
    const int y = stripY + i * lineH - marqueeOffset + 2;
    if (y + lineH < stripY)            continue;  // already scrolled off top
    if (y > stripY + stripH)           continue;  // not yet entered from below
    M5.Display.setCursor(4, y);
    M5.Display.print(wrappedLines[i]);
  }

  M5.Display.clearClipRect();

  marqueeOffset += MARQUEE_SPEED_PX;
  // Loop: when last line has fully exited the top, restart with a blank strip
  // and let the first line scroll in from below again.
  if (marqueeOffset > totalH) marqueeOffset = -stripH;
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
// (enum AppState and appState are declared at the top of the file because
// drawMarquee() — defined earlier — guards on ST_TALKING.)

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

  // Pre-wrap the reply for the vertical-scroll marquee.
  wrapTrumpText();

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
  marqueeOffset = 0;       // marquee starts off-screen right
  lastMarqueeMs = 0;       // and ticks immediately
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
  drawMarquee();  // no-op unless ST_TALKING

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
