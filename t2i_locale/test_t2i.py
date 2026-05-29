#!/usr/bin/env python3
"""
Test script for t2i_locale server.

Usage:
    python test_t2i.py                          # health check only
    python test_t2i.py portrait                 # generate portrait (no reference)
    python test_t2i.py portrait --char jenny    # portrait with reference (if stored)
    python test_t2i.py portrait --char jenny --save-ref  # generate + store as reference
    python test_t2i.py scene                    # scene with multiple NPCs
    python test_t2i.py add-ref --char jenny --file path/to/jenny.jpg
    python test_t2i.py refs                     # list stored references
    python test_t2i.py loras                    # list available LoRAs

    # Remote server (Mac dev → Linux GPU)
    python test_t2i.py --url http://192.168.1.41:8003 portrait --char jenny
"""

import argparse
import os
import sys
import time

try:
    import requests
except ImportError:
    print("Missing: pip install requests")
    sys.exit(1)

DEFAULT_BASE = "http://127.0.0.1:8003"
DEFAULT_PORTRAIT_PROMPT = (
    "portrait photo of a young woman, brown eyes, confident expression, "
    "soft studio lighting, shallow depth of field, ultra detailed"
)
DEFAULT_SCENE_PROMPT = (
    "two women sitting in a cafe, warm afternoon light, photorealistic, "
    "8k, cinematic composition"
)


# ─────────────────────────────────────────────────────────────────────────────
def _section(title: str):
    print(f"\n{'─' * 52}")
    print(f"  {title}")
    print(f"{'─' * 52}")


def health_check(base: str) -> bool:
    _section("Health")
    try:
        r = requests.get(f"{base}/health", timeout=10)
        r.raise_for_status()
        d = r.json()
        print(f"  Status         : {d.get('status')}")
        print(f"  Model          : {d.get('model')}")
        print(f"  Device         : {d.get('device')}")
        print(f"  Dtype          : {d.get('dtype')}")
        print(f"  Quantize       : {d.get('quantize') or 'none'}")
        print(f"  PuLID active   : {d.get('pulid_active')}")
        print(f"  Default steps  : {d.get('default_steps')}")
        print(f"  Default guidance: {d.get('default_guidance')}")
        print(f"  References     : {d.get('references')}")
        print(f"  LoRAs loaded   : {d.get('loras_loaded')}")
        return True
    except requests.exceptions.ConnectionError:
        print("  ERROR: Server not running.")
        print(f"  Start with: python server.py --dtype bf16 --host 0.0.0.0")
        return False
    except Exception as e:
        print(f"  ERROR: {e}")
        return False


def cmd_portrait(base: str, args):
    _section("Generate portrait")
    payload = {
        "prompt":            args.prompt or DEFAULT_PORTRAIT_PROMPT,
        "character_id":      args.char or "",
        "id_scale":          args.id_scale,
        "save_as_reference": args.save_ref,
        "seed":              args.seed,
    }
    if args.steps:  payload["steps"]          = args.steps
    if args.width:  payload["width"]           = args.width
    if args.height: payload["height"]          = args.height
    if args.lora:   payload["lora_name"]       = args.lora

    print(f"  character_id : {payload['character_id'] or '(none)'}")
    print(f"  save_as_ref  : {payload['save_as_reference']}")
    print(f"  prompt       : {payload['prompt'][:80]}...")
    t0 = time.time()
    r = requests.post(f"{base}/generate_portrait", json=payload, timeout=300)
    elapsed = time.time() - t0

    if r.status_code != 200:
        print(f"  ERROR {r.status_code}: {r.text[:300]}")
        return
    out = args.output or f"portrait_{args.char or 'output'}_{int(time.time())}.png"
    with open(out, "wb") as f:
        f.write(r.content)
    seed = r.headers.get("X-Seed", "?")
    print(f"  Saved  : {out}  ({len(r.content)//1024} KB)")
    print(f"  Seed   : {seed}")
    print(f"  Time   : {elapsed:.1f}s")


def cmd_scene(base: str, args):
    _section("Generate scene")
    chars = []
    for c in (args.chars or []):
        chars.append({"id": c, "id_scale": args.id_scale})
    payload = {
        "prompt":     args.prompt or DEFAULT_SCENE_PROMPT,
        "characters": chars,
        "seed":       args.seed,
    }
    if args.steps:  payload["steps"]    = args.steps
    if args.width:  payload["width"]    = args.width
    if args.height: payload["height"]   = args.height
    if args.lora:   payload["lora_name"] = args.lora

    print(f"  characters : {[c['id'] for c in chars] or '(none)'}")
    print(f"  prompt     : {payload['prompt'][:80]}...")
    t0 = time.time()
    r = requests.post(f"{base}/generate_scene", json=payload, timeout=300)
    elapsed = time.time() - t0

    if r.status_code != 200:
        print(f"  ERROR {r.status_code}: {r.text[:300]}")
        return
    out = args.output or f"scene_{int(time.time())}.png"
    with open(out, "wb") as f:
        f.write(r.content)
    seed = r.headers.get("X-Seed", "?")
    print(f"  Saved  : {out}  ({len(r.content)//1024} KB)")
    print(f"  Seed   : {seed}")
    print(f"  Time   : {elapsed:.1f}s")


def cmd_add_ref(base: str, args):
    _section("Add reference face")
    if not args.char:
        print("  ERROR: --char required")
        return
    if not args.file or not os.path.exists(args.file):
        print(f"  ERROR: --file {args.file!r} not found")
        return
    with open(args.file, "rb") as f:
        r = requests.post(
            f"{base}/references/add",
            data={"character_id": args.char},
            files={"file": (os.path.basename(args.file), f, "image/png")},
            timeout=30,
        )
    if r.status_code == 200:
        d = r.json()
        print(f"  Saved: {d.get('path')}")
    else:
        print(f"  ERROR {r.status_code}: {r.text[:300]}")


def cmd_refs(base: str):
    _section("Reference faces")
    r = requests.get(f"{base}/references", timeout=10)
    if r.status_code != 200:
        print(f"  ERROR {r.status_code}: {r.text}")
        return
    refs = r.json().get("references", [])
    if not refs:
        print("  (none stored)")
    for ref in refs:
        print(f"  {ref['character_id']:<20}  {ref['file']}")


def cmd_loras(base: str):
    _section("LoRAs")
    r = requests.get(f"{base}/loras", timeout=10)
    if r.status_code != 200:
        print(f"  ERROR {r.status_code}: {r.text}")
        return
    d = r.json()
    available = d.get("loras", [])
    loaded    = d.get("loaded", [])
    if not available:
        print("  (none available in ./loras/)")
    for name in available:
        status = " [loaded]" if name in loaded else ""
        print(f"  {name}{status}")


# ─────────────────────────────────────────────────────────────────────────────
def main():
    p = argparse.ArgumentParser(description="test_t2i — t2i_locale server test")
    p.add_argument("--url", default=DEFAULT_BASE,
                   help=f"Server URL (default: {DEFAULT_BASE})")
    sub = p.add_subparsers(dest="cmd")

    # portrait
    pp = sub.add_parser("portrait", help="Generate NPC portrait")
    pp.add_argument("--prompt",    default=None)
    pp.add_argument("--char",      default="",      help="Character ID (looks up reference face)")
    pp.add_argument("--save-ref",  action="store_true", help="Save output as reference for --char")
    pp.add_argument("--id-scale",  type=float, default=0.8)
    pp.add_argument("--steps",     type=int,   default=None)
    pp.add_argument("--width",     type=int,   default=None)
    pp.add_argument("--height",    type=int,   default=None)
    pp.add_argument("--lora",      default=None)
    pp.add_argument("--seed",      type=int,   default=-1)
    pp.add_argument("--output",    default=None, help="Output filename (default: auto)")

    # scene
    sp = sub.add_parser("scene", help="Generate scene with multiple NPCs")
    sp.add_argument("--prompt",    default=None)
    sp.add_argument("--chars",     nargs="+",  default=[], help="Character IDs")
    sp.add_argument("--id-scale",  type=float, default=0.7)
    sp.add_argument("--steps",     type=int,   default=None)
    sp.add_argument("--width",     type=int,   default=None)
    sp.add_argument("--height",    type=int,   default=None)
    sp.add_argument("--lora",      default=None)
    sp.add_argument("--seed",      type=int,   default=-1)
    sp.add_argument("--output",    default=None, help="Output filename (default: auto)")

    # add-ref
    rp = sub.add_parser("add-ref", help="Upload reference face for a character")
    rp.add_argument("--char", required=True)
    rp.add_argument("--file", required=True)

    # refs
    sub.add_parser("refs",  help="List stored reference faces")

    # loras
    sub.add_parser("loras", help="List available LoRAs")

    args = p.parse_args()
    base = args.url.rstrip("/")

    if not health_check(base):
        if args.cmd:
            sys.exit(1)
        return

    if   args.cmd == "portrait": cmd_portrait(base, args)
    elif args.cmd == "scene":    cmd_scene(base, args)
    elif args.cmd == "add-ref":  cmd_add_ref(base, args)
    elif args.cmd == "refs":     cmd_refs(base)
    elif args.cmd == "loras":    cmd_loras(base)


if __name__ == "__main__":
    main()
