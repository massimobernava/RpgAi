"""
RpgAi FaceSwap + Image Tools Server

RESPONSIBLE USE NOTICE
----------------------
This tool is intended exclusively for creative, fictional, and artistic purposes
within the RpgAi game engine. It must NOT be used to swap real people's faces
without consent or to create deceptive/non-consensual content.

Endpoints
---------
POST /swap        — face swap (target + source references; enhance=gfpgan|codeformer)
POST /detect      — detect faces, return bounding boxes + landmarks
POST /embed       — extract face embedding vector
POST /register    — store NPC face embedding (faces/<npc_id>.npy, multi: append=true)
DELETE /register/<id> — remove registered NPC
POST /identify    — match face against registered NPC embeddings (max over set)
POST /upscale     — Real-ESRGAN upscaling (optional, 2x/4x)
GET  /registered  — list NPCs with n_embeddings
GET  /health      — loaded models status
GET  /job/<id>    — poll async job {status, step, done, total, result|error}
GET  /jobs        — list recent jobs
Video pipeline (/video/*): heavy ops accept run_async=true → {job_id}.

Dependencies:
  pip install insightface onnxruntime fastapi uvicorn python-multipart pillow numpy opencv-python-headless
  pip install gfpgan        # optional — face enhancement after swap
  pip install realesrgan    # optional — upscaling

Models:
  insightface FaceAnalysis (buffalo_l) — auto-downloaded to ~/.insightface/models/
  inswapper_128.onnx  — place in ./models/inswapper_128.onnx
  GFPGANv1.4.pth      — place in ./models/GFPGANv1.4.pth (optional)
  RealESRGAN_x4plus.pth — place in ./models/RealESRGAN_x4plus.pth (optional)
"""

import io
import json
import os
import re
import shutil
import subprocess
import threading
import time
import uuid
from contextlib import asynccontextmanager
from pathlib import Path
from types import SimpleNamespace
from typing import List, Optional

import cv2
import numpy as np
from fastapi import FastAPI, File, Form, HTTPException, UploadFile
from fastapi.responses import JSONResponse, Response
from PIL import Image

# ---------------------------------------------------------------------------
# Config
# ---------------------------------------------------------------------------
INSWAPPER_PATH   = os.path.join(os.path.dirname(__file__), "models", "inswapper_128.onnx")
GFPGAN_PATH      = os.path.join(os.path.dirname(__file__), "models", "GFPGANv1.4.pth")
REALESRGAN_PATH  = os.path.join(os.path.dirname(__file__), "models", "RealESRGAN_x4plus.pth")
FACEPARSER_PATH  = os.path.join(os.path.dirname(__file__), "models", "faceparser_resnet34.onnx")
OCCLUDER_PATH    = os.path.join(os.path.dirname(__file__), "models", "occluder.onnx")
CODEFORMER_PATH  = os.path.join(os.path.dirname(__file__), "models", "codeformer_fp16.onnx")
FACES_DIR        = Path(os.path.dirname(__file__)) / "faces"
FACES_DIR.mkdir(exist_ok=True)
DET_SIZE         = (640, 640)
CTX_ID           = 0           # GPU index; use -1 for CPU
DEFAULT_DET_THRESH = 0.5       # InsightFace default; per-call override via _get_faces_sorted

# InsightFace det_thresh is a mutable attribute on a shared model: every caller
# must set it under this lock or a concurrent request inherits the wrong value.
model_lock = threading.Lock()


def _ort_providers() -> list:
    """Execution providers for all ONNX sessions.

    CoreML is opt-in (FACESWAP_COREML=1): faster on Apple Silicon but some
    insightface graphs fall back per-node or misbehave — test before trusting.
    """
    provs = ["CUDAExecutionProvider", "CPUExecutionProvider"]
    if os.environ.get("FACESWAP_COREML") == "1":
        provs.insert(0, "CoreMLExecutionProvider")
    return provs

# ---------------------------------------------------------------------------
# Global model handles
# ---------------------------------------------------------------------------
face_app         = None
swapper          = None
gfpganer         = None
upscaler         = None   # Real-ESRGAN (optional)
faceparser_sess  = None   # faceparser_resnet34.onnx — primary parts parser (onnxruntime only)
face_parser      = None   # BiSeNet — fallback parts parser (facexlib + torch)
occluder_sess    = None   # occluder.onnx — detects hands/objects occluding the face
codeformer_sess  = None   # codeformer_fp16.onnx — face restoration (no torch needed)
clipseg_model    = None   # CLIPSeg text-guided segmentation (optional)
clipseg_proc     = None
xseg_sess        = None   # XSeg DFL face mask (optional, onnxruntime)

FACE_PARTS = {
    # coarse
    "face":        [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13],
    "skin":        [1],
    "brow":        [2, 3],
    "left_brow":   [2],
    "right_brow":  [3],
    "eye":         [4, 5],
    "left_eye":    [4],
    "right_eye":   [5],
    "glasses":     [6],
    "ear":         [7, 8, 9],
    "left_ear":    [7],
    "right_ear":   [8],
    "ear_ring":    [9],
    "nose":        [10],
    "mouth":       [11, 12, 13],
    "inner_mouth": [11],
    "upper_lip":   [12],
    "lower_lip":   [13],
    "lips":        [12, 13],
    "neck":        [14, 15],
    "hair":        [17],
    "hat":         [18],
}


XSEG_PATH = os.path.join(os.path.dirname(__file__), "models", "XSeg_model.onnx")

# FFHQ 5-point template at 512x512 — matches CodeFormer/GFPGAN training alignment
# order: left_eye, right_eye, nose, left_mouth, right_mouth
_CF_DST = np.array([
    [192.98138, 239.94708],
    [318.90277, 240.19360],
    [256.63416, 314.01935],
    [201.26117, 371.41043],
    [313.08905, 371.15118]], dtype=np.float32)


def _load_models():
    global face_app, swapper, gfpganer, faceparser_sess, face_parser, occluder_sess, codeformer_sess, clipseg_model, clipseg_proc, xseg_sess

    try:
        import insightface
        from insightface.app import FaceAnalysis
    except ImportError:
        raise RuntimeError(
            "insightface not installed. Run: pip install insightface onnxruntime"
        )

    # Try CUDA first, fall back to CPU
    providers = _ort_providers()

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

    # faceparser_resnet34.onnx — primary parts parser (no torch, onnxruntime only)
    # If missing but torch+facexlib available, export BiSeNet to ONNX once automatically.
    if not os.path.exists(FACEPARSER_PATH):
        print(f"[startup] faceparser_resnet34.onnx not found — trying to export from BiSeNet...")
        try:
            import torch
            from facexlib.parsing import init_parsing_model

            class _FaceParserWrapper(torch.nn.Module):
                def __init__(self, model):
                    super().__init__()
                    self.model = model
                def forward(self, x):
                    return self.model(x)[0]   # only main logits, drop aux outputs

            _bisenet  = init_parsing_model(model_name="bisenet", half=False, device="cpu")
            _wrapped  = _FaceParserWrapper(_bisenet).eval()
            _dummy    = torch.zeros(1, 3, 512, 512)
            torch.onnx.export(
                _wrapped, _dummy, FACEPARSER_PATH,
                input_names=["input"], output_names=["output"],
                dynamic_axes={"input": {0: "batch"}},
                opset_version=12,
            )
            print(f"[startup] BiSeNet exported to ONNX → {FACEPARSER_PATH}")
            del _bisenet, _wrapped, _dummy
        except Exception as e:
            print(f"[startup] ONNX export failed ({e}) — install torch+facexlib once to generate it.")

    if os.path.exists(FACEPARSER_PATH):
        try:
            import onnxruntime as ort
            fp_providers = _ort_providers()
            faceparser_sess = ort.InferenceSession(FACEPARSER_PATH, providers=fp_providers)
            print(f"[startup] FaceParser ready. weights: {FACEPARSER_PATH}")
        except Exception as e:
            print(f"[startup] FaceParser load error: {e}")

    # Occluder — detects hands/objects blocking the face (optional, onnxruntime only)
    if os.path.exists(OCCLUDER_PATH):
        try:
            import onnxruntime as ort
            occ_providers = _ort_providers()
            occluder_sess = ort.InferenceSession(OCCLUDER_PATH, providers=occ_providers)
            print(f"[startup] Occluder ready. weights: {OCCLUDER_PATH}")
        except Exception as e:
            print(f"[startup] Occluder load error: {e}")
    else:
        print(f"[startup] occluder.onnx not found at {OCCLUDER_PATH}")
        print( "          curl -L -o faceswap_locale/models/occluder.onnx \\")
        print( "            https://github.com/visomaster/visomaster-assets/releases/download/v0.1.0/occluder.onnx")

    # CodeFormer — face restoration ONNX (no torch, onnxruntime only)
    if os.path.exists(CODEFORMER_PATH):
        try:
            import onnxruntime as ort
            cf_opts = ort.SessionOptions()
            # fp16 model triggers SimplifiedLayerNormFusion bug on some ORT versions
            cf_opts.graph_optimization_level = ort.GraphOptimizationLevel.ORT_DISABLE_ALL
            cf_providers = _ort_providers()
            codeformer_sess = ort.InferenceSession(CODEFORMER_PATH, sess_options=cf_opts,
                                                   providers=cf_providers)
            print(f"[startup] CodeFormer ready. weights: {CODEFORMER_PATH}")
        except Exception as e:
            print(f"[startup] CodeFormer load error: {e}")
    else:
        print(f"[startup] codeformer_fp16.onnx not found at {CODEFORMER_PATH}")
        print( "          curl -L -o faceswap_locale/models/codeformer_fp16.onnx \\")
        print( "            https://github.com/visomaster/visomaster-assets/releases/download/v0.1.0/codeformer_fp16.onnx")

    # GFPGAN — optional
    try:
        from gfpgan import GFPGANer
        if os.path.exists(GFPGAN_PATH):
            gfpganer = GFPGANer(model_path=GFPGAN_PATH, upscale=1, arch="clean", channel_multiplier=2)
            print(f"[startup] GFPGAN ready. weights: {GFPGAN_PATH}")
        else:
            print(f"[startup] GFPGAN weights not found at {GFPGAN_PATH} — enhancement disabled.")
    except ImportError:
        print("[startup] gfpgan not installed — enhancement disabled.")

    # BiSeNet — fallback parts parser (facexlib + torch; used only if faceparser_resnet34 missing)
    if faceparser_sess is None:
        try:
            from facexlib.parsing import init_parsing_model
            face_parser = init_parsing_model(model_name="bisenet", half=False, device="cpu")
            print("[startup] BiSeNet face parser ready (fallback).")
        except Exception as e:
            print(f"[startup] BiSeNet not available either — 'parts' in /segment disabled. ({e})")

    # XSeg (DFL) face mask — optional, onnxruntime only (no torch needed)
    if os.path.exists(XSEG_PATH):
        try:
            import onnxruntime as ort
            providers = _ort_providers()
            xseg_sess = ort.InferenceSession(XSEG_PATH, providers=providers)
            print(f"[startup] XSeg ready. weights: {XSEG_PATH}")
        except Exception as e:
            print(f"[startup] XSeg load error: {e}")
    else:
        print(f"[startup] XSeg not found at {XSEG_PATH} — download:")
        print( "          https://github.com/visomaster/visomaster-assets/releases/download/v0.1.0/XSeg_model.onnx")

    # CLIPSeg text-guided segmentation — optional
    try:
        from transformers import CLIPSegProcessor, CLIPSegForImageSegmentation
        clipseg_proc  = CLIPSegProcessor.from_pretrained("CIDAS/clipseg-rd64-refined")
        clipseg_model = CLIPSegForImageSegmentation.from_pretrained("CIDAS/clipseg-rd64-refined")
        clipseg_model.eval()
        print("[startup] CLIPSeg ready (CIDAS/clipseg-rd64-refined).")
    except Exception as e:
        print(f"[startup] CLIPSeg not available — text-guided /segment disabled. ({e})")

    # Real-ESRGAN — optional
    try:
        from realesrgan import RealESRGANer
        from basicsr.archs.rrdbnet_arch import RRDBNet
        if os.path.exists(REALESRGAN_PATH):
            model_arch = RRDBNet(num_in_ch=3, num_out_ch=3, num_feat=64, num_block=23, num_grow_ch=32, scale=4)
            upscaler = RealESRGANer(scale=4, model_path=REALESRGAN_PATH, model=model_arch,
                                    tile=0, tile_pad=10, pre_pad=0, half=False)
            print(f"[startup] Real-ESRGAN ready. weights: {REALESRGAN_PATH}")
        else:
            print(f"[startup] RealESRGAN_x4plus.pth not found at {REALESRGAN_PATH} — upscaling uses PIL fallback.")
    except ImportError:
        print("[startup] realesrgan/basicsr not installed — upscaling uses PIL fallback.")


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


def _run_occluder(img_bgr: np.ndarray, thresh: float = 0.0) -> np.ndarray:
    """Returns float32 (h,w) mask: 1.0 = visible face, 0.0 = occluded by hand/object.

    thresh = binarization threshold on the model logits (default 0). RAISING it
    makes the occluder MORE aggressive (more pixels counted as occluded →
    more area preserved from the original); lowering it, less. Useful range
    ≈ -2..+2.
    """
    h, w = img_bgr.shape[:2]
    img_rgb = cv2.cvtColor(img_bgr, cv2.COLOR_BGR2RGB)
    img256 = cv2.resize(img_rgb, (256, 256)).astype(np.float32) / 255.0
    inp = np.expand_dims(np.transpose(img256, (2, 0, 1)), 0)  # (1,3,256,256)
    in_name  = occluder_sess.get_inputs()[0].name
    out_name = occluder_sess.get_outputs()[0].name
    out = occluder_sess.run([out_name], {in_name: inp})[0]    # (1,1,256,256)
    mask256 = (out.squeeze() > thresh).astype(np.float32)     # 1=face, 0=occluded
    return cv2.resize(mask256, (w, h), interpolation=cv2.INTER_NEAREST)


def _encode_png(img: np.ndarray) -> bytes:
    ok, buf = cv2.imencode(".png", img)
    if not ok:
        raise RuntimeError("Failed to encode result as PNG.")
    return buf.tobytes()


def _get_faces_sorted(img: np.ndarray, det_thresh: float = DEFAULT_DET_THRESH):
    """Detect faces and return them sorted left-to-right by bounding-box x.

    det_thresh is applied per call (and reverts to DEFAULT_DET_THRESH on the
    next call) so a low-threshold video analyze can no longer leak into
    subsequent /swap, /detect or /register requests.
    """
    with model_lock:
        face_app.det_model.det_thresh = det_thresh
        faces = face_app.get(img)
    faces.sort(key=lambda f: f.bbox[0])
    return faces


# ── NPC identity storage ─────────────────────────────────────────────────────
# faces/<id>.npy holds one or more normed embeddings, shape (N, 512).
# Legacy single-embedding files (shape (512,)) are read transparently.
_NPC_ID_RE = re.compile(r"^[A-Za-z0-9_\-]+$")


def _check_npc_id(npc_id: str) -> str:
    """Reject ids that could escape faces/ (path traversal) — raises 400."""
    if not npc_id or not _NPC_ID_RE.match(npc_id):
        raise HTTPException(status_code=400,
            detail=f"Invalid npc_id {npc_id!r} — allowed: letters, digits, '_', '-'.")
    return npc_id


def _load_npc_embs(npc_id: str) -> Optional[np.ndarray]:
    """All stored embeddings for an NPC as a (N,512) array, or None."""
    if not npc_id or not _NPC_ID_RE.match(npc_id):
        return None
    npy = FACES_DIR / f"{npc_id}.npy"
    if not npy.exists():
        return None
    embs = np.load(str(npy))
    if embs.ndim == 1:
        embs = embs.reshape(1, -1)
    return embs


def _npc_best_sim(emb: np.ndarray, ref_embs: np.ndarray) -> float:
    """Max cosine similarity of emb against a (N,512) reference set."""
    return float(np.max(ref_embs @ emb))


# ── Alternative swappers: GhostFace v2 + SimSwap 512 (VisoMaster assets) ─────
# Each swapper has its OWN ArcFace recognizer — the registered insightface
# embeddings (.npy) are NOT compatible: the source identity is computed from
# the source image (NPC crop or upload) with the swapper's recognizer.
# Preprocessing/latents replicated from VisoMaster-Fusion face_swappers.py.
GHOST_UNET_PATH   = os.path.join(os.path.dirname(__file__), "models", "ghost_unet_2_block.onnx")
GHOST_ARC_PATH    = os.path.join(os.path.dirname(__file__), "models", "ghost_arcface_backbone.onnx")
SIMSWAP_PATH      = os.path.join(os.path.dirname(__file__), "models", "simswap_512_unoff.onnx")
SIMSWAP_ARC_PATH  = os.path.join(os.path.dirname(__file__), "models", "simswap_arcface_model.onnx")

_alt_sessions: dict = {}
_alt_sess_lock = threading.Lock()

# ArcFace 5-point template, 112x112 space (insightface standard)
_ARC112 = np.array([
    [38.2946, 51.6963], [73.5318, 51.5014], [56.0252, 71.7366],
    [41.5493, 92.3655], [70.7299, 92.2041]], dtype=np.float32)

# Pose-aware templates (VisoMaster faceutil src1..src7, 112 space) — GhostFace
_GHOST_SRC112 = np.array([
    [[51.642, 50.115], [57.617, 49.990], [35.740, 69.007], [51.157, 89.050], [57.025, 89.702]],
    [[45.031, 50.118], [65.568, 50.872], [39.677, 68.111], [45.177, 86.190], [64.246, 86.758]],
    [[39.730, 51.138], [72.270, 51.138], [56.000, 68.493], [42.463, 87.010], [69.537, 87.010]],
    [[46.845, 50.872], [67.382, 50.118], [72.737, 68.111], [48.167, 86.758], [67.236, 86.190]],
    [[54.796, 49.990], [60.771, 50.115], [76.673, 69.007], [55.388, 89.702], [61.257, 89.050]],
    [[39.730, 55.000], [72.270, 55.000], [56.000, 64.000], [42.463, 78.000], [69.537, 78.000]],
    [[39.730, 45.000], [72.270, 45.000], [56.000, 75.000], [42.463, 95.000], [69.537, 95.000]],
], dtype=np.float32)

VALID_SWAPPERS = ("inswapper", "ghost", "simswap")


def _check_swapper(name: str) -> str:
    name = (name or "inswapper").strip().lower()
    if name not in VALID_SWAPPERS:
        raise HTTPException(400, f"swapper must be one of {VALID_SWAPPERS}")
    if name == "ghost" and not (os.path.exists(GHOST_UNET_PATH) and os.path.exists(GHOST_ARC_PATH)):
        raise HTTPException(503, "GhostFace models missing — download ghost_unet_2_block.onnx "
                                 "and ghost_arcface_backbone.onnx to models/")
    if name == "simswap" and not (os.path.exists(SIMSWAP_PATH) and os.path.exists(SIMSWAP_ARC_PATH)):
        raise HTTPException(503, "SimSwap models missing — download simswap_512_unoff.onnx "
                                 "and simswap_arcface_model.onnx to models/")
    return name


def _alt_session(key: str, path: str):
    with _alt_sess_lock:
        if key not in _alt_sessions:
            import onnxruntime as ort
            _alt_sessions[key] = ort.InferenceSession(path, providers=_ort_providers())
            print(f"[swapper] loaded {key}: {path}", flush=True)
        return _alt_sessions[key]


def _similarity_transform(kps: np.ndarray, dst: np.ndarray):
    M, _ = cv2.estimateAffinePartial2D(kps.astype(np.float32), dst, method=cv2.LMEDS)
    return M


def _align_best_template(kps: np.ndarray, templates: np.ndarray):
    """Min-reprojection-error similarity transform over a template set."""
    lmk_h = np.insert(kps.astype(np.float32), 2, 1.0, axis=1)   # (5,3)
    best_err, best_M = float("inf"), None
    for t in templates:
        M = _similarity_transform(kps, t)
        if M is None:
            continue
        proj = (M @ lmk_h.T).T
        err = float(np.sum(np.sqrt(np.sum((proj - t) ** 2, axis=1))))
        if err < best_err:
            best_err, best_M = err, M
    return best_M


def _alt_swapper_available(swapper: str) -> bool:
    if swapper == "ghost":
        return os.path.exists(GHOST_UNET_PATH) and os.path.exists(GHOST_ARC_PATH)
    if swapper == "simswap":
        return os.path.exists(SIMSWAP_PATH) and os.path.exists(SIMSWAP_ARC_PATH)
    return False


def _alt_arcface_embed(swapper: str, src_img_bgr: np.ndarray, kps: np.ndarray) -> Optional[np.ndarray]:
    """RAW identity embedding (512,) with the swapper's own ArcFace."""
    from insightface.utils import face_align
    crop = face_align.norm_crop(src_img_bgr, kps, 112)          # BGR 112, arcface112
    rgb = cv2.cvtColor(crop, cv2.COLOR_BGR2RGB).astype(np.float32)
    if swapper == "ghost":
        x = rgb / 127.5 - 1.0
        sess = _alt_session("ghost_arc", GHOST_ARC_PATH)
    else:  # simswap
        x = rgb / 255.0
        x = (x - np.array([0.485, 0.456, 0.406], np.float32)) / np.array([0.229, 0.224, 0.225], np.float32)
        sess = _alt_session("simswap_arc", SIMSWAP_ARC_PATH)
    inp = np.expand_dims(np.transpose(x, (2, 0, 1)), 0).astype(np.float32)
    return sess.run(None, {sess.get_inputs()[0].name: inp})[0].flatten().astype(np.float32)


def _alt_latent_from_embs(swapper: str, embs: np.ndarray) -> Optional[np.ndarray]:
    """Swap latent from a (N,512) embedding set: mean identity, like inswapper."""
    if embs is None:
        return None
    if embs.ndim == 1:
        embs = embs.reshape(1, -1)
    if swapper == "ghost":
        return embs.mean(axis=0).reshape(1, -1)                 # raw space
    # simswap: L2-norm each row, mean, renormalize
    norms = np.linalg.norm(embs, axis=1, keepdims=True)
    norms[norms == 0] = 1.0
    mean = (embs / norms).mean(axis=0)
    n = np.linalg.norm(mean)
    return (mean / n).reshape(1, -1) if n > 0 else None


def _alt_source_latent(swapper: str, src_img_bgr: np.ndarray, kps: np.ndarray) -> Optional[np.ndarray]:
    """Identity latent for ghost/simswap from a source image + 5-point kps."""
    emb = _alt_arcface_embed(swapper, src_img_bgr, kps)
    return _alt_latent_from_embs(swapper, emb)


def _alt_emb_path(npc_id: str, swapper: str) -> Path:
    return FACES_DIR / f"{npc_id}.{swapper}.npy"


def _run_faceparser_labels(crop_bgr: np.ndarray) -> Optional[np.ndarray]:
    """FaceParser on a crop → (512,512) label map, or None if unavailable."""
    if faceparser_sess is None:
        return None
    _mean = np.array([0.485, 0.456, 0.406], dtype=np.float32)
    _std  = np.array([0.229, 0.224, 0.225], dtype=np.float32)
    rgb = cv2.cvtColor(crop_bgr, cv2.COLOR_BGR2RGB)
    x = cv2.resize(rgb, (512, 512), interpolation=cv2.INTER_LINEAR).astype(np.float32) / 255.0
    x = (x - _mean) / _std
    inp = np.expand_dims(np.transpose(x, (2, 0, 1)), 0).astype(np.float32)
    logits = faceparser_sess.run([faceparser_sess.get_outputs()[0].name],
                                 {faceparser_sess.get_inputs()[0].name: inp})[0]
    return np.argmax(logits[0], axis=0)


def _lab_color_match(src_bgr: np.ndarray, ref_bgr: np.ndarray,
                     mask01: np.ndarray) -> np.ndarray:
    """Match src color statistics (LAB mean/std) to ref inside the mask region."""
    m = mask01 > 0.5
    if m.sum() < 100:
        return src_bgr
    src_lab = cv2.cvtColor(src_bgr, cv2.COLOR_BGR2LAB).astype(np.float32)
    ref_lab = cv2.cvtColor(ref_bgr, cv2.COLOR_BGR2LAB).astype(np.float32)
    out = src_lab.copy()
    for c in range(3):
        s_mean = float(src_lab[..., c][m].mean()); s_std = float(src_lab[..., c][m].std())
        r_mean = float(ref_lab[..., c][m].mean()); r_std = float(ref_lab[..., c][m].std())
        if s_std < 1e-3:
            continue
        out[..., c] = (src_lab[..., c] - s_mean) * (r_std / s_std) + r_mean
    return cv2.cvtColor(np.clip(out, 0, 255).astype(np.uint8), cv2.COLOR_LAB2BGR)


def _paste_back(frame_bgr: np.ndarray, face_bgr: np.ndarray, M: np.ndarray,
                crop_size: int, orig_crop_bgr: Optional[np.ndarray] = None,
                scale: float = 1.0) -> np.ndarray:
    """Warp the swapped crop back into the frame.

    Mask = face region from the parser (labels 1-13: skin/brows/eyes/nose/
    mouth) so only the FACE is pasted — pasting the whole crop rectangle gave
    a visible "collage" patch (background/hair around the face replaced).
    Fallback without parser: centered ellipse. Both intersected with a
    feathered border. If orig_crop_bgr is given, the swapped crop is first
    color-matched (LAB mean/std) to it inside the face region.
    scale != 1.0 enlarges/shrinks the pasted face around the crop center
    (extreme-pose swaps land slightly smaller than the head — manual fix).
    """
    h, w = frame_bgr.shape[:2]

    # face-region mask in crop space
    parsing = _run_faceparser_labels(face_bgr)
    if parsing is not None:
        face_m = np.isin(parsing, list(range(1, 14))).astype(np.uint8) * 255
        face_m = cv2.resize(face_m, (crop_size, crop_size), interpolation=cv2.INTER_NEAREST)
        k = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (9, 9))
        face_m = cv2.dilate(face_m, k)                          # small margin past chin/brow
    else:
        face_m = np.zeros((crop_size, crop_size), dtype=np.uint8)
        cv2.ellipse(face_m, (crop_size // 2, crop_size // 2),
                    (int(crop_size * 0.34), int(crop_size * 0.45)), 0, 0, 360, 255, -1)

    # feathered border guard (warp edges must never show)
    b = max(6, int(crop_size * 0.04))
    border = np.full((crop_size, crop_size), 255, dtype=np.uint8)
    border[:b, :] = 0; border[-b:, :] = 0; border[:, :b] = 0; border[:, -b:] = 0
    mask = np.minimum(face_m, border)
    fk = (max(10, crop_size // 26) * 2) | 1
    mask = cv2.GaussianBlur(mask, (fk, fk), 0)
    mask01 = mask.astype(np.float32) / 255.0

    # tone/lighting match to the original crop — mismatch amplifies the seam
    if orig_crop_bgr is not None:
        face_bgr = _lab_color_match(face_bgr, orig_crop_bgr, mask01)

    inv = cv2.invertAffineTransform(M)
    if abs(scale - 1.0) > 1e-3:
        # compose with a scaling around the crop center: crop pixel at offset v
        # from center lands where the pixel at offset scale*v used to → content
        # magnified by `scale` in the frame. Mask warps with the same matrix.
        c = crop_size / 2.0
        K = np.array([[scale, 0.0,  c * (1.0 - scale)],
                      [0.0,  scale, c * (1.0 - scale)],
                      [0.0,  0.0,   1.0]], dtype=np.float64)
        inv = (np.vstack([inv, [0.0, 0.0, 1.0]]) @ K)[:2]
    back  = cv2.warpAffine(face_bgr, inv, (w, h), flags=cv2.INTER_LINEAR)
    mback = cv2.warpAffine(mask, inv, (w, h), flags=cv2.INTER_LINEAR)
    alpha = np.stack([mback.astype(np.float32) / 255.0] * 3, axis=-1)
    return (back.astype(np.float32) * alpha +
            frame_bgr.astype(np.float32) * (1.0 - alpha)).astype(np.uint8)


def _alt_swap_face(frame_bgr: np.ndarray, target_kps: np.ndarray,
                   latent: np.ndarray, swapper: str,
                   scale: float = 1.0) -> np.ndarray:
    """Swap one face with GhostFace v2 (256) or SimSwap (512). Returns new frame."""
    if swapper == "ghost":
        # pose-aware alignment at 512, model works at 256 (VisoMaster path)
        M = _align_best_template(target_kps, _GHOST_SRC112 * (512.0 / 112.0))
        if M is None:
            return frame_bgr
        crop512 = cv2.warpAffine(frame_bgr, M, (512, 512), flags=cv2.INTER_LINEAR,
                                 borderMode=cv2.BORDER_REPLICATE)
        crop = cv2.resize(crop512, (256, 256), interpolation=cv2.INTER_LINEAR)
        rgb = cv2.cvtColor(crop, cv2.COLOR_BGR2RGB).astype(np.float32)
        x = rgb / 127.5 - 1.0
        sess = _alt_session("ghost_unet", GHOST_UNET_PATH)
        inp = np.expand_dims(np.transpose(x, (2, 0, 1)), 0).astype(np.float32)
        out = sess.run([sess.get_outputs()[0].name],
                       {"target": inp, "source": latent.astype(np.float32)})[0][0]
        out_rgb = np.clip(out.transpose(1, 2, 0) * 127.5 + 127.5, 0, 255).astype(np.uint8)
        out_bgr = cv2.cvtColor(out_rgb, cv2.COLOR_RGB2BGR)
        out512 = cv2.resize(out_bgr, (512, 512), interpolation=cv2.INTER_LINEAR)
        return _paste_back(frame_bgr, out512, M, 512, orig_crop_bgr=crop512, scale=scale)

    # simswap: arcface128 template scaled to 512 (== insightface norm_crop 512)
    dst = _ARC112 * (512.0 / 128.0)
    dst = dst.copy(); dst[:, 0] += (512.0 / 128.0) * 8.0
    M = _similarity_transform(target_kps, dst)
    if M is None:
        return frame_bgr
    crop = cv2.warpAffine(frame_bgr, M, (512, 512), flags=cv2.INTER_LINEAR)
    rgb = cv2.cvtColor(crop, cv2.COLOR_BGR2RGB).astype(np.float32)
    x = rgb / 255.0
    sess = _alt_session("simswap", SIMSWAP_PATH)
    inp = np.expand_dims(np.transpose(x, (2, 0, 1)), 0).astype(np.float32)
    out = sess.run(["output"], {"input": inp, "onnx::Gemm_1": latent.astype(np.float32)})[0][0]
    out_rgb = np.clip(out.transpose(1, 2, 0) * 255.0, 0, 255).astype(np.uint8)
    out_bgr = cv2.cvtColor(out_rgb, cv2.COLOR_RGB2BGR)
    return _paste_back(frame_bgr, out_bgr, M, 512, orig_crop_bgr=crop, scale=scale)


def _swap_one_face(frame_bgr: np.ndarray, target_face, src_face,
                   swp: str, scale: float = 1.0) -> np.ndarray:
    """Dispatch one face swap. scale != 1.0 enlarges the pasted face around
    its center (extreme-pose fix). inswapper normally uses its own internal
    paste-back; with scale it routes through _paste_back (paste_back=False
    returns the 128 swapped crop + align matrix M)."""
    if swp == "inswapper":
        if abs(scale - 1.0) <= 1e-3:
            return swapper.get(frame_bgr, target_face, src_face, paste_back=True)
        fake, M = swapper.get(frame_bgr, target_face, src_face, paste_back=False)
        size = fake.shape[0]
        crop = cv2.warpAffine(frame_bgr, M, (size, size), flags=cv2.INTER_LINEAR)
        return _paste_back(frame_bgr, fake, M, size, orig_crop_bgr=crop, scale=scale)
    return _alt_swap_face(frame_bgr, target_face.kps, src_face, swp, scale=scale)


def _npc_alt_latent(npc_id: str, swapper: str) -> Optional[np.ndarray]:
    """Ghost/simswap identity latent for a registered NPC.

    Preferred source: per-model embedding set faces/<id>.<swapper>.npy
    (written by /register, mean over the set like inswapper). Legacy NPCs
    without it fall back to the crop jpg — and the computed embedding is
    backfilled to disk so the detection cost is paid once.
    """
    if not npc_id or not _NPC_ID_RE.match(npc_id):
        return None
    ep = _alt_emb_path(npc_id, swapper)
    if ep.exists():
        try:
            return _alt_latent_from_embs(swapper, np.load(str(ep)))
        except Exception as e:
            print(f"[swapper] corrupt {ep.name}: {e} — recomputing from crop", flush=True)
    p = FACES_DIR / f"{npc_id}.jpg"
    img = cv2.imread(str(p)) if p.exists() else None
    if img is None:
        return None
    faces = _get_faces_sorted(img)
    if not faces:
        return None
    emb = _alt_arcface_embed(swapper, img, faces[0].kps)
    if emb is None:
        return None
    np.save(str(ep), emb.reshape(1, -1))   # backfill for next time
    return _alt_latent_from_embs(swapper, emb)


def _npc_source_face(npc_id: str):
    """Synthetic source 'face' for inswapper, built from stored embeddings.

    inswapper.get() only reads source_face.normed_embedding, so no crop image
    or detection pass is needed. Mean of the registered embeddings gives a
    more stable identity than any single reference.
    """
    embs = _load_npc_embs(npc_id)
    if embs is None:
        return None
    mean = embs.mean(axis=0)
    norm = np.linalg.norm(mean)
    if norm == 0:
        return None
    return SimpleNamespace(normed_embedding=(mean / norm).astype(np.float32))


# ---------------------------------------------------------------------------
# /swap endpoint
# ---------------------------------------------------------------------------
@app.post("/swap")
async def swap_faces(
    target:    UploadFile                  = File(...),
    sources:   Optional[List[UploadFile]]  = File(None),   # source face images
    npc_ids:   str                         = Form(""),      # JSON array e.g. '["jenny","marco"]'
    positions: str                         = Form(""),      # JSON array e.g. "[0,1]" — auto if empty
    enhance:   str                         = Form(""),      # ""|"false" off; "true"/"gfpgan"; "codeformer"
    fidelity:  float                       = Form(0.7),     # codeformer only: 0=creative, 1=faithful
    mask:      Optional[UploadFile]        = File(None),    # grayscale PNG: white=preserve original
    swapper_name: str                      = Form("inswapper", alias="swapper"),  # inswapper|ghost|simswap
    scale:     float                       = Form(1.0),      # enlarge pasted face (1.05 = +5%)
):
    swp = _check_swapper(swapper_name)
    scale = min(1.5, max(0.8, float(scale or 1.0)))
    # --- Resolve source faces: npc_ids (stored embeddings) OR uploaded files ---
    src_faces_resolved: list = []   # face objects (or None when no face found)
    src_labels:         list = []   # for log messages

    if npc_ids:
        try:
            npc_id_list: List[str] = json.loads(npc_ids)
        except Exception:
            raise HTTPException(status_code=400, detail=f"Invalid npc_ids JSON: {npc_ids!r}")
        for nid in npc_id_list:
            if swp == "inswapper":
                src_face = _npc_source_face(nid)
                if src_face is None:
                    raise HTTPException(
                        status_code=422,
                        detail=f"NPC '{nid}' not registered (faces/{nid}.npy missing). Run /register first."
                    )
            else:
                # ghost/simswap: identity from the crop image via their own arcface
                src_face = _npc_alt_latent(nid, swp)
                if src_face is None:
                    raise HTTPException(
                        status_code=422,
                        detail=f"NPC '{nid}': crop faces/{nid}.jpg missing or no face in it "
                               f"— required by swapper '{swp}'. Re-run /register with a photo."
                    )
            src_faces_resolved.append(src_face)
            src_labels.append(nid)
    elif sources:
        for src_file in sources:
            raw = await src_file.read()
            try:
                src_img = _decode_image(raw)
            except ValueError as e:
                raise HTTPException(status_code=400, detail=f"Source image error: {e}")
            faces = _get_faces_sorted(src_img)
            if not faces:
                src_faces_resolved.append(None)
            elif swp == "inswapper":
                src_faces_resolved.append(faces[0])
            else:
                src_faces_resolved.append(_alt_source_latent(swp, src_img, faces[0].kps))
            src_labels.append(src_file.filename or "upload")
    else:
        raise HTTPException(
            status_code=400,
            detail="Provide either 'sources' (uploaded files) or 'npc_ids' (JSON array of registered NPC ids)."
        )

    # --- Parse positions (auto-generate if empty) ---
    if positions:
        try:
            pos_list: List[int] = json.loads(positions)
        except Exception:
            raise HTTPException(status_code=400, detail=f"Invalid positions JSON: {positions!r}")
    else:
        pos_list = list(range(len(src_faces_resolved)))

    if len(pos_list) != len(src_faces_resolved):
        raise HTTPException(
            status_code=400,
            detail=f"positions length ({len(pos_list)}) must match sources count ({len(src_faces_resolved)})."
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
    for slot_idx, src_face, label in zip(pos_list, src_faces_resolved, src_labels):
        if slot_idx >= len(target_faces):
            print(f"[swap] Slot {slot_idx} out of range "
                  f"(only {len(target_faces)} face(s) detected). Skipping.")
            continue

        if src_face is None:
            print(f"[swap] No face detected in source '{label}' for slot {slot_idx}. Skipping.")
            continue

        target_face = target_faces[slot_idx]

        try:
            result_img = _swap_one_face(result_img, target_face, src_face, swp, scale)
        except Exception as e:
            print(f"[swap] Swap error for slot {slot_idx} ({swp}): {e}. Skipping.")

    # --- Optional face enhancement: gfpgan (legacy "true") or codeformer ---
    enh = enhance.strip().lower()
    if enh in ("true", "1"):
        enh = "gfpgan"           # backward compat with old boolean param
    elif enh in ("", "false", "0", "none"):
        enh = ""
    if enh == "gfpgan":
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
            print("[swap] enhance=gfpgan but GFPGAN not available — skipping enhancement.")
    elif enh == "codeformer":
        if codeformer_sess is not None:
            try:
                result_img = _restore_codeformer(result_img, fidelity=fidelity)
                print("[swap] CodeFormer enhancement applied.")
            except Exception as e:
                print(f"[swap] CodeFormer enhancement error: {e}. Returning unenhanced result.")
        else:
            print("[swap] enhance=codeformer but CodeFormer not loaded — skipping enhancement.")
    elif enh:
        raise HTTPException(status_code=400,
            detail=f"Unknown enhance value {enhance!r} — use 'gfpgan' or 'codeformer'.")

    # --- Optional mask blending: white=preserve original, black=use swapped ---
    if mask is not None:
        mask_bytes = await mask.read()
        try:
            mask_arr = np.frombuffer(mask_bytes, np.uint8)
            mask_img = cv2.imdecode(mask_arr, cv2.IMREAD_GRAYSCALE)
            if mask_img is None:
                raise ValueError("cannot decode mask")
            mask_img = cv2.resize(mask_img, (result_img.shape[1], result_img.shape[0]),
                                  interpolation=cv2.INTER_LINEAR)
            alpha = mask_img.astype(np.float32) / 255.0
            alpha3 = np.stack([alpha] * 3, axis=-1)
            result_img = (target_img.astype(np.float32) * alpha3 +
                          result_img.astype(np.float32) * (1.0 - alpha3)).astype(np.uint8)
            print("[swap] mask blending applied.")
        except Exception as e:
            print(f"[swap] mask error: {e} — returning unmasked result.")

    return Response(content=_encode_png(result_img), media_type="image/png")


# ---------------------------------------------------------------------------
# /detect  — face bounding boxes + landmarks
# ---------------------------------------------------------------------------
@app.post("/detect")
async def detect_faces(image: UploadFile = File(...)):
    if face_app is None:
        raise HTTPException(status_code=503, detail="model not loaded")
    raw = await image.read()
    img = _decode_image(raw)
    faces = _get_faces_sorted(img)
    result = []
    for f in faces:
        result.append({
            "bbox":       [float(x) for x in f.bbox],   # [x1,y1,x2,y2]
            "score":      float(f.det_score),
            "landmarks":  f.kps.tolist() if f.kps is not None else [],
        })
    return JSONResponse({"faces": result, "count": len(result)})


# ---------------------------------------------------------------------------
# /embed  — face embedding vector
# ---------------------------------------------------------------------------
@app.post("/embed")
async def embed_face(image: UploadFile = File(...)):
    if face_app is None:
        raise HTTPException(status_code=503, detail="model not loaded")
    raw = await image.read()
    img = _decode_image(raw)
    faces = _get_faces_sorted(img)
    if not faces:
        raise HTTPException(status_code=422, detail="no face detected")
    emb = faces[0].normed_embedding
    return JSONResponse({"embedding": emb.tolist(), "face_found": True})


# ---------------------------------------------------------------------------
# /register  — store NPC face embedding to disk
# ---------------------------------------------------------------------------
def _register_face(img: np.ndarray, face, npc_id: str, append: bool) -> dict:
    """Register ONE detected face: main embedding set + per-model (ghost/simswap)
    sets + crop jpg. Shared by /register (uploads) and /video/register_face."""
    emb = face.normed_embedding.reshape(1, -1)

    # Main embedding set — append stacks a new row (profile/lighting variants)
    npy_path = FACES_DIR / f"{npc_id}.npy"
    if append and npy_path.exists():
        existing = np.load(str(npy_path))
        if existing.ndim == 1:
            existing = existing.reshape(1, -1)
        embs = np.vstack([existing, emb])
    else:
        embs = emb
    np.save(str(npy_path), embs)

    # Per-model embedding sets for ghost/simswap (their own ArcFace space)
    alt_counts: dict = {}
    for alt in ("ghost", "simswap"):
        if not _alt_swapper_available(alt):
            continue
        try:
            a_emb = _alt_arcface_embed(alt, img, face.kps)
        except Exception as e:
            print(f"[register] {alt} embed failed for '{npc_id}': {e}", flush=True)
            continue
        if a_emb is None:
            continue
        a_path = _alt_emb_path(npc_id, alt)
        a_new = a_emb.reshape(1, -1)
        if append and a_path.exists():
            try:
                a_old = np.load(str(a_path))
                if a_old.ndim == 1:
                    a_old = a_old.reshape(1, -1)
                a_new = np.vstack([a_old, a_new])
            except Exception:
                pass
        np.save(str(a_path), a_new)
        alt_counts[alt] = int(a_new.shape[0])

    # Face crop (reference/display): padded bbox. When appending, keep the first.
    crop_path = FACES_DIR / f"{npc_id}.jpg"
    if not (append and crop_path.exists()):
        x1, y1, x2, y2 = face.bbox
        fh, fw = img.shape[:2]
        bw, bh = x2 - x1, y2 - y1
        pad = 0.4
        cx1 = max(0, int(x1 - bw * pad))
        cy1 = max(0, int(y1 - bh * pad))
        cx2 = min(fw, int(x2 + bw * pad))
        cy2 = min(fh, int(y2 + bh * pad))
        cv2.imwrite(str(crop_path), img[cy1:cy2, cx1:cx2], [cv2.IMWRITE_JPEG_QUALITY, 95])

    print(f"[register] NPC '{npc_id}': {embs.shape[0]} embedding(s), alt: {alt_counts}", flush=True)
    return {"ok": True, "npc_id": npc_id,
            "n_embeddings": int(embs.shape[0]),
            "alt_embeddings": alt_counts,
            "crop_saved": str(crop_path)}


@app.post("/register")
async def register_npc(
    image:  UploadFile = File(...),
    npc_id: str        = Form(...),
    append: bool       = Form(False),   # add embedding to existing set (multi-angle)
):
    if face_app is None:
        raise HTTPException(status_code=503, detail="model not loaded")
    _check_npc_id(npc_id)
    raw = await image.read()
    img = _decode_image(raw)
    faces = _get_faces_sorted(img)
    if not faces:
        raise HTTPException(status_code=422, detail="no face detected in reference image")
    return JSONResponse(_register_face(img, faces[0], npc_id, append))


# ---------------------------------------------------------------------------
# /identify  — match face in image against registered NPC embeddings
# ---------------------------------------------------------------------------
@app.post("/identify")
async def identify_face(
    image:     UploadFile = File(...),
    threshold: float      = Form(0.35),   # cosine similarity threshold
):
    if face_app is None:
        raise HTTPException(status_code=503, detail="model not loaded")
    raw = await image.read()
    img = _decode_image(raw)
    faces = _get_faces_sorted(img)
    if not faces:
        raise HTTPException(status_code=422, detail="no face detected")

    query_emb = faces[0].normed_embedding

    # Load all registered embeddings and compute cosine similarity
    best_id, best_score = None, -1.0
    results = []
    for npy_file in sorted(FACES_DIR.glob("*.npy")):
        npc_id = npy_file.stem
        if "." in npc_id:      # skip per-model sets (<id>.ghost.npy / <id>.simswap.npy)
            continue
        ref_embs = np.load(str(npy_file))
        if ref_embs.ndim == 1:
            ref_embs = ref_embs.reshape(1, -1)
        score = _npc_best_sim(query_emb, ref_embs)   # max over stored variants
        results.append({"npc_id": npc_id, "similarity": round(score, 4)})
        if score > best_score:
            best_score = score
            best_id = npc_id

    results.sort(key=lambda x: x["similarity"], reverse=True)
    matched = best_score >= threshold
    return JSONResponse({
        "matched":    matched,
        "npc_id":     best_id if matched else None,
        "similarity": round(best_score, 4),
        "threshold":  threshold,
        "all":        results[:5],   # top 5
    })


# ---------------------------------------------------------------------------
# /upscale  — Real-ESRGAN 4x (PIL lanczos fallback if model not loaded)
# ---------------------------------------------------------------------------
@app.post("/upscale")
async def upscale_image(
    image: UploadFile = File(...),
    scale: int        = Form(4),
):
    raw = await image.read()
    if upscaler is not None:
        img_cv = _decode_image(raw)
        try:
            out_cv, _ = upscaler.enhance(img_cv, outscale=scale)
            return Response(content=_encode_png(out_cv), media_type="image/png")
        except Exception as e:
            print(f"[upscale] Real-ESRGAN error: {e}. Falling back to PIL.")

    # PIL lanczos fallback
    pil = Image.open(io.BytesIO(raw)).convert("RGB")
    w, h = pil.size
    pil = pil.resize((w * scale, h * scale), Image.LANCZOS)
    buf = io.BytesIO()
    pil.save(buf, format="PNG")
    return Response(content=buf.getvalue(), media_type="image/png")


# ---------------------------------------------------------------------------
# /segment  — BiSeNet face parsing → per-region mask
# ---------------------------------------------------------------------------
@app.post("/segment")
async def segment_face(
    image:        UploadFile = File(...),
    parts:        str        = Form(""),     # FaceParser/BiSeNet: comma-separated parts
    text:         str        = Form(""),     # CLIPSeg: text prompt e.g. "mouth,hair"
    xseg:         bool       = Form(False),  # XSeg DFL face mask
    xseg_amount:  int        = Form(0),      # XSeg erosion(+)/dilation(-) in pixels
    threshold:    float      = Form(0.4),    # CLIPSeg sigmoid threshold
    occlude:      bool       = Form(False),  # Occluder: ADD hands/objects blocking the face as white (preserved in /swap)
    occ_thresh:   float      = Form(0.0),    # occluder aggressiveness: >0 more occluded area, <0 less (≈ -2..+2)
    expand:       int        = Form(0),      # dilate(+) or erode(-) final mask in pixels
    blur:         int        = Form(0),      # gaussian blur radius on final mask (feather edges)
    invert:       bool       = Form(False),
):
    if not parts and not text and not xseg and not occlude:
        raise HTTPException(status_code=400,
            detail="Provide at least one of: 'parts', 'text', 'xseg=true', 'occlude=true'.")
    if faceparser_sess is None and face_parser is None and clipseg_model is None and xseg_sess is None and occluder_sess is None:
        raise HTTPException(status_code=503,
            detail="No segmentation model loaded.")

    raw = await image.read()
    img_bgr = _decode_image(raw)
    h, w = img_bgr.shape[:2]
    combined = np.zeros((h, w), dtype=np.uint8)

    # ── FaceParser ResNet34 / BiSeNet (parts) ────────────────────────────────
    if parts:
        if faceparser_sess is None and face_parser is None:
            raise HTTPException(status_code=503,
                detail="No parts parser loaded. Download faceparser_resnet34.onnx to models/ "
                       "or install: pip install torch torchvision facexlib")
        parts_list = [p.strip() for p in parts.split(",") if p.strip()]
        unknown = [p for p in parts_list if p not in FACE_PARTS]
        if unknown:
            raise HTTPException(status_code=400,
                detail=f"unknown parts: {unknown}. Valid: {list(FACE_PARTS)}")
        labels = set()
        for key in parts_list:
            labels.update(FACE_PARTS.get(key, []))

        # Crop to face region so the parser uses all 512px on the face, not background
        faces = _get_faces_sorted(img_bgr)
        if faces:
            x1b, y1b, x2b, y2b = faces[0].bbox
            bw, bh = x2b - x1b, y2b - y1b
            pad = 0.4
            px1 = max(0, int(x1b - bw * pad))
            py1 = max(0, int(y1b - bh * pad))
            px2 = min(w, int(x2b + bw * pad))
            py2 = min(h, int(y2b + bh * pad))
        else:
            px1, py1, px2, py2 = 0, 0, w, h

        crop = img_bgr[py1:py2, px1:px2]

        if faceparser_sess is not None:
            _mean = np.array([0.485, 0.456, 0.406], dtype=np.float32)
            _std  = np.array([0.229, 0.224, 0.225], dtype=np.float32)
            crop_rgb = cv2.cvtColor(crop, cv2.COLOR_BGR2RGB)
            img512   = cv2.resize(crop_rgb, (512, 512), interpolation=cv2.INTER_LINEAR).astype(np.float32) / 255.0
            img512   = (img512 - _mean) / _std
            inp      = np.expand_dims(np.transpose(img512, (2, 0, 1)), 0).astype(np.float32)
            in_name  = faceparser_sess.get_inputs()[0].name
            out_name = faceparser_sess.get_outputs()[0].name
            logits   = faceparser_sess.run([out_name], {in_name: inp})[0]
            parsing  = np.argmax(logits[0], axis=0)
        else:
            import torch
            import torch.nn.functional as F
            from torchvision.transforms.functional import normalize as tv_normalize
            crop_rgb = cv2.cvtColor(crop, cv2.COLOR_BGR2RGB)
            t = torch.from_numpy(crop_rgb).float().permute(2, 0, 1).unsqueeze(0) / 255.0
            tv_normalize(t, [0.485, 0.456, 0.406], [0.229, 0.224, 0.225], inplace=True)
            t = F.interpolate(t, size=(512, 512), mode="bilinear", align_corners=False)
            with torch.no_grad():
                out = face_parser(t)[0]
            parsing = out.squeeze(0).argmax(0).cpu().numpy()

        # Scale parsing mask back to crop size, place on full canvas
        part_crop = np.isin(parsing, list(labels)).astype(np.uint8) * 255
        part_crop = cv2.resize(part_crop, (px2 - px1, py2 - py1), interpolation=cv2.INTER_NEAREST)
        part_full = np.zeros((h, w), dtype=np.uint8)
        part_full[py1:py2, px1:px2] = part_crop
        combined  = np.maximum(combined, part_full)

    # ── XSeg (face boundary mask) ────────────────────────────────────────────
    if xseg:
        if xseg_sess is None:
            raise HTTPException(status_code=503,
                detail=f"XSeg not loaded. Download XSeg_model.onnx to {XSEG_PATH}")
        img256 = cv2.resize(img_bgr, (256, 256))
        img_rgb256 = cv2.cvtColor(img256, cv2.COLOR_BGR2RGB).astype(np.float32) / 255.0
        inp = np.expand_dims(np.transpose(img_rgb256, (2, 0, 1)), 0)  # NCHW
        out = xseg_sess.run(["out_mask:0"], {"in_face:0": inp})[0]
        # out: face→0, background→1 (DFL convention) → invert to face=1
        xraw = np.clip(out.squeeze(), 0.0, 1.0)
        xmask = ((1.0 - xraw) * 255).astype(np.uint8)
        # erosion (amount>0) / dilation (amount<0)
        if xseg_amount != 0:
            r = abs(xseg_amount)
            k = 2 * r + 1
            kernel = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (k, k))
            if xseg_amount > 0:
                xmask = cv2.erode(xmask, kernel)
            else:
                xmask = cv2.dilate(xmask, kernel)
        xmask = cv2.resize(xmask, (w, h), interpolation=cv2.INTER_LINEAR)
        combined = np.maximum(combined, xmask)

    # ── CLIPSeg (text) — runs on face crop for better accuracy ──────────────
    if text:
        if clipseg_model is None:
            raise HTTPException(status_code=503,
                detail="CLIPSeg not loaded. Install: pip install transformers")
        import torch

        prompts = [t.strip() for t in text.split(",") if t.strip()]

        # Crop to face region (with padding) so CLIPSeg focuses on the face
        faces = _get_faces_sorted(img_bgr)
        if faces:
            x1b, y1b, x2b, y2b = faces[0].bbox
            bw, bh = x2b - x1b, y2b - y1b
            pad = 0.35
            cx1 = max(0, int(x1b - bw * pad))
            cy1 = max(0, int(y1b - bh * pad))
            cx2 = min(w, int(x2b + bw * pad))
            cy2 = min(h, int(y2b + bh * pad))
        else:
            cx1, cy1, cx2, cy2 = 0, 0, w, h   # fallback: full image

        crop_bgr = img_bgr[cy1:cy2, cx1:cx2]
        pil_crop = Image.fromarray(cv2.cvtColor(crop_bgr, cv2.COLOR_BGR2RGB))

        inputs = clipseg_proc(
            text=prompts,
            images=[pil_crop] * len(prompts),
            return_tensors="pt",
            padding=True,
        )
        with torch.no_grad():
            logits = clipseg_model(**inputs).logits
        if logits.dim() == 2:
            logits = logits.unsqueeze(0)
        clip_local = torch.zeros(logits.shape[-2], logits.shape[-1])
        for i in range(len(prompts)):
            clip_local = torch.maximum(clip_local, torch.sigmoid(logits[i]))
        clip_local = (clip_local > threshold).numpy().astype(np.uint8) * 255

        # Scale crop mask back to crop dimensions, then place on full canvas
        clip_local = cv2.resize(clip_local, (cx2 - cx1, cy2 - cy1),
                                interpolation=cv2.INTER_LINEAR)
        clip_full = np.zeros((h, w), dtype=np.uint8)
        clip_full[cy1:cy2, cx1:cx2] = clip_local
        combined = np.maximum(combined, clip_full)

    # ── Occluder (hands/objects in front of the face → WHITE in the mask) ────
    # White = preserved by /swap, so occluded regions are ADDED to the mask.
    # The model is trained on face crops: run it on the padded face bbox, not
    # the full image (full-image output is garbage and used to wipe the mask).
    if occlude:
        if occluder_sess is None:
            raise HTTPException(status_code=503,
                detail=f"Occluder not loaded. Download occluder.onnx to {OCCLUDER_PATH}")
        faces = _get_faces_sorted(img_bgr)
        if faces:
            x1o, y1o, x2o, y2o = faces[0].bbox
            bwo, bho = x2o - x1o, y2o - y1o
            pado = 0.4
            ox1 = max(0, int(x1o - bwo * pado))
            oy1 = max(0, int(y1o - bho * pado))
            ox2 = min(w, int(x2o + bwo * pado))
            oy2 = min(h, int(y2o + bho * pado))
            vis_crop = _run_occluder(img_bgr[oy1:oy2, ox1:ox2],
                                     min(3.0, max(-3.0, occ_thresh)))  # 1=visible
            occ_full = np.zeros((h, w), dtype=np.float32)      # occluded map, full canvas
            occ_full[oy1:oy2, ox1:ox2] = 1.0 - vis_crop
            combined = np.maximum(combined, (occ_full * 255).astype(np.uint8))
        else:
            # no face window → skip: full-image occluder output is garbage
            print("[segment] occlude requested but no face detected — occluder skipped")

    if expand != 0:
        r = abs(expand)
        kernel = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (2 * r + 1, 2 * r + 1))
        combined = cv2.dilate(combined, kernel) if expand > 0 else cv2.erode(combined, kernel)

    if blur > 0:
        k = blur * 2 + 1  # must be odd
        combined = cv2.GaussianBlur(combined, (k, k), 0)

    if invert:
        combined = 255 - combined

    buf = io.BytesIO()
    Image.fromarray(combined, mode="L").save(buf, format="PNG")
    return Response(content=buf.getvalue(), media_type="image/png")


# ---------------------------------------------------------------------------
# /restore  — face restoration: CodeFormer (ONNX) or GFPGAN
# ---------------------------------------------------------------------------
@app.post("/restore")
async def restore_face(
    image:            UploadFile = File(...),
    restorer:         str        = Form("codeformer"),  # "codeformer" | "gfpgan"
    fidelity:         float      = Form(0.7),           # CodeFormer: 0=creative, 1=faithful
    only_center_face: bool       = Form(False),
):
    raw = await image.read()
    img_bgr = _decode_image(raw)

    if restorer == "codeformer":
        if codeformer_sess is None:
            raise HTTPException(status_code=503,
                detail="CodeFormer not loaded — download codeformer_fp16.onnx to models/")
        result = _restore_codeformer(img_bgr, fidelity=fidelity,
                                     only_center=only_center_face)
    else:
        if gfpganer is None:
            raise HTTPException(status_code=503,
                detail="GFPGAN not loaded — place GFPGANv1.4.pth in models/ and pip install gfpgan")
        try:
            _, _, result = gfpganer.enhance(
                img_bgr, has_aligned=False,
                only_center_face=only_center_face, paste_back=True)
        except Exception as e:
            raise HTTPException(status_code=500, detail=f"GFPGAN failed: {e}")

    return Response(content=_encode_png(result), media_type="image/png")


def _restore_codeformer(img_bgr: np.ndarray, fidelity: float = 0.7,
                         only_center: bool = False) -> np.ndarray:
    """CodeFormer restoration: align each face → restore → paste back."""
    h, w = img_bgr.shape[:2]
    result = img_bgr.copy()
    img_rgb = cv2.cvtColor(img_bgr, cv2.COLOR_BGR2RGB)

    faces = _get_faces_sorted(img_bgr)
    if not faces:
        return img_bgr

    if only_center:
        # pick face closest to image center
        cx, cy = w / 2, h / 2
        faces = [min(faces, key=lambda f: abs((f.bbox[0]+f.bbox[2])/2 - cx)
                                         + abs((f.bbox[1]+f.bbox[3])/2 - cy))]

    w_arr = np.array([float(fidelity)], dtype=np.float64)

    for face in faces:
        kps = face.kps.astype(np.float32)
        M, _ = cv2.estimateAffinePartial2D(kps, _CF_DST, method=cv2.LMEDS)
        if M is None:
            continue

        # Warp face to 512x512 aligned crop
        aligned = cv2.warpAffine(img_rgb, M, (512, 512), flags=cv2.INTER_LINEAR)

        # Preprocess: [0,255] -> [-1,1]
        inp = aligned.astype(np.float32) / 255.0
        inp = (inp - 0.5) / 0.5
        inp = np.expand_dims(np.transpose(inp, (2, 0, 1)), 0)  # NCHW

        # Run CodeFormer
        out = codeformer_sess.run(["y"], {"x": inp, "w": w_arr})[0]  # (1,3,512,512)

        # Postprocess: [-1,1] -> [0,255] BGR
        out_rgb = np.clip(out[0].transpose(1, 2, 0) * 0.5 + 0.5, 0, 1)
        out_rgb = (out_rgb * 255).astype(np.uint8)
        out_bgr = cv2.cvtColor(out_rgb, cv2.COLOR_RGB2BGR)

        # Inverse warp back to original image
        M_inv = cv2.invertAffineTransform(M)
        restored_full = cv2.warpAffine(out_bgr, M_inv, (w, h), flags=cv2.INTER_LINEAR)

        # Elliptical blend mask — face is always centred at (256,256) in aligned
        # space; ellipse avoids rectangular warpAffine edge artefacts entirely.
        mask = np.zeros((512, 512), dtype=np.float32)
        cv2.ellipse(mask, (256, 256), (210, 240), 0, 0, 360, 1.0, -1)
        mask = cv2.GaussianBlur(mask, (61, 61), 0)
        mask_full = cv2.warpAffine(mask, M_inv, (w, h), flags=cv2.INTER_LINEAR)
        mask3 = mask_full[:, :, np.newaxis]

        result = (restored_full.astype(np.float32) * mask3
                  + result.astype(np.float32) * (1.0 - mask3)).astype(np.uint8)

    return result


# ---------------------------------------------------------------------------
# /health
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# Video pipeline — multi-pass face swap on video clips
#
# Flow:
#   /video/extract        → frames/ + audio.aac
#   /video/analyze        → manifest.json + cluster_review/ + cluster previews
#   /video/patch_manifest → assign NPCs to clusters, update swap settings
#   /video/swap_frames    → swap all pending frames, write frames_swapped/
#   /video/quality        → score swapped frames, flag sequences
#   /video/patch_manifest → refine flagged sequences (mask, expand, blur)
#   /video/swap_frames    → retry_only=true — reprocess flagged frames
#   /video/assemble       → final video with optional restore + upscale + audio
# ---------------------------------------------------------------------------

# ── Async job registry ───────────────────────────────────────────────────────
# Long video ops accept run_async=true → immediate {job_id}, work in a thread.
# Poll GET /job/<id> for {status, step, done, total, result|error}.
JOBS: dict = {}
jobs_lock = threading.Lock()


def _progress(job_id: Optional[str], done: int, total: int, step: str):
    if not job_id:
        return
    with jobs_lock:
        j = JOBS.get(job_id)
        if j:
            j.update(done=done, total=total, step=step)


def _spawn_job(kind: str, fn, params: dict) -> JSONResponse:
    """Run endpoint fn(**params) in a background thread, tracked in JOBS."""
    job_id = uuid.uuid4().hex[:12]
    with jobs_lock:
        # prune old finished jobs
        if len(JOBS) > 80:
            for k in sorted(JOBS, key=lambda k: JOBS[k]["started"])[:len(JOBS) - 50]:
                if JOBS[k]["status"] != "running":
                    JOBS.pop(k, None)
        JOBS[job_id] = {"job_id": job_id, "kind": kind, "status": "running",
                        "step": kind, "done": 0, "total": 0,
                        "result": None, "error": None, "started": time.time()}
    params = dict(params)
    params["run_async"] = False
    params["_job_id"]   = job_id

    def _worker():
        try:
            res  = fn(**params)
            body = json.loads(bytes(res.body)) if isinstance(res, JSONResponse) else res
            with jobs_lock:
                JOBS[job_id].update(status="done", result=body,
                                    done=JOBS[job_id]["total"])
        except HTTPException as e:
            with jobs_lock:
                JOBS[job_id].update(status="error", error=str(e.detail))
        except Exception as e:
            with jobs_lock:
                JOBS[job_id].update(status="error", error=f"{type(e).__name__}: {e}")

    threading.Thread(target=_worker, daemon=True).start()
    return JSONResponse({"ok": True, "job_id": job_id, "kind": kind, "poll": f"/job/{job_id}"})


@app.get("/job/{job_id}")
def job_status(job_id: str):
    with jobs_lock:
        j = JOBS.get(job_id)
        if j is None:
            raise HTTPException(404, f"unknown job '{job_id}'")
        return JSONResponse(dict(j))


@app.get("/jobs")
def jobs_list():
    with jobs_lock:
        items = [{k: v for k, v in j.items() if k != "result"}
                 for j in sorted(JOBS.values(), key=lambda x: x["started"], reverse=True)]
    return JSONResponse({"jobs": items, "count": len(items)})


def _ffmpeg_ok() -> bool:
    try:
        return subprocess.run(["ffmpeg", "-version"], capture_output=True, timeout=5).returncode == 0
    except Exception:
        return False

def _time_to_s(t: str) -> float:
    """HH:MM:SS, MM:SS, or float seconds → float seconds."""
    if not t:
        return 0.0
    parts = t.strip().split(":")
    try:
        if len(parts) == 3:
            return int(parts[0]) * 3600 + int(parts[1]) * 60 + float(parts[2])
        elif len(parts) == 2:
            return int(parts[0]) * 60 + float(parts[1])
        return float(parts[0])
    except ValueError:
        return 0.0

def _probe_fps(video_path: str) -> str:
    try:
        r = subprocess.run(
            ["ffprobe", "-v", "0", "-select_streams", "v:0",
             "-show_entries", "stream=r_frame_rate", "-of", "csv=p=0", video_path],
            capture_output=True, text=True, timeout=10)
        if r.returncode == 0:
            n, d = r.stdout.strip().split("/")
            return str(round(int(n) / int(d), 3))
    except Exception:
        pass
    return "unknown"

def _probe_audio_codec(video_path: str) -> str:
    try:
        r = subprocess.run(
            ["ffprobe", "-v", "0", "-select_streams", "a:0",
             "-show_entries", "stream=codec_name", "-of", "csv=p=0", video_path],
            capture_output=True, text=True, timeout=10)
        return r.stdout.strip()
    except Exception:
        return ""

def _bbox_iou(a, b) -> float:
    """Intersection-over-Union of two [x1,y1,x2,y2] bboxes."""
    ix1 = max(a[0], b[0]); iy1 = max(a[1], b[1])
    ix2 = min(a[2], b[2]); iy2 = min(a[3], b[3])
    inter = max(0.0, ix2 - ix1) * max(0.0, iy2 - iy1)
    if inter == 0.0:
        return 0.0
    area_a = (a[2] - a[0]) * (a[3] - a[1])
    area_b = (b[2] - b[0]) * (b[3] - b[1])
    union = area_a + area_b - inter
    return inter / union if union > 0 else 0.0


def _load_manifest(path: str) -> dict:
    with open(path) as f:
        return json.load(f)

class _NumpyEncoder(json.JSONEncoder):
    def default(self, obj):
        if isinstance(obj, np.floating):
            return float(obj)
        if isinstance(obj, np.integer):
            return int(obj)
        if isinstance(obj, np.ndarray):
            return obj.tolist()
        return super().default(obj)

def _save_manifest(path: str, data: dict):
    with open(path + ".tmp", "w") as f:
        json.dump(data, f, indent=2, cls=_NumpyEncoder)
    os.replace(path + ".tmp", path)


# ── Video work-dir manager (clip ids) ────────────────────────────────────────
# Remote-safe: the GUI speaks video_id/clip_id, never server filesystem paths.
#   uploads/<video_id>.<ext>  — raw uploaded videos (one upload → N clips)
#   clips/<clip_id>/          — work dir (frames/, frames_swapped/, manifest…)
# Path-based usage (faceswap.sh, same-machine) keeps working everywhere.
VIDEO_WORK_DIR = Path(os.environ.get("FACESWAP_WORK_DIR",
                      os.path.join(os.path.dirname(__file__), "video_work")))
UPLOADS_DIR = VIDEO_WORK_DIR / "uploads"
CLIPS_DIR   = VIDEO_WORK_DIR / "clips"
_WORK_ID_RE = re.compile(r"^[a-f0-9]{6,32}$")
_VIDEO_EXTS = (".mp4", ".mov", ".mkv", ".avi", ".webm", ".m4v")


def _new_work_id() -> str:
    return uuid.uuid4().hex[:12]


def _check_work_id(wid: str) -> str:
    if not wid or not _WORK_ID_RE.match(wid):
        raise HTTPException(400, f"Invalid id {wid!r}")
    return wid


def _upload_file(video_id: str) -> Path:
    _check_work_id(video_id)
    hits = list(UPLOADS_DIR.glob(f"{video_id}.*"))
    if not hits:
        raise HTTPException(404, f"video_id '{video_id}' not found — POST /video/upload first")
    return hits[0]


def _clip_dir(clip_id: str) -> Path:
    _check_work_id(clip_id)
    d = CLIPS_DIR / clip_id
    if not d.is_dir():
        raise HTTPException(404, f"clip '{clip_id}' not found")
    return d


def _resolve_clip(clip_id: str) -> dict:
    """Standard paths inside a clip work dir."""
    d = _clip_dir(clip_id)
    return {
        "dir":      str(d),
        "frames":   str(d / "frames"),
        "swapped":  str(d / "frames_swapped"),
        "restored": str(d / "frames_restored"),
        "manifest": str(d / "manifest.json"),
        "audio":    str(d / "audio.aac"),
        "result":   str(d / "final.mp4"),
    }


@app.post("/video/upload")
async def video_upload(video: UploadFile = File(...)):
    """Store a raw video on the server; returns video_id for /video/extract."""
    UPLOADS_DIR.mkdir(parents=True, exist_ok=True)
    ext = os.path.splitext(video.filename or "")[1].lower()
    if ext not in _VIDEO_EXTS:
        ext = ".mp4"
    vid  = _new_work_id()
    dest = UPLOADS_DIR / f"{vid}{ext}"
    with open(dest, "wb") as f:
        shutil.copyfileobj(video.file, f, length=1024 * 1024)
    size_mb = round(dest.stat().st_size / 1024 / 1024, 2)
    if size_mb == 0:
        dest.unlink(missing_ok=True)
        raise HTTPException(400, "empty upload")
    print(f"[video/upload] {video.filename} → {dest.name} ({size_mb} MB)", flush=True)
    return JSONResponse({"ok": True, "video_id": vid,
                         "filename": video.filename, "size_mb": size_mb,
                         "next_step": "POST /video/extract with video_id (+ start/end/fps) — repeatable for multiple clips"})


@app.get("/video/clips")
def video_clips():
    """List clip work dirs with a status summary."""
    out = []
    if CLIPS_DIR.is_dir():
        for d in sorted(CLIPS_DIR.iterdir(), key=lambda p: p.stat().st_mtime, reverse=True):
            if not d.is_dir() or not _WORK_ID_RE.match(d.name):
                continue
            frames = d / "frames"
            entry = {
                "clip_id":     d.name,
                "created":     int(d.stat().st_mtime),
                "frame_count": len(list(frames.glob("*.png"))) if frames.is_dir() else 0,
                "has_manifest": (d / "manifest.json").exists(),
                "has_result":   (d / "final.mp4").exists(),
                "statuses":    {},
            }
            if entry["has_manifest"]:
                try:
                    m = _load_manifest(str(d / "manifest.json"))
                    counts: dict = {}
                    for f in m.get("frames", {}).values():
                        s = f.get("status", "?")
                        counts[s] = counts.get(s, 0) + 1
                    entry["statuses"] = counts
                    entry["references"] = m.get("video_info", {}).get("references", [])
                except Exception:
                    pass
            out.append(entry)
    return JSONResponse({"clips": out, "count": len(out)})


@app.get("/video/manifest")
def video_manifest(clip: str = "", dir: str = ""):
    """Full manifest for a clip (or a legacy path-based work dir)."""
    if clip:
        mp = _resolve_clip(clip)["manifest"]
    elif dir:
        mp = os.path.join(dir, "manifest.json")
    else:
        raise HTTPException(400, "provide 'clip' or 'dir'")
    if not os.path.exists(mp):
        raise HTTPException(404, "manifest not found — run /video/analyze first")
    return JSONResponse(_load_manifest(mp))


@app.get("/video/frame")
def video_frame(fid: str, clip: str = "", dir: str = "", which: str = "orig"):
    """One frame as image bytes: which = orig | swapped | restored."""
    if not re.fullmatch(r"\d{1,8}", fid):
        raise HTTPException(400, f"invalid fid {fid!r}")
    sub = {"orig": "frames", "swapped": "frames_swapped", "restored": "frames_restored"}.get(which)
    if sub is None:
        raise HTTPException(400, "which must be orig|swapped|restored")
    base = _clip_dir(clip) if clip else Path(dir)
    if not clip and not dir:
        raise HTTPException(400, "provide 'clip' or 'dir'")
    p = base / sub / f"{int(fid):06d}.png"
    if not p.exists():
        raise HTTPException(404, f"frame {which}/{int(fid):06d} not found")
    return Response(content=p.read_bytes(), media_type="image/png")


def _clip_frame_path(clip: str, dir: str, fid: str) -> Path:
    if not re.fullmatch(r"\d{1,8}", fid):
        raise HTTPException(400, f"invalid fid {fid!r}")
    base = _clip_dir(clip) if clip else Path(dir)
    if not clip and not dir:
        raise HTTPException(400, "provide 'clip' or 'dir'")
    p = base / "frames" / f"{int(fid):06d}.png"
    if not p.exists():
        raise HTTPException(404, f"frame {int(fid):06d} not found")
    return p


@app.get("/video/detect_frame")
def video_detect_frame(fid: str, clip: str = "", dir: str = "",
                       det_thresh: float = 0.35):
    """Detect faces on ONE stored frame — boxes for the GUI's click-to-register."""
    if face_app is None:
        raise HTTPException(503, "face model not loaded")
    img = cv2.imread(str(_clip_frame_path(clip, dir, fid)))
    if img is None:
        raise HTTPException(500, "cannot read frame")
    faces = _get_faces_sorted(img, det_thresh)
    return JSONResponse({"fid": f"{int(fid):06d}", "faces": [
        {"face_idx": i,
         "bbox": [round(float(v), 1) for v in f.bbox],
         "det_score": round(float(f.det_score), 3)}
        for i, f in enumerate(faces)]})


@app.post("/video/register_face")
def video_register_face(
    fid:        str   = Form(...),
    npc_id:     str   = Form(...),
    clip:       str   = Form(""),
    dir:        str   = Form(""),
    face_idx:   int   = Form(-1),      # index in left-to-right order; -1 = biggest face
    append:     bool  = Form(True),    # default: accumulate multi-angle set
    det_thresh: float = Form(0.35),
):
    """Register an NPC identity from a face IN a video frame ("questo volto è X").

    Scrub to a good frame, click the face, name it — repeated on a couple of
    angles this builds the multi-angle embedding set used by analyze/swap."""
    if face_app is None:
        raise HTTPException(503, "face model not loaded")
    _check_npc_id(npc_id)
    img = cv2.imread(str(_clip_frame_path(clip, dir, fid)))
    if img is None:
        raise HTTPException(500, "cannot read frame")
    faces = _get_faces_sorted(img, det_thresh)
    if not faces:
        raise HTTPException(422, f"no face detected in frame {fid}")
    if face_idx >= 0:
        if face_idx >= len(faces):
            raise HTTPException(400, f"face_idx {face_idx} out of range ({len(faces)} faces)")
        face = faces[face_idx]
    else:
        face = max(faces, key=lambda f: (f.bbox[2]-f.bbox[0]) * (f.bbox[3]-f.bbox[1]))
    res = _register_face(img, face, npc_id, append)
    res["fid"] = f"{int(fid):06d}"
    return JSONResponse(res)


@app.get("/video/result")
def video_result(clip: str):
    """Final assembled mp4 for a clip."""
    p = Path(_resolve_clip(clip)["result"])
    if not p.exists():
        raise HTTPException(404, "no final.mp4 — run /video/assemble first")
    return Response(content=p.read_bytes(), media_type="video/mp4",
                    headers={"Content-Disposition": f'attachment; filename="{clip}.mp4"'})


@app.post("/video/extract")
def video_extract(
    video_path:  str = Form(""),    # server-local path (same-machine mode) …
    video_id:    str = Form(""),    # … or an uploaded video id (remote mode)
    output_dir:  str = Form(""),    # legacy explicit dir; empty = new clip work dir
    start_time:  str = Form(""),    # HH:MM:SS or seconds float
    end_time:    str = Form(""),
    fps:         str = Form(""),    # output fps (empty = source fps)
):
    """Extract frames+audio. Repeatable on the same video_id with different
    ranges → multiple clips from one upload. Returns clip_id (id mode)."""
    if not _ffmpeg_ok():
        raise HTTPException(503, "ffmpeg not found. Install: brew install ffmpeg")
    if video_id:
        video_path = str(_upload_file(video_id))
    if not video_path:
        raise HTTPException(400, "provide 'video_path' (local) or 'video_id' (uploaded)")
    if not os.path.exists(video_path):
        raise HTTPException(400, f"Video not found: {video_path}")

    clip_id = ""
    if output_dir:
        out = Path(output_dir)              # legacy path mode
    else:
        clip_id = _new_work_id()
        out = CLIPS_DIR / clip_id
    frames_dir = out / "frames"
    frames_dir.mkdir(parents=True, exist_ok=True)

    start_s  = _time_to_s(start_time)
    end_s    = _time_to_s(end_time) if end_time else None
    duration = (end_s - start_s) if end_s is not None else None

    # Extract frames (accurate seek: -ss after -i)
    cmd = ["ffmpeg", "-y", "-i", video_path]
    if start_time:
        cmd += ["-ss", str(start_s)]
    if duration is not None:
        cmd += ["-t", str(duration)]
    if fps:
        cmd += ["-vf", f"fps={fps}"]
    cmd += [str(frames_dir / "%06d.png")]

    r = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    if r.returncode != 0:
        raise HTTPException(500, f"ffmpeg frame extraction failed:\n{r.stderr[-600:]}")

    # Extract audio (copy AAC if possible, else re-encode)
    audio_path   = str(out / "audio.aac")
    audio_codec  = _probe_audio_codec(video_path)
    a_codec_args = ["-c:a", "copy"] if audio_codec == "aac" else ["-c:a", "aac", "-b:a", "192k"]

    cmd_a = ["ffmpeg", "-y", "-i", video_path]
    if start_time:
        cmd_a += ["-ss", str(start_s)]
    if duration is not None:
        cmd_a += ["-t", str(duration)]
    cmd_a += ["-vn"] + a_codec_args + [audio_path]

    ra        = subprocess.run(cmd_a, capture_output=True, text=True, timeout=120)
    has_audio = ra.returncode == 0 and os.path.exists(audio_path) and os.path.getsize(audio_path) > 0
    if not has_audio:
        print(f"[video/extract] Audio warning (no audio track?): {ra.stderr[-200:]}")

    frame_files = sorted(frames_dir.glob("*.png"))
    source_fps  = fps if fps else _probe_fps(video_path)

    info = {
        "ok":          True,
        "clip_id":     clip_id,          # "" in legacy path mode
        "output_dir":  str(out),
        "frames_dir":  str(frames_dir),
        "audio_path":  audio_path if has_audio else None,
        "frame_count": len(frame_files),
        "source_fps":  source_fps,
        "start_time":  start_time or "0",
        "end_time":    end_time or "end",
    }
    # Persist for later stages (assemble reads source_fps when fps not given)
    with open(out / "video_info.json", "w") as f:
        json.dump(info, f, indent=2)
    return JSONResponse(info)


@app.post("/video/analyze")
def video_analyze(
    clip:                str   = Form(""),   # clip id — alternative to explicit paths
    frames_dir:          str   = Form(""),
    output_dir:          str   = Form(""),
    # Explicit NPC IDs to track (comma-separated): loads faces/<id>.npy
    # REQUIRED for reference mode. Avoids cross-NPC confusion.
    npc_ids:             str   = Form(""),
    # Optional photo overrides: JSON {"jenny":"/abs/path/photo.jpg"}
    refs:                str   = Form("{}"),
    threshold:           float = Form(0.45),
    iou_threshold:       float = Form(0.30),
    det_thresh:          float = Form(0.35),
    max_track_gap:       int   = Form(3),
    occlusion_det_score: float = Form(0.30),   # keep below det_thresh to avoid flagging valid detections
    # Partial re-analysis: comma-separated frame IDs or ranges "000043,000100-000115"
    # If set, loads existing manifest and updates ONLY these frames (others untouched).
    reanalyze_frames:    str   = Form(""),
    make_previews:       bool  = Form(True),
    preview_fps:         str   = Form("30"),
    run_async:           bool  = Form(False),
    _job_id:             Optional[str] = None,
):
    """Reference-based face tracking. With reanalyze_frames: patch-only mode on specified frames."""
    if clip:
        cp = _resolve_clip(clip)
        frames_dir, output_dir = cp["frames"], cp["dir"]
    if not frames_dir or not output_dir:
        raise HTTPException(400, "provide 'clip' or frames_dir+output_dir")
    if run_async:
        return _spawn_job("analyze", video_analyze, dict(
            clip="", frames_dir=frames_dir, output_dir=output_dir, npc_ids=npc_ids, refs=refs,
            threshold=threshold, iou_threshold=iou_threshold, det_thresh=det_thresh,
            max_track_gap=max_track_gap, occlusion_det_score=occlusion_det_score,
            reanalyze_frames=reanalyze_frames, make_previews=make_previews,
            preview_fps=preview_fps))
    if face_app is None:
        raise HTTPException(503, "face model not loaded")

    fd  = Path(frames_dir)
    out = Path(output_dir)

    # -- 1. Load reference embeddings for SPECIFIED NPCs only --
    ref_embs: dict = {}   # npc_id -> (N,512) embedding set

    requested_ids = [x.strip() for x in npc_ids.split(",") if x.strip()] if npc_ids.strip() else []

    # Load .npy for each requested NPC (may hold multiple embeddings)
    for npc_id in requested_ids:
        embs = _load_npc_embs(npc_id)
        if embs is None:
            raise HTTPException(400, f"NPC '{npc_id}' not registered — run /register first")
        ref_embs[npc_id] = embs

    # Optional photo overrides (augment registered set or add NPCs not yet registered)
    refs_map: dict = {}
    if refs.strip() and refs.strip() not in ("{}", ""):
        try:
            refs_map = json.loads(refs)
        except Exception as e:
            raise HTTPException(400, f"Invalid refs JSON: {e}")
    for npc_id, photo_path in refs_map.items():
        img_r = cv2.imread(str(photo_path))
        if img_r is None:
            print(f"[analyze] WARNING: cannot load ref photo for {npc_id}: {photo_path}", flush=True)
            continue
        fr = _get_faces_sorted(img_r)
        if not fr:
            print(f"[analyze] WARNING: no face in ref photo for {npc_id}", flush=True)
            continue
        new_emb = fr[0].normed_embedding.reshape(1, -1)
        if npc_id in ref_embs:
            ref_embs[npc_id] = np.vstack([ref_embs[npc_id], new_emb])
        else:
            ref_embs[npc_id] = new_emb
        print(f"[analyze] Loaded ref photo for {npc_id} ({ref_embs[npc_id].shape[0]} embedding(s))", flush=True)

    if not ref_embs:
        print("[analyze] No NPC specified — running unsupervised clustering (use --npc to specify)", flush=True)

    print(f"[analyze] Tracking: {list(ref_embs.keys()) or '(unsupervised)'}", flush=True)

    # -- Parse reanalyze_frames filter (patch-only mode) --
    reanalyze_set: set = set()
    if reanalyze_frames.strip():
        manifest_path_existing = str(out / "manifest.json")
        if not os.path.exists(manifest_path_existing):
            raise HTTPException(400, "reanalyze_frames requires an existing manifest.json — run full analyze first")
        for token in reanalyze_frames.split(","):
            token = token.strip()
            if not token:
                continue
            if "-" in token:
                a, b = token.split("-", 1)
                reanalyze_set.update(f"{n:06d}" for n in range(int(a), int(b) + 1))
            else:
                reanalyze_set.add(token.zfill(6))
        print(f"[analyze] Patch-only mode: {len(reanalyze_set)} frames", flush=True)

    frame_files = sorted(fd.glob("*.png"))
    if not frame_files:
        raise HTTPException(400, f"No PNG frames in {frames_dir}")

    # In patch-only mode: restrict to target frames + load existing manifest
    if reanalyze_set:
        frame_files = [f for f in frame_files if f.stem in reanalyze_set]
        existing_manifest = _load_manifest(str(out / "manifest.json"))
        manifest_frames = existing_manifest.get("frames", {})
    else:
        manifest_frames = {}

    total = len(frame_files)
    print(f"[analyze] Processing {total} frames (det_thresh={det_thresh})...", flush=True)

    # -- 2. Per-frame: detect + match --
    prev_tracks:     list  = []   # [{"bbox", "npc_id", "last_sim"}]
    gap_counter:     int   = 0    # consecutive no-face frames counter
    npc_frame_counts: dict = {}
    unknown_count   = 0
    occluded_count  = 0
    # Unsupervised fallback state (when no refs)
    clusters: dict  = {}
    next_cid        = 0

    for i, fpath in enumerate(frame_files):
        _progress(_job_id, i, total, "analyze")
        if i % 25 == 0:
            print(f"[analyze] {i}/{total}", flush=True)

        fid = fpath.stem
        img = cv2.imread(str(fpath))
        if img is None:
            manifest_frames[fid] = {"faces": [], "status": "decode_error", "skip": True}
            prev_tracks = []
            gap_counter = 0
            continue

        faces = _get_faces_sorted(img, det_thresh)

        # No face detected: try inheriting from previous track (gap fill)
        if not faces and prev_tracks and gap_counter < max_track_gap:
            gap_counter += 1
            entries = []
            for pt in prev_tracks:
                npc_frame_counts[pt["npc_id"]] = npc_frame_counts.get(pt["npc_id"], 0) + 1
                entries.append({
                    "face_idx":   0,
                    "npc_id":     pt["npc_id"],
                    "matched_by": "gap_fill",
                    "similarity": round(pt["last_sim"], 3),
                    "det_score":  0.0,
                    "bbox":       pt["bbox"],
                    "pose":       [0.0, 0.0, 0.0],
                    "occluded":   True,
                    "swap_settings": {"mask_parts": [], "expand": 0, "blur": 8, "occlude": True},
                    "swap_quality":  None,
                    "flagged":       False,
                    "flag_reasons":  [],
                })
            manifest_frames[fid] = {"faces": entries, "status": "pending", "skip": False}
            continue

        if not faces:
            gap_counter = 0
            prev_tracks = []
            manifest_frames[fid] = {"faces": [], "status": "no_face", "skip": True}
            continue

        gap_counter = 0
        entries    = []
        new_tracks = []

        h_img, w_img = img.shape[:2]
        for fidx, face in enumerate(faces):
            x1, y1, x2, y2 = face.bbox
            # Skip false detections: bbox extends significantly outside image bounds
            if x1 < -30 or y1 < -30 or x2 > w_img + 30 or y2 > h_img + 30:
                continue
            # Skip if bbox area is tiny (< 20x20 px)
            if (x2 - x1) < 20 or (y2 - y1) < 20:
                continue

            emb  = face.normed_embedding
            pose = face.pose.tolist() if hasattr(face, "pose") and face.pose is not None else [0.0, 0.0, 0.0]

            best_npc     = None
            best_sim     = 0.0
            best_sim_emb = -2.0  # init below any possible dot product to capture negatives
            matched_by   = None

            if ref_embs:
                # Embedding match against references (max over stored variants)
                for npc_id, ref_emb in ref_embs.items():
                    sim = _npc_best_sim(emb, ref_emb)
                    if sim > best_sim_emb:
                        best_sim_emb = sim
                        best_npc     = npc_id

                if best_sim_emb >= threshold:
                    best_sim   = best_sim_emb
                    matched_by = "embedding"
                else:
                    # Temporal tracking: inherit identity if bbox overlaps prev frame
                    best_iou   = 0.0
                    best_track = None
                    for pt in prev_tracks:
                        iou = _bbox_iou(face.bbox, pt["bbox"])
                        if iou > best_iou:
                            best_iou   = iou
                            best_track = pt
                    if best_iou >= iou_threshold and best_track:
                        best_npc   = best_track["npc_id"]
                        best_sim   = best_track["last_sim"]
                        matched_by = "track"
                    else:
                        # Store rejected sim so user can diagnose threshold
                        best_npc   = None
                        best_sim   = best_sim_emb   # keep real value, not 0
                        matched_by = None
            else:
                # Unsupervised clustering fallback
                for cid, cdata in clusters.items():
                    sim = float(np.dot(emb, cdata["rep_emb"]))
                    if sim > best_sim:
                        best_sim = sim
                        best_npc = cid
                if best_sim >= 0.40:
                    clusters[best_npc]["count"] += 1
                    matched_by = "cluster"
                else:
                    cid = f"face_{next_cid}"
                    next_cid += 1
                    clusters[cid] = {"rep_emb": emb.copy(), "count": 1, "first_frame": fid}
                    best_npc   = cid
                    matched_by = "cluster_new"

            # Occlusion: low detection confidence → face partly covered.
            # Preserve semantics: no mask_parts (swap the whole face); the
            # occluder ADDS the covered pixels (hands/objects) to what is kept.
            occluded    = float(face.det_score) < occlusion_det_score
            auto_parts  = []
            auto_expand = 0
            auto_blur   = 8 if occluded else 0

            if occluded:
                occluded_count += 1

            if best_npc and matched_by:
                npc_frame_counts[best_npc] = npc_frame_counts.get(best_npc, 0) + 1
                new_tracks.append({"bbox": list(face.bbox), "npc_id": best_npc, "last_sim": best_sim})
            else:
                unknown_count += 1

            entries.append({
                "face_idx":   fidx,
                "npc_id":     best_npc if matched_by else None,
                "matched_by": matched_by,
                "similarity": round(best_sim, 3),
                "det_score":  round(float(face.det_score), 4),
                "bbox":       [round(float(v), 1) for v in face.bbox],
                "pose":       [round(float(v), 2) for v in pose],
                "occluded":   occluded,
                "swap_settings": {"mask_parts": auto_parts, "expand": auto_expand, "blur": auto_blur, "occlude": occluded},
                "swap_quality":  None,
                "flagged":       False,
                "flag_reasons":  [],
            })

        prev_tracks = new_tracks
        manifest_frames[fid] = {
            "faces":  entries,
            "status": "pending" if entries else "no_face",
            "skip":   not bool(entries),
        }

    print(f"[analyze] Done. Saving manifest...", flush=True)

    manifest = {
        "video_info": {
            "frames_dir":    str(fd),
            "total_frames":  total,
            "mode":          "reference" if ref_embs else "unsupervised",
            "references":    list(ref_embs.keys()),
            "threshold":     threshold,
            "det_thresh":    det_thresh,   # reused by swap_frames/quality for consistent detection
        },
        "flagged_sequences": [],
        "frames":            manifest_frames,
    }
    manifest_path = str(out / "manifest.json")
    _save_manifest(manifest_path, manifest)

    # -- 3. Per-NPC preview videos --
    _progress(_job_id, total, total, "previews")
    pr = out / "npc_review"
    if make_previews and _ffmpeg_ok():
        pr.mkdir(exist_ok=True)
        npc_frame_map: dict = {}
        for fid, fdata in manifest_frames.items():
            for fe in fdata.get("faces", []):
                npc = fe.get("npc_id")
                if npc:
                    npc_frame_map.setdefault(npc, []).append(fid)

        for npc_id, fids in npc_frame_map.items():
            fids_sorted = sorted(fids)
            cdir = pr / npc_id
            cdir.mkdir(exist_ok=True)
            list_file = cdir / "frames.txt"
            dur = str(round(1.0 / max(float(preview_fps), 1.0), 6))
            with open(list_file, "w") as lf:
                for fid in fids_sorted:
                    lf.write(f"file '{str((fd / f'{fid}.png').resolve())}'\n")
                    lf.write(f"duration {dur}\n")
            preview_path = str(pr / f"{npc_id}_preview.mp4")
            rp = subprocess.run(
                ["ffmpeg", "-y", "-f", "concat", "-safe", "0",
                 "-i", str(list_file), "-c:v", "libx264",
                 "-pix_fmt", "yuv420p", "-r", str(preview_fps), preview_path],
                capture_output=True, timeout=120)
            if rp.returncode != 0:
                print(f"[analyze] ffmpeg preview failed for {npc_id}: {rp.stderr.decode()[:200]}", flush=True)

    mode = "reference" if ref_embs else "unsupervised"
    result: dict = {
        "ok":             True,
        "manifest_path":  manifest_path,
        "mode":           mode,
        "total_frames":   total,
        "npc_frames":     npc_frame_counts,
        "unknown_frames": unknown_count,
        "occluded_faces": occluded_count,
        "references":     list(ref_embs.keys()),
        "review_dir":     str(pr),
    }
    if mode == "unsupervised":
        result["clusters_found"] = next_cid
        result["next_step"]      = "Register NPCs with /register, then re-run analyze. Or use /video/patch_manifest to assign unknown faces."
    else:
        result["next_step"] = "Check npc_frames counts and npc_review/ previews, then run /video/swap_frames."
    return JSONResponse(result)


@app.post("/video/patch_manifest")
def video_patch_manifest(
    clip:          str  = Form(""),
    manifest_path: str  = Form(""),
    frame_start:   str  = Form(""),        # "000043" or "0" — empty = all frames
    frame_end:     str  = Form(""),
    npc_assign:    str  = Form(""),        # JSON: {"face_0":"jenny","face_1":"marco"}
    cluster_id:    str  = Form(""),        # if set, patch only this cluster
    mask_parts:    str  = Form(""),        # comma-separated parts; "none" = clear the list
    expand:        int  = Form(-1),        # -1 = don't change
    blur:          int  = Form(-1),
    occlude:       str  = Form(""),        # "" = don't change, "true"/"false" = set occluder blend
    occ_thresh:    str  = Form(""),        # "" = don't change; "none"/"0" clears; e.g. "1.0" = occluder più aggressivo
    invert:        str  = Form(""),        # "" = don't change; "true" = swap ONLY the mask (negative)
    unlock:        bool = Form(False),      # clear the 'locked' flag (redo a manual correction)
    swapper_ovr:   str  = Form("", alias="swapper"),  # per-frame swapper override; "global" clears it
    scale:         str  = Form(""),        # "" = don't change; "1"/"none" clears; e.g. "1.05" = +5% face
    set_npc:       str  = Form(""),        # force dest npc on all faces in range (also unknowns)
    skip:          str  = Form(""),        # "" = don't change, "true" = skip, "false" = un-skip
    status:        str  = Form("pending_retry"),
):
    """Update NPC assignments, swap settings, or skip flags for a frame range."""
    frames_dir_pm = ""
    if clip:
        cp_pm = _resolve_clip(clip)
        manifest_path = cp_pm["manifest"]
        frames_dir_pm = cp_pm["frames"]
    if not manifest_path:
        raise HTTPException(400, "provide 'clip' or manifest_path")
    manifest = _load_manifest(manifest_path)

    def norm_fid(s: str) -> str:
        try:
            return f"{int(s):06d}"
        except ValueError:
            return s

    all_fids   = sorted(manifest["frames"].keys())
    fid_start  = norm_fid(frame_start) if frame_start else all_fids[0]
    fid_end    = norm_fid(frame_end)   if frame_end   else all_fids[-1]

    npc_assign_map: dict = {}
    if npc_assign:
        try:
            npc_assign_map = json.loads(npc_assign)
        except Exception as e:
            raise HTTPException(400, f"Invalid npc_assign JSON: {e}")
        for v in npc_assign_map.values():
            _check_npc_id(str(v))   # ids end up in faces/<id>.npy paths at swap time

    if set_npc:
        _check_npc_id(set_npc)
    skip_l   = skip.strip().lower()
    clear_m  = mask_parts.strip().lower() == "none"

    patched = 0
    for fid in all_fids:
        if fid < fid_start or fid > fid_end:
            continue

        fdata = manifest["frames"][fid]

        if skip_l == "true":
            fdata["skip"]   = True
            fdata["status"] = "skip"
            patched += 1
            continue
        if skip_l == "false" and fdata.get("skip"):
            fdata["skip"] = False   # un-skip; status set below
        if unlock and fdata.get("locked"):
            fdata["locked"] = False           # redo: batch can touch it again
            fdata.pop("manual", None); fdata.pop("manual_params", None)

        # Forcing an npc on a frame the analysis found no face in: re-detect
        # (lower threshold) and create the face entries so the swap has a target.
        if set_npc and not fdata.get("faces") and frames_dir_pm:
            fp = Path(frames_dir_pm) / f"{fid}.png"
            img_pm = cv2.imread(str(fp)) if fp.exists() else None
            if img_pm is not None:
                det = _get_faces_sorted(img_pm, 0.25)
                fdata["faces"] = [{
                    "face_idx":  i,
                    "npc_id":    None,   # set_npc below assigns it
                    "matched_by": "manual",
                    "similarity": 0.0,
                    "det_score":  round(float(f.det_score), 4),
                    "bbox":       [round(float(v), 1) for v in f.bbox],
                    "pose":       [0.0, 0.0, 0.0],
                    "occluded":   False,
                    "swap_settings": {"mask_parts": [], "expand": 0, "blur": 0},
                    "swap_quality": None, "flagged": False, "flag_reasons": [],
                } for i, f in enumerate(det)]
                fdata["skip"] = False

        fdata["status"] = status

        for fe in fdata.get("faces", []):
            # Support both old cluster_id (clustering mode) and npc_id (reference mode)
            cid = fe.get("cluster_id") or fe.get("npc_id") or ""

            # Filter by cluster if requested
            if cluster_id and cid != cluster_id:
                continue

            # NPC assignment: key can be cluster_id OR current npc_id → replacement
            if cid in npc_assign_map:
                new_npc = npc_assign_map[cid]
                if fe.get("npc_id") and fe["npc_id"] != new_npc:
                    fe["source_npc_id"] = fe["npc_id"]   # preserve original identity
                fe["npc_id"] = new_npc

            # Force dest npc on every face in range — assigns unknowns too
            if set_npc:
                if fe.get("npc_id") and fe["npc_id"] != set_npc:
                    fe["source_npc_id"] = fe["npc_id"]
                fe["npc_id"] = set_npc

            # Swap settings update
            s = fe.setdefault("swap_settings", {"mask_parts": [], "expand": 0, "blur": 0})
            if clear_m:
                s["mask_parts"] = []
            elif mask_parts:
                s["mask_parts"] = [p.strip() for p in mask_parts.split(",") if p.strip()]
            if expand >= 0:
                s["expand"] = expand
            if blur >= 0:
                s["blur"] = blur
            if occlude.strip().lower() in ("true", "false"):
                s["occlude"] = occlude.strip().lower() == "true"
            ot_l = occ_thresh.strip().lower()
            if ot_l:
                if ot_l in ("none", "global", "0", "0.0", "0.00"):
                    s.pop("occ_thresh", None)
                else:
                    try:
                        s["occ_thresh"] = round(min(3.0, max(-3.0, float(ot_l))), 2)
                    except ValueError:
                        pass
            if invert.strip().lower() in ("true", "false"):
                s["invert"] = invert.strip().lower() == "true"
            sw_l = swapper_ovr.strip().lower()
            if sw_l == "global":
                s.pop("swapper", None)          # revert to the global swapper
            elif sw_l in VALID_SWAPPERS:
                s["swapper"] = sw_l
            sc_l = scale.strip().lower()
            if sc_l:
                if sc_l in ("none", "global", "1", "1.0", "1.00"):
                    s.pop("scale", None)
                else:
                    try:
                        s["scale"] = round(min(1.5, max(0.8, float(sc_l))), 3)
                    except ValueError:
                        pass

            # Clear quality flags for retry
            if status in ("pending_retry", "pending"):
                fe["swap_quality"] = None
                fe["flagged"]      = False
                fe["flag_reasons"] = []

        patched += 1

    # Update cluster-level npc_id for book-keeping (old clustering mode only)
    if npc_assign_map and "clusters" in manifest:
        for cid2, nid in npc_assign_map.items():
            if cid2 in manifest["clusters"]:
                manifest["clusters"][cid2]["npc_id"] = nid

    _save_manifest(manifest_path, manifest)
    return JSONResponse({"ok": True, "patched_frames": patched, "range": f"{fid_start}–{fid_end}"})


# ── Manual per-frame corrections (expression / texture) → save + lock ────────
# These operate on the SWAPPED frame using the ORIGINAL frame as driving/donor,
# overwrite frames_swapped/<fid>.png, and mark the frame locked so the batch
# "Swappa tutti" never recomputes it. Both are moving-video-unfriendly in bulk
# (LivePortrait cost / grain flicker) — hence manual, on the few bad frames.
def _lock_frame(manifest_path: str, fid: str, note: str, params: Optional[dict] = None):
    m = _load_manifest(manifest_path)
    fdata = m.get("frames", {}).get(fid)
    if fdata is not None:
        fdata["locked"] = True
        fdata["status"] = "swapped"
        fdata["manual"] = note
        if params is not None:
            fdata["manual_params"] = params   # exact values, for GUI reload + replicate
        _save_manifest(manifest_path, m)
    # the correction rewrote frames_swapped/<fid> — a stale frames_restored copy
    # would win in assemble (it prefers restored) and hide the manual work
    stale = Path(manifest_path).parent / "frames_restored" / f"{fid}.png"
    if stale.exists():
        try:
            stale.unlink()
        except OSError:
            pass


def _frame_range_fids(clip_dir, fid: str, fid_end: str):
    """[fid] or [fid..fid_end] as zero-padded ids that exist as swapped frames."""
    a = int(fid)
    b = int(fid_end) if fid_end.strip() else a
    if b < a:
        a, b = b, a
    out = []
    for k in range(a, b + 1):
        if (Path(clip_dir) / "frames_swapped" / f"{k:06d}.png").exists():
            out.append(f"{k:06d}")
    return out


@app.post("/video/frame_expression")
def video_frame_expression(
    fid:      str   = Form(...),
    clip:     str   = Form(""),
    fid_end:  str   = Form(""),         # optional: apply to fid..fid_end
    region:   str   = Form("exp"),      # exp | eyes | lip
    strength: float = Form(1.0),
):
    """Expression restore on swapped frame(s) (driving = original). Locks them."""
    import requests
    cp = _resolve_clip(clip) if clip else None
    if cp is None:
        raise HTTPException(400, "provide 'clip'")
    _ensure_expression_worker()
    params = {"kind": "expression", "region": region, "strength": strength}
    done = []
    for f6 in _frame_range_fids(cp["dir"], fid, fid_end):
        swp_p  = Path(cp["swapped"]) / f"{f6}.png"
        orig_p = Path(cp["frames"])  / f"{f6}.png"
        if not orig_p.exists():
            continue
        try:
            r = requests.post(EXPRESSION_URL + "/restore",
                files={"source":  ("s.png", swp_p.read_bytes(),  "application/octet-stream"),
                       "driving": ("d.png", orig_p.read_bytes(), "application/octet-stream")},
                data={"region": region, "strength": str(strength)}, timeout=300)
        except Exception as e:
            raise HTTPException(502, f"expression worker unreachable: {e}")
        if r.status_code != 200:
            raise HTTPException(r.status_code, f"expression frame {f6}: {r.text[:200]}")
        cv2.imwrite(str(swp_p), _decode_image(r.content))
        _lock_frame(cp["manifest"], f6, f"expression:{region}", params)
        done.append(f6)
    if not done:
        raise HTTPException(404, "no swapped frames in range")
    return JSONResponse({"ok": True, "frames": done, "count": len(done), "locked": True})


def _build_parts_mask(img: np.ndarray, parts: str, occlude: bool,
                      expand: int, blur: int, invert: bool,
                      det_thresh: float = DEFAULT_DET_THRESH,
                      bbox=None, occ_thresh: float = 0.0) -> np.ndarray:
    """PRESERVE mask — WHITE = keep original, black = use swap.

    Same 'white = preserve' semantics as /swap (images): parts = face regions
    to KEEP from the original (list the EXCEPTIONS not to swap: hair, mouth…),
    the occluder ADDS (OR) hands/objects in front of the face to the preserved
    set. This is the single source of truth used by both the video swap blend
    and the GUI mask preview, so they can never diverge.

    det_thresh/bbox: the frames that NEED the occluder (hand over the face,
    tilted head) are exactly those where detection fails at the default
    threshold — pass the video's det_thresh and the manifest bbox as fallback
    so parser+occluder still run on the right window instead of being skipped.
    """
    h, w = img.shape[:2]
    faces = _get_faces_sorted(img, det_thresh)
    fb = faces[0].bbox if faces else (bbox[:4] if bbox is not None else None)
    have_face = fb is not None
    px1 = py1 = 0; px2, py2 = w, h
    if have_face:
        x1b, y1b, x2b, y2b = fb
        bw, bh = x2b - x1b, y2b - y1b; pad = 0.4
        px1 = max(0, int(x1b - bw*pad)); py1 = max(0, int(y1b - bh*pad))
        px2 = min(w, int(x2b + bw*pad)); py2 = min(h, int(y2b + bh*pad))

    combined = np.zeros((h, w), dtype=np.uint8)
    parts_list = [p.strip() for p in parts.split(",") if p.strip()]
    if parts_list and faceparser_sess is not None:
        labels = set()
        for p in parts_list:
            labels.update(FACE_PARTS.get(p, []))
        parsing = _run_faceparser_labels(img[py1:py2, px1:px2])
        if parsing is not None:
            pc = np.isin(parsing, list(labels)).astype(np.uint8) * 255
            pc = cv2.resize(pc, (px2-px1, py2-py1), interpolation=cv2.INTER_NEAREST)
            combined[py1:py2, px1:px2] = pc

    # occluder ADDS occluded pixels (hands/objects) to the preserved set (OR)
    # — BEFORE expand/blur so those act on the whole mask (occluder included).
    # Needs a face window (detected or manifest bbox): full-image output is junk.
    if occlude and occluder_sess is not None and have_face:
        vis = _run_occluder(img[py1:py2, px1:px2], occ_thresh)   # 1=visible, 0=occluded
        occ = np.zeros((h, w), dtype=np.float32)
        occ[py1:py2, px1:px2] = 1.0 - vis
        combined = np.maximum(combined, (occ * 255).astype(np.uint8))

    if expand != 0:
        r = abs(expand)
        k = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (2*r+1, 2*r+1))
        combined = cv2.dilate(combined, k) if expand > 0 else cv2.erode(combined, k)
    if blur > 0:
        combined = cv2.GaussianBlur(combined, (blur*2+1, blur*2+1), 0)

    if invert:
        combined = 255 - combined
    return combined


@app.get("/video/frame_mask")
def video_frame_mask(fid: str, clip: str = "", parts: str = "skin",
                     occlude: bool = False, expand: int = 0, blur: int = 0,
                     invert: bool = False, occ_thresh: float = 0.0):
    """Preview the swap mask for a frame (parser parts + occluder + expand/blur)."""
    if face_app is None:
        raise HTTPException(503, "face model not loaded")
    cp = _resolve_clip(clip) if clip else None
    if cp is None:
        raise HTTPException(400, "provide 'clip'")
    img = cv2.imread(str(_clip_frame_path(clip, "", fid)))
    if img is None:
        raise HTTPException(500, "cannot read frame")
    # same det_thresh + bbox fallback the video blend uses → preview == blend
    det_th, fb = DEFAULT_DET_THRESH, None
    try:
        m = _load_manifest(cp["manifest"])
        det_th = float(m.get("video_info", {}).get("det_thresh", DEFAULT_DET_THRESH))
        ffs = m.get("frames", {}).get(fid, {}).get("faces", [])
        fb = ffs[0].get("bbox") if ffs else None
    except Exception:
        pass
    mask = _build_parts_mask(img, parts, occlude, expand, blur, invert,
                             det_thresh=det_th, bbox=fb,
                             occ_thresh=min(3.0, max(-3.0, occ_thresh)))
    return Response(content=_encode_png(mask), media_type="image/png")


@app.post("/video/frame_restore")
def video_frame_restore(
    fid:      str   = Form(...),
    clip:     str   = Form(""),
    fid_end:  str   = Form(""),              # optional: apply to fid..fid_end
    restorer: str   = Form("codeformer"),    # codeformer | gfpgan
    fidelity: float = Form(0.7),
):
    """Face restore on swapped frame(s) → save + lock."""
    cp = _resolve_clip(clip) if clip else None
    if cp is None:
        raise HTTPException(400, "provide 'clip'")
    if restorer == "codeformer" and codeformer_sess is None:
        raise HTTPException(503, "CodeFormer not loaded")
    if restorer == "gfpgan" and gfpganer is None:
        raise HTTPException(503, "GFPGAN not loaded")
    params = {"kind": "restore", "restorer": restorer, "fidelity": fidelity}
    done = []
    for f6 in _frame_range_fids(cp["dir"], fid, fid_end):
        swp_p = Path(cp["swapped"]) / f"{f6}.png"
        img = cv2.imread(str(swp_p))
        if restorer == "codeformer":
            out = _restore_codeformer(img, fidelity=fidelity)
        else:
            _, _, out = gfpganer.enhance(img, has_aligned=False,
                                         only_center_face=False, paste_back=True)
        cv2.imwrite(str(swp_p), out)
        _lock_frame(cp["manifest"], f6, f"restore:{restorer}", params)
        done.append(f6)
    if not done:
        raise HTTPException(404, "no swapped frames in range")
    return JSONResponse({"ok": True, "frames": done, "count": len(done), "locked": True})


@app.post("/video/frame_texture")
def video_frame_texture(
    fid:    str   = Form(...),
    clip:   str   = Form(""),
    fid_end: str  = Form(""),            # optional: apply to fid..fid_end
    amount: float = Form(1.0),
    radius: int   = Form(3),
    parts:  str   = Form("skin,nose"),
    mono:   bool  = Form(True),
):
    """Skin-texture transfer on swapped frame(s) (donor = original). Locks them."""
    if face_app is None:
        raise HTTPException(503, "face model not loaded")
    cp = _resolve_clip(clip) if clip else None
    if cp is None:
        raise HTTPException(400, "provide 'clip'")
    params = {"kind": "texture", "amount": amount, "radius": radius, "parts": parts, "mono": mono}
    done = []; last_delta = 0.0
    for f6 in _frame_range_fids(cp["dir"], fid, fid_end):
        swp_p  = Path(cp["swapped"]) / f"{f6}.png"
        orig_p = Path(cp["frames"])  / f"{f6}.png"
        if not orig_p.exists():
            continue
        out, last_delta = _apply_texture(cv2.imread(str(swp_p)), cv2.imread(str(orig_p)),
                                         max(0.0, min(3.0, amount)), max(1, min(12, radius)),
                                         parts, mono)
        cv2.imwrite(str(swp_p), out)
        _lock_frame(cp["manifest"], f6, "texture", params)
        done.append(f6)
    if not done:
        raise HTTPException(404, "no swapped frames in range")
    return JSONResponse({"ok": True, "frames": done, "count": len(done),
                         "locked": True, "delta": round(last_delta, 2)})


@app.post("/video/swap_frames")
def video_swap_frames(
    clip:          str  = Form(""),
    manifest_path: str  = Form(""),
    frames_dir:    str  = Form(""),
    output_dir:    str  = Form(""),
    retry_only:    bool = Form(False),   # True = only process pending_retry frames
    reprocess:     bool = Form(False),    # True = re-swap already-swapped frames too (skip locked)
    only_fid:      str  = Form(""),      # "Prova qui": comma-separated fids — FORCED re-swap
    swapper_name:  str  = Form("inswapper", alias="swapper"),  # inswapper|ghost|simswap
    run_async:     bool = Form(False),
    _job_id:       Optional[str] = None,
):
    """Swap faces in all pending frames according to manifest NPC assignments.

    only_fid: restrict to specific frames and IGNORE status/skip gating —
    re-swaps them with the CURRENT manifest settings even if already swapped
    (the GUI's "Prova qui": patch_manifest the frame, then swap it alone).
    reprocess: also re-swap already-swapped frames (e.g. global swapper changed)
    — locked (manually corrected) and skipped frames are always left alone.
    """
    if clip:
        cp = _resolve_clip(clip)
        manifest_path, frames_dir, output_dir = cp["manifest"], cp["frames"], cp["swapped"]
    if not (manifest_path and frames_dir and output_dir):
        raise HTTPException(400, "provide 'clip' or manifest_path+frames_dir+output_dir")
    if run_async:
        return _spawn_job("swap", video_swap_frames, dict(
            clip="", manifest_path=manifest_path, frames_dir=frames_dir,
            output_dir=output_dir, retry_only=retry_only, reprocess=reprocess,
            only_fid=only_fid, swapper_name=swapper_name))
    if face_app is None or swapper is None:
        raise HTTPException(503, "face/swap model not loaded")
    swp = _check_swapper(swapper_name)

    manifest = _load_manifest(manifest_path)
    fd  = Path(frames_dir)
    out = Path(output_dir)
    out.mkdir(parents=True, exist_ok=True)

    # Detection threshold: reuse the one analyze ran with (bboxes must line up for IoU match)
    swap_det_thresh = float(manifest.get("video_info", {}).get("det_thresh", 0.35))

    # Source identity cached per (npc, swapper) — a frame may override the
    # global swapper, and each swapper needs its own identity representation.
    src_face_cache: dict = {}
    def _get_src_face(npc_id: str, use_swp: str):
        key = (npc_id, use_swp)
        if key not in src_face_cache:
            if use_swp != "inswapper":
                src_face_cache[key] = _npc_alt_latent(npc_id, use_swp)
                return src_face_cache[key]
            face = _npc_source_face(npc_id)
            if face is None:
                p = FACES_DIR / f"{npc_id}.jpg" if npc_id and _NPC_ID_RE.match(npc_id) else None
                img_c = cv2.imread(str(p)) if p and p.exists() else None
                if img_c is not None:
                    fcs = _get_faces_sorted(img_c)
                    face = fcs[0] if fcs else None
            src_face_cache[key] = face
        return src_face_cache[key]

    only_set: set = set()
    for tok in only_fid.split(","):
        tok = tok.strip()
        if tok:
            only_set.add(f"{int(tok):06d}" if tok.isdigit() else tok)

    swapped_count = skipped_count = error_count = 0
    sorted_fids   = sorted(manifest["frames"].keys())
    if only_set:
        missing = only_set - set(sorted_fids)
        if missing:
            raise HTTPException(400, f"only_fid: frames not in manifest: {sorted(missing)}")
        sorted_fids = [f for f in sorted_fids if f in only_set]
    total_swap    = len(sorted_fids)

    for i, fid in enumerate(sorted_fids):
        _progress(_job_id, i, total_swap, "swap")
        if i % 25 == 0:
            print(f"[swap] {i}/{total_swap}", flush=True)
        fdata = manifest["frames"][fid]
        # only_fid = forced re-swap: bypass skip/status/lock gating entirely
        if not only_set:
            if fdata.get("skip"):
                skipped_count += 1
                continue
            # locked = manually corrected (expression/texture) → never batch-swap
            if fdata.get("locked"):
                skipped_count += 1
                continue

            status = fdata.get("status", "pending")
            if retry_only and status != "pending_retry":
                skipped_count += 1
                continue
            # reprocess: swap any non-locked frame regardless of status
            if not retry_only and not reprocess and status not in ("pending", "pending_retry"):
                skipped_count += 1
                continue

        actionable = [fe for fe in fdata.get("faces", []) if fe.get("npc_id")]
        if not actionable:
            fdata["status"] = "no_npc"
            skipped_count += 1
            continue

        src_path = fd / f"{fid}.png"
        if not src_path.exists():
            fdata["status"] = "missing_frame"
            error_count += 1
            continue

        frame_img = cv2.imread(str(src_path))
        if frame_img is None:
            fdata["status"] = "decode_error"
            error_count += 1
            continue

        # Use same det_thresh as analyze to get consistent face list
        target_faces = _get_faces_sorted(frame_img, swap_det_thresh)
        result_img   = frame_img.copy()

        for fe in actionable:
            npc_id   = fe["npc_id"]
            settings = fe.get("swap_settings", {})
            fe["flag_reasons"] = []  # clear previous
            # per-face swapper override (else the global one)
            f_swp = settings.get("swapper") or swp
            if f_swp not in VALID_SWAPPERS or not (
                    f_swp == "inswapper" or _alt_swapper_available(f_swp)):
                f_swp = swp

            # Match target face by bbox IoU (robust to det_thresh/index differences)
            manifest_bbox = fe.get("bbox")
            target_face   = None
            if manifest_bbox and target_faces:
                best_iou = 0.0
                for tf in target_faces:
                    iou = _bbox_iou(manifest_bbox, tf.bbox)
                    if iou > best_iou:
                        best_iou  = iou
                        target_face = tf
                if best_iou < 0.20:
                    fe["flag_reasons"].append(f"bbox_mismatch:best_iou={best_iou:.2f}")
                    target_face = None
            elif target_faces:
                target_face = target_faces[fe.get("face_idx", 0)] if fe.get("face_idx", 0) < len(target_faces) else None

            if target_face is None:
                fe["flag_reasons"].append("no_matching_face")
                continue

            # temporally smoothed kps (written by /video/smooth_kps) beat the
            # fresh per-frame detection: kills the warp jitter between frames
            sm_kps = fe.get("kps_smooth")
            if sm_kps and len(sm_kps) == 5:
                try:
                    target_face.kps = np.array(sm_kps, dtype=np.float32)
                except Exception:
                    pass

            src_face = _get_src_face(npc_id, f_swp)
            if src_face is None:
                fe["flag_reasons"].append(f"npc_source_missing:{npc_id}")
                continue

            try:
                f_scale = 1.0
                try:
                    f_scale = min(1.5, max(0.8, float(settings.get("scale") or 1.0)))
                except (TypeError, ValueError):
                    pass
                result_img = _swap_one_face(result_img, target_face, src_face,
                                            f_swp, f_scale)
            except Exception as e:
                fe["flag_reasons"].append(f"swap_error:{e}")
                continue

            # Optional PRESERVE-mask blend (same 'white=preserve' semantics as
            # the image /swap): mask_parts = face regions to KEEP from the
            # original (hair, mouth…), occluder ADDS hands/objects to keep.
            # Built by the shared _build_parts_mask so preview == blend.
            mask_parts = settings.get("mask_parts", [])
            use_occl   = bool(settings.get("occlude")) and occluder_sess is not None
            expand_px  = settings.get("expand", 0)
            blur_px    = settings.get("blur", 0)
            invert_m   = bool(settings.get("invert"))
            if (mask_parts and faceparser_sess is not None) or use_occl:
                try:
                    occ_th = 0.0
                    try:
                        occ_th = min(3.0, max(-3.0, float(settings.get("occ_thresh") or 0.0)))
                    except (TypeError, ValueError):
                        pass
                    # detect from the ORIGINAL frame (clean features for parser/occluder);
                    # video det_thresh + manifest bbox so occluded/tilted faces still work
                    pmask = _build_parts_mask(frame_img, ",".join(mask_parts),
                                              use_occl, int(expand_px), int(blur_px), invert_m,
                                              det_thresh=swap_det_thresh,
                                              bbox=fe.get("bbox"), occ_thresh=occ_th)
                    alpha3 = np.stack([pmask.astype(np.float32) / 255.0] * 3, axis=-1)
                    # white = preserve original, black = keep swap
                    result_img = (frame_img.astype(np.float32) * alpha3 +
                                  result_img.astype(np.float32) * (1.0 - alpha3)).astype(np.uint8)
                except Exception as e:
                    fe["flag_reasons"].append(f"mask_error:{e}")

        cv2.imwrite(str(out / f"{fid}.png"), result_img)
        # invalidate the restored copy: it was built from the PREVIOUS swap —
        # assemble prefers frames_restored/, a stale one would hide this re-swap
        stale_restored = out.parent / "frames_restored" / f"{fid}.png"
        if stale_restored.exists():
            try:
                stale_restored.unlink()
            except OSError:
                pass
        fdata["status"]       = "swapped"
        fdata["swapped_path"] = str(out / f"{fid}.png")
        swapped_count += 1

    _save_manifest(manifest_path, manifest)
    return JSONResponse({
        "ok":            True,
        "swapped_count": swapped_count,
        "skipped_count": skipped_count,
        "error_count":   error_count,
        "output_dir":    str(out),
    })


@app.post("/video/smooth_kps")
def video_smooth_kps(
    clip:          str  = Form(""),
    manifest_path: str  = Form(""),
    frames_dir:    str  = Form(""),
    window:        int  = Form(5),      # gaussian smoothing window (frames, odd)
    clear:         bool = Form(False),  # remove kps_smooth from every frame
    run_async:     bool = Form(False),
    _job_id:       Optional[str] = None,
):
    """Temporal stabilization of face keypoints (anti-jitter).

    Detection jitter (kps moving 1-2px frame to frame) becomes warp jitter —
    the swapped face "swims" on the head. This pass re-detects faces on the
    ORIGINAL frames, builds one kps track per manifest face (matched by bbox
    IoU like the swap), gaussian-smooths each track over contiguous runs and
    stores the result as fe["kps_smooth"] in the manifest. video_swap_frames
    uses kps_smooth instead of the fresh detection when present — re-swap
    (Riprocessa tutto) to benefit. Identity/analysis data untouched.
    """
    if clip:
        cp = _resolve_clip(clip)
        manifest_path, frames_dir = cp["manifest"], cp["frames"]
    if not manifest_path:
        raise HTTPException(400, "provide 'clip' or manifest_path")
    if not frames_dir or not Path(frames_dir).is_dir():
        # legacy path-mode: original frames dir recorded by analyze
        mf = _load_manifest(manifest_path)
        frames_dir = mf.get("video_info", {}).get("frames_dir", "")
        if not frames_dir:
            raise HTTPException(400, "frames_dir missing (not in manifest either)")
    if run_async:
        return _spawn_job("smooth", video_smooth_kps, dict(
            clip="", manifest_path=manifest_path, frames_dir=frames_dir,
            window=window, clear=clear))

    manifest = _load_manifest(manifest_path)
    fids = sorted(manifest["frames"].keys())

    if clear:
        removed = 0
        for fid in fids:
            for fe in manifest["frames"][fid].get("faces", []):
                if fe.pop("kps_smooth", None) is not None:
                    removed += 1
        manifest.get("video_info", {}).pop("kps_smoothed", None)
        _save_manifest(manifest_path, manifest)
        return JSONResponse({"ok": True, "cleared": removed})

    window = max(3, min(15, int(window))) | 1   # odd, 3..15
    det_thresh = float(manifest.get("video_info", {}).get("det_thresh", 0.35))

    # 1) detect + match: one kps sample per manifest face per frame
    tracks: dict = {}   # key -> list of (frame_index, face_entry, kps (5,2))
    for i, fid in enumerate(fids):
        _progress(_job_id, i, len(fids), "smooth_kps")
        faces = manifest["frames"][fid].get("faces", [])
        if not faces:
            continue
        img = cv2.imread(str(Path(frames_dir) / f"{fid}.png"))
        if img is None:
            continue
        det = _get_faces_sorted(img, det_thresh)
        if not det:
            continue
        for fe in faces:
            bb = fe.get("bbox")
            if not bb:
                continue
            best, best_iou = None, 0.0
            for tf in det:
                iou = _bbox_iou(bb, tf.bbox)
                if iou > best_iou:
                    best_iou, best = iou, tf
            if best is None or best_iou < 0.20:
                continue
            key = fe.get("npc_id") or f"idx{fe.get('face_idx', 0)}"
            tracks.setdefault(key, []).append(
                (i, fe, np.array(best.kps, dtype=np.float64)))

    # 2) gaussian smoothing per track, only across contiguous frame runs
    half  = window // 2
    sigma = max(0.8, window / 3.0)
    gauss = np.exp(-0.5 * (np.arange(-half, half + 1) / sigma) ** 2)
    smoothed = 0
    for key, seq in tracks.items():
        runs, cur = [], [seq[0]]
        for prev, nxt in zip(seq, seq[1:]):
            if nxt[0] == prev[0] + 1:
                cur.append(nxt)
            else:
                runs.append(cur); cur = [nxt]
        runs.append(cur)
        for run in runs:
            arr = np.stack([k for _, _, k in run])          # (T, 5, 2)
            T = len(run)
            for t in range(T):
                lo, hi = max(0, t - half), min(T, t + half + 1)
                wts = gauss[(lo - t + half):(hi - t + half)]
                sm = (arr[lo:hi] * wts[:, None, None]).sum(0) / wts.sum()
                run[t][1]["kps_smooth"] = [
                    [round(float(x), 2), round(float(y), 2)] for x, y in sm]
                smoothed += 1

    manifest.setdefault("video_info", {})["kps_smoothed"] = {"window": window}
    _save_manifest(manifest_path, manifest)
    return JSONResponse({
        "ok":        True,
        "smoothed":  smoothed,
        "tracks":    {k: len(v) for k, v in tracks.items()},
        "window":    window,
        "next_step": "Re-swap the frames (reprocess) so the smoothed kps take effect.",
    })


@app.post("/video/quality")
def video_quality(
    clip:           str   = Form(""),
    manifest_path:  str   = Form(""),
    swapped_dir:    str   = Form(""),
    sim_threshold:  float = Form(0.30),
    det_threshold:  float = Form(0.50),
    blur_threshold: float = Form(50.0),
    pose_threshold: float = Form(30.0),
    gap_frames:     int   = Form(5),
    run_async:      bool  = Form(False),
    _job_id:        Optional[str] = None,
):
    """Analyze swapped frames: similarity to dest NPC (from manifest npc_id), det score, blur, pose."""
    if clip:
        cp = _resolve_clip(clip)
        manifest_path, swapped_dir = cp["manifest"], cp["swapped"]
    if not (manifest_path and swapped_dir):
        raise HTTPException(400, "provide 'clip' or manifest_path+swapped_dir")
    if run_async:
        return _spawn_job("quality", video_quality, dict(
            clip="", manifest_path=manifest_path, swapped_dir=swapped_dir,
            sim_threshold=sim_threshold, det_threshold=det_threshold,
            blur_threshold=blur_threshold, pose_threshold=pose_threshold,
            gap_frames=gap_frames))
    if face_app is None:
        raise HTTPException(503, "face model not loaded")

    manifest    = _load_manifest(manifest_path)
    sd          = Path(swapped_dir)
    prev_pose   = {}   # cid -> (pose, frame_index) — index needed to reset across gaps
    flagged_ids = []
    quality_det_thresh = float(manifest.get("video_info", {}).get("det_thresh", 0.35))

    # Pre-load embedding sets for all dest NPCs referenced in manifest
    dest_embs: dict = {}
    for fdata in manifest["frames"].values():
        for fe in fdata.get("faces", []):
            nid = fe.get("npc_id")
            if nid and nid not in dest_embs:
                dest_embs[nid] = _load_npc_embs(nid)
                if dest_embs[nid] is not None:
                    print(f"[quality] Loaded dest embeddings: {nid} ({dest_embs[nid].shape[0]})", flush=True)
                else:
                    print(f"[quality] WARNING: dest NPC '{nid}' not registered — sim check skipped", flush=True)

    all_fids    = sorted(manifest["frames"].keys())
    total_q     = len(all_fids)

    for qi, fid in enumerate(all_fids):
        _progress(_job_id, qi, total_q, "quality")
        if qi % 50 == 0:
            print(f"[quality] {qi}/{total_q}", flush=True)

        fdata  = manifest["frames"][fid]
        if fdata.get("status") not in ("swapped", "approved", "flagged"):
            continue

        img_path = sd / f"{fid}.png"
        if not img_path.exists():
            continue

        img       = cv2.imread(str(img_path))
        out_faces = _get_faces_sorted(img, quality_det_thresh) if img is not None else []
        frame_flagged = False

        for fe in fdata.get("faces", []):
            if not fe.get("npc_id"):
                continue

            # Skip frames that were already problematic before swap
            orig_method = fe.get("matched_by")
            if orig_method in (None, "gap_fill") or fe.get("occluded"):
                continue

            cid     = fe.get("cluster_id") or fe.get("npc_id", "")
            reasons = []
            quality = {}

            # Find matching face in swapped frame by bbox IoU
            manifest_bbox = fe.get("bbox")
            of = None
            if manifest_bbox and out_faces:
                best_iou = 0.0
                for tf in out_faces:
                    iou = _bbox_iou(manifest_bbox, tf.bbox)
                    if iou > best_iou:
                        best_iou = iou
                        of = tf
                if best_iou < 0.15:
                    of = None

            if of is not None:
                det_score = float(of.det_score)
                quality["det_score"] = round(det_score, 4)

                # Similarity to dest NPC (max over stored embedding variants)
                dest_emb = dest_embs.get(fe.get("npc_id"))
                if dest_emb is not None:
                    sim = _npc_best_sim(of.normed_embedding, dest_emb)
                    quality["dest_sim"] = round(sim, 3)
                    if sim < sim_threshold:
                        reasons.append(f"sim:{sim:.2f}<{sim_threshold}")

                # Blur
                x1, y1, x2, y2 = [max(0, int(v)) for v in of.bbox]
                crop = img[y1:y2, x1:x2]
                if crop.size > 0:
                    gray = cv2.cvtColor(crop, cv2.COLOR_BGR2GRAY)
                    blur = float(cv2.Laplacian(gray, cv2.CV_64F).var())
                else:
                    blur = 0.0
                quality["blur_score"] = round(blur, 2)

                # Pose + inter-frame delta
                pose = of.pose.tolist() if hasattr(of, "pose") and of.pose is not None else [0.0, 0.0, 0.0]
                quality["pose"] = [round(v, 2) for v in pose]
                quality["pose_extreme"] = abs(pose[1]) > 45  # yaw > 45° = side profile
                # Delta only vs (near-)adjacent frames — skipped/occluded frames in
                # between would otherwise compare distant frames → false flags
                if cid in prev_pose and qi - prev_pose[cid][1] <= 2:
                    delta = sum(abs(c - p) for c, p in zip(pose, prev_pose[cid][0]))
                else:
                    delta = 0.0
                quality["pose_delta"] = round(delta, 2)
                prev_pose[cid] = (pose, qi)

                if det_score < det_threshold:
                    reasons.append(f"det:{det_score:.2f}<{det_threshold}")
                if blur < blur_threshold:
                    reasons.append(f"blur:{blur:.1f}<{blur_threshold}")
                if delta > pose_threshold:
                    reasons.append(f"pose_delta:{delta:.1f}>{pose_threshold}")
                # side_profile: context reason, appended only when sim already failed
                if quality.get("pose_extreme") and any(r.startswith("sim:") for r in reasons):
                    reasons.append(f"side_profile:yaw={pose[1]:.0f}")
            else:
                quality = {"det_score": 0.0, "blur_score": 0.0, "pose": [0,0,0], "pose_delta": 0.0}
                reasons.append("face_not_detected")

            fe["swap_quality"] = quality
            fe["flagged"]      = bool(reasons)
            fe["flag_reasons"] = reasons
            if reasons:
                frame_flagged = True

        if frame_flagged:
            fdata["status"] = "flagged"
            flagged_ids.append(fid)
        elif fdata["status"] == "swapped":
            fdata["status"] = "approved"

    # Build sequences: consecutive flagged frames within gap_frames of each other
    all_fids        = sorted(manifest["frames"].keys())
    frame_to_idx    = {fid: i for i, fid in enumerate(all_fids)}
    flagged_indices = sorted(frame_to_idx[fid] for fid in flagged_ids)

    seqs = []
    if flagged_indices:
        s = e = flagged_indices[0]
        for idx in flagged_indices[1:]:
            if idx - e <= gap_frames:
                e = idx
            else:
                seqs.append((s, e))
                s = e = idx
        seqs.append((s, e))

    seq_list = []
    for si, (s, e) in enumerate(seqs):
        seq_fids = all_fids[s:e + 1]
        reasons  = set()
        for fid2 in seq_fids:
            for fe in manifest["frames"].get(fid2, {}).get("faces", []):
                reasons.update(r.split(":")[0] for r in fe.get("flag_reasons", []))
        seq_list.append({
            "seq_id":      si,
            "start_frame": all_fids[s],
            "end_frame":   all_fids[e],
            "frame_count": e - s + 1,
            "reasons":     sorted(reasons),
            "status":      "pending_review",
        })

    manifest["flagged_sequences"] = seq_list
    _save_manifest(manifest_path, manifest)

    return JSONResponse({
        "ok":             True,
        "approved_count": sum(1 for f in manifest["frames"].values() if f.get("status") == "approved"),
        "flagged_count":  len(flagged_ids),
        "sequences":      seq_list,
        "next_step": "Review flagged sequences, then call /video/patch_manifest + /video/swap_frames?retry_only=true",
    })


@app.post("/video/restore_frames")
def video_restore_frames(
    clip:          str   = Form(""),
    manifest_path: str   = Form(""),
    swapped_dir:   str   = Form(""),
    restore:       str   = Form("codeformer"),
    fidelity:      float = Form(0.7),
    upscale:       int   = Form(0),
    frames:        str   = Form(""),   # comma-separated frame ids to (re)process; empty = all
    run_async:     bool  = Form(False),
    _job_id:       Optional[str] = None,
):
    """Apply face restoration to swapped frames, saving to frames_restored/. Incremental."""
    if clip:
        cp = _resolve_clip(clip)
        manifest_path, swapped_dir = cp["manifest"], cp["swapped"]
    if not (manifest_path and swapped_dir):
        raise HTTPException(400, "provide 'clip' or manifest_path+swapped_dir")
    if run_async:
        return _spawn_job("restore", video_restore_frames, dict(
            clip="", manifest_path=manifest_path, swapped_dir=swapped_dir, restore=restore,
            fidelity=fidelity, upscale=upscale, frames=frames))
    sd          = Path(swapped_dir)
    restore_dir = sd.parent / "frames_restored"
    restore_dir.mkdir(exist_ok=True)

    manifest   = _load_manifest(manifest_path)
    all_fids   = sorted(manifest["frames"].keys())

    # Filter to specific frames if requested
    if frames.strip():
        target_fids = set(f.strip() for f in frames.split(",") if f.strip())
    else:
        target_fids = None  # all swapped frames

    # True incrementality: a full run only restores frames whose swapped input
    # is newer than the restored output (e.g. after re-swapping a few frames).
    # If the restore params changed, everything is stale and gets redone.
    params_now  = {"restore": restore, "fidelity": round(float(fidelity), 3),
                   "upscale": int(upscale)}
    params_file = restore_dir / "_restore_params.json"
    params_changed = True
    if params_file.exists():
        try:
            with open(params_file) as pf:
                params_changed = json.load(pf) != params_now
        except Exception:
            pass

    done = 0; skipped = 0; errors = 0
    total = len(all_fids)

    for i, fid in enumerate(all_fids):
        _progress(_job_id, i, total, "restore")
        if i % 50 == 0:
            print(f"[restore_frames] {i}/{total}", flush=True)

        if target_fids and fid not in target_fids:
            continue

        fdata  = manifest["frames"][fid]
        status = fdata.get("status", "")
        src    = sd / f"{fid}.png"

        if not src.exists() or status not in ("approved", "flagged", "swapped"):
            skipped += 1
            continue
        # locked = manually corrected (restore/texture/expression already applied)
        # → never re-restore, would undo the correction. Explicit --frames overrides.
        if fdata.get("locked") and not (frames.strip() and fid in target_fids):
            skipped += 1
            continue

        # up to date: restored output newer than the swapped input, same params
        dst = restore_dir / f"{fid}.png"
        if (not params_changed and target_fids is None and dst.exists()
                and dst.stat().st_mtime >= src.stat().st_mtime):
            skipped += 1
            continue

        img = cv2.imread(str(src))
        if img is None:
            skipped += 1
            continue

        try:
            if restore == "codeformer" and codeformer_sess is not None:
                img = _restore_codeformer(img, fidelity=fidelity)
            elif restore == "gfpgan" and gfpganer is not None:
                _, _, img = gfpganer.enhance(img, has_aligned=False,
                                             only_center_face=False, paste_back=True)
        except Exception as e:
            print(f"[restore_frames] restore {fid}: {e}", flush=True)
            errors += 1
            # save unrestored copy so assemble doesn't fall back to un-swapped original
            cv2.imwrite(str(restore_dir / f"{fid}.png"), img)
            continue

        if upscale in (2, 4):
            if upscaler is not None:
                try:
                    img, _ = upscaler.enhance(img, outscale=upscale)
                except Exception as e:
                    print(f"[restore_frames] upscale {fid}: {e}", flush=True)
            else:
                pil = Image.fromarray(cv2.cvtColor(img, cv2.COLOR_BGR2RGB))
                w, h = pil.size
                pil = pil.resize((w * upscale, h * upscale), Image.LANCZOS)
                img = cv2.cvtColor(np.array(pil), cv2.COLOR_RGB2BGR)

        cv2.imwrite(str(restore_dir / f"{fid}.png"), img)
        done += 1

    # record params only after a FULL run (a targeted run with different params
    # must not claim the whole dir is at the new params)
    if target_fids is None:
        try:
            with open(params_file, "w") as pf:
                json.dump(params_now, pf)
        except Exception:
            pass

    print(f"[restore_frames] done={done} skipped={skipped} errors={errors}", flush=True)
    return JSONResponse({
        "ok":           True,
        "restore_dir":  str(restore_dir),
        "processed":    done,
        "skipped":      skipped,
        "errors":       errors,
        "next_step":    "Run video-assemble — it will use frames_restored/ automatically",
    })


def _motion_blur_face(img: np.ndarray, bbox, vec, strength: float = 0.6,
                      max_len: int = 31) -> np.ndarray:
    """Directional blur on the face region, matching the frame's motion.

    The swapped face is synthetically sharp; on frames with camera/subject
    motion the rest of the image has motion blur → "cutout" look. vec = face
    center displacement (px) between neighbor frames; kernel = line at that
    angle, length = speed * strength (180° shutter ≈ 0.5). Applied inside a
    feathered ellipse over the bbox — the background keeps its natural blur.
    """
    speed = float(np.hypot(vec[0], vec[1]))
    length = min(speed * strength, float(max_len))
    if length < 3.0:
        return img                      # static enough — no blur needed
    n = int(length) | 1
    kern = np.zeros((n, n), np.float32)
    c = n // 2
    ux, uy = vec[0] / speed, vec[1] / speed
    cv2.line(kern, (int(round(c - ux * c)), int(round(c - uy * c))),
             (int(round(c + ux * c)), int(round(c + uy * c))), 1.0, 1)
    s = kern.sum()
    if s <= 0:
        return img
    kern /= s

    h, w = img.shape[:2]
    x1, y1, x2, y2 = [int(v) for v in bbox[:4]]
    ax, ay = int((x2 - x1) * 0.62), int((y2 - y1) * 0.66)
    if ax < 2 or ay < 2:
        return img
    fk = (max(9, (x2 - x1) // 6)) | 1
    # work on a ROI around the face only — filter2D on the full frame would
    # make the per-assemble recompute needlessly expensive
    cx, cy = (x1 + x2) // 2, (y1 + y2) // 2
    pad = max(ax, ay) + n + fk
    rx1, ry1 = max(0, cx - pad), max(0, cy - pad)
    rx2, ry2 = min(w, cx + pad), min(h, cy + pad)
    if rx2 - rx1 < 4 or ry2 - ry1 < 4:
        return img
    roi = img[ry1:ry2, rx1:rx2]
    blurred = cv2.filter2D(roi, -1, kern)
    m = np.zeros(roi.shape[:2], np.float32)
    cv2.ellipse(m, (cx - rx1, cy - ry1), (ax, ay), 0, 0, 360, 1.0, -1)
    m = cv2.GaussianBlur(m, (fk, fk), 0)
    m3 = m[..., None]
    img[ry1:ry2, rx1:rx2] = (blurred.astype(np.float32) * m3 +
                             roi.astype(np.float32) * (1.0 - m3)).astype(np.uint8)
    return img


@app.post("/video/assemble")
def video_assemble(
    clip:          str   = Form(""),
    manifest_path: str   = Form(""),
    swapped_dir:   str   = Form(""),
    audio_path:    str   = Form(""),
    output_path:   str   = Form(""),
    fps:           str   = Form(""),        # empty = auto from video_info.json, fallback 30
    restore:       str   = Form(""),        # "codeformer" | "gfpgan" | ""
    fidelity:      float = Form(0.7),
    upscale:       int   = Form(0),         # 0=none, 2 or 4
    motion_blur:   str   = Form(""),        # "true" = directional blur on moving faces
    mb_strength:   float = Form(0.6),       # blur length = px speed * strength
    run_async:     bool  = Form(False),
    _job_id:       Optional[str] = None,
):
    """Assemble final video from swapped frames + audio. Optionally restore/upscale before encode."""
    if clip:
        cp = _resolve_clip(clip)
        manifest_path, swapped_dir = cp["manifest"], cp["swapped"]
        if not output_path:
            output_path = cp["result"]
        if not audio_path and os.path.exists(cp["audio"]):
            audio_path = cp["audio"]
    if not (manifest_path and swapped_dir and output_path):
        raise HTTPException(400, "provide 'clip' or manifest_path+swapped_dir+output_path")
    if run_async:
        return _spawn_job("assemble", video_assemble, dict(
            clip="", manifest_path=manifest_path, swapped_dir=swapped_dir, audio_path=audio_path,
            output_path=output_path, fps=fps, restore=restore,
            fidelity=fidelity, upscale=upscale,
            motion_blur=motion_blur, mb_strength=mb_strength))
    if not _ffmpeg_ok():
        raise HTTPException(503, "ffmpeg not found")

    manifest = _load_manifest(manifest_path)
    sd       = Path(swapped_dir)

    # fps: explicit > video_info.json (written by /video/extract) > 30
    if not fps.strip():
        fps = "30"
        vi_path = sd.parent / "video_info.json"
        if vi_path.exists():
            try:
                src_fps = str(json.load(open(vi_path)).get("source_fps", "") or "")
                if src_fps and src_fps != "unknown":
                    fps = src_fps
                    print(f"[video/assemble] fps auto from video_info.json: {fps}", flush=True)
            except Exception as e:
                print(f"[video/assemble] video_info.json read error: {e}", flush=True)

    # Temp dir for post-processed frames
    tmp_dir = sd.parent / "_assemble_tmp"
    tmp_dir.mkdir(exist_ok=True)

    orig_frames_dir = manifest["video_info"].get("frames_dir", "")
    restore_dir     = sd.parent / "frames_restored"
    use_restored    = restore_dir.exists() and any(restore_dir.iterdir())
    if use_restored:
        print(f"[video/assemble] Using pre-restored frames from frames_restored/", flush=True)
    processed       = 0
    has_flagged     = False

    print(f"[video/assemble] Assembling (restore={restore or ('pre-restored' if use_restored else 'none')}, upscale={upscale}x)...")
    asm_fids  = sorted(manifest["frames"].keys())
    asm_total = len(asm_fids)

    # Motion blur: face bbox per npc per frame → displacement between neighbor
    # frames. Applied to EVERY swapped frame, locked included (it adapts the
    # face to the frame's motion, it does not undo manual corrections).
    mb_on = motion_blur.strip().lower() in ("true", "1")
    mb_boxes: dict = {}
    if mb_on:
        for fid2 in asm_fids:
            for fe2 in manifest["frames"][fid2].get("faces", []):
                if fe2.get("npc_id") and fe2.get("bbox"):
                    mb_boxes.setdefault(fid2, {})[fe2["npc_id"]] = fe2["bbox"]
    for asm_i, fid in enumerate(asm_fids):
        _progress(_job_id, asm_i, asm_total, "assemble")
        fdata  = manifest["frames"][fid]
        status = fdata.get("status", "")

        # Priority: frames_restored/ > frames_swapped/ > original
        used_swapped = not fdata.get("skip") and status in ("approved", "flagged", "swapped")
        from_restored = False
        if used_swapped:
            rp, sp = restore_dir / f"{fid}.png", sd / f"{fid}.png"
            # restored copy must be NEWER than the swapped frame — an older one
            # predates a re-swap/manual fix and would hide it
            if use_restored and rp.exists() and (
                    not sp.exists() or rp.stat().st_mtime >= sp.stat().st_mtime):
                src = rp
                from_restored = True
            else:
                src = sp
            if status == "flagged":
                has_flagged = True
        else:
            src = Path(orig_frames_dir) / f"{fid}.png"

        if not src.exists():
            continue

        img = cv2.imread(str(src))
        if img is None:
            continue

        # Optional face restoration (CodeFormer or GFPGAN) — but NEVER on locked
        # frames (manually corrected, re-restoring would undo it) and never on
        # frames already taken from frames_restored/ (double restore = mushy face).
        if not fdata.get("locked") and not from_restored:
            if restore == "codeformer" and codeformer_sess is not None:
                try:
                    img = _restore_codeformer(img, fidelity=fidelity)
                except Exception as e:
                    print(f"[video/assemble] CodeFormer {fid}: {e}")
            elif restore == "gfpgan" and gfpganer is not None:
                try:
                    _, _, img = gfpganer.enhance(img, has_aligned=False,
                                                  only_center_face=False, paste_back=True)
                except Exception as e:
                    print(f"[video/assemble] GFPGAN {fid}: {e}")

        # Motion blur AFTER restore (restore sharpens, blur re-matches motion)
        # and BEFORE upscale (kernel length computed in native pixels).
        if mb_on and used_swapped and fid in mb_boxes:
            for key, bb in mb_boxes[fid].items():
                # central difference on neighbor frames; one-sided at gaps/ends
                pb = mb_boxes.get(asm_fids[asm_i - 1], {}).get(key) if asm_i > 0 else None
                nb = mb_boxes.get(asm_fids[asm_i + 1], {}).get(key) if asm_i + 1 < asm_total else None
                c0 = ((bb[0] + bb[2]) / 2.0, (bb[1] + bb[3]) / 2.0)
                if pb and nb:
                    vec = (((nb[0] + nb[2]) - (pb[0] + pb[2])) / 4.0,
                           ((nb[1] + nb[3]) - (pb[1] + pb[3])) / 4.0)
                elif pb:
                    vec = (c0[0] - (pb[0] + pb[2]) / 2.0, c0[1] - (pb[1] + pb[3]) / 2.0)
                elif nb:
                    vec = ((nb[0] + nb[2]) / 2.0 - c0[0], (nb[1] + nb[3]) / 2.0 - c0[1])
                else:
                    continue
                try:
                    img = _motion_blur_face(img, bb, vec, strength=mb_strength)
                except Exception as e:
                    print(f"[video/assemble] motion blur {fid}: {e}")

        # Optional upscale (Real-ESRGAN or PIL fallback)
        if upscale in (2, 4):
            if upscaler is not None:
                try:
                    img, _ = upscaler.enhance(img, outscale=upscale)
                except Exception as e:
                    print(f"[video/assemble] ESRGAN {fid}: {e}")
            else:
                pil = Image.fromarray(cv2.cvtColor(img, cv2.COLOR_BGR2RGB))
                w, h = pil.size
                pil = pil.resize((w * upscale, h * upscale), Image.LANCZOS)
                img = cv2.cvtColor(np.array(pil), cv2.COLOR_RGB2BGR)

        cv2.imwrite(str(tmp_dir / f"{fid}.png"), img)
        processed += 1

    if processed == 0:
        shutil.rmtree(str(tmp_dir), ignore_errors=True)
        raise HTTPException(500, "No frames to assemble")

    # ffmpeg assemble
    op = Path(output_path)
    op.parent.mkdir(parents=True, exist_ok=True)
    frame_pattern = str(tmp_dir / "%06d.png")

    if audio_path and os.path.exists(audio_path):
        cmd = ["ffmpeg", "-y", "-framerate", fps, "-i", frame_pattern,
               "-i", audio_path,
               "-c:v", "libx264", "-pix_fmt", "yuv420p",
               "-c:a", "aac", "-b:a", "192k", "-shortest", str(op)]
    else:
        cmd = ["ffmpeg", "-y", "-framerate", fps, "-i", frame_pattern,
               "-c:v", "libx264", "-pix_fmt", "yuv420p", str(op)]

    r = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    shutil.rmtree(str(tmp_dir), ignore_errors=True)

    if r.returncode != 0:
        raise HTTPException(500, f"ffmpeg assembly failed:\n{r.stderr[-600:]}")

    size_mb = round(os.path.getsize(str(op)) / 1024 / 1024, 2)
    return JSONResponse({
        "ok":               True,
        "output_path":      str(op),
        "frames_processed": processed,
        "size_mb":          size_mb,
        "restore":          restore or "none",
        "upscale":          f"{upscale}x" if upscale else "none",
        "motion_blur":      f"strength {mb_strength}" if mb_on else "none",
        "has_flagged_frames": has_flagged,
        "warning": "Some frames were still flagged — consider reviewing before final export" if has_flagged else None,
    })


# ---------------------------------------------------------------------------
# /expression — LivePortrait expression restore (proxied to persistent worker)
#
# The worker (liveportrait_proto/expression_server.py) has its own venv with
# torch and keeps the models loaded (~2.7s/request warm). This server spawns
# it on first use and proxies multipart requests to it.
# ---------------------------------------------------------------------------
EXPRESSION_URL  = "http://127.0.0.1:8002"
LIVEPORTRAIT_DIR = os.path.join(os.path.dirname(__file__), "liveportrait_proto")
_expr_spawn_lock = threading.Lock()


def _expression_worker_up() -> bool:
    import requests
    try:
        r = requests.get(EXPRESSION_URL + "/health", timeout=2)
        return r.status_code == 200 and r.json().get("loaded", False)
    except Exception:
        return False


def _ensure_expression_worker() -> None:
    """Spawn the LivePortrait worker if it is not running. Blocks until ready."""
    if _expression_worker_up():
        return
    py = os.path.join(LIVEPORTRAIT_DIR, ".venv", "bin", "python")
    srv = os.path.join(LIVEPORTRAIT_DIR, "expression_server.py")
    if not (os.path.exists(py) and os.path.exists(srv)):
        raise HTTPException(503,
            f"LivePortrait worker not installed — expected {srv} with its .venv "
            "(see liveportrait_proto/).")
    with _expr_spawn_lock:
        if _expression_worker_up():
            return
        print("[expression] spawning LivePortrait worker (model load ~15s)...", flush=True)
        env = dict(os.environ, PYTORCH_ENABLE_MPS_FALLBACK="1")
        log = open("/tmp/rpgai_expression_worker.log", "ab")
        subprocess.Popen([py, srv, "--port", "8002"], cwd=LIVEPORTRAIT_DIR,
                         stdout=log, stderr=log, start_new_session=True)
        deadline = time.time() + 120
        while time.time() < deadline:
            if _expression_worker_up():
                print("[expression] worker ready", flush=True)
                return
            time.sleep(2)
    raise HTTPException(503,
        "LivePortrait worker did not come up — check /tmp/rpgai_expression_worker.log")


@app.post("/expression")
async def expression_restore(
    source:   UploadFile = File(...),   # swapped image (appearance to keep)
    driving:  UploadFile = File(...),   # original image (expression/gaze to copy)
    region:   str        = Form("exp"), # exp | eyes | lip | pose | all
    strength: float      = Form(1.0),   # 0..1: quanto del movimento applicare
):
    import requests
    _ensure_expression_worker()
    src_bytes = await source.read()
    drv_bytes = await driving.read()
    try:
        r = requests.post(EXPRESSION_URL + "/restore",
            files={"source":  ("source.png",  src_bytes, "application/octet-stream"),
                   "driving": ("driving.png", drv_bytes, "application/octet-stream")},
            data={"region": region, "strength": str(strength)},
            timeout=300)
    except Exception as e:
        raise HTTPException(502, f"expression worker unreachable: {e}")
    if r.status_code != 200:
        detail = r.text[:400]
        try:
            detail = r.json().get("detail", detail)
        except Exception:
            pass
        raise HTTPException(r.status_code, f"expression worker: {detail}")
    media = r.headers.get("content-type", "image/png")
    return Response(content=r.content, media_type=media)


# ---------------------------------------------------------------------------
# /texture — high-frequency skin texture transfer (anti-plastic)
#
# The swapped face and the original are the SAME frame, pixel-aligned: the
# original's high-frequency detail (pores, grain) can be added back onto the
# swapped face with no warping. Frequency separation, masked to skin regions
# (parser labels; eyes/lips excluded by default — their shapes changed).
# ---------------------------------------------------------------------------
def _apply_texture(src: np.ndarray, ref: np.ndarray, amount: float, radius: int,
                   parts: str, mono: bool):
    """High-freq skin texture of ref → src (pixel-aligned). Returns (out, delta)."""
    if ref.shape[:2] != src.shape[:2]:
        ref = cv2.resize(ref, (src.shape[1], src.shape[0]), interpolation=cv2.INTER_LINEAR)
    h, w = src.shape[:2]
    faces = _get_faces_sorted(ref)
    if not faces:
        raise HTTPException(422, "no face detected in reference image")
    labels = set()
    for p in [x.strip() for x in parts.split(",") if x.strip()]:
        if p not in FACE_PARTS:
            raise HTTPException(400, f"unknown part '{p}'. Valid: {list(FACE_PARTS)}")
        labels.update(FACE_PARTS[p])
    x1b, y1b, x2b, y2b = faces[0].bbox
    bw, bh = x2b - x1b, y2b - y1b
    pad = 0.4
    px1 = max(0, int(x1b - bw * pad)); py1 = max(0, int(y1b - bh * pad))
    px2 = min(w, int(x2b + bw * pad)); py2 = min(h, int(y2b + bh * pad))
    parsing = _run_faceparser_labels(ref[py1:py2, px1:px2])
    if parsing is None:
        raise HTTPException(503, "face parser not loaded — needed for the skin mask")
    pc = np.isin(parsing, list(labels)).astype(np.uint8) * 255
    pc = cv2.resize(pc, (px2 - px1, py2 - py1), interpolation=cv2.INTER_NEAREST)
    mask = np.zeros((h, w), dtype=np.uint8)
    mask[py1:py2, px1:px2] = pc
    mask = cv2.erode(mask, cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (5, 5)))
    mask = cv2.GaussianBlur(mask, (11, 11), 0)
    alpha = (mask.astype(np.float32) / 255.0)[:, :, np.newaxis] * amount

    k = radius * 2 + 1
    hi = ref.astype(np.float32) - cv2.GaussianBlur(ref, (k, k), 0).astype(np.float32)
    if mono:
        hi = np.repeat(hi.mean(axis=2, keepdims=True), 3, axis=2)
    out = np.clip(src.astype(np.float32) + hi * alpha, 0, 255).astype(np.uint8)
    m = alpha[:, :, 0] > 0.3
    delta = float(np.abs(out.astype(np.float32) - src.astype(np.float32))[m].mean()) if m.any() else 0.0
    print(f"[texture] amount={amount} radius={radius} mono={mono} "
          f"mask={100*m.mean():.1f}% delta_medio={delta:.2f}/255", flush=True)
    return out, delta


@app.post("/texture")
async def texture_transfer(
    source:    UploadFile = File(...),   # swapped image (gets the texture)
    reference: UploadFile = File(...),   # original image (texture donor)
    amount:    float      = Form(1.0),   # gain: >1 AMPLIFICA la grana del donatore (sorgenti soft ne hanno poca)
    radius:    int        = Form(3),     # raggio blur del passa-alto (px): 1-2=pori, 4+=grana grossa
    parts:     str        = Form("skin,nose"),  # regioni parser dove applicare
    mono:      bool       = Form(True),  # grana in sola luminanza (evita rumore croma a gain alto)
):
    if face_app is None:
        raise HTTPException(503, "face model not loaded")
    src = _decode_image(await source.read())
    ref = _decode_image(await reference.read())
    out, delta = _apply_texture(src, ref, max(0.0, min(3.0, amount)),
                                max(1, min(12, radius)), parts, mono)
    return Response(content=_encode_png(out), media_type="image/png",
                    headers={"X-Texture-Delta": f"{delta:.2f}"})


@app.get("/face/{npc_id}/crop")
def face_crop(npc_id: str):
    """Face crop image for a registered NPC — remote-safe (bytes, not a path)."""
    _check_npc_id(npc_id)
    p = FACES_DIR / f"{npc_id}.jpg"
    if not p.exists():
        raise HTTPException(status_code=404, detail=f"No crop for NPC '{npc_id}'")
    return Response(content=p.read_bytes(), media_type="image/jpeg")


@app.delete("/register/{npc_id}")
def unregister_npc(npc_id: str):
    """Remove a registered NPC (embedding set + crop)."""
    _check_npc_id(npc_id)
    removed = []
    for ext in (".npy", ".jpg", ".ghost.npy", ".simswap.npy"):
        p = FACES_DIR / f"{npc_id}{ext}"
        if p.exists():
            p.unlink()
            removed.append(p.name)
    if not removed:
        raise HTTPException(status_code=404, detail=f"NPC '{npc_id}' not registered")
    print(f"[unregister] NPC '{npc_id}' removed: {removed}")
    return JSONResponse({"ok": True, "npc_id": npc_id, "removed": removed})


@app.get("/registered")
def list_registered():
    """List all registered NPCs with embedding + crop status."""
    result = []
    for npy in sorted(FACES_DIR.glob("*.npy")):
        nid  = npy.stem
        if "." in nid:         # skip per-model sets (<id>.ghost.npy / <id>.simswap.npy)
            continue
        crop = FACES_DIR / f"{nid}.jpg"
        try:
            embs = np.load(str(npy))
            n_embs = 1 if embs.ndim == 1 else int(embs.shape[0])
        except Exception:
            n_embs = 0
        alt_counts = {}
        for alt in ("ghost", "simswap"):
            ap = _alt_emb_path(nid, alt)
            if ap.exists():
                try:
                    a = np.load(str(ap))
                    alt_counts[alt] = 1 if a.ndim == 1 else int(a.shape[0])
                except Exception:
                    alt_counts[alt] = 0
        result.append({
            "npc_id":         nid,
            "embedding":      str(npy),
            "n_embeddings":   n_embs,
            "alt_embeddings": alt_counts,
            "crop":           str(crop) if crop.exists() else None,
            "swap_ready":     n_embs > 0,   # swap now works from embeddings alone
        })
    return JSONResponse({"npcs": result, "count": len(result)})


@app.get("/health")
def health():
    registered = [p.stem for p in sorted(FACES_DIR.glob("*.npy")) if "." not in p.stem]
    swap_ready = registered   # embedding alone is enough for /swap since multi-emb refactor
    return {
        "status":      "ok",
        "inswapper":   os.path.exists(INSWAPPER_PATH),
        "gfpgan":      gfpganer is not None,
        "codeformer":  codeformer_sess is not None,
        "realesrgan":  upscaler is not None,
        "faceparser":  faceparser_sess is not None,   # primary (onnxruntime, no torch)
        "bisenet":     face_parser is not None,        # fallback (torch + facexlib)
        "occluder":    occluder_sess is not None,
        "xseg":        xseg_sess is not None,
        "clipseg":     clipseg_model is not None,
        "expression":  _expression_worker_up(),   # LivePortrait worker (spawned on first /expression)
        "swappers": {
            "inswapper": os.path.exists(INSWAPPER_PATH),
            "ghost":     os.path.exists(GHOST_UNET_PATH) and os.path.exists(GHOST_ARC_PATH),
            "simswap":   os.path.exists(SIMSWAP_PATH) and os.path.exists(SIMSWAP_ARC_PATH),
        },
        "registered":  registered,    # all NPCs with embedding (.npy)
        "swap_ready":  swap_ready,    # subset usable in /swap with npc_ids (.jpg crop present)
        "parts":       list(FACE_PARTS.keys()),
    }


@app.get("/")
def root():
    return health()


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------
if __name__ == "__main__":
    import argparse, uvicorn
    p = argparse.ArgumentParser()
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=8001)
    args, _ = p.parse_known_args()
    uvicorn.run(app, host=args.host, port=args.port)
