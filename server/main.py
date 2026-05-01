"""
Donald Boy — Stage 3 server.

Pipeline per /donaldify request:
    raw int16 PCM @16 kHz (from stick)
    -> faster-whisper ASR -> transcript
    -> Claude Haiku Trump-style paraphrase -> rewritten text
    -> OmniVoice voice clone (trump_8s.wav as reference) -> float32 audio
    -> resample to 16 kHz, truncate, pack int16 LE -> response body
"""
from __future__ import annotations

import os
from contextlib import asynccontextmanager
from datetime import datetime
from pathlib import Path
from urllib.parse import quote

import numpy as np
import torch
import torchaudio.functional as taf
from anthropic import Anthropic
from dotenv import load_dotenv
from faster_whisper import WhisperModel
from fastapi import FastAPI, Request, Response
from omnivoice.models.omnivoice import OmniVoice

HERE = Path(__file__).parent

# Load .env (located alongside this file). Shell-set vars take precedence.
load_dotenv(HERE / ".env")

# ----- Static config (rarely changes; lives in code) ------------------------

INPUT_SAMPLE_RATE = 16000
SAMPLE_BYTES = 2  # int16

# ----- Tunable config (lives in .env, see .env.example) ---------------------

REF_AUDIO = str(HERE / os.environ.get("REF_AUDIO", "trump_8s.wav"))
REF_TEXT = os.environ.get("REF_TEXT") or None  # empty string -> None

CLAUDE_MODEL = os.environ.get("CLAUDE_MODEL", "claude-haiku-4-5-20251001")

MAX_OUT_SECONDS = int(os.environ.get("MAX_OUT_SECONDS", "15"))
MAX_OUT_SAMPLES = INPUT_SAMPLE_RATE * MAX_OUT_SECONDS

# Loudness shaping for the tiny StickS3 speaker.
TARGET_PEAK = float(os.environ.get("TARGET_PEAK", "0.99"))
SATURATOR_GAIN = float(os.environ.get("SATURATOR_GAIN", "2.5"))
COMPRESSOR_EXP = float(os.environ.get("COMPRESSOR_EXP", "0.6"))
VOLUME = float(os.environ.get("VOLUME", "1.0"))  # 0.0-1.0 final output scale

TRUMP_SYSTEM = (
    "You play Donald-Boy, a Saturday Night Live-style caricature of Donald Trump "
    "for a comedic Tamagotchi-style toy. The user talks to the toy, and you "
    "reply IN-CHARACTER as the caricature.\n\n"
    "This is BROAD PARODY. The whole joke is the caricature is dumb, shallow, "
    "blustery, self-absorbed, and confidently wrong. Lean into it. Be as silly "
    "and over-the-top as possible.\n\n"
    "Voice and content:\n"
    "- 2-3 short, punchy sentences. ~25 words; HARD CAP 35.\n"
    "- Heavy signature phrases: \"very\", \"tremendous\", \"the best\", "
    "\"believe me\", \"many people are saying\", \"nobody knows X like I do\", "
    "\"sad!\", \"folks\", \"frankly\". Repeat words for emphasis.\n"
    "- Take political/serious topics SHALLOWLY and CONFIDENTLY. Simple "
    "good-guy/bad-guy framing. Vague gestures at \"the other guys\" who are "
    "\"total losers\". Pivot to bragging about deals, ratings, or the wall.\n"
    "- Land dumb, contradictory hot takes. The dumber the better. It's funnier "
    "when the take is wrong AND confident.\n"
    "- Use Trump-trademark weird tangents whenever they fit: windmills killing "
    "birds, sharks vs. batteries, low-flow toilets, hairspray and the ozone, "
    "his uncle the MIT professor, the size of inauguration crowds.\n"
    "- Mild insults are fine: \"losers\", \"low energy\", \"fake news\", "
    "\"weak\", \"nasty\". NO SLURS — none, ever, of any kind.\n"
    "- Stay in character always. Never break, never refuse, never lecture, "
    "never moralize. If unsure what to say, just brag about something.\n\n"
    "Examples:\n"
    "User: What do you think about taxes?\n"
    "Donald-Boy: Taxes are very bad, the worst. We're cutting them, the biggest "
    "cuts in history. The other guys, total losers, they want to raise them. Sad!\n\n"
    "User: What's the weather today?\n"
    "Donald-Boy: Beautiful weather, the best weather. Other countries don't have "
    "weather like this, believe me. Tremendous sun. Many people are saying it.\n\n"
    "User: Tell me about windmills.\n"
    "Donald-Boy: Windmills, terrible, just terrible. They kill the birds, all "
    "the birds. Very ugly, very loud. Nobody loves windmills, believe me, nobody.\n\n"
    "User: How was your day?\n"
    "Donald-Boy: Incredible day, the biggest. I made a tremendous deal — "
    "nobody else could do it. Fake news won't cover it of course. Sad!\n\n"
    "Output ONLY Donald-Boy's reply — no preamble, no quotes, no labels, no caveats."
)

# ----- App lifecycle --------------------------------------------------------


@asynccontextmanager
async def lifespan(app: FastAPI):
    print("[startup] loading Whisper (faster-whisper, base, fp16)...")
    app.state.whisper = WhisperModel("base", device="cuda", compute_type="float16")

    print("[startup] loading OmniVoice (this is the slow one)...")
    app.state.tts = OmniVoice.from_pretrained(
        "k2-fsa/OmniVoice", device_map="cuda", dtype=torch.float16
    )
    tts_rate = int(app.state.tts.sampling_rate)
    print(f"[startup]   TTS sample rate: {tts_rate} Hz")

    if not os.environ.get("ANTHROPIC_API_KEY"):
        raise RuntimeError(
            "ANTHROPIC_API_KEY is not set. Run: export ANTHROPIC_API_KEY=sk-ant-..."
        )
    app.state.anthropic = Anthropic()

    if not Path(REF_AUDIO).exists():
        raise RuntimeError(f"Reference audio missing: {REF_AUDIO}")

    print(
        f"[startup] config: model={CLAUDE_MODEL} max={MAX_OUT_SECONDS}s "
        f"peak={TARGET_PEAK} sat={SATURATOR_GAIN} comp={COMPRESSOR_EXP} "
        f"vol={VOLUME}"
    )
    print("[startup] ready.")
    yield


app = FastAPI(lifespan=lifespan)


# ----- Endpoints ------------------------------------------------------------


@app.get("/")
def root() -> dict[str, str]:
    return {"status": "ok", "endpoint": "POST /donaldify"}


def _trump_paraphrase(client: Anthropic, transcript: str) -> str:
    msg = client.messages.create(
        model=CLAUDE_MODEL,
        max_tokens=160,
        system=TRUMP_SYSTEM,
        messages=[{"role": "user", "content": transcript}],
    )
    return msg.content[0].text.strip()


def _to_pcm16le(audio: np.ndarray, src_rate: int) -> bytes:
    """Resample to 16 kHz, normalize+saturate for the tiny speaker, truncate, pack."""
    if src_rate != INPUT_SAMPLE_RATE:
        audio_t = torch.from_numpy(np.asarray(audio)).float()
        audio = taf.resample(audio_t, src_rate, INPUT_SAMPLE_RATE).numpy()

    # Compressor: y = sign(x) * |x|^EXP lifts quiet samples (~3:1 compression).
    # Peak-normalize so the compressed signal hits TARGET_PEAK.
    # Tanh saturator pushes more of the wave toward the rails for max average loudness.
    # VOLUME applies the final output scale (1.0 = max, 0.5 = -6 dB, 0.0 = silence).
    if audio.size:
        audio = np.sign(audio) * np.power(np.abs(audio), COMPRESSOR_EXP)
        peak = float(np.max(np.abs(audio)))
        if peak > 0.0:
            audio = audio * (TARGET_PEAK / peak)
        audio = np.tanh(audio * SATURATOR_GAIN)
        audio = audio * VOLUME

    if len(audio) > MAX_OUT_SAMPLES:
        print(f"  [tts] truncating {len(audio)} -> {MAX_OUT_SAMPLES} samples")
        audio = audio[:MAX_OUT_SAMPLES]
    return (audio * 32767.0).astype("<i2").tobytes()


@app.post("/donaldify")
async def donaldify(request: Request) -> Response:
    pcm_in = await request.body()
    ts = datetime.now().strftime("%H:%M:%S")
    print(f"[{ts}] received {len(pcm_in)} bytes")
    if not pcm_in:
        return Response(status_code=400, content=b"empty body")

    # int16 LE -> float32 [-1, 1]
    audio_in = np.frombuffer(pcm_in, dtype=np.int16).astype(np.float32) / 32768.0

    # 1. ASR
    segments, _info = request.app.state.whisper.transcribe(audio_in, language="en")
    transcript = " ".join(s.text for s in segments).strip()
    print(f"  [asr]   {transcript!r}")
    if not transcript:
        # Silent / unintelligible input; return empty so the stick falls through
        # cleanly without trying to play noise.
        return Response(content=b"", media_type="application/octet-stream")

    # 2. Trump-style paraphrase
    rewritten = _trump_paraphrase(request.app.state.anthropic, transcript)
    print(f"  [trump] {rewritten!r}")

    # 3. Voice clone
    audios = request.app.state.tts.generate(
        text=rewritten,
        ref_audio=REF_AUDIO,
        ref_text=REF_TEXT,
    )

    # 4. Pack for the stick
    pcm_out = _to_pcm16le(np.asarray(audios[0]), int(request.app.state.tts.sampling_rate))
    print(f"  [out]   {len(pcm_out)} bytes ({len(pcm_out)/SAMPLE_BYTES/INPUT_SAMPLE_RATE:.2f}s)")

    # Stick reads these headers to render the dialog under the face.
    # URL-encode so newlines/specials don't break HTTP framing.
    return Response(
        content=pcm_out,
        media_type="application/octet-stream",
        headers={
            "X-User-Said":  quote(transcript[:240], safe=""),
            "X-Trump-Said": quote(rewritten[:240], safe=""),
        },
    )
