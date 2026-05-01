# Donald Boy

A Tamagotchi-style ESP32 desktop toy that listens to you for a few seconds, then replies in a cloned Donald Trump voice with a Trump-style paraphrase of what you said.

```
 ─ listening ─        ─ donaldifying ─        ─── speaking ───

      〜〜                    〜〜                    〜〜
  ¯＼_(O_O)_/¯           ¯＼_(?_?)_/¯           ¯＼_(-o-)_/¯
   listening...          donaldifying...        speaking...
                                            you: what about taxes?

                                            ─────────────────
                                            ◄ Taxes terrible,
                                              the worst!     ◄
                                            ─────────────────
```

`speaking` view shows a small static "you:" line above a big scrolling marquee with Donald-Boy's reply.

It's a fun, comedic parody project — pure entertainment, not political commentary. Run it on your desk for laughs.

---

## How it works

The stick is a thin client. All ML inference runs on a laptop or desktop with an NVIDIA GPU on the same WiFi network.

```
[M5StickC S3]                     [Your laptop / server]
  ──────────────                    ──────────────────────
  capture mic         ── POST ─→    faster-whisper (ASR)
  show face                              ↓
  (HTTP keepalive)                  Claude Haiku rewrite
                                    in Donald-Boy voice
                                         ↓
  play speaker        ←─ PCM ──     OmniVoice voice clone
  show transcript                   (uses trump_8s.wav as ref)
```

Per request: ~100 ms ASR + ~500 ms Claude Haiku + ~1–2 s OmniVoice TTS. End-to-end on LAN: 2–3 s wall time.

---

## Cost per request

Everything except the Claude Haiku paraphrase runs locally on your hardware (free). Per-press cost breakdown:

| Component | Per request | Per 1,000 requests |
|---|---|---|
| Claude Haiku 4.5 (paraphrase) | **~$0.0006** | **~$0.60** |
| faster-whisper ASR (local GPU) | free | free |
| OmniVoice TTS (local GPU) | free | free |
| WiFi / HTTP | free | free |

**Calculation:** typical request is ~425 input tokens (≈400-token system prompt + 25-token transcript) and ~35 output tokens (~25-word reply). At Claude Haiku 4.5 pricing of ~$1/MTok input and ~$5/MTok output:

```
input:   425 × $1 / 1,000,000 = $0.000425
output:   35 × $5 / 1,000,000 = $0.000175
                                ─────────
                       total =  $0.0006/press
```

Real cost will drift up or down a bit depending on how much you say and how long Donald-Boy rambles. Heavy use is still pocket change — pressing the button **100 times a day for a year would cost ~$22**.

If you want to push it lower, enable Anthropic prompt caching for the system prompt: cuts input cost ~60% during active sessions (5-minute cache TTL). Not implemented in this codebase by default; see the `claude-api` skill or the [Anthropic prompt caching docs](https://docs.anthropic.com/en/docs/build-with-claude/prompt-caching) if you want to add it.

---

## Deployment options

By default the server runs on your laptop and the stick connects over your home WiFi. If you want the toy to work *away from home*, four paths — ranked by cost-effectiveness:

| Option | Monthly cost (30/day) | Latency per press | Tradeoff |
|---|---|---|---|
| **Local laptop** _(default)_ | ~$0.54 | 2–3 s | Stick must stay on your WiFi |
| **Home server + [Cloudflare Tunnel](https://developers.cloudflare.com/cloudflare-one/connections/connect-networks/)** | ~$1.50 | 2–3 s | Free public HTTPS to your home GPU; stick works anywhere |
| **Cloud serverless GPU** ([RunPod](https://runpod.io/serverless), [Modal](https://modal.com)) | ~$1.50–4 | 2–3 s warm, **12–32 s cold** | No home GPU needed; cold starts ruin sporadic use |
| **Cloud always-on GPU** | ~$280 | 2–3 s | Snappy but ~95% wasted compute on a hobby toy |

(Cost assumes 30 presses/day plus Claude Haiku — see [Cost per request](#cost-per-request) for the math.)

### Home server + Cloudflare Tunnel _(recommended for portability)_

Keep your existing GPU machine as the brain; expose its `:8000` endpoint via a free Cloudflare Tunnel. The stick gets a public HTTPS URL it can reach from any WiFi — no port-forwarding, no dynamic-DNS, no exposed home IP.

```bash
# One-time setup
sudo apt install cloudflared
cloudflared tunnel login                                         # opens browser
cloudflared tunnel create donald-boy
cloudflared tunnel route dns donald-boy donald-boy.your-domain.com

# Run (or wrap as a systemd service for auto-start)
cloudflared tunnel --url http://localhost:8000 run donald-boy
```

Then update `include/secrets.h`:
```c
#define SERVER_URL "https://donald-boy.your-domain.com/donaldify"
```
Re-flash. The stick now reaches your home server from anywhere it can get to the public internet. Edge hop adds ~50–150 ms per request — well within snappy territory.

Cloudflare Tunnel is free for personal use and handles HTTPS termination, so your home server keeps speaking plain HTTP locally.

### Cloud serverless GPU _(when you don't have a home GPU)_

If your laptop doesn't have an NVIDIA card, containerize the server and deploy to **RunPod Serverless** or **Modal**. The ESP32 firmware doesn't change — only the `SERVER_URL` does.

Catch: serverless workers spin down after ~5 min of idle. For a Tamagotchi pressed every few hours, *every press is a cold start* — the worker has to load 2.45 GB of OmniVoice + 150 MB of Whisper before answering. Expect the thinking face to hold for **12–32 seconds** on those.

Workaround: configure the worker to keep one hot replica (`min_workers=1` on RunPod). That's effectively the always-on option below at slightly lower cost.

### Cloud always-on GPU _(not recommended for a hobby toy)_

Rent a 24/7 RTX 4090 from RunPod / Vast.ai (~$0.39/hr × 730 hr/mo = ~$280/mo). Snappy responses, but ~95% of the GPU is idle for personal use. Only worth it if you're doing public demos or running many toys.

---

## Prerequisites

Before you start, make sure you have all of the following. Missing any one of these will block setup partway through — fix gaps now, not at debug time.

### Hardware

- **M5StickC S3** dev kit (ESP32-S3-PICO-1-N8R8 — 8 MB flash, 8 MB PSRAM, 1.14" LCD, ES8311 codec with mic + speaker)
- **USB-C data cable** — many phone-charging cables omit data lines and won't enumerate as `/dev/ttyACM*`. Use a known-good data cable
- **Server machine** with an **NVIDIA GPU**, ≥6 GB VRAM (tested on RTX 4070 12 GB; smaller GPUs may work but the model is tight at 6 GB)
- **2.4 GHz WiFi** the stick can join. ESP32-S3 does not support 5 GHz; many home routers expose both bands under the same SSID, but if your network is 5 GHz only, you'll need to enable 2.4 GHz first

### Server software

- **Python 3.10+** with `venv` support
- **NVIDIA driver** new enough for CUDA 12.x (driver R535 or newer; check with `nvidia-smi`)
- **ffmpeg** and **yt-dlp** (used to extract the voice reference clip): `sudo apt install -y ffmpeg yt-dlp`
- **~5 GB free disk** for the OmniVoice + Whisper model caches in `~/.cache/huggingface/`

### Firmware software

- **VS Code** with the **PlatformIO IDE** extension installed
- ESP32-S3 toolchain — PlatformIO downloads it on first build (~500 MB, one time, automatic)
- On Linux: membership in the `dialout` group so you can talk to the serial port (see [Troubleshooting](#troubleshooting))

### Accounts / API keys

- **Anthropic API key** for Claude Haiku — sign up at <https://console.anthropic.com>. Per-press cost ~$0.0006 (see [Cost per request](#cost-per-request) above).
- *(Optional)* **HuggingFace account + read token** — anonymous downloads work but can rate-limit. A free token at <https://huggingface.co/settings/tokens> avoids this.

### Skills assumed

- Comfortable with a terminal, Python virtual environments, and editing config files
- Basic networking literacy: knowing how to find your laptop's LAN IP (`hostname -I` on Linux)

### Reference audio

- A 5–10 second WAV clip of Donald Trump speaking — single voice, no music, no crowd noise. The setup steps below show how to extract one from a public-domain inauguration speech.

---

## Setup

### 1. Clone

```bash
git clone https://github.com/YOUR-FORK/donald-boy
cd donald-boy
```

### 2. Server (Python on your laptop)

```bash
cd server
python3 -m venv .venv
source .venv/bin/activate

# PyTorch CUDA wheels — separate index URL, install before requirements.txt
pip install torch==2.8.0 torchaudio==2.8.0 \
    --extra-index-url https://download.pytorch.org/whl/cu128

pip install -r requirements.txt
```

Install ffmpeg + yt-dlp at the system level (used for reference-clip extraction):

```bash
sudo apt install -y ffmpeg yt-dlp
```

Configure secrets and tunables:

```bash
cp .env.example .env
# Edit .env and set ANTHROPIC_API_KEY at minimum
```

#### Reference audio

OmniVoice clones the voice from a reference WAV. You need a 5–10 second clip that is **single-voice, no music, no crowd noise, no overlapping speakers** — clean broadcast/podium audio works best.

A reliable source is the 2017 inauguration speech:

```bash
# Download full audio
yt-dlp -x --audio-format wav -o trump_full.%(ext)s \
    "https://www.youtube.com/watch?v=a-mfhjaPvsM"

# Trim a clean 8-second window. Iterate -ss until you find a stretch
# without applause, pauses, or mumbling.
ffmpeg -i trump_full.wav -ss 00:08:00 -t 8 \
    -ar 16000 -ac 1 -c:a pcm_s16le trump_8s.wav

aplay trump_8s.wav   # spot check it sounds clean
```

The path to this file goes in `.env` as `REF_AUDIO=trump_8s.wav` (default).

#### Start the server

```bash
uvicorn main:app --host 0.0.0.0 --port 8000
```

The first boot downloads OmniVoice (~2.45 GB) and Whisper base (~150 MB) into `~/.cache/huggingface/`. Subsequent boots are fast.

You should see:

```
[startup] loading Whisper (faster-whisper, base, fp16)...
[startup] loading OmniVoice (this is the slow one)...
[startup]   TTS sample rate: 24000 Hz
[startup] config: model=claude-haiku-4-5-20251001 max=15s peak=0.99 sat=2.5 comp=0.6 vol=1.0
[startup] ready.
INFO:     Uvicorn running on http://0.0.0.0:8000
```

`--host 0.0.0.0` is important — it binds on every interface so the stick on your WiFi can reach your laptop. `127.0.0.1` would only listen on loopback.

#### Smoke test

In another terminal:

```bash
cd server
source .venv/bin/activate
python3 test_client.py
```

Expected:
```
status: 200
sent:   96000 bytes
got:    <some bytes>  (echo intact: ...)
```

If this fails, fix the server before flashing.

### 3. Firmware (the StickC S3)

Install [PlatformIO IDE](https://platformio.org/install/ide?install=vscode) as a VS Code extension.

Open the project root in VS Code (`code .` from the repo root).

Configure WiFi + server URL:

```bash
cp include/secrets.h.example include/secrets.h
# Edit include/secrets.h:
#   - WIFI_SSID  = your 2.4 GHz network name
#   - WIFI_PASS  = its password
#   - SERVER_URL = http://YOUR_LAPTOP_LAN_IP:8000/donaldify
```

Find your laptop's LAN IP with `hostname -I` (Linux) or `ipconfig` (Windows).

#### Flash

In VS Code, bottom blue bar → click the **→ Upload** icon (or `Ctrl+Alt+U`).

If the upload fails with `Connecting....` followed by errors, force the chip into download mode manually:

1. Hold the **BtnB** (side button)
2. Briefly press **PWR** (bottom-edge button)
3. Release **BtnB**
4. Click Upload again

This works for any ESP32-S3 board — the BOOT strap pin needs to be held LOW at reset to enter the ROM bootloader.

#### Linux serial port permissions

If the upload fails with `Permission denied: /dev/ttyACM0`:

```bash
sudo usermod -a -G dialout $USER
# log out and log back in
```

---

## Usage

| Button | Action |
|---|---|
| **BtnA** (front M5 logo) | Press to talk: records 5 s, posts to server, plays Trump's reply |
| **BtnB** (right side) | Cycle volume: 100% → 75% → 50% → 25% → mute → 100% |
| **PWR** (bottom edge) | Power on / brief press = soft reset / hold ~6 s = power off |

### Boot sequence

1. `WiFi...` → `connected!` and your IP, briefly
2. Idle face: `〜〜  ¯＼_(ツ)_/¯` (with a periodic blink)
3. Press **BtnA** → listening face appears, talk for ≤5 seconds
4. Recording auto-stops, thinking face cycles for 1–3 seconds while the server runs ASR + Claude paraphrase + TTS
5. Speaking face appears, cloned-Trump audio plays. Below the face: a small static `you: <transcript>` line and a big scrolling marquee with Donald-Boy's reply.
6. Returns to idle when audio finishes.

### Volume

Press **BtnB** at any time to step through volume levels: **100% → 75% → 50% → 25% → mute → 100%**. A full-screen indicator flashes briefly showing the new level. Volume change is per-session — boot always starts at max. To set the *ceiling* loudness across sessions, edit `VOLUME=` in `server/.env`.

---

## Configuration reference

`server/.env` (copy from `.env.example`). Anything set in your shell with `export FOO=...` overrides what's in `.env`.

| Variable | Default | What it does |
|---|---|---|
| `ANTHROPIC_API_KEY` | _(required)_ | Your Anthropic API key |
| `HF_TOKEN` | _(unset)_ | Optional. Set for faster HuggingFace downloads. Get a free read token at <https://huggingface.co/settings/tokens> |
| `CLAUDE_MODEL` | `claude-haiku-4-5-20251001` | Which Claude model to use for paraphrase |
| `REF_AUDIO` | `trump_8s.wav` | Path to reference clip, relative to `server/` |
| `REF_TEXT` | _(unset)_ | Optional exact transcript of reference. If unset, OmniVoice auto-Whispers it |
| `MAX_OUT_SECONDS` | `15` | Cap on response audio length. Stick must have a buffer at least this large |
| `TARGET_PEAK` | `0.99` | Peak after compressor, before saturator (0.0–1.0) |
| `SATURATOR_GAIN` | `2.5` | tanh saturator drive. Higher = louder + more distortion |
| `COMPRESSOR_EXP` | `0.6` | Dynamics compression. `1.0` disables; lower = more squashing |
| `VOLUME` | `1.0` | Final output scale (0.0–1.0). `0.5` = -6 dB; `0.0` = silent |

`include/secrets.h` (copy from `.example`):

| Macro | What it does |
|---|---|
| `WIFI_SSID`  | Your 2.4 GHz WiFi network name |
| `WIFI_PASS`  | WiFi password |
| `SERVER_URL` | Full URL to the server, e.g. `http://192.168.1.42:8000/donaldify` |

`include/config.h` (firmware tunables, compile-time — change requires re-flash):

| Constant | Default | What it does |
|---|---|---|
| `RECORD_SECONDS` | `5` | How long the user can talk per press |
| `MAX_PLAY_SECONDS` | `15` | How long Trump can reply. Must be ≥ server `MAX_OUT_SECONDS` |
| `MARQUEE_SPEED_PX` | `2` | Scrolling marquee advance per tick (1 = slow, 4 = fast) |
| `MARQUEE_TICK_MS` | `30` | Marquee redraw interval (ms). Lower = smoother but more SPI traffic |
| `MARQUEE_STRIP_H` | `40` | Marquee strip height in pixels |
| `VOLUME_LEVELS[]` | `{255,192,128,64,0}` | Preset levels cycled by BtnB |

Architectural constant in `src/main.cpp` — only change if you also change the server:

| Constant | Default | What it does |
|---|---|---|
| `SAMPLE_RATE` | `16000` | Mic + speaker sample rate. Must match server |

---

## Project structure

```
donald-boy/
├── README.md
├── LICENSE
├── .gitignore
├── platformio.ini              # PIO build config (ESP32-S3, Arduino)
├── src/main.cpp                # Firmware: state machine + audio + WiFi/HTTP
├── include/
│   ├── secrets.h.example       # Template for WiFi creds (commit-safe)
│   ├── secrets.h               # Your real creds (git-ignored)
│   └── config.h                # Firmware tunables (commit-safe)
├── lib/                        # Local Arduino libs (empty by default)
└── server/
    ├── main.py                 # FastAPI: ASR -> LLM -> TTS pipeline
    ├── requirements.txt        # Python deps (excluding torch — see README)
    ├── test_client.py          # Smoke-test client
    ├── .env.example            # Template for runtime config (commit-safe)
    ├── .env                    # Your real config (git-ignored)
    └── trump_8s.wav            # Reference audio for voice cloning (you provide)
```

---

## Architecture details

### Firmware state machine

```
        BtnA            mic done             postAudio() ok
IDLE  ───────► LISTENING ─────────► THINKING ─────────────► TALKING
  ▲                                     │                      │
  │                                     │  postAudio() failed  │ playback done
  └─────────────────────────────────────┴──────────────────────┘
```

`STATE_THINKING` blocks the main loop on the synchronous HTTP round-trip. The thinking face is static during that time; audio recording / playback continues via I²S DMA in the background.

### Server pipeline

```
raw int16 PCM @ 16 kHz (5 s)
        │
        ▼
faster-whisper ASR ─→ English transcript
        │
        ▼
Claude Haiku ─→ Donald-Boy character paraphrase
        │
        ▼
OmniVoice TTS clone (uses trump_8s.wav as voice reference)
        │
        ▼
torchaudio resample to 16 kHz
        │
        ▼
compressor → peak-normalize → tanh saturator → VOLUME → int16 LE
        │
        ▼
HTTP body + X-User-Said + X-Trump-Said headers
```

Models are loaded once at startup via FastAPI's `lifespan` handler — they live in GPU memory across requests.

### Display layout (TALKING state)

```
┌─────────────────┐  ← y = 0
│      〜〜       │     hair (orange, gothic_24)
│  ¯＼_(-o-)_/¯  │     body (green, gothic_24, animated)
│   speaking...   │     status label (light grey, gothic_16)
│                 │
│ you: what about │     transcript (cyan, gothic_12, wrapped)
│      taxes?     │
│                 │
│ ─────────────── │
│ ◄ Taxes terri…  │     marquee strip (yellow, gothic_24, scrolling)
│ ─────────────── │
└─────────────────┘  ← y = 240
```

Each pass writes to a different region:
- `drawAsciiFrame()` redraws the upper area (face + label + "you:" line) on every animation frame
- `drawMarquee()` redraws ONLY the bottom strip every ~30 ms, leaving the face above untouched

This split lets the marquee scroll smoothly without touching the rest of the screen.

### Audio buffer

The stick allocates a single buffer in PSRAM (8 MB available). The same buffer is used for both recording and playback:

- During listening: mic fills the first `RECORD_SECONDS × SAMPLE_RATE` samples
- During HTTP POST: those bytes are sent as the request body
- During HTTP read: response overwrites the buffer (request is already on the wire)
- During playback: speaker reads up to `audioLen` samples

Reusing the buffer halves memory usage compared to keeping separate input/output arrays.

### Why these choices

- **ESP32-S3 over standard ESP32:** the PSRAM lets us hold 15 s of mono 16-bit audio in a single buffer.
- **Server-side ML:** OmniVoice and Whisper-base both need GPU/PSRAM scale resources that the stick doesn't have. The stick is a thin client.
- **HTTP headers for dialog text:** free metadata channel; no JSON parsing on the stick.
- **Two-tier volume (server + stick):** server `VOLUME` sets the ceiling for the environment; stick's BtnB cycles in-session.
- **Compressor + tanh saturator:** maxes out the tiny speaker's perceived loudness without harsh clipping.

---

## Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| `Permission denied: /dev/ttyACM0` on upload | Linux dialout group | `sudo usermod -a -G dialout $USER`, then log out / in |
| `OSError: [Errno 71] Protocol error` on upload | Auto-reset failed | Hold BtnB, tap PWR, release BtnB; then upload |
| Stick screen blank / no USB enumeration | Charge-only USB-C cable | Use a known-good data cable |
| `WiFi failed` on boot | Wrong creds or 5 GHz-only network | ESP32-S3 is 2.4 GHz only. Most routers expose both bands under one SSID — check for a separate 2.4 GHz one |
| Thinking → idle, no audio plays | HTTP read failed | Open PIO Serial Monitor; check `[postAudio]` lines for status code and bytes |
| Audio plays but cuts off mid-rant | TTS longer than buffer | Bump `MAX_OUT_SECONDS` (server) and `MAX_PLAY_SECONDS` (firmware) |
| Audio is too quiet | Server gain low | Raise `SATURATOR_GAIN` and/or `VOLUME` in `.env` |
| Audio is distorted/buzzy | Saturator over-driven | Lower `SATURATOR_GAIN` (try `2.0` or `1.5`) |
| Trump face shows tofu boxes (`□`) | Font missing the glyph | Use fullwidth Unicode in source: `＼` (U+FF3C) not `\`, `〜` (U+301C) not `~` |
| Transcript is wrong / "World" only | User spoke late or off-mic | Press BtnA, wait for listening face, then talk |
| HF download stalls | Anonymous rate limiting | Set `HF_TOKEN=hf_...` in `.env` (free at huggingface.co/settings/tokens) |
| Reference audio fails: `empty after silence removal` | Reference clip is too quiet or all silence | Pick a different timestamp; verify with `aplay`. Optionally normalize: `ffmpeg -i in.wav -filter:a loudnorm out.wav` |
| Claude refuses, response sounds like a refusal | System prompt too restrictive | Edit `TRUMP_SYSTEM` in `server/main.py` — add few-shot examples covering the topic |
| `omnivoice-infer: command not found` after `pip install` | zsh hash table not refreshed | Run `rehash` |

---

## Customization

- **Change Donald-Boy's personality:** edit `TRUMP_SYSTEM` in `server/main.py`. Few-shot examples teach the voice better than rule lists.
- **Use a different voice:** swap `trump_8s.wav` with any 5–10 s clean clip. Update `REF_AUDIO` in `.env`. Optionally provide `REF_TEXT` (exact transcript).
- **Different ASR model:** edit `WhisperModel("base", ...)` in `server/main.py`. Options: `tiny`, `base`, `small`, `medium`, `large-v3`. Larger = better accuracy + more VRAM + slower.
- **Different LLM:** change `CLAUDE_MODEL` in `.env`. Or rewrite `_trump_paraphrase` in `server/main.py` to call a different provider.
- **Different face:** edit `IDLE_FRAMES`, `LISTENING_FRAMES`, `THINKING_FRAMES`, `TALKING_FRAMES` in `src/main.cpp`. Each frame is `{ hair, body, durationMs }`. Stick to ASCII or fullwidth Unicode for reliable rendering.
- **Marquee speed:** in `include/config.h`, raise `MARQUEE_SPEED_PX` for faster scroll, lower `MARQUEE_TICK_MS` for smoother updates. Both require a re-flash.
- **Volume presets:** edit `VOLUME_LEVELS[]` in `include/config.h` to change BtnB's stepped levels. Last entry should stay `0` so wrap-around mute always works.
- **Recording duration:** bump `RECORD_SECONDS` in `include/config.h` if 5 s isn't enough. The audio buffer in PSRAM scales automatically.

---

## Caveats

- This is a **comedic parody toy** for personal use. Voice cloning a public figure for a desktop toy in your drawer is fine; publishing convincing fake audio of real people online is not. Keep it private.
- The Anthropic API key incurs cost — about $0.0006 per press (see [Cost per request](#cost-per-request)). Cheap but not free.
- Server requires an NVIDIA GPU. Apple Silicon (MPS) is supported by both Whisper and OmniVoice but not specifically tested in this project. CPU-only is theoretically possible but inference will be 10–30× slower.
- 2.4 GHz WiFi only.

---

## Tech credits

- [M5Stack](https://m5stack.com) — the StickC S3 hardware and the M5Unified library
- [k2-fsa/OmniVoice](https://github.com/k2-fsa/OmniVoice) — voice cloning model
- [SYSTRAN/faster-whisper](https://github.com/SYSTRAN/faster-whisper) — ASR
- [Anthropic Claude](https://www.anthropic.com) — Donald-Boy paraphrasing
- [PlatformIO](https://platformio.org) — embedded build system
- [FastAPI](https://fastapi.tiangolo.com) — HTTP server

---

## License

[MIT](LICENSE).
