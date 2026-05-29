"""
RpgAi Local Text-to-Image Server  (FLUX.1-dev + IP-Adapter)

NPC portrait generation pipeline:
  /generate_portrait : portrait with optional IP-Adapter face/style conditioning.
                       Provide character_id + a reference face in ./references/
                       to inject that identity. Pass save_as_reference=true to
                       store the output as the canonical reference.
  /generate_scene    : scene with N consistent NPCs.

Hardware notes (RTX 5090, Blackwell SM_120, 32 GB GDDR7):
  Recommended launch:
    python server.py --dtype bf16 --host 0.0.0.0 --ip-adapter --fast

  FLUX.1-dev fits in ~22 GB BF16 (full precision, no quantization needed on 32 GB).
  If you see OOM: add --cpu-offload or --quantize torchao_int8.

IP-Adapter notes (InstantX/FLUX.1-dev-IP-Adapter):
  Requires:
    models/ip_adapter/ip-adapter.bin          (from InstantX repo)
    models/ip_adapter/image_encoder/          (SigLIP: google/siglip-so400m-patch14-384)
    models/ip_adapter/attention_processor.py  (IPAFluxAttnProcessor2_0)
    models/ip_adapter/pipeline_flux_ipa.py    (FluxPipeline with image_emb kwarg)
    models/ip_adapter/transformer_flux.py     (FluxTransformer2DModel)

  Download:
    hf download InstantX/FLUX.1-dev-IP-Adapter --local-dir models/ip_adapter
    hf download google/siglip-so400m-patch14-384 --local-dir models/ip_adapter/image_encoder

  IMPORTANT: model must be in diffusers format (not single .safetensors).
  The IP-Adapter stack loads transformer via subfolder="transformer".

  Uses torch.no_grad() (NOT inference_mode()): LoRA may call requires_grad_() between requests.

LoRA notes:
  Place .safetensors or subdirectory in ./loras/
  Loaded once on first request, toggled via set_adapters — never fused.

Endpoints:
  POST /generate_portrait  — single-NPC portrait (JSON body)
  POST /generate_scene     — multi-NPC scene     (JSON body)
  POST /references/add     — upload reference face (multipart)
  GET  /references         — list stored reference faces
  GET  /loras              — list available LoRAs
  GET  /health             — liveness + config
"""

import torch

import argparse, inspect, io, os, re, sys
from contextlib import asynccontextmanager
from datetime import datetime, timezone
from typing import List, Optional

import numpy as np
from fastapi import FastAPI, File, Form, HTTPException, UploadFile
from fastapi.responses import JSONResponse, Response
from diffusers import AutoPipelineForText2Image
from PIL import Image
from pydantic import BaseModel


# ════════════════════════════════════════════════════════════════════════════
# CLI
# ════════════════════════════════════════════════════════════════════════════
def _parse_args():
    p = argparse.ArgumentParser(description="RpgAi T2I Server (FLUX.1-dev + IP-Adapter)")
    p.add_argument("--model",          default=None,
                   help="HF repo or local path (default: black-forest-labs/FLUX.1-dev)")
    p.add_argument("--dtype",          default=None, choices=["fp16", "bf16", "fp32"],
                   help="Force dtype. Default: bf16 on CUDA, fp16 on MPS, fp32 on CPU")
    p.add_argument("--quantize",       default=None,
                   choices=["torchao_int8", "torchao_float8", "4bit", "8bit"],
                   help="torchao_int8/float8 = TorchAO (Blackwell-safe). "
                        "4bit/8bit = bitsandbytes (NOT for SM_120 / RTX 5090). "
                        "NOTE: ignored when --ip-adapter is set (incompatible with custom transformer).")
    p.add_argument("--cpu-offload",    action="store_true",
                   help="Sequential CPU offload (reduces peak VRAM at cost of speed)")
    p.add_argument("--fast",           action="store_true",
                   help="Flash SDP + VAE tiling/slicing + torch.compile on transformer")
    p.add_argument("--ip-adapter",     action="store_true",
                   help="Enable IP-Adapter face conditioning (InstantX/FLUX.1-dev-IP-Adapter)")
    p.add_argument("--ip-adapter-dir", default=None,
                   help="IP-Adapter directory with ip-adapter.bin + image_encoder/ "
                        "(default: ./models/ip_adapter)")
    p.add_argument("--ip-tokens",      type=int, default=128,
                   help="IP-Adapter num_tokens (default: 128, matches InstantX training)")
    p.add_argument("--host",           default="127.0.0.1",
                   help="Bind host (default: 127.0.0.1, use 0.0.0.0 for LAN)")
    p.add_argument("--port",           type=int, default=8003)
    p.add_argument("--model-dir",      default=None,
                   help="Local model directory (default: ./models/flux1-dev)")
    p.add_argument("--references-dir", default=None,
                   help="NPC reference faces directory (default: ./references)")
    p.add_argument("--lora-dir",       default=None,
                   help="LoRA directory (default: ./loras)")
    p.add_argument("--steps",          type=int, default=20,
                   help="Default inference steps (default: 20 for FLUX.1-dev)")
    p.add_argument("--guidance",       type=float, default=3.5,
                   help="Default guidance scale (default: 3.5 for FLUX.1-dev)")
    p.add_argument("--faceswap",       action="store_true",
                   help="Enable face-swap post-processing via InsightFace inswapper "
                        "(runs after FLUX generation, uses same reference images as IP-Adapter)")
    p.add_argument("--inswapper-model", default=None,
                   help="Path to inswapper_128.onnx "
                        "(default: ../faceswap_locale/models/inswapper_128.onnx)")
    p.add_argument("--gfpgan-model",   default=None,
                   help="Path to GFPGANv1.4.pth for optional face enhancement "
                        "(default: ../faceswap_locale/models/GFPGANv1.4.pth)")
    return p.parse_args()

ARGS = _parse_args()


# ════════════════════════════════════════════════════════════════════════════
# Config
# ════════════════════════════════════════════════════════════════════════════
MODEL_REPO = ARGS.model or os.environ.get("MODEL_REPO", "black-forest-labs/FLUX.1-dev")
HF_TOKEN   = os.environ.get("HF_TOKEN", None)

_base = os.path.dirname(os.path.abspath(__file__))

def _resolve(path: Optional[str], default: str) -> str:
    if not path:
        return default
    return path if os.path.isabs(path) else os.path.abspath(os.path.join(_base, path))

def _derive_model_dir() -> str:
    """
    Resolve MODEL_DIR with this priority:
      1. --model-dir / MODEL_DIR env           (explicit override)
      2. --model if it's an existing local path (e.g. /data/my-flux or ./models/custom)
      3. --model repo slug → ./models/<slug>   (e.g. "org/FLUX.1-dev" → models/FLUX.1-dev)
      4. default: ./models/flux1-dev
    """
    explicit = ARGS.model_dir or os.environ.get("MODEL_DIR")
    if explicit:
        return _resolve(explicit, "")

    model_arg = ARGS.model or os.environ.get("MODEL_REPO", "")
    if model_arg:
        # Local absolute or relative path that exists → use directly
        candidate = model_arg if os.path.isabs(model_arg) else os.path.join(_base, model_arg)
        if os.path.exists(candidate):
            return os.path.abspath(candidate)
        # HF repo id (org/name) → ./models/<name>
        slug = model_arg.split("/")[-1]
        return os.path.join(_base, "models", slug)

    return os.path.join(_base, "models", "flux1-dev")

MODEL_DIR      = _derive_model_dir()
REF_DIR        = _resolve(ARGS.references_dir  or os.environ.get("REF_DIR"),
                          os.path.join(_base, "references"))
LORA_DIR       = _resolve(ARGS.lora_dir        or os.environ.get("LORA_DIR"),
                          os.path.join(_base, "loras"))
IP_ADAPTER_DIR = _resolve(ARGS.ip_adapter_dir  or os.environ.get("IP_ADAPTER_DIR"),
                          os.path.join(_base, "models", "ip_adapter"))
OUTPUT_DIR     = os.path.join(_base, "output")

USE_IP_ADAPTER = ARGS.ip_adapter or (os.environ.get("USE_IP_ADAPTER", "0") == "1")
USE_OFFLOAD    = ARGS.cpu_offload or (os.environ.get("CPU_OFFLOAD", "0") == "1")
USE_FACESWAP   = ARGS.faceswap or (os.environ.get("USE_FACESWAP", "0") == "1")
QUANTIZE       = ARGS.quantize or os.environ.get("QUANTIZE", None)
IS_TORCHAO     = QUANTIZE in ("torchao_int8", "torchao_float8")
IS_BNB         = QUANTIZE in ("4bit", "8bit")
IS_QUANTIZED   = IS_TORCHAO or IS_BNB

INSWAPPER_PATH = _resolve(
    ARGS.inswapper_model or os.environ.get("INSWAPPER_PATH"),
    os.path.join(_base, "models", "inswapper_128.onnx"),
)
GFPGAN_PATH = _resolve(
    ARGS.gfpgan_model or os.environ.get("GFPGAN_PATH"),
    os.path.join(_base, "models", "GFPGANv1.4.pth"),
)

quantization_active = False


# ════════════════════════════════════════════════════════════════════════════
# Device / dtype
# ════════════════════════════════════════════════════════════════════════════
def _best_device() -> str:
    if (ov := os.environ.get("DEVICE")):
        return ov
    if torch.cuda.is_available():
        return "cuda"
    if hasattr(torch.backends, "mps") and torch.backends.mps.is_available():
        return "mps"
    return "cpu"

def _best_dtype():
    if ARGS.dtype == "fp16": return torch.float16
    if ARGS.dtype == "bf16": return torch.bfloat16
    if ARGS.dtype == "fp32": return torch.float32
    dev = _best_device()
    if dev == "cuda":  return torch.bfloat16
    if dev == "mps":   return torch.float16
    return torch.float32

DEVICE = _best_device()
DTYPE  = _best_dtype()

if DEVICE == "cuda":
    torch.set_float32_matmul_precision('high')
    torch.backends.cuda.matmul.allow_tf32 = True


# ════════════════════════════════════════════════════════════════════════════
# Banner
# ════════════════════════════════════════════════════════════════════════════
def _banner():
    model_short = os.path.basename(MODEL_DIR)
    w = 52
    def row(label, val):
        line = f"  {label:<15}{str(val)}"
        print(f"║{line:<{w}}║")
    print("╔" + "═" * w + "╗")
    print(f"║{'  RpgAi Local T2I Server (FLUX.1-dev + IP-Adapter)':<{w}}║")
    print("╠" + "═" * w + "╣")
    row("Modello:",    model_short)
    row("Device:",     DEVICE)
    row("Dtype:",      str(DTYPE))
    if QUANTIZE and not USE_IP_ADAPTER:
        ql = QUANTIZE + (" [TorchAO]" if IS_TORCHAO else " [bitsandbytes]")
        row("Quant:",  ql)
    elif QUANTIZE and USE_IP_ADAPTER:
        row("Quant:",  f"{QUANTIZE} [IGNORATO: --ip-adapter]")
    row("Offload:",    "SI" if USE_OFFLOAD else "NO")
    row("IP-Adapter:", "SI" if USE_IP_ADAPTER else "NO")
    row("IP-Tokens:",  ARGS.ip_tokens if USE_IP_ADAPTER else "-")
    row("FaceSwap:",   "SI" if USE_FACESWAP else "NO")
    row("Fast:",       "SI" if ARGS.fast else "NO")
    row("Steps:",      ARGS.steps)
    row("Guidance:",   ARGS.guidance)
    print("╚" + "═" * w + "╝")

_banner()


# ════════════════════════════════════════════════════════════════════════════
# Global state
# ════════════════════════════════════════════════════════════════════════════
pipe = None
base_pipe = None
loaded_loras: dict[str, bool] = {}
detected_arch = "unknown"
ip_adapter_loaded = False

# IP-Adapter components (set in _load_model when --ip-adapter)
_ipa_image_encoder    = None   # SiglipVisionModel
_ipa_processor        = None   # AutoProcessor
_ipa_proj_model       = None   # MLPProjModel
_IPAFluxAttnProc      = None   # IPAFluxAttnProcessor2_0 class

# Face-swap components (set in lifespan when --faceswap)
_fs_face_app          = None   # InsightFace FaceAnalysis (buffalo_l)
_fs_swapper           = None   # inswapper_128.onnx ONNX model
_fs_gfpganer          = None   # GFPGANer (optional)
faceswap_loaded       = False
_fs_best_ref_cache: dict = {}  # char_id → (dir_mtime, best_path)


# ════════════════════════════════════════════════════════════════════════════
# IP-Adapter MLP projection model (inlined from InstantX)
# ════════════════════════════════════════════════════════════════════════════
import torch.nn as nn

class _MLPProjModel(nn.Module):
    """Projects SigLIP pooler_output → FLUX cross-attention tokens."""
    def __init__(self, cross_attention_dim=4096, id_embeddings_dim=1152, num_tokens=128):
        super().__init__()
        self.num_tokens = num_tokens
        self.cross_attention_dim = cross_attention_dim
        self.proj = nn.Sequential(
            nn.Linear(id_embeddings_dim, id_embeddings_dim * 2),
            nn.GELU(),
            nn.Linear(id_embeddings_dim * 2, cross_attention_dim * num_tokens),
        )
        self.norm = nn.LayerNorm(cross_attention_dim)

    def forward(self, id_embeds):
        x = self.proj(id_embeds)
        x = x.reshape(-1, self.num_tokens, self.cross_attention_dim)
        return self.norm(x)


# ════════════════════════════════════════════════════════════════════════════
# Model loading
# ════════════════════════════════════════════════════════════════════════════
def _detect_model_source() -> tuple[str, str]:
    if MODEL_DIR.endswith(".safetensors") and os.path.isfile(MODEL_DIR):
        return "single_file", MODEL_DIR
    if os.path.isdir(MODEL_DIR):
        if os.path.exists(os.path.join(MODEL_DIR, "model_index.json")):
            return "pretrained", MODEL_DIR
        sf = [f for f in os.listdir(MODEL_DIR) if f.endswith(".safetensors")]
        if sf:
            chosen = next((f for f in sf if "bf16" in f.lower()), sf[0])
            if len(sf) > 1:
                print(f"[model] Multiple .safetensors found — using: {chosen}")
            return "single_file", os.path.join(MODEL_DIR, chosen)
    return "pretrained", MODEL_DIR


def _detect_sf_arch(sf_path: str) -> str:
    try:
        from safetensors import safe_open
        with safe_open(sf_path, framework="pt", device="cpu") as f:
            keys = list(f.keys())[:120]
    except Exception as e:
        print(f"[model] Architecture detection failed: {e}")
        return "unknown"
    if any("double_stream_blocks" in k or "single_stream_blocks" in k for k in keys):
        return "flux"
    if (any(k.startswith("transformer.") for k in keys)
            and not any("model.diffusion_model" in k for k in keys)):
        return "flux"
    if any("conditioner.embedders" in k for k in keys):
        return "sdxl"
    if any("cond_stage_model" in k or "model.diffusion_model" in k for k in keys):
        return "sd15"
    return "unknown"


def _download_if_missing(mode: str):
    if mode == "single_file":
        return
    if os.path.exists(os.path.join(MODEL_DIR, "model_index.json")):
        return
    print(f"[model] Not found in {MODEL_DIR}, downloading from {MODEL_REPO} ...")
    try:
        from huggingface_hub import snapshot_download
    except ImportError:
        raise RuntimeError("huggingface_hub not installed.\n  pip install huggingface_hub")
    snapshot_download(
        repo_id=MODEL_REPO, local_dir=MODEL_DIR, token=HF_TOKEN,
        ignore_patterns=["*.msgpack", "flax_model*", "tf_model*", "rust_model*"],
    )
    print("[model] Download complete.")


def _build_load_kwargs() -> dict:
    global quantization_active
    kwargs = dict(use_safetensors=True, local_files_only=True, low_cpu_mem_usage=True)
    if IS_QUANTIZED:
        kwargs["torch_dtype"] = DTYPE
        try:
            from diffusers import PipelineQuantizationConfig
            if IS_TORCHAO:
                qt = "float8_weight_only" if QUANTIZE == "torchao_float8" else "int8_weight_only"
                qcfg = PipelineQuantizationConfig(
                    quant_backend="torchao",
                    quant_kwargs={"quant_type": qt},
                    components_to_quantize=["transformer"],
                )
                print(f"[model] TorchAO: {qt}")
            else:
                backend = f"bitsandbytes_{QUANTIZE}"
                qkw = ({"bnb_4bit_compute_dtype": DTYPE, "bnb_4bit_quant_type": "nf4"}
                       if QUANTIZE == "4bit" else None)
                qcfg = PipelineQuantizationConfig(
                    quant_backend=backend, quant_kwargs=qkw,
                    components_to_quantize=["transformer"],
                )
                print(f"[model] bitsandbytes: {backend}")
            kwargs["quantization_config"] = qcfg
            quantization_active = True
        except Exception as e:
            print(f"[model] Quantization unavailable: {e} — loading full precision")
            kwargs.pop("quantization_config", None)
    else:
        kwargs["torch_dtype"] = DTYPE
    return kwargs


def _apply_optimizations(target: object):
    if not ARGS.fast:
        return
    try:
        target.enable_xformers_memory_efficient_attention()
        print("[model] xformers: ON")
    except Exception:
        try:
            torch.backends.cuda.enable_flash_sdp(True)
            print("[model] flash SDP: ON")
        except Exception as e:
            print(f"[model] flash attention unavailable: {e}")
    try:
        if hasattr(target, "vae"):
            target.vae.enable_tiling()
            target.vae.enable_slicing()
            print("[model] VAE tiling + slicing: ON")
    except Exception as e:
        print(f"[model] VAE optimizations: {e}")
    if quantization_active:
        print("[model] torch.compile: skipped (quantization active)")
        return
    if USE_OFFLOAD:
        print("[model] torch.compile: skipped (cpu-offload active)")
        return
    try:
        if hasattr(target, "transformer") and target.transformer is not None:
            target.transformer = torch.compile(target.transformer, mode="max-autotune")
            print("[model] torch.compile (transformer): ON")
        elif hasattr(target, "unet") and target.unet is not None:
            target.unet = torch.compile(target.unet, mode="max-autotune")
            print("[model] torch.compile (unet): ON")
    except Exception as e:
        print(f"[model] torch.compile: {e}")


def _load_ipa_pipeline(model_path: str):
    """
    Load FLUX pipeline using InstantX's modified stack:
      transformer_flux.py   — FluxTransformer2DModel with IP attention processor support
      pipeline_flux_ipa.py  — FluxPipeline with image_emb kwarg in __call__

    Both files live in IP_ADAPTER_DIR (downloaded from InstantX repo).
    Quantization is NOT applied here — incompatible with loading transformer separately.
    """
    global pipe, base_pipe, ip_adapter_loaded
    global _ipa_image_encoder, _ipa_processor, _ipa_proj_model, _IPAFluxAttnProc

    # Add ip_adapter dir to path so their imports work (attention_processor etc.)
    if IP_ADAPTER_DIR not in sys.path:
        sys.path.insert(0, IP_ADAPTER_DIR)

    required = ["transformer_flux.py", "pipeline_flux_ipa.py", "attention_processor.py", "ip-adapter.bin"]
    missing = [f for f in required if not os.path.exists(os.path.join(IP_ADAPTER_DIR, f))]
    if missing:
        raise RuntimeError(
            f"[ip-adapter] Missing files in {IP_ADAPTER_DIR}: {missing}\n"
            f"  Download: hf download InstantX/FLUX.1-dev-IP-Adapter --local-dir {IP_ADAPTER_DIR}"
        )

    image_encoder_dir = os.path.join(IP_ADAPTER_DIR, "image_encoder")
    if not os.path.isdir(image_encoder_dir):
        raise RuntimeError(
            f"[ip-adapter] image_encoder/ not found in {IP_ADAPTER_DIR}\n"
            f"  Download: hf download google/siglip-so400m-patch14-384 "
            f"--local-dir {image_encoder_dir}"
        )

    print("[ip-adapter] Loading FluxTransformer2DModel (transformer_flux.py) ...")
    from transformer_flux import FluxTransformer2DModel as _FluxT
    from pipeline_flux_ipa import FluxPipeline as _FluxP
    from attention_processor import IPAFluxAttnProcessor2_0
    _IPAFluxAttnProc = IPAFluxAttnProcessor2_0

    transformer = _FluxT.from_pretrained(
        model_path,
        subfolder="transformer",
        torch_dtype=DTYPE,
        local_files_only=True,
        low_cpu_mem_usage=True,
    )

    print("[ip-adapter] Loading FluxPipeline (pipeline_flux_ipa.py) ...")
    loaded = _FluxP.from_pretrained(
        model_path,
        transformer=transformer,
        torch_dtype=DTYPE,
        local_files_only=True,
        low_cpu_mem_usage=True,
    )

    # Device placement
    if USE_OFFLOAD:
        loaded.enable_sequential_cpu_offload()
        print("[model] Sequential CPU offload: ON")
    else:
        loaded.to(DEVICE)

    _apply_optimizations(loaded)
    pipe = loaded
    base_pipe = loaded

    # ── Install IPAFluxAttnProcessor2_0 on all transformer blocks ────────────
    # Mirrors IPAdapter.set_ip_adapter() — all transformer_blocks + single_transformer_blocks
    num_tokens = ARGS.ip_tokens
    transformer_obj = loaded.transformer
    ip_attn_procs = {}
    for name in transformer_obj.attn_processors.keys():
        if name.startswith("transformer_blocks.") or name.startswith("single_transformer_blocks"):
            ip_attn_procs[name] = IPAFluxAttnProcessor2_0(
                hidden_size=transformer_obj.config.num_attention_heads * transformer_obj.config.attention_head_dim,
                cross_attention_dim=transformer_obj.config.joint_attention_dim,
                num_tokens=num_tokens,
            ).to(DEVICE, dtype=torch.bfloat16)
        else:
            ip_attn_procs[name] = transformer_obj.attn_processors[name]
    transformer_obj.set_attn_processor(ip_attn_procs)
    n_ip = sum(1 for n in ip_attn_procs if "transformer_blocks" in n)
    print(f"[ip-adapter] {n_ip} IPAFluxAttnProcessor2_0 installed (num_tokens={num_tokens})")

    # ── Load SigLIP image encoder ─────────────────────────────────────────────
    from transformers import SiglipVisionModel, AutoProcessor
    print(f"[ip-adapter] Loading SigLIP from {image_encoder_dir} ...")
    _ipa_image_encoder = SiglipVisionModel.from_pretrained(
        image_encoder_dir, local_files_only=True
    ).to(DEVICE, dtype=torch.bfloat16)
    _ipa_processor = AutoProcessor.from_pretrained(image_encoder_dir, local_files_only=True)

    # ── MLPProjModel: 1152→4096×128 ──────────────────────────────────────────
    _ipa_proj_model = _MLPProjModel(
        cross_attention_dim=transformer_obj.config.joint_attention_dim,
        id_embeddings_dim=1152,
        num_tokens=num_tokens,
    ).to(DEVICE, dtype=torch.bfloat16)

    # ── Load ip-adapter.bin (pickle format → weights_only=False required) ─────
    ip_ckpt = os.path.join(IP_ADAPTER_DIR, "ip-adapter.bin")
    print(f"[ip-adapter] Loading weights from {os.path.basename(ip_ckpt)} ...")
    state_dict = torch.load(ip_ckpt, map_location="cpu", weights_only=False)
    _ipa_proj_model.load_state_dict(state_dict["image_proj"], strict=True)
    ip_layers = torch.nn.ModuleList(transformer_obj.attn_processors.values())
    ip_layers.load_state_dict(state_dict["ip_adapter"], strict=False)
    print("[ip-adapter] Weights loaded.")

    # ── Verify proj weights are non-trivial ───────────────────────────────────
    _w = next(_ipa_proj_model.parameters()).abs().sum().item()
    print(f"[ip-adapter] proj_model weight norm sum: {_w:.2f}"
          + (" — WARNING: looks unloaded" if _w < 1.0 else " — OK"))

    ip_adapter_loaded = True
    print("[ip-adapter] ACTIVE")


def _load_standard_pipeline(model_path: str, mode: str):
    """Load plain FluxPipeline via AutoPipelineForText2Image (no IP-Adapter)."""
    global pipe, base_pipe, DTYPE
    kwargs = _build_load_kwargs()

    if mode == "single_file":
        global detected_arch
        detected_arch = _detect_sf_arch(model_path)
        print(f"[model] Detected architecture: {detected_arch}")
        if detected_arch in ("flux", "unknown"):
            from diffusers import FluxPipeline as _PClass
        elif detected_arch == "sdxl":
            from diffusers import StableDiffusionXLPipeline as _PClass
        else:
            from diffusers import StableDiffusionPipeline as _PClass

        sf_kwargs = {k: v for k, v in kwargs.items()
                     if k not in ("local_files_only", "use_safetensors")}

        def _try_single_file(extra_kw: dict = {}):
            try:
                return _PClass.from_single_file(model_path, **sf_kwargs, **extra_kw)
            except Exception as e:
                err = str(e)
                # Transformer-only checkpoint: CLIP/T5 not embedded in the file.
                # Load text encoders from base FLUX.1-dev diffusers dir and retry.
                if ("CLIPTextModel" in err or "T5EncoderModel" in err
                        or "AutoencoderKL" in err) and not extra_kw:
                    return _single_file_with_base_components(sf_kwargs)
                if sf_kwargs.get("torch_dtype") == torch.float16:
                    sf_kwargs["torch_dtype"] = torch.bfloat16
                    global DTYPE; DTYPE = torch.bfloat16
                    return _PClass.from_single_file(model_path, **sf_kwargs, **extra_kw)
                if "torch_dtype" in sf_kwargs and not extra_kw:
                    sf_kwargs.pop("torch_dtype")
                    return _PClass.from_single_file(model_path, **sf_kwargs)
                raise

        def _single_file_with_base_components(kw: dict):
            """
            Transformer-only .safetensors: load ALL missing components
            (CLIP, T5, VAE) from base FLUX.1-dev diffusers dir and inject
            into from_single_file().

            Base dir priority:
              1. BASE_MODEL_DIR env var
              2. models/flux1-dev  (default base install)
            """
            base_dir = os.environ.get(
                "BASE_MODEL_DIR",
                os.path.join(_base, "models", "flux1-dev"),
            )
            if not os.path.isdir(base_dir):
                raise RuntimeError(
                    f"[model] Transformer-only checkpoint — base FLUX.1-dev dir not found.\n"
                    f"  Expected at: {base_dir}\n"
                    f"  Download: hf download black-forest-labs/FLUX.1-dev --local-dir {base_dir}\n"
                    f"  Or: BASE_MODEL_DIR=/path/to/flux1-dev python server.py ..."
                )
            print(f"[model] Transformer-only checkpoint — loading components from {base_dir}")
            from transformers import CLIPTextModel, T5EncoderModel
            from diffusers import AutoencoderKL
            enc_kw = dict(torch_dtype=kw.get("torch_dtype", DTYPE),
                          local_files_only=True, low_cpu_mem_usage=True)
            text_encoder   = CLIPTextModel.from_pretrained(
                base_dir, subfolder="text_encoder",   **enc_kw)
            print("[model]   CLIP text encoder loaded")
            text_encoder_2 = T5EncoderModel.from_pretrained(
                base_dir, subfolder="text_encoder_2", **enc_kw)
            print("[model]   T5 text encoder loaded")
            vae = AutoencoderKL.from_pretrained(
                base_dir, subfolder="vae",            **enc_kw)
            print("[model]   VAE loaded")
            print("[model] Retrying from_single_file with pre-loaded components ...")
            return _PClass.from_single_file(
                model_path,
                text_encoder=text_encoder,
                text_encoder_2=text_encoder_2,
                vae=vae,
                **kw,
            )

        loaded = _try_single_file()
    else:
        try:
            loaded = AutoPipelineForText2Image.from_pretrained(model_path, **kwargs)
        except Exception as e:
            print(f"[model] Load failed ({e})")
            if kwargs.get("torch_dtype") == torch.float16:
                print("[model] Retrying with bfloat16 ...")
                kwargs["torch_dtype"] = torch.bfloat16
                DTYPE = torch.bfloat16
                loaded = AutoPipelineForText2Image.from_pretrained(model_path, **kwargs)
            elif "torch_dtype" in kwargs:
                print("[model] Retrying without dtype override ...")
                kwargs.pop("torch_dtype")
                loaded = AutoPipelineForText2Image.from_pretrained(model_path, **kwargs)
            else:
                raise

    if IS_QUANTIZED:
        pass
    elif USE_OFFLOAD:
        loaded.enable_sequential_cpu_offload()
        print("[model] Sequential CPU offload: ON")
    else:
        loaded.to(DEVICE)

    _apply_optimizations(loaded)
    pipe = loaded
    base_pipe = loaded


def _load_model():
    mode, model_path = _detect_model_source()
    _download_if_missing(mode)

    print(f"[model] Format: {'single_file (.safetensors)' if mode == 'single_file' else 'diffusers directory'}")
    print(f"[model] Path: {model_path}")

    if USE_IP_ADAPTER:
        if mode == "single_file":
            print("[ip-adapter] WARNING: single_file (.safetensors) format incompatible with IP-Adapter.")
            print("[ip-adapter] IP-Adapter requires diffusers directory with transformer/ subfolder.")
            print("[ip-adapter] Falling back to plain pipeline — no face conditioning.")
            _load_standard_pipeline(model_path, mode)
        else:
            try:
                _load_ipa_pipeline(model_path)
            except Exception as e:
                print(f"[ip-adapter] Setup failed: {e}")
                import traceback; traceback.print_exc()
                print("[ip-adapter] Falling back to plain pipeline — no face conditioning.")
                _load_standard_pipeline(model_path, mode)
    else:
        _load_standard_pipeline(model_path, mode)

    os.environ["TRANSFORMERS_OFFLINE"] = "1"
    os.environ["HF_HUB_OFFLINE"] = "1"
    print("[model] Ready.")


# ════════════════════════════════════════════════════════════════════════════
# LoRA management
# ════════════════════════════════════════════════════════════════════════════
def _load_lora(name: str):
    if name in loaded_loras:
        return
    path = os.path.join(LORA_DIR, name)
    if not os.path.exists(path):
        print(f"[lora] WARNING: '{name}' not found in {LORA_DIR}")
        return
    if os.path.isdir(path):
        weight_file = next(
            (os.path.join(path, f) for f in sorted(os.listdir(path))
             if f.endswith((".safetensors", ".bin"))),
            None,
        )
        if not weight_file:
            print(f"[lora] WARNING: no .safetensors/.bin in {path}")
            return
    else:
        weight_file = path

    print(f"[lora] Loading {os.path.basename(weight_file)}")
    if weight_file.endswith(".safetensors"):
        from safetensors.torch import load_file
        state_dict = load_file(weight_file)
    else:
        state_dict = torch.load(weight_file, map_location="cpu", weights_only=True)

    base_pipe.load_lora_weights(state_dict, adapter_name=name)
    loaded_loras[name] = True
    print(f"[lora] '{name}' loaded.")


def _apply_loras(lora_name: Optional[str], lora_scale: float):
    if not loaded_loras:
        return
    if lora_name and lora_name in loaded_loras:
        adapters = list(loaded_loras.keys())
        weights  = [lora_scale if n == lora_name else 1.0 for n in adapters]
        base_pipe.set_adapters(adapters, adapter_weights=weights)
    else:
        base_pipe.disable_lora()


# ════════════════════════════════════════════════════════════════════════════
# Reference face management
# ════════════════════════════════════════════════════════════════════════════
def _ref_pt_path(character_id: str) -> Optional[str]:
    """Path to pre-computed embedding file, if it exists."""
    p = os.path.join(REF_DIR, f"{character_id}.pt")
    return p if os.path.exists(p) else None

def _ref_image_path(character_id: str) -> Optional[str]:
    for ext in ("png", "jpg", "jpeg", "webp"):
        p = os.path.join(REF_DIR, f"{character_id}.{ext}")
        if os.path.exists(p):
            return p
    return None

def _ref_set_dir(character_id: str) -> Optional[str]:
    """Path to multi-image set folder, if it exists and has images."""
    d = os.path.join(REF_DIR, character_id)
    if os.path.isdir(d) and any(
        f.lower().endswith((".png", ".jpg", ".jpeg", ".webp"))
        for f in os.listdir(d)
    ):
        return d
    return None

def _load_ref_embedding(character_id: str) -> Optional[torch.Tensor]:
    """Load pre-computed image_prompt_embeds from .pt file → [1, num_tokens, 4096]."""
    p = _ref_pt_path(character_id)
    if p is None:
        return None
    data = torch.load(p, map_location="cpu", weights_only=False)
    return data["image_prompt_embeds"].to(DEVICE, dtype=torch.bfloat16)

def _load_ref_image(character_id: str) -> Optional[Image.Image]:
    p = _ref_image_path(character_id)
    return Image.open(p).convert("RGB") if p else None


# ════════════════════════════════════════════════════════════════════════════
# Reference image quality check
# ════════════════════════════════════════════════════════════════════════════
_iface_app_cache = None   # InsightFace FaceAnalysis, lazily initialized

def _get_iface_app():
    """Lazily init InsightFace FaceAnalysis for face detection. Returns None if unavailable."""
    global _iface_app_cache
    if _iface_app_cache is not None:
        return _iface_app_cache
    try:
        import cv2  # noqa
        from insightface.app import FaceAnalysis
        os.environ.setdefault("INSIGHTFACE_HOME", _base)
        app = FaceAnalysis(name="antelopev2", providers=["CPUExecutionProvider"])
        app.prepare(ctx_id=-1, det_size=(320, 320))
        _iface_app_cache = app
        return app
    except Exception:
        return None


def _check_image_quality(pil_image: Image.Image) -> dict:
    """
    Evaluate a reference image for IP-Adapter conditioning quality.

    Checks (all optional/graceful):
      - size        : minimum 128×128
      - sharpness   : Laplacian variance via PIL edge filter; low = blurry
      - siglip_norm : SigLIP pooler_output norm; very low = featureless image
      - face        : InsightFace detection count + face area ratio (optional)

    Returns dict with keys: size, sharpness, siglip_norm, face_count,
    face_area_ratio, warnings (list[str]), quality_ok (bool).
    """
    from PIL import ImageFilter

    metrics: dict = {"warnings": [], "quality_ok": True}
    w, h = pil_image.size
    metrics["size"] = [w, h]

    # ── Size ──────────────────────────────────────────────────────────────────
    if w < 128 or h < 128:
        metrics["warnings"].append(f"image too small ({w}×{h}) — recommend ≥256×256")
        metrics["quality_ok"] = False

    # ── Sharpness (Laplacian energy via edge filter) ───────────────────────────
    try:
        gray  = pil_image.convert("L")
        edges = np.array(gray.filter(ImageFilter.FIND_EDGES), dtype=np.float32)
        sharpness = float(edges.var())
        metrics["sharpness"] = round(sharpness, 1)
        if sharpness < 20.0:
            metrics["warnings"].append(
                f"possibly blurry (sharpness={sharpness:.1f}, recommend >20)"
            )
    except Exception:
        metrics["sharpness"] = None

    # ── SigLIP norm ────────────────────────────────────────────────────────────
    if ip_adapter_loaded and _ipa_image_encoder is not None and _ipa_processor is not None:
        try:
            with torch.no_grad():
                pv   = _ipa_processor(images=[pil_image], return_tensors="pt").pixel_values
                pv   = pv.to(DEVICE, dtype=torch.bfloat16)
                norm = float(_ipa_image_encoder(pv).pooler_output.norm())
            metrics["siglip_norm"] = round(norm, 3)
            if norm < 2.0:
                metrics["warnings"].append(
                    f"low SigLIP norm ({norm:.2f}) — possibly featureless or corrupted"
                )
                metrics["quality_ok"] = False
        except Exception:
            metrics["siglip_norm"] = None
    else:
        metrics["siglip_norm"] = None

    # ── InsightFace face detection (optional) ─────────────────────────────────
    app = _get_iface_app()
    if app is not None:
        try:
            import cv2 as _cv2
            np_bgr = _cv2.cvtColor(np.array(pil_image.convert("RGB")), _cv2.COLOR_RGB2BGR)
            faces  = app.get(np_bgr)
            metrics["face_count"] = len(faces)
            if len(faces) == 0:
                metrics["warnings"].append("no face detected — check image content")
                metrics["quality_ok"] = False
            elif len(faces) > 1:
                metrics["warnings"].append(
                    f"{len(faces)} faces detected — use single-face images for best results"
                )
            else:
                bbox  = faces[0].bbox
                ratio = float((bbox[2] - bbox[0]) * (bbox[3] - bbox[1])) / (w * h)
                metrics["face_area_ratio"] = round(ratio, 3)
                if ratio < 0.05:
                    metrics["warnings"].append(
                        f"face too small ({ratio*100:.1f}% of image) — use a closer crop"
                    )
        except Exception:
            pass

    return metrics


# ════════════════════════════════════════════════════════════════════════════
# Embedding build (multi-image set → averaged .pt)
# ════════════════════════════════════════════════════════════════════════════
def _build_embedding(character_id: str) -> dict:
    """
    Process all images in references/{character_id}/ through SigLIP,
    average the pooler_outputs, project via MLPProjModel, save to .pt.

    Averaging in SigLIP space (before MLPProjModel) is more stable than
    averaging after the non-linear projection.

    Returns stats dict: character_id, n_images, path, embed_norm.
    """
    if not ip_adapter_loaded:
        raise RuntimeError("IP-Adapter not loaded — cannot build embedding")

    char_dir = _ref_set_dir(character_id)
    if char_dir is None:
        raise ValueError(
            f"No image set for '{character_id}'. "
            f"Upload images first with save_to_set=true."
        )

    image_files = sorted(
        f for f in os.listdir(char_dir)
        if f.lower().endswith((".png", ".jpg", ".jpeg", ".webp"))
    )

    print(f"[embed] Building embedding for '{character_id}' from {len(image_files)} images ...")
    pooler_outputs = []
    with torch.no_grad():
        for fname in image_files:
            path = os.path.join(char_dir, fname)
            try:
                pil = Image.open(path).convert("RGB")
            except Exception as e:
                print(f"[embed] Skipping {fname}: {e}")
                continue
            pixel_values = _ipa_processor(images=[pil], return_tensors="pt").pixel_values
            pixel_values = pixel_values.to(DEVICE, dtype=torch.bfloat16)
            out = _ipa_image_encoder(pixel_values).pooler_output  # [1, 1152]
            pooler_outputs.append(out)
            print(f"[embed]   {fname}: pooler_norm={out.norm().item():.3f}")

    if not pooler_outputs:
        raise ValueError(f"No valid images in {char_dir}")

    # Average in SigLIP embedding space, then project
    with torch.no_grad():
        pooler_mean = torch.stack(pooler_outputs, dim=0).mean(dim=0)   # [1, 1152]
        image_prompt_embeds = _ipa_proj_model(pooler_mean)              # [1, num_tokens, 4096]

    pt_path = os.path.join(REF_DIR, f"{character_id}.pt")
    torch.save({
        "image_prompt_embeds": image_prompt_embeds.cpu(),
        "character_id":        character_id,
        "n_images":            len(pooler_outputs),
        "created":             datetime.now(timezone.utc).isoformat(),
    }, pt_path)

    norm = image_prompt_embeds.norm().item()
    print(f"[embed] Saved: {pt_path}  n={len(pooler_outputs)}  norm={norm:.3f}")
    return {
        "character_id": character_id,
        "n_images":     len(pooler_outputs),
        "path":         pt_path,
        "embed_norm":   round(norm, 4),
    }


# ════════════════════════════════════════════════════════════════════════════
# IP-Adapter inference helpers
# ════════════════════════════════════════════════════════════════════════════
@torch.no_grad()
def _ipa_get_image_embeds(pil_image: Image.Image) -> torch.Tensor:
    """SigLIP pooler_output → MLPProjModel → [1, num_tokens, 4096]"""
    pixel_values = _ipa_processor(images=[pil_image], return_tensors="pt").pixel_values
    pixel_values = pixel_values.to(DEVICE, dtype=torch.bfloat16)
    embeds = _ipa_image_encoder(pixel_values).pooler_output   # [1, 1152]
    return _ipa_proj_model(embeds)                            # [1, num_tokens, 4096]


def _ipa_set_scale(scale: float):
    for proc in pipe.transformer.attn_processors.values():
        if _IPAFluxAttnProc and isinstance(proc, _IPAFluxAttnProc):
            proc.scale = scale


# ════════════════════════════════════════════════════════════════════════════
# Face-swap post-processing (InsightFace inswapper_128)
# ════════════════════════════════════════════════════════════════════════════
def _load_faceswap():
    """
    Load buffalo_l face detector + inswapper_128.onnx + GFPGAN (optional).
    Called from lifespan when --faceswap is set.
    Runs on CPU (ONNX) — no GPU VRAM impact.
    """
    global _fs_face_app, _fs_swapper, _fs_gfpganer, faceswap_loaded
    try:
        from insightface.app import FaceAnalysis
    except ImportError:
        print("[faceswap] insightface not installed — face-swap disabled.")
        print("           pip install insightface onnxruntime")
        return

    providers = ["CUDAExecutionProvider", "CPUExecutionProvider"]
    print("[faceswap] Loading buffalo_l face detector ...")
    _fs_face_app = FaceAnalysis(name="buffalo_l", providers=providers)
    _fs_face_app.prepare(ctx_id=0 if DEVICE == "cuda" else -1, det_size=(640, 640))

    if not os.path.exists(INSWAPPER_PATH):
        print(f"[faceswap] inswapper_128.onnx not found at {INSWAPPER_PATH}")
        print("           Download: hf download deepinsight/inswapper "
              "--local-dir ../faceswap_locale/models --include 'inswapper_128.onnx'")
        _fs_face_app = None
        return

    import insightface
    print(f"[faceswap] Loading inswapper from {os.path.basename(INSWAPPER_PATH)} ...")
    _fs_swapper = insightface.model_zoo.get_model(INSWAPPER_PATH, providers=providers)

    # GFPGAN — optional
    try:
        from gfpgan import GFPGANer
        if os.path.exists(GFPGAN_PATH):
            _fs_gfpganer = GFPGANer(
                model_path=GFPGAN_PATH, upscale=1,
                arch="clean", channel_multiplier=2,
            )
            print(f"[faceswap] GFPGAN ready ({os.path.basename(GFPGAN_PATH)})")
        else:
            print(f"[faceswap] GFPGANv1.4.pth not found — enhancement disabled")
    except ImportError:
        print("[faceswap] gfpgan not installed — enhancement disabled")

    faceswap_loaded = True
    print("[faceswap] ACTIVE")


def _faceswap_best_ref(character_id: str) -> Optional[str]:
    """
    Select best reference image for face-swap using buffalo_l (same detector used at swap time).
    Scores by face area ratio — only images where buffalo_l actually detects a face qualify.
    Priority: set dir (best scoring) → single ref image → None.
    Cache invalidated by set dir mtime.
    """
    global _fs_best_ref_cache
    import cv2 as _cv2

    set_dir = _ref_set_dir(character_id)
    if set_dir:
        try:
            dir_mtime = os.path.getmtime(set_dir)
        except OSError:
            dir_mtime = 0.0
        cached = _fs_best_ref_cache.get(character_id)
        if cached and cached[0] == dir_mtime and os.path.exists(cached[1]):
            return cached[1]
        images = sorted(
            f for f in os.listdir(set_dir)
            if f.lower().endswith((".png", ".jpg", ".jpeg", ".webp"))
        )
        best_path, best_score = None, -1.0
        for fname in images:
            path = os.path.join(set_dir, fname)
            try:
                bgr = _cv2.cvtColor(
                    np.array(Image.open(path).convert("RGB")), _cv2.COLOR_RGB2BGR
                )
                faces = _fs_face_app.get(bgr)
                if not faces:
                    continue
                # Score = face area ratio of largest face
                w, h = bgr.shape[1], bgr.shape[0]
                bbox = faces[0].bbox
                score = float((bbox[2] - bbox[0]) * (bbox[3] - bbox[1])) / (w * h)
                if score > best_score:
                    best_score, best_path = score, path
            except Exception:
                continue
        if best_path:
            _fs_best_ref_cache[character_id] = (dir_mtime, best_path)
            print(f"[faceswap] Best ref for '{character_id}': {os.path.basename(best_path)} "
                  f"(face_area={best_score*100:.1f}%)")
            return best_path
    return _ref_image_path(character_id)


def _faceswap_postprocess(
    pil_image: Image.Image,
    character_id: str,
    enhance: bool = False,
    slot: int = 0,
) -> Image.Image:
    """
    Swap face in generated image using character's best reference.
    Returns modified PIL image (or original if swap fails/no reference/no face).
    """
    import cv2 as _cv2
    ref_path = _faceswap_best_ref(character_id)
    if ref_path is None:
        print(f"[faceswap] No reference for '{character_id}' — skipping")
        return pil_image
    try:
        ref_bgr = _cv2.cvtColor(np.array(Image.open(ref_path).convert("RGB")), _cv2.COLOR_RGB2BGR)
        gen_bgr = _cv2.cvtColor(np.array(pil_image), _cv2.COLOR_RGB2BGR)

        target_faces = _fs_face_app.get(gen_bgr)
        target_faces.sort(key=lambda f: f.bbox[0])
        if not target_faces:
            print(f"[faceswap] No face detected in generated image — skipping swap")
            return pil_image
        slot = min(slot, len(target_faces) - 1)

        src_faces = _fs_face_app.get(ref_bgr)
        if not src_faces:
            print(f"[faceswap] No face in reference '{os.path.basename(ref_path)}' — skipping")
            return pil_image

        result_bgr = _fs_swapper.get(gen_bgr.copy(), target_faces[slot], src_faces[0], paste_back=True)
        print(f"[faceswap] Swapped '{character_id}' (ref: {os.path.basename(ref_path)}, slot={slot})")

        if enhance:
            if _fs_gfpganer is not None:
                _, _, result_bgr = _fs_gfpganer.enhance(
                    result_bgr, has_aligned=False, only_center_face=False, paste_back=True,
                )
                print("[faceswap] GFPGAN enhancement applied")
            else:
                print("[faceswap] enhance=True but GFPGAN not available")

        return Image.fromarray(_cv2.cvtColor(result_bgr, _cv2.COLOR_BGR2RGB))
    except Exception as e:
        print(f"[faceswap] Error: {e} — returning original")
        return pil_image


# ════════════════════════════════════════════════════════════════════════════
# Inference core
# ════════════════════════════════════════════════════════════════════════════
def _utc_ts() -> str:
    return datetime.now(timezone.utc).strftime("%Y%m%d_%H%M%S")

def _safe_name(s: str, maxlen: int = 40) -> str:
    return re.sub(r"[^a-zA-Z0-9_-]", "_", s[:maxlen]).strip("_")


def _run_generation(
    prompt: str,
    width: int,
    height: int,
    steps: int,
    guidance: float,
    id_embeds: Optional[torch.Tensor] = None,    # pre-computed [1, num_tokens, 4096]
    id_images: Optional[List[Image.Image]] = None, # fallback: compute via SigLIP
    id_scales: Optional[List[float]] = None,
    lora_name: Optional[str] = None,
    lora_scale: float = 1.0,
    seed: int = -1,
) -> tuple[Image.Image, int]:
    """
    Core inference.  torch.no_grad() — NOT inference_mode().
    LoRA may call requires_grad_() between requests; inference_mode freezes tensors → RuntimeError.

    IP-Adapter conditioning priority:
      1. id_embeds  — pre-computed tensor from .pt (fastest, skips SigLIP)
      2. id_images  — PIL image(s), encode on-the-fly via SigLIP + MLPProjModel
      3. neither    — plain generation (scale set to 0.0 to no-op the processors)
    """
    if lora_name:
        _load_lora(lora_name)
    _apply_loras(lora_name, lora_scale)

    import random
    if seed < 0:
        seed = random.randint(0, 2 ** 32 - 1)
    generator = torch.Generator(device=DEVICE).manual_seed(seed)

    with torch.no_grad():
        sig = set(inspect.signature(pipe.__call__).parameters)
        kw = dict(
            prompt=prompt,
            width=width,
            height=height,
            num_inference_steps=steps,
            generator=generator,
        )
        if "guidance_scale" in sig:
            kw["guidance_scale"] = guidance

        # ── IP-Adapter conditioning ─────────────────────────────────────────
        if ip_adapter_loaded:
            id_scale = id_scales[0] if id_scales else 0.8
            image_prompt_embeds = None

            if id_embeds is not None:
                # Pre-computed embedding from .pt — skip SigLIP entirely
                image_prompt_embeds = id_embeds
                print(f"[gen] → IP-Adapter (pre-computed embed), id_scale={id_scale}")
            elif id_images:
                try:
                    image_prompt_embeds = _ipa_get_image_embeds(id_images[0])
                    print(f"[gen] → IP-Adapter (on-the-fly SigLIP), id_scale={id_scale}")
                except Exception as e:
                    print(f"[gen] IP-Adapter embed failed: {e} — falling back to plain")

            if image_prompt_embeds is not None:
                _ipa_set_scale(id_scale)
                kw["image_emb"] = image_prompt_embeds
            else:
                _ipa_set_scale(0.0)

        # ── Generate ────────────────────────────────────────────────────────
        try:
            result = pipe(**kw).images[0]
        except TypeError as e:
            m = re.search(r"unexpected keyword argument '(\w+)'", str(e))
            if m:
                removed = m.group(1)
                print(f"[gen] Removed unsupported kwarg: '{removed}' — retrying")
                kw.pop(removed, None)
                result = pipe(**kw).images[0]
            else:
                raise

    return result, seed


def _save_output(img: Image.Image, label: str) -> str:
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    path = os.path.join(OUTPUT_DIR, f"{_utc_ts()}_{_safe_name(label)}.png")
    img.save(path, format="PNG")
    print(f"[output] Saved: {path}")
    return path


# ════════════════════════════════════════════════════════════════════════════
# Lifespan
# ════════════════════════════════════════════════════════════════════════════
@asynccontextmanager
async def lifespan(app: FastAPI):
    for d in (REF_DIR, LORA_DIR, OUTPUT_DIR):
        os.makedirs(d, exist_ok=True)
    try:
        _load_model()
        if USE_FACESWAP:
            _load_faceswap()
        print("[startup] Server ready.")
    except Exception as e:
        print(f"\n[FATAL] {e}")
        print("[FATAL] Try: --dtype bf16 --quantize torchao_int8 --cpu-offload")
        raise
    yield
    print("[shutdown] Server stopped.")

app = FastAPI(title="RpgAi T2I Server (FLUX.1-dev + IP-Adapter)", lifespan=lifespan)


# ════════════════════════════════════════════════════════════════════════════
# POST /generate_portrait
# ════════════════════════════════════════════════════════════════════════════
class PortraitRequest(BaseModel):
    prompt: str
    character_id: str = ""
    id_scale: float = 0.8
    steps: int = -1
    width: int = 512
    height: int = 768
    guidance_scale: float = -1.0
    save_as_reference: bool = False
    lora_name: str = ""
    lora_scale: float = 1.0
    seed: int = -1
    faceswap: bool = False
    faceswap_enhance: bool = False

@app.post("/generate_portrait")
async def generate_portrait(req: PortraitRequest):
    """
    Generate an NPC portrait.
    - character_id + reference in ./references/ + IP-Adapter active → injects face identity.
    - save_as_reference=true stores output as ./references/{character_id}.png.
    - Returns PNG bytes. Headers: X-Seed, X-Character-Id.
    """
    id_embeds = id_images = id_scales = None
    if req.character_id:
        if not USE_IP_ADAPTER:
            print(f"[portrait] WARNING: '{req.character_id}' ignored — server started without --ip-adapter")
        elif not ip_adapter_loaded:
            print(f"[portrait] WARNING: '{req.character_id}' ignored — IP-Adapter not loaded")
        else:
            # Priority: pre-computed .pt > single image on-the-fly
            emb = _load_ref_embedding(req.character_id)
            if emb is not None:
                id_embeds = emb
                id_scales = [req.id_scale]
                print(f"[portrait] Pre-computed embedding loaded for '{req.character_id}'")
            else:
                ref = _load_ref_image(req.character_id)
                if ref:
                    id_images = [ref]
                    id_scales = [req.id_scale]
                    print(f"[portrait] Single-image reference loaded for '{req.character_id}'")
                elif _ref_set_dir(req.character_id):
                    print(f"[portrait] WARNING: set folder found for '{req.character_id}' but no .pt — run build-ref first")
                else:
                    print(f"[portrait] No reference for '{req.character_id}' — generating plain")

    steps    = req.steps if req.steps > 0 else ARGS.steps
    guidance = req.guidance_scale if req.guidance_scale >= 0 else ARGS.guidance

    try:
        result, seed_used = _run_generation(
            prompt=req.prompt, width=req.width, height=req.height,
            steps=steps, guidance=guidance,
            id_embeds=id_embeds, id_images=id_images, id_scales=id_scales,
            lora_name=req.lora_name or None, lora_scale=req.lora_scale,
            seed=req.seed,
        )
    except torch.cuda.OutOfMemoryError:
        raise HTTPException(503, "GPU OOM. Try --cpu-offload or smaller dimensions.")
    except Exception as e:
        raise HTTPException(500, f"Generation error: {e}")

    if req.faceswap and req.character_id:
        if faceswap_loaded:
            result = _faceswap_postprocess(result, req.character_id, req.faceswap_enhance)
        else:
            print(f"[portrait] faceswap=True but face-swap not loaded — skipping")

    _save_output(result, req.character_id or "portrait")

    if req.save_as_reference and req.character_id:
        ref_path = os.path.join(REF_DIR, f"{req.character_id}.png")
        result.save(ref_path, format="PNG")
        print(f"[portrait] Saved reference: {ref_path}")

    buf = io.BytesIO()
    result.save(buf, format="PNG")
    return Response(
        content=buf.getvalue(), media_type="image/png",
        headers={"X-Seed": str(seed_used), "X-Character-Id": req.character_id},
    )


# ════════════════════════════════════════════════════════════════════════════
# POST /generate_scene
# ════════════════════════════════════════════════════════════════════════════
class SceneCharacter(BaseModel):
    id: str
    id_scale: float = 0.7

class SceneRequest(BaseModel):
    prompt: str
    characters: List[SceneCharacter] = []
    steps: int = -1
    width: int = 1024
    height: int = 576
    guidance_scale: float = -1.0
    lora_name: str = ""
    lora_scale: float = 1.0
    seed: int = -1
    faceswap: bool = False
    faceswap_enhance: bool = False

@app.post("/generate_scene")
async def generate_scene(req: SceneRequest):
    """
    Generate a scene. First character with a reference is used as IP-Adapter conditioning.
    Returns PNG bytes. Header: X-Seed.
    """
    # For scenes: use first character that has a .pt or image reference
    id_embeds = id_images = id_scales = None
    if req.characters and not USE_IP_ADAPTER:
        print(f"[scene] WARNING: characters ignored — server started without --ip-adapter")
    elif req.characters and not ip_adapter_loaded:
        print(f"[scene] WARNING: characters ignored — IP-Adapter not loaded")
    else:
        for ch in req.characters:
            emb = _load_ref_embedding(ch.id)
            if emb is not None:
                id_embeds = emb
                id_scales = [ch.id_scale]
                print(f"[scene] Pre-computed embedding for '{ch.id}'")
                break
            ref = _load_ref_image(ch.id)
            if ref:
                id_images = [ref]
                id_scales = [ch.id_scale]
                print(f"[scene] Single-image reference for '{ch.id}'")
                break
            print(f"[scene] No reference for '{ch.id}' — skipped")

    steps    = req.steps if req.steps > 0 else ARGS.steps
    guidance = req.guidance_scale if req.guidance_scale >= 0 else ARGS.guidance

    try:
        result, seed_used = _run_generation(
            prompt=req.prompt, width=req.width, height=req.height,
            steps=steps, guidance=guidance,
            id_embeds=id_embeds, id_images=id_images, id_scales=id_scales,
            lora_name=req.lora_name or None, lora_scale=req.lora_scale,
            seed=req.seed,
        )
    except torch.cuda.OutOfMemoryError:
        raise HTTPException(503, "GPU OOM. Try --cpu-offload or smaller dimensions.")
    except Exception as e:
        raise HTTPException(500, f"Generation error: {e}")

    if req.faceswap and req.characters:
        if faceswap_loaded:
            # Swap faces left-to-right: each character in slot order (only those with refs)
            slot = 0
            for ch in req.characters:
                if _faceswap_best_ref(ch.id) is not None:
                    result = _faceswap_postprocess(result, ch.id, req.faceswap_enhance, slot=slot)
                    slot += 1
        else:
            print("[scene] faceswap=True but face-swap not loaded — skipping")

    _save_output(result, req.prompt)

    buf = io.BytesIO()
    result.save(buf, format="PNG")
    return Response(
        content=buf.getvalue(), media_type="image/png",
        headers={"X-Seed": str(seed_used)},
    )


# ════════════════════════════════════════════════════════════════════════════
# Reference management
# ════════════════════════════════════════════════════════════════════════════
@app.post("/references/check")
async def references_check(file: UploadFile = File(...)):
    """
    Evaluate a reference image quality without saving it.
    Returns metrics: size, sharpness, siglip_norm, face_count (if InsightFace available),
    face_area_ratio, warnings, quality_ok.
    Call this before add-ref to screen images.
    """
    data = await file.read()
    try:
        img = Image.open(io.BytesIO(data)).convert("RGB")
    except Exception as e:
        raise HTTPException(400, f"Invalid image: {e}")
    return JSONResponse(_check_image_quality(img))


@app.post("/references/add")
async def references_add(
    character_id: str = Form(...),
    file: UploadFile = File(...),
    save_to_set: bool = Form(False),
):
    """
    Upload a reference face.
    - save_to_set=false (default): saves as references/{char}.png (single reference, overwrites).
    - save_to_set=true: appends to references/{char}/ folder (NNN.png).
      After uploading all images, call POST /references/build to compute the averaged embedding.
    """
    data = await file.read()
    try:
        img = Image.open(io.BytesIO(data)).convert("RGB")
    except Exception as e:
        raise HTTPException(400, f"Invalid image: {e}")

    char_safe = _safe_name(character_id)

    if save_to_set:
        char_dir = os.path.join(REF_DIR, char_safe)
        os.makedirs(char_dir, exist_ok=True)
        existing = [f for f in os.listdir(char_dir)
                    if f.lower().endswith((".png", ".jpg", ".jpeg", ".webp"))]
        idx = len(existing) + 1
        ref_path = os.path.join(char_dir, f"{idx:03d}.png")
        img.save(ref_path, format="PNG")
        quality = _check_image_quality(img)
        if quality["warnings"]:
            print(f"[ref] Quality warnings for {os.path.basename(ref_path)}: {quality['warnings']}")
        else:
            print(f"[ref] Added to set: {ref_path} ({idx} total) — quality OK")
        return JSONResponse({
            "character_id": character_id,
            "path":         ref_path,
            "set_size":     idx,
            "status":       "added_to_set",
            "quality":      quality,
            "next_step":    f"POST /references/build with character_id={character_id}",
        })
    else:
        ref_path = os.path.join(REF_DIR, f"{char_safe}.png")
        img.save(ref_path, format="PNG")
        print(f"[ref] Saved: {ref_path}")
        return JSONResponse({"character_id": character_id, "path": ref_path, "status": "saved"})


class BuildEmbeddingRequest(BaseModel):
    character_id: str

@app.post("/references/build")
async def references_build(req: BuildEmbeddingRequest):
    """
    Build a pre-computed embedding from all images in references/{character_id}/.
    Averages SigLIP pooler_outputs across all images, then projects via MLPProjModel.
    Saves result to references/{character_id}.pt — used automatically on next generation.
    """
    if not ip_adapter_loaded:
        raise HTTPException(503, "IP-Adapter not loaded — cannot build embedding")
    try:
        result = _build_embedding(req.character_id)
        return JSONResponse(result)
    except ValueError as e:
        raise HTTPException(400, str(e))
    except Exception as e:
        import traceback; traceback.print_exc()
        raise HTTPException(500, f"Build failed: {e}")


@app.get("/references")
def references_list():
    """List all stored references: single images, sets, and pre-computed embeddings."""
    if not os.path.isdir(REF_DIR):
        return {"references": []}
    entries: dict[str, dict] = {}
    for f in sorted(os.listdir(REF_DIR)):
        full = os.path.join(REF_DIR, f)
        name = os.path.splitext(f)[0]
        if f.lower().endswith((".png", ".jpg", ".jpeg", ".webp")):
            e = entries.setdefault(name, {"character_id": name})
            e["single_image"] = f
        elif f.endswith(".pt"):
            e = entries.setdefault(name, {"character_id": name})
            data = torch.load(full, map_location="cpu", weights_only=False)
            e["embedding"] = {"file": f, "n_images": data.get("n_images", "?"),
                              "created": data.get("created", "?")}
        elif os.path.isdir(full):
            imgs = [i for i in os.listdir(full)
                    if i.lower().endswith((".png", ".jpg", ".jpeg", ".webp"))]
            if imgs:
                e = entries.setdefault(f, {"character_id": f})
                e["set"] = {"dir": f, "n_images": len(imgs)}
    return {"references": list(entries.values())}


# ════════════════════════════════════════════════════════════════════════════
# LoRA list
# ════════════════════════════════════════════════════════════════════════════
@app.get("/loras")
def loras_list():
    if not os.path.isdir(LORA_DIR):
        return {"loras": [], "loaded": []}
    available = []
    for entry in sorted(os.listdir(LORA_DIR)):
        full = os.path.join(LORA_DIR, entry)
        if os.path.isdir(full):
            if any(f.endswith((".safetensors", ".bin")) for f in os.listdir(full)):
                available.append(entry)
        elif entry.endswith((".safetensors", ".bin")):
            available.append(entry)
    return {"loras": available, "loaded": list(loaded_loras.keys())}


# ════════════════════════════════════════════════════════════════════════════
# Health
# ════════════════════════════════════════════════════════════════════════════
@app.get("/health")
def health():
    refs = []
    if os.path.isdir(REF_DIR):
        refs = [os.path.splitext(f)[0]
                for f in sorted(os.listdir(REF_DIR))
                if f.lower().endswith((".png", ".jpg", ".jpeg", ".webp"))]
    return {
        "status":               "ok",
        "model":                MODEL_REPO,
        "model_dir":            MODEL_DIR,
        "device":               DEVICE,
        "dtype":                str(DTYPE),
        "quantize":             QUANTIZE if not USE_IP_ADAPTER else None,
        "offload":              USE_OFFLOAD,
        "fast":                 ARGS.fast,
        "ip_adapter_requested": USE_IP_ADAPTER,
        "ip_adapter_active":    ip_adapter_loaded,
        "ip_adapter_dir":       IP_ADAPTER_DIR,
        "ip_tokens":            ARGS.ip_tokens,
        "faceswap_requested":   USE_FACESWAP,
        "faceswap_active":      faceswap_loaded,
        "faceswap_gfpgan":      _fs_gfpganer is not None,
        "default_steps":        ARGS.steps,
        "default_guidance":     ARGS.guidance,
        "loras_loaded":         list(loaded_loras.keys()),
        "references":           refs,
    }


# ════════════════════════════════════════════════════════════════════════════
# Entry point
# ════════════════════════════════════════════════════════════════════════════
if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host=ARGS.host, port=ARGS.port)
