"""
RpgAi Local Text-to-Image Server

Endpoints:
  POST /txt2img
    JSON body:
      prompt          (str, required)
      model           (str, optional) — subfolder name under ./models/; uses loaded model if omitted
      steps           (int, optional, default 30)
      width           (int, optional, default 512)
      height          (int, optional, default 768)
      guidance_scale  (float, optional, default 7.5)
    Returns: JSON { "image": "<base64 PNG>" }

  GET /models
    Returns: JSON { "models": ["<name>", ...], "loaded": "<name or null>" }

  GET /health
    Returns: JSON { "status": "ok", ... }

Models:
  Place any diffusers-compatible txt2img checkpoint in ./models/<name>/
  AutoPipelineForText2Image will attempt to load it automatically.
  Model switching is supported: the server reloads when a different model is requested.
"""

import base64
import io
import os
from contextlib import asynccontextmanager

import torch
from diffusers import AutoPipelineForText2Image
from fastapi import FastAPI, HTTPException
from fastapi.responses import JSONResponse
from pydantic import BaseModel

# ---------------------------------------------------------------------------
# Config
# ---------------------------------------------------------------------------
MODELS_BASE = os.path.abspath(os.path.join(os.path.dirname(__file__), "models"))
DEFAULT_PORT = 8003


def _best_device() -> str:
    if torch.cuda.is_available():
        return "cuda"
    if hasattr(torch.backends, "mps") and torch.backends.mps.is_available():
        return "mps"
    return "cpu"


def _best_dtype(device: str):
    if device == "cuda":
        return torch.bfloat16
    if device == "mps":
        return torch.float16
    return torch.float32


DEVICE = _best_device()

# ---------------------------------------------------------------------------
# Global state
# ---------------------------------------------------------------------------
pipe = None
loaded_model_name: str | None = None


def _list_models() -> list[str]:
    """Return sorted list of model directory names under MODELS_BASE."""
    if not os.path.isdir(MODELS_BASE):
        return []
    return sorted(
        d for d in os.listdir(MODELS_BASE)
        if os.path.isdir(os.path.join(MODELS_BASE, d))
    )


def _load_model(name: str):
    global pipe, loaded_model_name

    model_path = os.path.join(MODELS_BASE, name)
    if not os.path.isdir(model_path):
        raise RuntimeError(
            f"Model directory not found: {model_path}\n"
            "Place a diffusers-compatible txt2img model there."
        )

    if loaded_model_name == name and pipe is not None:
        return  # already loaded

    print(f"[t2i] Loading model '{name}' from {model_path}  device={DEVICE}")
    if pipe is not None:
        # Release previous model from VRAM before loading new one
        del pipe
        if DEVICE == "cuda":
            torch.cuda.empty_cache()

    pipe = AutoPipelineForText2Image.from_pretrained(
        model_path,
        torch_dtype=_best_dtype(DEVICE),
        local_files_only=True,
        use_safetensors=True,
    )
    pipe.to(DEVICE)
    loaded_model_name = name
    print(f"[t2i] Model '{name}' ready.")


@asynccontextmanager
async def lifespan(app: FastAPI):
    # Auto-load the first available model at startup
    models = _list_models()
    if models:
        try:
            _load_model(models[0])
        except Exception as e:
            print(f"[t2i] WARNING: auto-load failed: {e}")
    else:
        print(f"[t2i] No models found in {MODELS_BASE} — server ready, waiting for models.")
    yield
    print("[t2i] Shutting down.")


app = FastAPI(title="RpgAi Local Text-to-Image Server", lifespan=lifespan)


# ---------------------------------------------------------------------------
# Request schema
# ---------------------------------------------------------------------------
class TxtImgRequest(BaseModel):
    prompt: str
    model: str = ""
    steps: int = 30
    width: int = 512
    height: int = 768
    guidance_scale: float = 7.5


# ---------------------------------------------------------------------------
# /txt2img
# ---------------------------------------------------------------------------
@app.post("/txt2img")
async def txt2img(req: TxtImgRequest):
    global pipe, loaded_model_name

    # Resolve which model to use
    target_model = req.model.strip() if req.model.strip() else None
    if target_model is None:
        # Use loaded model if any, else try first available
        if loaded_model_name is None:
            models = _list_models()
            if not models:
                raise HTTPException(
                    status_code=503,
                    detail=f"No models found in {MODELS_BASE}. "
                           "Place a diffusers txt2img model in a subdirectory."
                )
            target_model = models[0]
        else:
            target_model = loaded_model_name

    try:
        _load_model(target_model)
    except RuntimeError as e:
        raise HTTPException(status_code=404, detail=str(e))

    try:
        with torch.inference_mode():
            image = pipe(
                prompt=req.prompt,
                num_inference_steps=req.steps,
                width=req.width,
                height=req.height,
                guidance_scale=req.guidance_scale,
            ).images[0]
    except torch.cuda.OutOfMemoryError:
        raise HTTPException(
            status_code=503,
            detail="GPU out of memory. Reduce image dimensions or enable CPU offload."
        )
    except Exception as e:
        raise HTTPException(status_code=500, detail=f"Inference error: {e}")

    buf = io.BytesIO()
    image.save(buf, format="PNG")
    b64 = base64.b64encode(buf.getvalue()).decode()
    return JSONResponse({"image": b64, "model": loaded_model_name})


# ---------------------------------------------------------------------------
# /models
# ---------------------------------------------------------------------------
@app.get("/models")
def models():
    return {"models": _list_models(), "loaded": loaded_model_name}


# ---------------------------------------------------------------------------
# /health
# ---------------------------------------------------------------------------
@app.get("/health")
def health():
    return {
        "status":       "ok",
        "device":       DEVICE,
        "models_base":  MODELS_BASE,
        "loaded_model": loaded_model_name,
        "models":       _list_models(),
    }


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------
if __name__ == "__main__":
    import uvicorn
    os.environ.setdefault("TRANSFORMERS_OFFLINE", "1")
    os.environ.setdefault("HF_HUB_OFFLINE", "1")
    uvicorn.run(app, host="127.0.0.1", port=DEFAULT_PORT)
