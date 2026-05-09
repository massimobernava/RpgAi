"""
RpgAi FaceSwap Locale Server

RESPONSIBLE USE NOTICE
----------------------
This tool is intended exclusively for creative, fictional, and artistic purposes
within the RpgAi game engine — replacing AI-generated NPC portraits in rendered
game scenes. It must NOT be used to:
  - Swap real people's faces without their explicit consent
  - Create deceptive, defamatory, or non-consensual intimate imagery
  - Produce content that violates applicable laws or platform policies

By running this server you accept sole responsibility for the content you generate.

Endpoint:
  POST /swap
    Files (multipart):
      target          — the scene image to edit
      sources[]       — reference face images in left-to-right slot order
    Form fields:
      positions       — JSON array of face-slot indices to replace, e.g. "[0, 2]"
                        Must have the same length as sources[].
                        Null slots are expressed by omitting them from positions
                        and not including a corresponding source.
      enhance         — bool (default False); if True, run GFPGAN face enhancement
                        after swapping (requires gfpgan package + GFPGANv1.4 weights)

    Example: /swap alice null bob
      C++ sends: sources=[alice_asset, bob_asset], positions=[0, 2], enhance=false
      Server: swaps face-slot 0 ← alice, face-slot 2 ← bob, slot 1 untouched.

Returns: raw PNG bytes (Content-Type: image/png)

Dependencies:
  pip install insightface onnxruntime fastapi uvicorn python-multipart pillow numpy opencv-python-headless
  pip install gfpgan   # optional — enables face enhancement

Models:
  insightface FaceAnalysis (buffalo_l) — auto-downloaded to ~/.insightface/models/
  inswapper_128.onnx                  — place in ./models/inswapper_128.onnx
  Download: https://huggingface.co/deepinsight/inswapper/resolve/main/inswapper_128.onnx

  GFPGANv1.4.pth (optional)          — place in ./models/GFPGANv1.4.pth
  Download: https://github.com/TencentARC/GFPGAN/releases/download/v1.3.4/GFPGANv1.4.pth
"""

import io
import json
import os
from contextlib import asynccontextmanager
from typing import List

import cv2
import numpy as np
from fastapi import FastAPI, File, Form, HTTPException, UploadFile
from fastapi.responses import Response
from PIL import Image

# ---------------------------------------------------------------------------
# Config
# ---------------------------------------------------------------------------
INSWAPPER_PATH = os.path.join(os.path.dirname(__file__), "models", "inswapper_128.onnx")
GFPGAN_PATH    = os.path.join(os.path.dirname(__file__), "models", "GFPGANv1.4.pth")
DET_SIZE       = (640, 640)   # face detection resolution
CTX_ID         = 0            # GPU index; use -1 for CPU

# ---------------------------------------------------------------------------
# Global model handles
# ---------------------------------------------------------------------------
face_app  = None
swapper   = None
gfpganer  = None


def _load_models():
    global face_app, swapper, gfpganer

    try:
        import insightface
        from insightface.app import FaceAnalysis
    except ImportError:
        raise RuntimeError(
            "insightface not installed. Run: pip install insightface onnxruntime"
        )

    # Try CUDA first, fall back to CPU
    providers = ["CUDAExecutionProvider", "CPUExecutionProvider"]

    face_app = FaceAnalysis(name="buffalo_l", providers=providers)
    face_app.prepare(ctx_id=CTX_ID, det_size=DET_SIZE)

    if not os.path.exists(INSWAPPER_PATH):
        raise RuntimeError(
            f"inswapper_128.onnx not found at {INSWAPPER_PATH}.\n"
            "Download from: https://huggingface.co/deepinsight/inswapper/resolve/main/inswapper_128.onnx\n"
            f"and place it at: {INSWAPPER_PATH}"
        )

    swapper = insightface.model_zoo.get_model(
        INSWAPPER_PATH, providers=providers
    )
    print(f"[startup] Models ready. inswapper: {INSWAPPER_PATH}")

    # GFPGAN — optional; warn and continue if not available
    try:
        from gfpgan import GFPGANer
        if os.path.exists(GFPGAN_PATH):
            gfpganer = GFPGANer(
                model_path=GFPGAN_PATH,
                upscale=1,
                arch="clean",
                channel_multiplier=2,
            )
            print(f"[startup] GFPGAN ready. weights: {GFPGAN_PATH}")
        else:
            print(f"[startup] GFPGAN weights not found at {GFPGAN_PATH} — enhancement disabled.")
            print(        "          Download: https://github.com/TencentARC/GFPGAN/releases/download/v1.3.4/GFPGANv1.4.pth")
    except ImportError:
        print("[startup] gfpgan package not installed — enhancement disabled. Run: pip install gfpgan")


@asynccontextmanager
async def lifespan(app: FastAPI):
    _load_models()
    yield


app = FastAPI(title="RpgAi Face-Swap Server", lifespan=lifespan)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
def _decode_image(data: bytes) -> np.ndarray:
    arr = np.frombuffer(data, np.uint8)
    img = cv2.imdecode(arr, cv2.IMREAD_COLOR)
    if img is None:
        raise ValueError("Cannot decode image — unsupported format or corrupt data.")
    return img


def _encode_png(img: np.ndarray) -> bytes:
    ok, buf = cv2.imencode(".png", img)
    if not ok:
        raise RuntimeError("Failed to encode result as PNG.")
    return buf.tobytes()


def _get_faces_sorted(img: np.ndarray):
    """Detect faces and return them sorted left-to-right by bounding-box x."""
    faces = face_app.get(img)
    faces.sort(key=lambda f: f.bbox[0])
    return faces


# ---------------------------------------------------------------------------
# /swap endpoint
# ---------------------------------------------------------------------------
@app.post("/swap")
async def swap_faces(
    target:    UploadFile        = File(...),
    sources:   List[UploadFile]  = File(...),
    positions: str               = Form(...),   # JSON array e.g. "[0, 2]"
    enhance:   bool              = Form(False),  # run GFPGAN after swap
):
    # --- Parse positions ---
    try:
        pos_list: List[int] = json.loads(positions)
    except Exception:
        raise HTTPException(status_code=400, detail=f"Invalid positions JSON: {positions!r}")

    if len(pos_list) != len(sources):
        raise HTTPException(
            status_code=400,
            detail=f"positions length ({len(pos_list)}) must match sources count ({len(sources)})."
        )

    # --- Load target ---
    target_bytes = await target.read()
    try:
        target_img = _decode_image(target_bytes)
    except ValueError as e:
        raise HTTPException(status_code=400, detail=f"Target image error: {e}")

    # --- Detect faces in target, sorted left-to-right ---
    target_faces = _get_faces_sorted(target_img)
    if not target_faces:
        raise HTTPException(status_code=422, detail="No faces detected in the target image.")

    result_img = target_img.copy()

    # --- For each (source, face_slot) pair: swap ---
    for slot_idx, src_file in zip(pos_list, sources):
        if slot_idx >= len(target_faces):
            print(f"[swap] Slot {slot_idx} out of range "
                  f"(only {len(target_faces)} face(s) detected). Skipping.")
            continue

        src_bytes = await src_file.read()
        try:
            src_img = _decode_image(src_bytes)
        except ValueError as e:
            print(f"[swap] Source for slot {slot_idx} decode error: {e}. Skipping.")
            continue

        src_faces = _get_faces_sorted(src_img)
        if not src_faces:
            print(f"[swap] No face detected in source for slot {slot_idx}. Skipping.")
            continue

        src_face    = src_faces[0]          # use first face found in reference
        target_face = target_faces[slot_idx]

        try:
            result_img = swapper.get(result_img, target_face, src_face, paste_back=True)
        except Exception as e:
            print(f"[swap] Swap error for slot {slot_idx}: {e}. Skipping.")

    # --- Optional GFPGAN face enhancement ---
    if enhance:
        if gfpganer is not None:
            try:
                _, _, result_img = gfpganer.enhance(
                    result_img,
                    has_aligned=False,
                    only_center_face=False,
                    paste_back=True,
                )
                print("[swap] GFPGAN enhancement applied.")
            except Exception as e:
                print(f"[swap] GFPGAN enhancement error: {e}. Returning unenhanced result.")
        else:
            print("[swap] enhance=True but GFPGAN not available — skipping enhancement.")

    return Response(content=_encode_png(result_img), media_type="image/png")


# ---------------------------------------------------------------------------
# /health
# ---------------------------------------------------------------------------
@app.get("/health")
def health():
    return {
        "status":    "ok",
        "inswapper": os.path.exists(INSWAPPER_PATH),
        "gfpgan":    gfpganer is not None,
    }


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------
if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="127.0.0.1", port=8001)
