#pragma once

#include <stdint.h>
#include <stddef.h>

// =============================================================================
// Donald Boy firmware tunables.
//
// Edit values here, then re-flash with PlatformIO Upload to apply.
// Architectural constants (SAMPLE_RATE, derived buffer sizes) live in main.cpp
// next to the code that uses them. Credentials/network live in secrets.h.
// =============================================================================

// ---- Audio durations --------------------------------------------------------

// How long the user can talk per BtnA press, in seconds.
constexpr int RECORD_SECONDS = 5;

// How long Trump's reply can be, in seconds. Must be >= the server's
// MAX_OUT_SECONDS or longer replies will be truncated. The audio buffer is
// sized for this at 16 kHz mono int16, allocated in PSRAM (≈ 32 KB/s).
constexpr int MAX_PLAY_SECONDS = 15;

// ---- Marquee (scrolling reply text on the bottom strip) --------------------

// Pixels per advance. Higher = faster scroll. Try 1 (slow), 2 (default), 4 (fast).
constexpr int MARQUEE_SPEED_PX = 2;

// Milliseconds between advances. Lower = smoother but heavier on SPI.
// 30 ms ≈ 33 Hz scroll rate — comfortable to read, light on CPU.
constexpr int MARQUEE_TICK_MS = 30;

// Height of the marquee strip in pixels — used as a *minimum* / clamping
// reference. Actual strip stretches dynamically from below the user-text line
// down to near the screen bottom (see drawMarquee in src/main.cpp).
constexpr int MARQUEE_STRIP_H = 30;

// ---- Volume cycle (BtnB) ---------------------------------------------------

// Each BtnB press steps through this list and wraps. First entry is the
// boot-time default (loudest). Last entry should be 0 (mute) so the
// "press to lower" UX always reaches silence before wrapping back to max.
constexpr uint8_t VOLUME_LEVELS[]   = { 255, 192, 128, 64, 0 };  // 100/75/50/25/mute
constexpr size_t  NUM_VOL_LEVELS    = sizeof(VOLUME_LEVELS) / sizeof(VOLUME_LEVELS[0]);
