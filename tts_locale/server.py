"""
RpgAi TTS Locale Server

RESPONSIBLE USE NOTICE
----------------------
This tool generates speech using AI voice cloning technology.
It is intended for creative and fictional purposes within the RpgAi
game engine — giving NPC characters distinct synthetic voices.
It must NOT be used to:
  - Impersonate real people without their explicit consent
  - Create deceptive, defamatory, or non-consensual audio content
  - Produce content that violates applicable laws or platform policies

By running this server you accept sole responsibility for the content
you generate and the voice samples you provide.

Model: Coqui XTTS v2 — multilingual, zero-shot voice cloning from a 6-30s WAV.
Architecture: GPT-2 autoregressive vocoder + HiFiGAN decoder (not a diffusion model).
Quantization and fuse_lora are not applicable. torch.compile is risky on XTTS v2
(complex multi-component model), so it is not enabled by default.

Recommended launch:
  python server.py --host 0.0.0.0

Endpoints:
  POST /synthesize
    text     — text to synthesize
    voice_id — voice profile name
    language — BCP-47 language code (default: "it")
    Returns: WAV audio bytes (Content-Type: audio/wav)

  POST /voices/add
    file     — reference WAV (6-30s, clean speech)
    voice_id — name for this voice profile
    Returns: JSON

  GET /voices  — list available voices
  GET /health  — liveness check
"""

import io
import os
import re
import argparse
from contextlib import asynccontextmanager

import numpy as np
import soundfile as sf
import torch
from fastapi import FastAPI, File, Form, HTTPException, UploadFile
from fastapi.responses import Response

# Coqui TTS checkpoints contain XttsConfig (pickle). PyTorch 2.6+ defaults
# weights_only=True which rejects pickle → patch before TTS import.
_orig_torch_load = torch.load
def _patched_torch_load(*args, **kwargs):
    kwargs.setdefault("weights_only", False)
    return _orig_torch_load(*args, **kwargs)
torch.load = _patched_torch_load

from TTS.api import TTS

# ============================================================
# ─── CLI ────────────────────────────────────────────────────
# ============================================================
def parse_args():
    parser = argparse.ArgumentParser(description="RpgAi TTS Locale Server")
    parser.add_argument("--host", default="127.0.0.1",
                        help="Host (default: 127.0.0.1, use 0.0.0.0 for network access)")
    parser.add_argument("--port", type=int, default=8004,
                        help="Port (default: 8004)")
    parser.add_argument("--device", default=None,
                        help="Device: cuda, mps, cpu (default: auto)")
    parser.add_argument("--model", default="tts_models/multilingual/multi-dataset/xtts_v2",
                        help="TTS model (default: xtts_v2)")
    # Voice similarity tuning
    parser.add_argument("--temperature", type=float, default=0.2,
                        help="GPT sampling temperature (default: 0.2). Lower = closer to reference voice.")
    parser.add_argument("--top-k", type=int, default=50,
                        help="Top-k sampling (default: 50). Lower = more deterministic.")
    parser.add_argument("--top-p", type=float, default=0.85,
                        help="Top-p nucleus sampling (default: 0.85).")
    parser.add_argument("--repetition-penalty", type=float, default=2.0,
                        help="Repetition penalty (default: 2.0).")
    parser.add_argument("--gpt-cond-len", type=int, default=30,
                        help="Seconds of reference audio used for conditioning (default: 30).")
    parser.add_argument("--gpt-cond-chunk-len", type=int, default=4,
                        help="Chunk length for conditioning latents (default: 4).")
    parser.add_argument("--clean-text", action="store_true", default=False,
                        help="Strip markdown and normalize punctuation before synthesis (replaces . with space, removes */#/`).")
    parser.add_argument("--strip-silence", action="store_true", default=False,
                        help="Strip leading/trailing silence from reference WAV before computing speaker embedding.")
    parser.add_argument("--silence-threshold-db", type=float, default=-35.0,
                        help="Silence threshold in dB for --strip-silence (default: -35).")
    return parser.parse_args()

ARGS = parse_args()

# ============================================================
# ─── CONFIG ─────────────────────────────────────────────────
# ============================================================
VOICES_DIR   = os.path.join(os.path.dirname(__file__), "voices")
MODEL_NAME   = ARGS.model
SAMPLE_RATE  = 24000
DEFAULT_LANG = "it"

os.makedirs(VOICES_DIR, exist_ok=True)

def _best_device() -> str:
    if ARGS.device:
        return ARGS.device
    if torch.cuda.is_available():
        return "cuda"
    if hasattr(torch.backends, "mps") and torch.backends.mps.is_available():
        return "mps"
    return "cpu"

DEVICE = _best_device()

# TF32 — ~3x matmul throughput on Ampere/Ada/Blackwell with negligible precision loss
if DEVICE == "cuda":
    torch.set_float32_matmul_precision('high')
    torch.backends.cuda.matmul.allow_tf32 = True

print("╔══════════════════════════════════════════════╗")
print("║   RpgAi TTS Locale Server                   ║")
print("╠══════════════════════════════════════════════╣")
print(f"║  Model:     {MODEL_NAME:<32}║")
print(f"║  Device:    {DEVICE:<32}║")
print(f"║  Host:      {ARGS.host:<32}║")
print(f"║  Port:      {ARGS.port:<32}║")
print(f"║  Temp:      {ARGS.temperature:<32}║")
print(f"║  Top-k:     {ARGS.top_k:<32}║")
print(f"║  Top-p:     {ARGS.top_p:<32}║")
print(f"║  Cond len:  {ARGS.gpt_cond_len:<32}║")
print("╚══════════════════════════════════════════════╝")

# ============================================================
# ─── MODEL ─────────────────────────────────────────────────
# ============================================================
tts_model = None

# speaker conditioning cache: safe_id → (gpt_cond_latent, speaker_embedding, mtime)
# Invalidated automatically when the voice WAV file changes (mtime check).
_speaker_cache: dict = {}

def _strip_silence(audio: np.ndarray, sr: int) -> np.ndarray:
    threshold = 10 ** (ARGS.silence_threshold_db / 20)
    above = np.where(np.abs(audio) > threshold)[0]
    if len(above) == 0:
        return audio
    return audio[above[0]:above[-1] + 1]

def _get_conditioning(voice_path: str, safe_id: str):
    """Return cached XTTS conditioning latents, recomputing only when WAV changes."""
    mtime = os.path.getmtime(voice_path)
    cached = _speaker_cache.get(safe_id)
    if cached and cached[2] == mtime:
        return cached[0], cached[1]
    print(f"[cache] Computing speaker embedding: '{safe_id}' ...")
    xtts = tts_model.synthesizer.tts_model

    ref_path = voice_path
    tmp_path = None
    if ARGS.strip_silence:
        audio, sr = sf.read(voice_path)
        cleaned = _strip_silence(audio, sr)
        duration_orig = len(audio) / sr
        duration_clean = len(cleaned) / sr
        print(f"[cache] Silence strip: {duration_orig:.1f}s → {duration_clean:.1f}s")
        import tempfile
        with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as tmp:
            sf.write(tmp.name, cleaned, sr)
            tmp_path = tmp.name
        ref_path = tmp_path

    try:
        with torch.no_grad():
            gpt_cond_latent, speaker_embedding = xtts.get_conditioning_latents(
                audio_path=[ref_path],
                gpt_cond_len=ARGS.gpt_cond_len,
                gpt_cond_chunk_len=ARGS.gpt_cond_chunk_len,
            )
    finally:
        if tmp_path:
            os.unlink(tmp_path)

    _speaker_cache[safe_id] = (gpt_cond_latent, speaker_embedding, mtime)
    print(f"[cache] '{safe_id}' ready.")
    return gpt_cond_latent, speaker_embedding

def _load_model():
    global tts_model
    print(f"[startup] Loading {MODEL_NAME} on {DEVICE} ...")
    tts_model = TTS(MODEL_NAME).to(DEVICE)
    print("[startup] TTS ready.")

@asynccontextmanager
async def lifespan(app: FastAPI):
    try:
        _load_model()
    except Exception as e:
        print(f"\n[FATAL] {e}")
        raise
    yield

app = FastAPI(title="RpgAi TTS Locale Server", lifespan=lifespan)

# ============================================================
# ─── HELPERS ───────────────────────────────────────────────
# ============================================================
def _wav_to_bytes(wav_array) -> bytes:
    buf = io.BytesIO()
    sf.write(buf, np.array(wav_array, dtype=np.float32),
             samplerate=SAMPLE_RATE, format="WAV", subtype="PCM_16")
    buf.seek(0)
    return buf.read()

def _sanitize_id(voice_id: str) -> str:
    return "".join(c for c in voice_id if c.isalnum() or c in "_-")

def _clean_for_tts(text: str) -> str:
    """Strip markdown and fix punctuation that XTTS v2 reads aloud incorrectly."""
    # Bold / italic markers
    text = re.sub(r'\*{1,3}([^*\n]+)\*{1,3}', r'\1', text)
    # Markdown headers
    text = re.sub(r'^#{1,6}\s+', '', text, flags=re.MULTILINE)
    # Ellipsis → single space (avoids "punto punto punto" without adding extra pauses)
    text = re.sub(r'\.{2,}', ' ', text)
    # Em-dash / en-dash → comma (short pause)
    text = re.sub(r'\s*[—–]\s*', ', ', text)
    # Remove ALL periods: XTTS GPT treats "." as sentence-end token, generating both
    # a long pause AND prosodic deformation of surrounding words. Space is neutral.
    text = text.replace('.', ' ')
    # Backticks
    text = text.replace('`', '')
    return text.strip()

# ============================================================
# ─── POST /synthesize ──────────────────────────────────────
# ============================================================
@app.post("/synthesize")
async def synthesize(
    text:     str = Form(...),
    voice_id: str = Form(...),
    language: str = Form(DEFAULT_LANG),
):
    # Always: periods → comma (Italian text normalizer reads "." as "punto").
    # Ellipsis handled first to avoid triple-comma.
    text = re.sub(r'\.{2,}', ',', text)
    text = text.replace('.', ',')
    if ARGS.clean_text:
        text = _clean_for_tts(text)
        if not text:
            raise HTTPException(400, "Text is empty after cleaning.")
    safe_id = _sanitize_id(voice_id)
    voice_path = os.path.join(VOICES_DIR, f"{safe_id}.wav")
    if not os.path.exists(voice_path):
        raise HTTPException(404, f"Voice '{safe_id}' not found. Add it via POST /voices/add")

    kw = dict(text=text, speaker_wav=voice_path, language=language, split_sentences=False)
    try:
        with torch.no_grad():
            wav = tts_model.tts(**kw)
        return Response(content=_wav_to_bytes(wav), media_type="audio/wav")
    except TypeError as e:
        m = re.search(r"unexpected keyword argument '(\w+)'", str(e))
        if m:
            kw.pop(m.group(1), None)
            try:
                with torch.no_grad():
                    wav = tts_model.tts(**kw)
                return Response(content=_wav_to_bytes(wav), media_type="audio/wav")
            except Exception as e2:
                raise HTTPException(500, str(e2))
        raise HTTPException(500, str(e))
    except Exception as e:
        raise HTTPException(500, str(e))

# ============================================================
# ─── POST /voices/add ──────────────────────────────────────
# ============================================================
@app.post("/voices/add")
async def add_voice(
    file:     UploadFile = File(...),
    voice_id: str        = Form(...),
):
    safe_id = _sanitize_id(voice_id)
    if not safe_id:
        raise HTTPException(400, "Invalid voice_id.")

    dest = os.path.join(VOICES_DIR, f"{safe_id}.wav")
    data = await file.read()
    with open(dest, "wb") as f:
        f.write(data)

    # Invalidate cached conditioning so next synthesis recomputes from new WAV
    _speaker_cache.pop(safe_id, None)

    return {"status": "ok", "voice_id": safe_id, "path": dest,
            "note": "Ideal reference: 6-30 seconds, clean speech, no background noise."}

# ============================================================
# ─── GET /voices ───────────────────────────────────────────
# ============================================================
@app.get("/voices")
def list_voices():
    voices = sorted(f[:-4] for f in os.listdir(VOICES_DIR) if f.endswith(".wav"))
    return {"voices": voices}

# ============================================================
# ─── GET /health ───────────────────────────────────────────
# ============================================================
@app.get("/health")
def health():
    voice_count = len([f for f in os.listdir(VOICES_DIR) if f.endswith(".wav")])
    return {"status": "ok", "model": MODEL_NAME, "device": DEVICE, "voices": voice_count}

# ============================================================
# ─── MAIN ──────────────────────────────────────────────────
# ============================================================
if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host=ARGS.host, port=ARGS.port)
