#!/usr/bin/env python3
"""
t2i.py — Client per t2i_locale server (FLUX.1-dev + IP-Adapter).

Uso tramite wrapper:
    ./tools/t2i.sh portrait "ritratto di jenny, capelli castani"
    ./tools/t2i.sh portrait "jenny al bar" --char jenny
    ./tools/t2i.sh scene "jenny e debbie al bar" --chars jenny debbie
    ./tools/t2i.sh add-ref --char jenny --file jenny.jpg
    ./tools/t2i.sh add-ref --char jenny --file jenny2.jpg --set
    ./tools/t2i.sh build-ref --char jenny
    ./tools/t2i.sh refs
    ./tools/t2i.sh health

Uso diretto:
    python3 tools/t2i.py --url http://192.168.1.41:8003 portrait "prompt"
"""

import argparse
import os
import subprocess
import sys
import time

try:
    import requests
except ImportError:
    print("Missing: pip install requests")
    sys.exit(1)

DEFAULT_URL = "http://127.0.0.1:8003"


def _section(title: str):
    print(f"\n{'─' * 52}")
    print(f"  {title}")
    print(f"{'─' * 52}")


def _open_image(path: str):
    try:
        if sys.platform == "darwin":
            subprocess.run(["open", path], check=False)
        elif sys.platform.startswith("linux"):
            subprocess.run(["xdg-open", path], check=False)
        else:
            subprocess.run(["start", path], shell=True, check=False)
    except Exception:
        pass


def cmd_health(base: str):
    _section("Health")
    try:
        r = requests.get(f"{base}/health", timeout=10)
        r.raise_for_status()
        d = r.json()
        print(f"  Status          : {d.get('status')}")
        print(f"  Model           : {d.get('model')}")
        print(f"  Device          : {d.get('device')} / {d.get('dtype')}")
        print(f"  Quantize        : {d.get('quantize') or 'nessuna'}")
        print(f"  IP-Adapter      : {d.get('ip_adapter_active')}  (tokens={d.get('ip_tokens')})")
        fs  = d.get("faceswap_active")
        gfp = d.get("faceswap_gfpgan")
        print(f"  FaceSwap        : {fs}  (GFPGAN={gfp})")
        print(f"  Steps default   : {d.get('default_steps')}")
        print(f"  Guidance def.   : {d.get('default_guidance')}")
        print(f"  References      : {d.get('references') or '(nessuna)'}")
        print(f"  LoRA caricati   : {d.get('loras_loaded') or '(nessuno)'}")
    except requests.exceptions.ConnectionError:
        print(f"  ERRORE: server non raggiungibile su {base}")
        sys.exit(1)
    except Exception as e:
        print(f"  ERRORE: {e}")
        sys.exit(1)


def cmd_portrait(base: str, args):
    _section("Genera ritratto")
    payload = {
        "prompt":            args.prompt,
        "character_id":      args.char or "",
        "id_scale":          args.id_scale,
        "save_as_reference": args.save_ref,
        "seed":              args.seed,
    }
    if args.steps    is not None: payload["steps"]         = args.steps
    if args.width    is not None: payload["width"]          = args.width
    if args.height   is not None: payload["height"]         = args.height
    if args.guidance is not None: payload["guidance_scale"] = args.guidance
    if args.lora:                 payload["lora_name"]      = args.lora
    if getattr(args, "faceswap", False):
        payload["faceswap"] = True
    if getattr(args, "faceswap_enhance", False):
        payload["faceswap_enhance"] = True

    print(f"  Prompt      : {args.prompt[:72]}")
    print(f"  Personaggio : {args.char or '(nessuno)'}")
    if args.char:
        print(f"  ID scale    : {args.id_scale}")
    print(f"  FaceSwap    : {getattr(args, 'faceswap', False)}")
    print(f"  Salva ref   : {args.save_ref}")

    t0 = time.time()
    try:
        r = requests.post(f"{base}/generate_portrait", json=payload, timeout=300)
    except requests.exceptions.ConnectionError:
        print(f"  ERRORE: server non raggiungibile su {base}")
        sys.exit(1)

    elapsed = time.time() - t0
    if r.status_code != 200:
        print(f"  ERRORE {r.status_code}: {r.text[:400]}")
        sys.exit(1)

    out = args.output or f"{args.char or 'portrait'}_{int(time.time())}.png"
    with open(out, "wb") as f:
        f.write(r.content)
    seed = r.headers.get("X-Seed", "?")
    print(f"  Salvato     : {out}  ({len(r.content)//1024} KB)")
    print(f"  Seed        : {seed}")
    print(f"  Tempo       : {elapsed:.1f}s")
    if args.show:
        _open_image(out)


def cmd_scene(base: str, args):
    _section("Genera scena")
    chars = [{"id": c, "id_scale": args.id_scale} for c in (args.chars or [])]
    payload = {
        "prompt":     args.prompt,
        "characters": chars,
        "seed":       args.seed,
    }
    if args.steps    is not None: payload["steps"]         = args.steps
    if args.width    is not None: payload["width"]          = args.width
    if args.height   is not None: payload["height"]         = args.height
    if args.guidance is not None: payload["guidance_scale"] = args.guidance
    if args.lora:                 payload["lora_name"]      = args.lora
    if getattr(args, "faceswap", False):
        payload["faceswap"] = True
    if getattr(args, "faceswap_enhance", False):
        payload["faceswap_enhance"] = True

    print(f"  Prompt      : {args.prompt[:72]}")
    print(f"  Personaggi  : {[c['id'] for c in chars] or '(nessuno)'}")
    print(f"  FaceSwap    : {getattr(args, 'faceswap', False)}")

    t0 = time.time()
    try:
        r = requests.post(f"{base}/generate_scene", json=payload, timeout=300)
    except requests.exceptions.ConnectionError:
        print(f"  ERRORE: server non raggiungibile su {base}")
        sys.exit(1)

    elapsed = time.time() - t0
    if r.status_code != 200:
        print(f"  ERRORE {r.status_code}: {r.text[:400]}")
        sys.exit(1)

    out = args.output or f"scene_{int(time.time())}.png"
    with open(out, "wb") as f:
        f.write(r.content)
    seed = r.headers.get("X-Seed", "?")
    print(f"  Salvato     : {out}  ({len(r.content)//1024} KB)")
    print(f"  Seed        : {seed}")
    print(f"  Tempo       : {elapsed:.1f}s")
    if args.show:
        _open_image(out)


_IMG_EXTS = (".png", ".jpg", ".jpeg", ".webp", ".bmp", ".tiff")

def _quality_line(q: dict) -> str:
    """Format quality metrics into a single display line with PASS/WARN/FAIL indicator."""
    if q is None:
        return ""
    ok    = q.get("quality_ok", True)
    warns = q.get("warnings", [])
    badge = "\033[32mPASS\033[0m" if (ok and not warns) else (
            "\033[33mWARN\033[0m" if (ok and warns) else "\033[31mFAIL\033[0m")

    parts = []
    if q.get("sharpness")   is not None: parts.append(f"sharp={q['sharpness']:.0f}")
    if q.get("siglip_norm") is not None: parts.append(f"siglip={q['siglip_norm']:.2f}")
    if q.get("face_count")  is not None: parts.append(f"faces={q['face_count']}")
    if q.get("face_area_ratio") is not None:
        parts.append(f"face_area={q['face_area_ratio']*100:.1f}%")
    size = q.get("size")
    if size: parts.append(f"{size[0]}×{size[1]}")

    line = f"[{badge}] {' | '.join(parts)}"
    if warns:
        line += f"\n         \033[33m⚠ {'; '.join(warns)}\033[0m"
    return line


def cmd_add_ref(base: str, args):
    _section("Carica reference face")
    if not args.char:
        print("  ERRORE: --char obbligatorio")
        sys.exit(1)

    # Collect files from --file, --files, or --dir
    files_to_upload = []
    if args.dir:
        if not os.path.isdir(args.dir):
            print(f"  ERRORE: directory non trovata: {args.dir!r}")
            sys.exit(1)
        files_to_upload = sorted(
            os.path.join(args.dir, f)
            for f in os.listdir(args.dir)
            if f.lower().endswith(_IMG_EXTS)
        )
        if not files_to_upload:
            print(f"  ERRORE: nessuna immagine in {args.dir!r}")
            sys.exit(1)
        print(f"  Directory: {args.dir}  ({len(files_to_upload)} immagini trovate)")
    elif args.files:
        for f in args.files:
            if not os.path.exists(f):
                print(f"  ERRORE: file non trovato: {f!r}")
                sys.exit(1)
            files_to_upload.append(f)
    elif args.file:
        if not os.path.exists(args.file):
            print(f"  ERRORE: file non trovato: {args.file!r}")
            sys.exit(1)
        files_to_upload.append(args.file)
    else:
        print("  ERRORE: specificare --file, --files, o --dir")
        sys.exit(1)

    save_to_set     = args.set or args.dir is not None or len(files_to_upload) > 1
    auto_build      = getattr(args, "auto_build", False) or args.dir is not None
    min_sharpness   = getattr(args, "min_sharpness", 0.0)
    skip_low        = getattr(args, "skip_low_quality", False)

    # ── Quality pre-check (only for set uploads, to screen before saving) ─────
    if save_to_set and len(files_to_upload) > 1:
        print(f"\n  Pre-check qualità ({len(files_to_upload)} immagini)...")
        checked = []
        for fpath in files_to_upload:
            fname = os.path.basename(fpath)
            try:
                with open(fpath, "rb") as f:
                    r = requests.post(
                        f"{base}/references/check",
                        files={"file": (fname, f, "image/jpeg")},
                        timeout=30,
                    )
                q = r.json() if r.status_code == 200 else None
            except Exception:
                q = None
            qline = _quality_line(q) if q else "(check non disponibile)"
            fname_pad = fname[:36].ljust(36)
            print(f"  {fname_pad}  {qline}")
            ok = (q is None or q.get("quality_ok", True))
            sharp_ok = (q is None or (q.get("sharpness") or 999) >= min_sharpness)
            if skip_low and q and (not ok or not sharp_ok):
                print(f"         \033[31mSkipped (--skip-low-quality)\033[0m")
                continue
            checked.append(fpath)
        if not checked:
            print("\n  Nessuna immagine supera il quality check. Esci.")
            sys.exit(1)
        if len(checked) < len(files_to_upload):
            skipped = len(files_to_upload) - len(checked)
            print(f"\n  {skipped} immagini saltate. Procedo con {len(checked)}.")
            inp = input("  Confermi? [S/n] ").strip().lower()
            if inp and inp != "s":
                sys.exit(0)
        files_to_upload = checked
        print()

    # ── Upload ────────────────────────────────────────────────────────────────
    uploaded = 0
    for fpath in files_to_upload:
        fname = os.path.basename(fpath)
        with open(fpath, "rb") as f:
            r = requests.post(
                f"{base}/references/add",
                data={"character_id": args.char, "save_to_set": str(save_to_set).lower()},
                files={"file": (fname, f, "image/jpeg")},
                timeout=30,
            )
        if r.status_code == 200:
            d = r.json()
            if d.get("status") == "added_to_set":
                q     = d.get("quality", {})
                qline = _quality_line(q) if q else ""
                print(f"  [{d.get('set_size'):>3}] {fname:<36}  {qline}")
            else:
                print(f"  Salvato: {d.get('path')}")
            uploaded += 1
        else:
            print(f"  ERRORE {r.status_code} su {fname}: {r.text[:200]}")

    if save_to_set and uploaded > 0:
        if auto_build:
            print(f"\n  {uploaded} immagini caricate. Build embedding automatico...")
            _do_build_ref(base, args.char)
        else:
            print(f"\n  {uploaded} immagini caricate nel set.")
            print(f"  Esegui:  build-ref --char {args.char}")


def _do_build_ref(base: str, char: str):
    """Internal: call /references/build and print result."""
    t0 = time.time()
    try:
        r = requests.post(f"{base}/references/build", json={"character_id": char}, timeout=120)
    except requests.exceptions.ConnectionError:
        print(f"  ERRORE: server non raggiungibile")
        return
    elapsed = time.time() - t0
    if r.status_code == 200:
        d = r.json()
        print(f"  Embedding salvato: {d.get('path')}")
        print(f"  Immagini usate   : {d.get('n_images')}")
        print(f"  Embedding norm   : {d.get('embed_norm')}")
        print(f"  Tempo            : {elapsed:.1f}s")
    else:
        print(f"  ERRORE build {r.status_code}: {r.text[:300]}")


def cmd_build_ref(base: str, args):
    _section(f"Build embedding: {args.char}")
    if not args.char:
        print("  ERRORE: --char obbligatorio")
        sys.exit(1)
    _do_build_ref(base, args.char)


def cmd_refs(base: str):
    _section("Reference faces")
    r = requests.get(f"{base}/references", timeout=10)
    if r.status_code != 200:
        print(f"  ERRORE {r.status_code}: {r.text}")
        sys.exit(1)
    refs = r.json().get("references", [])
    if not refs:
        print("  (nessuna reference salvata)")
        return
    for ref in refs:
        cid = ref["character_id"]
        parts = []
        if "embedding" in ref:
            e = ref["embedding"]
            parts.append(f"embedding ({e.get('n_images')} img, {e.get('file')})")
        if "set" in ref:
            parts.append(f"set ({ref['set']['n_images']} img)")
        if "single_image" in ref:
            parts.append(f"single ({ref['single_image']})")
        print(f"  {cid:<24}  {' | '.join(parts) or '?'}")


# ─────────────────────────────────────────────────────────────────────────────
def main():
    p = argparse.ArgumentParser(
        description="t2i.py — client per t2i_locale (FLUX.1-dev + IP-Adapter)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument("--url",      default=DEFAULT_URL, help="URL server")
    p.add_argument("--steps",    type=int,   default=None, help="Step inference")
    p.add_argument("--guidance", type=float, default=None, help="Guidance scale")
    p.add_argument("--width",    type=int,   default=None)
    p.add_argument("--height",   type=int,   default=None)
    p.add_argument("--id-scale", type=float, default=0.8, dest="id_scale",
                   help="Forza IP-Adapter face conditioning (default: 0.8)")

    sub = p.add_subparsers(dest="cmd")

    # portrait
    pp = sub.add_parser("portrait", help="Genera ritratto NPC")
    pp.add_argument("prompt")
    pp.add_argument("--char",     default="", help="ID personaggio (usa reference se presente)")
    pp.add_argument("--save-ref", action="store_true", dest="save_ref",
                    help="Salva output come reference per --char")
    pp.add_argument("--id-scale", type=float, default=argparse.SUPPRESS, dest="id_scale",
                    help="Forza IP-Adapter scale (override globale)")
    pp.add_argument("--steps",    type=int,   default=argparse.SUPPRESS,
                    help="Override --steps globale per questa generazione")
    pp.add_argument("--width",    type=int,   default=argparse.SUPPRESS)
    pp.add_argument("--height",   type=int,   default=argparse.SUPPRESS)
    pp.add_argument("--guidance", type=float, default=argparse.SUPPRESS)
    pp.add_argument("--lora",     default=None)
    pp.add_argument("--seed",     type=int, default=-1)
    pp.add_argument("--output",   default=None)
    pp.add_argument("--show",     action="store_true", help="Apri immagine al termine")
    pp.add_argument("--faceswap", action="store_true",
                    help="Abilita face-swap post-processing (richiede --faceswap sul server)")
    pp.add_argument("--faceswap-enhance", action="store_true", dest="faceswap_enhance",
                    help="Applica GFPGAN dopo face-swap")

    # scene
    sp = sub.add_parser("scene", help="Genera scena con più NPC")
    sp.add_argument("prompt")
    sp.add_argument("--chars",    nargs="+", default=[], help="ID personaggi")
    sp.add_argument("--id-scale", type=float, default=argparse.SUPPRESS, dest="id_scale",
                    help="Forza IP-Adapter scale (override globale)")
    sp.add_argument("--steps",    type=int,   default=argparse.SUPPRESS,
                    help="Override --steps globale per questa generazione")
    sp.add_argument("--width",    type=int,   default=argparse.SUPPRESS)
    sp.add_argument("--height",   type=int,   default=argparse.SUPPRESS)
    sp.add_argument("--guidance", type=float, default=argparse.SUPPRESS)
    sp.add_argument("--lora",     default=None)
    sp.add_argument("--seed",     type=int, default=-1)
    sp.add_argument("--output",   default=None)
    sp.add_argument("--show",     action="store_true")
    sp.add_argument("--faceswap", action="store_true",
                    help="Abilita face-swap post-processing per ogni personaggio con reference")
    sp.add_argument("--faceswap-enhance", action="store_true", dest="faceswap_enhance",
                    help="Applica GFPGAN dopo face-swap")

    # add-ref
    rp = sub.add_parser("add-ref", help="Carica reference face per un personaggio")
    rp.add_argument("--char", required=True)
    rp.add_argument("--file",  default=None,
                    help="Singola immagine (sovrascrive, oppure --set per aggiungere al set)")
    rp.add_argument("--files", nargs="+", default=None,
                    help="Più immagini — salva tutte nel set (implica --set)")
    rp.add_argument("--dir",   default=None,
                    help="Directory con immagini — carica tutto il set + build automatico")
    rp.add_argument("--set",   action="store_true",
                    help="Salva in references/{char}/ invece di sovrascrivere references/{char}.png")
    rp.add_argument("--auto-build", action="store_true", dest="auto_build",
                    help="Esegui build-ref automaticamente dopo il caricamento")
    rp.add_argument("--skip-low-quality", action="store_true", dest="skip_low_quality",
                    help="Salta automaticamente immagini che falliscono il quality check")
    rp.add_argument("--min-sharpness", type=float, default=0.0, dest="min_sharpness",
                    help="Salta immagini con sharpness < N (default: 0 = disabilitato)")

    # build-ref
    bp = sub.add_parser("build-ref", help="Calcola embedding pre-computato da set di immagini")
    bp.add_argument("--char", required=True)

    # refs
    sub.add_parser("refs",   help="Lista reference faces e embedding salvati")

    # health
    sub.add_parser("health", help="Stato del server")

    args = p.parse_args()
    base = args.url.rstrip("/")

    if not args.cmd:
        p.print_help()
        sys.exit(1)

    if   args.cmd == "portrait":   cmd_portrait(base, args)
    elif args.cmd == "scene":      cmd_scene(base, args)
    elif args.cmd == "add-ref":    cmd_add_ref(base, args)
    elif args.cmd == "build-ref":  cmd_build_ref(base, args)
    elif args.cmd == "refs":       cmd_refs(base)
    elif args.cmd == "health":     cmd_health(base)


if __name__ == "__main__":
    main()
