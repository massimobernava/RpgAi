#!/usr/bin/env python3
"""
img2text.py — Descrive un'immagine usando un modello vision su ollama.

Utilizzo:
    python3 tools/img2text.py <image_path> [opzioni]

Opzioni:
    --model   <nome>   Modello vision (default: llama3.2-vision)
    --prompt  <testo>  Prompt da inviare (default: descrizione generica)
    --url     <url>    URL base di ollama (default: http://localhost:11434)
    --json             Stampa il JSON grezzo della risposta
    --scene            Prompt ottimizzato per descrivere scene di gioco (composizione, background, personaggi)
    --debug            Prompt ottimizzato per debug layer compositing (verifica montaggio layer PNG)

Esempi:
    python3 tools/img2text.py scripts/images/scene.png
    python3 tools/img2text.py scripts/images/scene.png --scene
    python3 tools/img2text.py scripts/images/scene.png --debug
    python3 tools/img2text.py image.png --model llava --url http://192.168.1.10:11434
"""

import sys
import json
import base64
import argparse
import urllib.request
import urllib.error


PROMPT_DEFAULT = (
    "Describe this image in detail. Focus on: the setting/background, "
    "any characters present (appearance, clothing, pose, expression), "
    "the overall mood and lighting."
)

PROMPT_SCENE = (
    "You are analyzing a visual novel scene. Describe: "
    "1) The background/setting (room, time of day, objects visible). "
    "2) Any characters (name if recognizable, clothing, pose, facial expression, position in frame). "
    "3) The overall mood and atmosphere. "
    "Be specific and concise."
)

PROMPT_DEBUG = (
    "You are checking a composite PNG image made by alpha-blending multiple layers. "
    "Describe what you see: background, any character or sprite visible, "
    "whether layers appear correctly composited (no obvious z-order errors, "
    "no transparent artifacts, no misaligned elements). "
    "Report any visual glitches or misalignments you notice."
)


def load_image_b64(path: str) -> str:
    with open(path, "rb") as f:
        return base64.b64encode(f.read()).decode("utf-8")


def call_ollama(image_b64: str, prompt: str, model: str, base_url: str) -> dict:
    url = base_url.rstrip("/") + "/api/generate"
    payload = {
        "model": model,
        "prompt": prompt,
        "images": [image_b64],
        "stream": False,
    }
    data = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(
        url,
        data=data,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=120) as resp:
            return json.loads(resp.read().decode("utf-8"))
    except urllib.error.URLError as e:
        print(f"[ERROR] Cannot reach ollama at {url}: {e}", file=sys.stderr)
        sys.exit(1)
    except json.JSONDecodeError as e:
        print(f"[ERROR] Invalid JSON response: {e}", file=sys.stderr)
        sys.exit(1)


def main():
    parser = argparse.ArgumentParser(
        description="Describe an image using an ollama vision model.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument("image", help="Path to the image file (PNG, JPG, WebP)")
    parser.add_argument("--model",  default="llama3.2-vision", help="Vision model name")
    parser.add_argument("--prompt", default=None, help="Custom prompt (overrides --scene/--debug)")
    parser.add_argument("--url",    default="http://localhost:11434", help="Ollama base URL")
    parser.add_argument("--json",   action="store_true", dest="raw_json", help="Print raw JSON response")
    parser.add_argument("--scene",  action="store_true", help="Use scene-analysis prompt")
    parser.add_argument("--debug",  action="store_true", help="Use layer-debug prompt")
    args = parser.parse_args()

    # Determine prompt
    if args.prompt:
        prompt = args.prompt
    elif args.debug:
        prompt = PROMPT_DEBUG
    elif args.scene:
        prompt = PROMPT_SCENE
    else:
        prompt = PROMPT_DEFAULT

    print(f"[img2text] model={args.model}  url={args.url}", file=sys.stderr)
    print(f"[img2text] image={args.image}", file=sys.stderr)
    print(f"[img2text] prompt={prompt[:60]}...", file=sys.stderr)
    print("", file=sys.stderr)

    image_b64 = load_image_b64(args.image)
    result = call_ollama(image_b64, prompt, args.model, args.url)

    if args.raw_json:
        print(json.dumps(result, indent=2, ensure_ascii=False))
    else:
        response_text = result.get("response", "[no response field in JSON]")
        print(response_text)


if __name__ == "__main__":
    main()
