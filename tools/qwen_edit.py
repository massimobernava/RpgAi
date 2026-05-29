#!/usr/bin/env python3
"""
qwen_edit.py — Invia un'immagine al server Qwen locale (remoto) per modificarla.

Utilizzo:
    python3 tools/qwen_edit.py <immagine> "<modifica>" [opzioni]

Opzioni:
    --url       <url>   URL server Qwen (default: http://192.168.1.41:8000)
    --output    <path>  Salva risultato (default: stessa cartella, suffisso _qwen.png)
    --lora      <nome>  LoRA da usare (opzionale, nome cartella in ./loras/)
    --lora-scale <n>    Scala LoRA (default: 1.0)
    --steps     <n>     Step inference (default: 4)
    --strength  <f>     Strength i2i — 0.0=nessuna modifica, 1.0=rifacimento totale (default: 0.75)
    --guidance  <f>     Guidance scale (default: 1.0)
    --show              Apri immagine risultato dopo la generazione

Esempi:
    python3 tools/qwen_edit.py scene.jpg "aggiungi nebbia mattutina"
    python3 tools/qwen_edit.py scene.jpg "cambia il cielo in tramonto" --strength 0.5 --show
    python3 tools/qwen_edit.py scene.jpg "stile anime" --lora anime_style --lora-scale 0.8
    python3 tools/qwen_edit.py scene.jpg "rimuovi la persona a sinistra" --url http://localhost:8000
"""

import os
import sys
import argparse
import subprocess

try:
    import requests
except ImportError:
    print("[ERROR] 'requests' non trovato. Installa: pip install requests", file=sys.stderr)
    sys.exit(1)


def check_health(url: str) -> bool:
    try:
        r = requests.get(url.rstrip("/") + "/health", timeout=5)
        r.raise_for_status()
        info = r.json()
        print(f"[qwen] Server ok — modello: {info.get('model','?')} | device: {info.get('device','?')} | lightning: {info.get('lightning','?')}")
        loras = info.get("loras", [])
        if loras:
            print(f"[qwen] LoRA attive: {', '.join(loras)}")
        return True
    except requests.exceptions.ConnectionError:
        print(f"[ERROR] Server non raggiungibile: {url}", file=sys.stderr)
        return False
    except Exception as e:
        print(f"[WARN] Health check fallito: {e}", file=sys.stderr)
        return False


def edit_image(
    url: str,
    image_path: str,
    prompt: str,
    lora_name: str | None,
    lora_scale: float,
    steps: int,
    strength: float,
    guidance: float,
) -> bytes:
    endpoint = url.rstrip("/") + "/edit"
    data = {
        "prompt": prompt,
        "steps": str(steps),
        "strength": str(strength),
        "guidance_scale": str(guidance),
        "lora_scale": str(lora_scale),
    }
    if lora_name:
        data["lora_name"] = lora_name

    with open(image_path, "rb") as f:
        files = {"images": (os.path.basename(image_path), f, "image/jpeg")}
        try:
            print(f"[qwen] POST {endpoint}")
            print(f"[qwen] Prompt : {prompt}")
            print(f"[qwen] Steps  : {steps}  Strength: {strength}  Guidance: {guidance}")
            if lora_name:
                print(f"[qwen] LoRA   : {lora_name} (scala: {lora_scale})")
            r = requests.post(endpoint, data=data, files=files, timeout=300)
            r.raise_for_status()
            return r.content
        except requests.exceptions.ConnectionError:
            print(f"[ERROR] Connessione a {endpoint} fallita.", file=sys.stderr)
            sys.exit(1)
        except requests.exceptions.Timeout:
            print("[ERROR] Timeout (300s). Il server è ancora in elaborazione?", file=sys.stderr)
            sys.exit(1)
        except requests.exceptions.HTTPError:
            print(f"[ERROR] HTTP {r.status_code}: {r.text}", file=sys.stderr)
            sys.exit(1)


def default_output_path(image_path: str) -> str:
    base, _ = os.path.splitext(image_path)
    return base + "_qwen.png"


def open_image(path: str):
    for cmd in ("open", "eog", "xdg-open", "display"):
        try:
            subprocess.Popen([cmd, path])
            return
        except FileNotFoundError:
            continue
    print(f"[WARN] Nessun viewer trovato. Immagine salvata in: {path}", file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(
        description="Invia un'immagine al server Qwen locale per modificarla.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument("image",        help="Path immagine sorgente")
    parser.add_argument("prompt",       help="Descrizione della modifica da applicare")
    parser.add_argument("--url",        default="http://192.168.1.41:8000", help="URL server Qwen")
    parser.add_argument("--output",     default=None,   help="Path output PNG (default: <immagine>_qwen.png)")
    parser.add_argument("--lora",       default=None,   help="Nome LoRA (cartella in ./loras/)")
    parser.add_argument("--lora-scale", type=float, default=1.0, help="Scala LoRA (default: 1.0)")
    parser.add_argument("--steps",      type=int,   default=4,   help="Step inference (default: 4)")
    parser.add_argument("--strength",   type=float, default=0.75, help="Strength i2i (default: 0.75)")
    parser.add_argument("--guidance",   type=float, default=1.0, help="Guidance scale (default: 1.0)")
    parser.add_argument("--show",       action="store_true", help="Apri immagine risultato")
    args = parser.parse_args()

    if not os.path.isfile(args.image):
        print(f"[ERROR] Immagine non trovata: {args.image}", file=sys.stderr)
        sys.exit(1)

    output_path = args.output or default_output_path(args.image)

    print(f"[qwen] Immagine : {args.image}")
    print(f"[qwen] Output   : {output_path}")
    print(f"[qwen] Server   : {args.url}")

    if not check_health(args.url):
        sys.exit(1)

    png_bytes = edit_image(
        url=args.url,
        image_path=args.image,
        prompt=args.prompt,
        lora_name=args.lora,
        lora_scale=args.lora_scale,
        steps=args.steps,
        strength=args.strength,
        guidance=args.guidance,
    )

    with open(output_path, "wb") as f:
        f.write(png_bytes)

    print(f"[qwen] Salvato : {output_path}  ({len(png_bytes) // 1024} KB)")

    if args.show:
        open_image(output_path)


if __name__ == "__main__":
    main()
