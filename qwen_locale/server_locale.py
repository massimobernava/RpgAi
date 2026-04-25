"""
Local image-editing server for RpgAi.

Exposes a single endpoint:
  POST /edit
    Form fields:
      prompt      (str, required)
      lora_name   (str, optional) — subfolder name under ./loras/
      lora_scale  (float, optional, default 1.0)
      steps       (int,   optional, default 30)
      strength    (float, optional, default 0.75) — i2i denoising strength
    Files:
      images      (one or more image files) — first is treated as base image;
                   additional images are composited / passed as extra conditioning
                   if the loaded model supports it (e.g. IP-Adapter multi-image).

Returns: raw PNG bytes (Content-Type: image/png)

Model:
  Place any diffusers-compatible img2img checkpoint in ./qwen_model.
  Recommended: a Qwen-compatible or instruction-following img2img model.
  AutoPipelineForImage2Image will attempt to load it automatically.

LoRA:
  Place LoRA weights in subdirectories of ./loras/<lora_name>/.
  Multiple LoRAs can be pre-loaded at startup by listing them in PRELOAD_LORAS.
"""

import io
import os
import torch
from contextlib import asynccontextmanager
from typing import List, Optional

from fastapi import FastAPI, File, UploadFile, Form, HTTPException
from fastapi.responses import Response
from diffusers import AutoPipelineForImage2Image
from PIL import Image

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
MODEL_PATH   = os.path.abspath("./qwen_model")
LORA_BASE    = os.path.abspath("./loras")

# LoRA names (subfolder names under ./loras/) to pre-load at startup.
# Pre-loading avoids per-request overhead. Leave empty to load on demand.
PRELOAD_LORAS: List[str] = []

# Auto-detect best available device: CUDA > MPS (Apple Silicon) > CPU
def _best_device() -> str:
    if torch.cuda.is_available():
        return "cuda"
    if hasattr(torch.backends, "mps") and torch.backends.mps.is_available():
        return "mps"
    return "cpu"

DEVICE = _best_device()

# bfloat16 not supported on MPS/CPU — fall back to float16 or float32
def _best_dtype():
    if DEVICE == "cuda":
        return torch.bfloat16
    if DEVICE == "mps":
        return torch.float16
    return torch.float32

# ---------------------------------------------------------------------------
# Global state
# ---------------------------------------------------------------------------
pipe = None
loaded_loras: dict[str, bool] = {}   # lora_name → True (already loaded)


# ---------------------------------------------------------------------------
# Lifespan (replaces deprecated @app.on_event("startup"))
# ---------------------------------------------------------------------------
@asynccontextmanager
async def lifespan(app: FastAPI):
    global pipe

    if not os.path.exists(MODEL_PATH):
        raise RuntimeError(
            f"Model directory not found: {MODEL_PATH}\n"
            "Download a diffusers-compatible img2img model and place it there."
        )

    print(f"[startup] Loading model from: {MODEL_PATH}  device={DEVICE}")
    pipe = AutoPipelineForImage2Image.from_pretrained(
        MODEL_PATH,
        torch_dtype=_best_dtype(),
        local_files_only=True,
        use_safetensors=True,
    )

    # On CUDA with large models, CPU offload reduces VRAM pressure.
    # Uncomment the line that suits your hardware:
    # pipe.enable_model_cpu_offload()   # recommended for <12 GB VRAM
    # pipe.enable_sequential_cpu_offload()  # extreme memory saving, very slow
    pipe.to(DEVICE)

    # Pre-load declared LoRAs so the first request is fast
    for name in PRELOAD_LORAS:
        _ensure_lora_loaded(name)

    print("[startup] Model ready.")
    yield
    # Cleanup on shutdown (optional)
    print("[shutdown] Releasing model.")


app = FastAPI(title="RpgAi Local Image-Edit Server", lifespan=lifespan)


# ---------------------------------------------------------------------------
# LoRA helpers
# ---------------------------------------------------------------------------
def _ensure_lora_loaded(lora_name: str):
    """Load a LoRA adapter if not already present in the pipeline."""
    if lora_name in loaded_loras:
        return
    lora_path = os.path.join(LORA_BASE, lora_name)
    if not os.path.exists(lora_path):
        print(f"[lora] WARNING: LoRA '{lora_name}' not found at {lora_path}. Skipping.")
        return
    print(f"[lora] Loading: {lora_name}")
    pipe.load_lora_weights(lora_path, adapter_name=lora_name, local_files_only=True)
    loaded_loras[lora_name] = True


# ---------------------------------------------------------------------------
# /edit endpoint
# ---------------------------------------------------------------------------
@app.post("/edit")
async def edit_image(
    prompt:     str            = Form(...),
    lora_name:  Optional[str]  = Form(None,  description="Subfolder name under ./loras/"),
    lora_scale: float          = Form(1.0,   description="LoRA weight scale [0.0 – 1.0]"),
    steps:      int            = Form(30,    description="Number of inference steps"),
    strength:   float          = Form(0.75,  description="i2i denoising strength [0.0 – 1.0]"),
    images:     List[UploadFile] = File(...),
):
    # --- Load images ---
    pil_images: List[Image.Image] = []
    for f in images:
        data = await f.read()
        pil_images.append(Image.open(io.BytesIO(data)).convert("RGB"))

    if not pil_images:
        raise HTTPException(status_code=400, detail="At least one image is required.")

    # Base image is always the first upload.
    # If the model supports multi-image conditioning (e.g. IP-Adapter), extra
    # images are passed via ip_adapter_image. Otherwise they are ignored with a warning.
    base_image = pil_images[0]
    extra_images = pil_images[1:] if len(pil_images) > 1 else []
    if extra_images:
        print(f"[edit] {len(extra_images)} extra conditioning image(s) provided.")

    # --- LoRA ---
    active_adapters = []
    if lora_name:
        _ensure_lora_loaded(lora_name)
        if lora_name in loaded_loras:
            active_adapters = [lora_name]
            pipe.set_adapters(active_adapters, adapter_weights=[lora_scale])

    # --- Inference ---
    try:
        call_kwargs = dict(
            prompt=prompt,
            image=base_image,
            strength=strength,
            num_inference_steps=steps,
            guidance_scale=2.5,
        )

        # Multi-image conditioning: pass extras as ip_adapter_image if supported.
        # This is model-dependent; remove if your model raises an error.
        if extra_images and hasattr(pipe, "image_encoder"):
            call_kwargs["ip_adapter_image"] = extra_images if len(extra_images) > 1 else extra_images[0]

        with torch.inference_mode():
            result: Image.Image = pipe(**call_kwargs).images[0]

    except torch.cuda.OutOfMemoryError:
        raise HTTPException(status_code=503, detail="GPU out of memory. Reduce image size or enable CPU offload.")
    except Exception as e:
        raise HTTPException(status_code=500, detail=f"Inference error: {e}")
    finally:
        # Disable adapters so the next request starts clean
        if active_adapters:
            pipe.disable_lora()

    # --- Return raw PNG bytes ---
    buf = io.BytesIO()
    result.save(buf, format="PNG")
    buf.seek(0)
    return Response(content=buf.read(), media_type="image/png")


# ---------------------------------------------------------------------------
# /health — quick liveness check
# ---------------------------------------------------------------------------
@app.get("/health")
def health():
    return {"status": "ok", "device": DEVICE, "model": MODEL_PATH,
            "loras_loaded": list(loaded_loras.keys())}


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------
if __name__ == "__main__":
    import uvicorn
    # Force offline mode — no HuggingFace Hub calls
    os.environ["TRANSFORMERS_OFFLINE"] = "1"
    os.environ["HF_HUB_OFFLINE"]       = "1"
    uvicorn.run(app, host="127.0.0.1", port=8000)
