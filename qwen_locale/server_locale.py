"""
Local image-editing server for RpgAi.
Auto-downloads from HuggingFace on first run, then runs fully offline.
Supports Qwen-Image-Edit-2511 and any AutoPipelineForImage2Image-compatible model.

Model architecture: Qwen-Image-Edit-2511 is a DiT (Diffusion Transformer, ~14B params,
~28 GB BF16) — NOT a UNet. torch.compile must target pipe.transformer, not pipe.unet.

Recommended launch for RTX 5090 (Blackwell SM_120, 32 GB GDDR7):
  python server_locale.py --dtype bf16 --host 0.0.0.0 --lightning --fast --cpu-offload
  → ~1 min/image. cpu-offload is required: model barely fits in 32 GB without it.

  DO NOT use --quantize 8bit on Blackwell: bitsandbytes 8bit has SM_120 incompatibility
  (RuntimeError: invalid argument to getCurrentStream). Use torchao_int8 instead.

  DO NOT use attention_slicing (wrong for DiT / high-VRAM GPUs). --fast enables
  xformers or flash SDP, which is the correct attention optimization.

LoRA architecture (two-tier):
  - Lightning LoRA (--lightning): loaded at startup as a persistent PEFT adapter.
    NOT fused via fuse_lora: calling fuse_lora after enable_sequential_cpu_offload
    corrupts the weights (layers offloaded to CPU during fuse, delta applied wrong).
    Kept active via set_adapters([lightning, ...]) on every inference request.
  - User LoRA (dynamic, per /image lora request): loaded on demand from ./loras/,
    added to the set_adapters call alongside Lightning. NOT fused per-request either:
    fusing + unload_lora_weights would destroy the adapter after the first call,
    crashing set_adapters on the second request.

Inference uses torch.no_grad() (NOT torch.inference_mode()): inference_mode creates
frozen tensors that PEFT cannot set requires_grad on between requests, causing
RuntimeError: Setting requires_grad=True on inference tensor outside InferenceMode.

LoRA offline loading: load_lora_weights() with a repo path triggers HF offline-mode
detection that fails even with weight_name kwarg. Fix: load state_dict manually via
safetensors.torch.load_file, then pass the dict directly to load_lora_weights().

Examples:
  python server_locale.py
  python server_locale.py --host 0.0.0.0 --dtype bf16
  python server_locale.py --host 0.0.0.0 --lightning --fast --cpu-offload
  python server_locale.py --host 0.0.0.0 --quantize torchao_int8 --lightning

Endpoints:
  POST /edit  — image editing
  GET /health — liveness check
"""

import io
import os
import sys
import re
import argparse
import torch
from contextlib import asynccontextmanager
from typing import List, Optional

from fastapi import FastAPI, File, UploadFile, Form, HTTPException
from fastapi.responses import Response
from diffusers import AutoPipelineForImage2Image
from PIL import Image
from datetime import datetime

# ============================================================
# CLI
# ============================================================
def parse_args():
    p = argparse.ArgumentParser(description="RpgAi Image-Edit Server")
    p.add_argument("--model", default=None, help="Repo HF (default: Qwen/Qwen-Image-Edit-2511)")
    p.add_argument("--dtype", default=None, choices=["fp16", "bf16", "fp32"], help="Precisione (fp16/bf16 dimezzano VRAM)")
    p.add_argument("--quantize", default=None, choices=["4bit", "8bit", "torchao_int8", "torchao_float8"], help="Quantizzazione: 4bit/8bit=bitsandbytes, torchao_int8/float8=TorchAO (consigliato Blackwell)")
    p.add_argument("--cpu-offload", action="store_true", help="Sequential CPU offload (VRAM minima)")
    p.add_argument("--fast", action="store_true", help="Attenzione sdpa + VAE tiling + torch.compile")
    p.add_argument("--lightning", action="store_true", help="Carica LoRA Lightning (4-step distillata, ~10x speedup)")
    p.add_argument("--host", default="127.0.0.1", help="Host (default: 127.0.0.1)")
    p.add_argument("--port", type=int, default=8000, help="Porta (8000)")
    p.add_argument("--model-dir", default=None, help="Directory locale (default: ./qwen_model)")
    p.add_argument("--lora-dir", default=None, help="Directory LoRA (default: ./loras)")
    p.add_argument("--guidance-scale", type=float, default=None, help="Guidance scale (default: 1.0)")
    return p.parse_args()

ARGS = parse_args()

# ============================================================
# Config
# ============================================================
MODEL_REPO = ARGS.model or os.environ.get("MODEL_REPO", "Qwen/Qwen-Image-Edit-2511")
HF_TOKEN = os.environ.get("HF_TOKEN", None)

MODEL_DIR = os.path.abspath(ARGS.model_dir or os.environ.get("MODEL_DIR", os.path.join(os.path.dirname(__file__), "qwen_model")))
LORA_DIR = os.path.abspath(ARGS.lora_dir or os.environ.get("LORA_DIR", os.path.join(os.path.dirname(__file__), "loras")))
OUTPUT_DIR = os.path.join(os.path.dirname(__file__), "output")

PRELOAD_LORAS: List[str] = []
USE_OFFLOAD = ARGS.cpu_offload or os.environ.get("CPU_OFFLOAD", "0") == "1"
QUANTIZE = ARGS.quantize or os.environ.get("QUANTIZE", None)
USE_LIGHTNING = ARGS.lightning or os.environ.get("LIGHTNING", "0") == "1"
IS_QUANTIZED   = QUANTIZE in ("4bit", "8bit", "torchao_int8", "torchao_float8")
IS_TORCHAO     = QUANTIZE in ("torchao_int8", "torchao_float8")
IS_BITSANDBYTES = QUANTIZE in ("4bit", "8bit")

# ============================================================
# Device / dtype
# ============================================================
def _best_device() -> str:
    override = os.environ.get("DEVICE", None)
    if override:
        return override
    if torch.cuda.is_available():
        return "cuda"
    if hasattr(torch.backends, "mps") and torch.backends.mps.is_available():
        return "mps"
    return "cpu"

def _best_dtype():
    if ARGS.dtype == "fp16":
        return torch.float16
    if ARGS.dtype == "bf16":
        return torch.bfloat16
    if ARGS.dtype == "fp32":
        return torch.float32
    dev = _best_device()
    if dev == "cuda":
        return torch.bfloat16
    if dev == "mps":
        return torch.float16
    return torch.float32

DEVICE = _best_device()
DTYPE = _best_dtype()

# TF32 — ~3x speedup on Ampere/Ada/Blackwell with negligible precision loss
if DEVICE == "cuda":
    torch.set_float32_matmul_precision('high')
    torch.backends.cuda.matmul.allow_tf32 = True

# ============================================================
# Banner
# ============================================================
print("╔══════════════════════════════════════════════╗")
print("║   RpgAi Local Image-Edit Server             ║")
print("╠══════════════════════════════════════════════╣")
print(f"║  Modello:   {MODEL_REPO:<32}║")
print(f"║  Device:    {DEVICE:<32}║")
print(f"║  Dtype:     {str(DTYPE):<32}║")
if ARGS.dtype:
    print(f"║  (forzato) {'':<32}║")
if QUANTIZE:
    quant_label = QUANTIZE + (" (TorchAO)" if IS_TORCHAO else " (bitsandbytes)")
    print(f"║  Quant:     {quant_label:<32}║")
print(f"║  Offload:   {'SI' if USE_OFFLOAD else 'NO':<32}║")
print(f"║  Fast:      {'SI' if ARGS.fast else 'NO':<32}║")
print(f"║  Lightning: {'SI' if USE_LIGHTNING else 'NO':<32}║")
print(f"║  Model dir: {os.path.basename(MODEL_DIR):<32}║")
print("╚══════════════════════════════════════════════╝")

# ============================================================
# Stato globale
# ============================================================
pipe = None
loaded_loras: dict[str, bool] = {}
quantization_active = False   # True only if quantization actually succeeded

# ============================================================
# Caricamento modello
# ============================================================
def _download_if_missing():
    if os.path.exists(os.path.join(MODEL_DIR, "model_index.json")):
        return
    print(f"[model] Modello non trovato in {MODEL_DIR}")
    print(f"[model] Download da {MODEL_REPO}...")
    try:
        from huggingface_hub import snapshot_download
    except ImportError:
        raise RuntimeError("huggingface_hub non installato.\n  pip install huggingface_hub")
    snapshot_download(
        repo_id=MODEL_REPO,
        local_dir=MODEL_DIR,
        token=HF_TOKEN,
        ignore_patterns=["*.msgpack", "flax_model*", "tf_model*", "rust_model*"],
    )
    print("[model] Download completato.")

def _build_load_kwargs():
    global quantization_active
    load_dtype = None if IS_QUANTIZED else DTYPE
    # low_cpu_mem_usage always: avoids staging full model (~28 GB) in RAM before GPU transfer
    kwargs = dict(use_safetensors=True, local_files_only=True, low_cpu_mem_usage=True)
    if IS_QUANTIZED:
        kwargs["torch_dtype"] = DTYPE
        try:
            from diffusers import PipelineQuantizationConfig
            if IS_TORCHAO:
                # TorchAO: PyTorch-native, full Blackwell (SM_120) support
                quant_type = "float8_weight_only" if QUANTIZE == "torchao_float8" else "int8_weight_only"
                quant_config = PipelineQuantizationConfig(
                    quant_backend="torchao",
                    quant_kwargs={"quant_type": quant_type},
                    components_to_quantize=["transformer"],
                )
                print(f"[model] TorchAO: {quant_type}")
            else:
                # bitsandbytes — note: 8bit has known issues on Blackwell (SM_120)
                backend = f"bitsandbytes_{QUANTIZE}"
                quant_kwargs = None
                if QUANTIZE == "4bit":
                    quant_kwargs = {"bnb_4bit_compute_dtype": DTYPE, "bnb_4bit_quant_type": "nf4"}
                quant_config = PipelineQuantizationConfig(
                    quant_backend=backend,
                    quant_kwargs=quant_kwargs,
                    components_to_quantize=["transformer"],
                )
                print(f"[model] bitsandbytes: {backend}")
            kwargs["quantization_config"] = quant_config
            quantization_active = True
        except Exception as e:
            print(f"[model] Quantization unavailable: {e} — loading full precision")
    else:
        kwargs["torch_dtype"] = load_dtype
    return kwargs, load_dtype

def _load_with_fallback(kwargs, load_dtype):
    global DTYPE
    print(f"[model] Caricamento: {MODEL_DIR}  (dtype={load_dtype or 'default'})")
    try:
        return AutoPipelineForImage2Image.from_pretrained(MODEL_DIR, **kwargs)
    except Exception as e:
        print(f"[model] ERRORE con dtype={load_dtype}: {e}")
        if load_dtype == torch.float16:
            print("[model] Riprovo con bfloat16...")
            kwargs["torch_dtype"] = torch.bfloat16
            DTYPE = torch.bfloat16
            return AutoPipelineForImage2Image.from_pretrained(MODEL_DIR, **kwargs)
        elif load_dtype is not None:
            print("[model] Riprovo senza forzare dtype...")
            kwargs.pop("torch_dtype", None)
            DTYPE = _best_dtype()
            return AutoPipelineForImage2Image.from_pretrained(MODEL_DIR, **kwargs)
        raise

def _load_lightning_lora():
    print("[model] Caricamento LoRA Lightning da lightx2v/Qwen-Image-Edit-2511-Lightning...")
    try:
        pipe.load_lora_weights("lightx2v/Qwen-Image-Edit-2511-Lightning", adapter_name="lightning")
        loaded_loras["lightning"] = True
        print("[model] Lightning LoRA: ATTIVA (4 step, ~10x speedup)")
    except Exception as e:
        print(f"[model] Lightning LoRA: ERRORE ({e})")

def _apply_optimizations():
    if not ARGS.fast:
        return

    # Flash attention and VAE tricks are safe for both full-precision and quantized models.
    # Only torch.compile is skipped for quantized/offload (incompatible).
    try:
        pipe.enable_xformers_memory_efficient_attention()
        print("[model] xformers efficient attention: ON")
    except Exception:
        try:
            torch.backends.cuda.enable_flash_sdp(True)
            print("[model] flash SDP: ON")
        except Exception as e:
            print(f"[model] flash attention unavailable: {e}")
    try:
        if hasattr(pipe, "vae"):
            pipe.vae.enable_tiling()
            pipe.vae.enable_slicing()   # reduces peak VRAM during decode
            print("[model] vae_tiling + vae_slicing: ON")
    except Exception as e:
        print(f"[model] vae optimizations: {e}")

    # torch.compile: skip only if quantization actually succeeded (bitsandbytes conflict)
    # and for cpu-offload (sequential offload hooks conflict with compiled graphs)
    if quantization_active:
        print("[model] torch.compile: skipped (quantization active)")
        return
    if USE_OFFLOAD:
        print("[model] torch.compile: skipped (cpu-offload active)")
        return
    try:
        # Qwen-Image-Edit-2511 is a DiT (transformer), not a UNet.
        if hasattr(pipe, "transformer") and pipe.transformer is not None:
            pipe.transformer = torch.compile(pipe.transformer, mode="max-autotune")
            print("[model] torch.compile (transformer): ON")
        elif hasattr(pipe, "unet") and pipe.unet is not None:
            pipe.unet = torch.compile(pipe.unet, mode="max-autotune")
            print("[model] torch.compile (unet): ON")
        else:
            print("[model] torch.compile: no transformer/unet found")
    except Exception as e:
        print(f"[model] torch.compile: {e}")

def _load_model():
    global pipe, DTYPE
    _download_if_missing()
    kwargs, load_dtype = _build_load_kwargs()

    pipe = _load_with_fallback(kwargs, load_dtype)

    if IS_QUANTIZED:
        pass  # transformer already placed on GPU by BnB; VAE/encoders moved below
    elif USE_OFFLOAD:
        pipe.enable_sequential_cpu_offload()
        print("[model] Sequential CPU offload: ON")
    else:
        pipe.to(DEVICE)

    _apply_optimizations()

    if USE_LIGHTNING:
        _load_lightning_lora()

    os.environ["TRANSFORMERS_OFFLINE"] = "1"
    os.environ["HF_HUB_OFFLINE"] = "1"

# ============================================================
# Lifespan
# ============================================================
@asynccontextmanager
async def lifespan(app: FastAPI):
    try:
        _load_model()
        for name in PRELOAD_LORAS:
            _load_lora(name)
        print("[startup] Server ready.")
    except Exception as e:
        print(f"\n[FATAL] {e}")
        print("[FATAL] Suggerimenti: --dtype bf16, --quantize 8bit/4bit, --cpu-offload")
        raise
    yield
    print("[shutdown] Server spento.")

app = FastAPI(title="RpgAi Image-Edit Server", lifespan=lifespan)

# ============================================================
# LoRA loader
# ============================================================
def _load_lora(name: str):
    if name in loaded_loras:
        return  # already loaded or fused
    path = os.path.join(LORA_DIR, name)
    if not os.path.exists(path):
        print(f"[lora] WARNING: '{name}' non trovata in {path}")
        return

    # Resolve the actual .safetensors/.bin file
    if os.path.isdir(path):
        weight_file = None
        for f in sorted(os.listdir(path)):
            if f.endswith(".safetensors") or f.endswith(".bin"):
                weight_file = os.path.join(path, f)
                break
        if weight_file is None:
            print(f"[lora] WARNING: nessun .safetensors/.bin in {path}")
            return
    else:
        weight_file = path

    # Load state dict manually and pass as dict — bypasses diffusers offline-mode
    # weight_name detection which fails even when weight_name kwarg is provided
    print(f"[lora] Caricamento: {os.path.basename(weight_file)}")
    if weight_file.endswith(".safetensors"):
        from safetensors.torch import load_file
        state_dict = load_file(weight_file)
    else:
        state_dict = torch.load(weight_file, map_location="cpu", weights_only=True)

    pipe.load_lora_weights(state_dict, adapter_name=name)
    loaded_loras[name] = True
    print(f"[lora] '{name}' caricata.")

# ============================================================
# POST /edit
# ============================================================
@app.post("/edit")
async def edit_image(
    prompt: str = Form(...),
    lora_name: Optional[str] = Form(None),
    lora_scale: float = Form(1.0),
    steps: int = Form(4),
    strength: float = Form(0.75),
    guidance_scale: Optional[float] = Form(None),
    images: List[UploadFile] = File(...),
):
    # Carica immagini
    pil_images = []
    for f in images:
        data = await f.read()
        pil_images.append(Image.open(io.BytesIO(data)).convert("RGB"))
    if not pil_images:
        raise HTTPException(400, "Almeno un'immagine e' richiesta.")
    base = pil_images[0]
    extra = pil_images[1:]

    # Build adapter list: Lightning always-on (if loaded) + optional user LoRA.
    # torch.no_grad() in _run_pipe() ensures tensors are not inference-mode-frozen,
    # so set_adapters/disable_lora can set requires_grad between requests.
    adapters: list[str] = []
    weights_list: list[float] = []
    if USE_LIGHTNING and "lightning" in loaded_loras:
        adapters.append("lightning")
        weights_list.append(1.0)
    if lora_name:
        _load_lora(lora_name)
        if lora_name in loaded_loras:
            adapters.append(lora_name)
            weights_list.append(lora_scale)
            print(f"[lora] '{lora_name}' applicata (scala: {lora_scale})")
    if adapters:
        pipe.set_adapters(adapters, adapter_weights=weights_list)
    elif loaded_loras:
        pipe.disable_lora()

    # Guidance scale — request param wins; fall back to server CLI arg or env var
    gs = guidance_scale if guidance_scale is not None else ARGS.guidance_scale
    if gs is None:
        gs = float(os.environ.get("GUIDANCE_SCALE", "1.0"))

    # Probe pipeline signature once to drop unsupported params before the first call
    import inspect
    _pipe_sig = set(inspect.signature(pipe.__call__).parameters)
    kw = dict(prompt=prompt, image=base, num_inference_steps=steps, guidance_scale=gs)
    if "strength" in _pipe_sig:
        kw["strength"] = strength
    if extra and hasattr(pipe, "image_encoder"):
        kw["ip_adapter_image"] = extra[0] if len(extra) == 1 else extra

    def _run_pipe():
        with torch.no_grad():
            return pipe(**kw).images[0]

    try:
        result = _run_pipe()
    except TypeError as e:
        # Remove any remaining unsupported kwarg and retry once
        m = re.search(r"unexpected keyword argument '(\w+)'", str(e))
        if m:
            kw.pop(m.group(1), None)
            print(f"[edit] Rimosso parametro non supportato: '{m.group(1)}'")
            try:
                result = _run_pipe()
            except Exception as e2:
                raise HTTPException(500, f"Inference error: {e2}")
        else:
            raise HTTPException(500, f"Inference error: {e}")
    except torch.cuda.OutOfMemoryError:
        raise HTTPException(503, "GPU out of memory. Prova --quantize 8bit, --cpu-offload, o un modello piu' piccolo.")
    except Exception as e:
        raise HTTPException(500, f"Inference error: {e}")

    # Salva in locale (utile se il client va in timeout)
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    ts = datetime.now().strftime("%Y%m%d_%H%M%S_%f")[:-3]
    safe = "".join(c for c in prompt[:40] if c.isalnum() or c in " _-").strip().replace(" ", "_")
    out_path = os.path.join(OUTPUT_DIR, f"{ts}_{safe}.png")
    result.save(out_path, format="PNG")
    print(f"[edit] Salvato: {out_path}")

    buf = io.BytesIO()
    result.save(buf, format="PNG")
    buf.seek(0)
    return Response(content=buf.read(), media_type="image/png")

# ============================================================
# GET /health
# ============================================================
@app.get("/health")
def health():
    return {
        "status": "ok",
        "model": MODEL_REPO,
        "device": DEVICE,
        "dtype": str(DTYPE),
        "quantize": QUANTIZE,
        "offload": USE_OFFLOAD,
        "lightning": USE_LIGHTNING,
        "loras": list(loaded_loras.keys()),
    }

# ============================================================
# Main
# ============================================================
if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host=ARGS.host, port=ARGS.port)