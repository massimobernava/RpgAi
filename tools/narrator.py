#!/usr/bin/env python3
"""
narrator.py — Narratore AI: Ollama genera storia in segmenti, TTS la legge ad alta voce in loop.

Utilizzo:
    python3 tools/narrator.py "trama" --model llama3.2 --voice elena [opzioni]

Opzioni:
    --ollama-url  <url>   URL Ollama (default: http://localhost:11434)
    --model       <nome>  Modello testo Ollama (richiesto)
    --tts-url     <url>   URL server TTS (default: http://localhost:8004)
    --voice       <id>    Voice profile TTS (richiesto)
    --lang        <code>  Lingua (default: it)
    --words       <n>     Parole target per segmento (default: 120)
    --max-history <n>     Max scambi in memoria — evita overflow contesto (default: 10)

Ctrl+C per interrompere.
"""

import os
import re
import sys
import json
import argparse
import tempfile
import subprocess
import urllib.request
import urllib.error

try:
    import requests
except ImportError:
    print("[ERROR] 'requests' non trovato. Usa narrator.sh che gestisce il venv.", file=sys.stderr)
    sys.exit(1)


SYSTEM_PROMPT = """\
Sei un narratore che racconta storie in modo evocativo e coinvolgente.
Narra in terza persona con ritmo teatrale, ricco di dettagli sensoriali.
Ogni risposta deve essere un segmento di narrazione di circa {words} parole.
Non aggiungere titoli, intestazioni, numerazioni o meta-commenti. Solo narrazione pura.
Ogni segmento deve concludersi in modo che possa continuare naturalmente al prossimo.

La trama è la seguente:
{trama}
"""


def call_ollama(messages: list, model: str, base_url: str) -> str:
    url = base_url.rstrip("/") + "/api/chat"
    payload = {"model": model, "messages": messages, "stream": False}
    data = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(
        url, data=data,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=120) as resp:
            result = json.loads(resp.read().decode("utf-8"))
            return result["message"]["content"]
    except urllib.error.URLError as e:
        print(f"\n[ERROR] Ollama non raggiungibile a {url}: {e}", file=sys.stderr)
        sys.exit(1)
    except (json.JSONDecodeError, KeyError) as e:
        print(f"\n[ERROR] Risposta Ollama non valida: {e}", file=sys.stderr)
        sys.exit(1)


def synthesize(text: str, voice: str, lang: str, tts_url: str) -> bytes:
    url = tts_url.rstrip("/") + "/synthesize"
    try:
        r = requests.post(
            url,
            data={"text": text, "voice_id": voice, "language": lang},
            timeout=180,
        )
        r.raise_for_status()
        return r.content
    except requests.exceptions.ConnectionError:
        print(f"\n[ERROR] TTS server non raggiungibile a {url}", file=sys.stderr)
        sys.exit(1)
    except requests.exceptions.HTTPError:
        print(f"\n[ERROR] TTS errore {r.status_code}: {r.text}", file=sys.stderr)
        sys.exit(1)


def split_tts_chunks(text: str, max_chars: int = 250) -> list:
    """Split text at clause boundaries into chunks safe for XTTS (400 token hard limit).
    Uses character count — XTTS tokenizer is roughly character-level."""
    parts = re.split(r'(?<=[.!?,;])\s+', text.strip())
    chunks, current, current_len = [], [], 0
    for part in parts:
        part_len = len(part)
        if current_len + part_len + 1 > max_chars and current:
            chunks.append(" ".join(current))
            current, current_len = [part], part_len
        else:
            current.append(part)
            current_len += part_len + 1
    if current:
        chunks.append(" ".join(current))
    return [c for c in chunks if c.strip()]


def play_wav(wav_bytes: bytes):
    with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as tmp:
        tmp.write(wav_bytes)
        tmp_path = tmp.name
    try:
        played = False
        for player in ("afplay", "aplay", "ffplay"):
            try:
                cmd = (
                    ["ffplay", "-nodisp", "-autoexit", tmp_path]
                    if player == "ffplay"
                    else [player, tmp_path]
                )
                subprocess.run(cmd, capture_output=True, check=False)
                played = True
                break
            except FileNotFoundError:
                continue
        if not played:
            print("[WARN] Nessun player trovato (afplay/aplay/ffplay). Audio non riprodotto.", file=sys.stderr)
    finally:
        try:
            os.unlink(tmp_path)
        except OSError:
            pass


def trim_history(messages: list, system_prompt: str, max_history: int) -> list:
    """Keep system prompt + last max_history assistant/user pairs."""
    # messages layout: [system, user, assistant, user, assistant, ...]
    non_system = [m for m in messages if m["role"] != "system"]
    if len(non_system) > max_history * 2:
        non_system = non_system[-(max_history * 2):]
    return [{"role": "system", "content": system_prompt}] + non_system


def main():
    parser = argparse.ArgumentParser(
        description="Narratore AI: Ollama genera storia in loop, TTS la legge ad alta voce.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument("trama",        help="Trama della storia da narrare")
    parser.add_argument("--ollama-url", default="http://localhost:11434", help="URL Ollama")
    parser.add_argument("--model",      required=True,                   help="Modello testo Ollama")
    parser.add_argument("--tts-url",    default="http://localhost:8004",  help="URL server TTS")
    parser.add_argument("--voice",      required=True,                   help="Voice profile TTS")
    parser.add_argument("--lang",       default="it",                    help="Lingua (default: it)")
    parser.add_argument("--words",      type=int, default=120,           help="Parole target per segmento (default: 120)")
    parser.add_argument("--max-history",type=int, default=10,            help="Max scambi narrativi in memoria (default: 10)")
    args = parser.parse_args()

    system_prompt = SYSTEM_PROMPT.format(words=args.words, trama=args.trama)

    messages = [
        {"role": "system", "content": system_prompt},
        {"role": "user",   "content": "Inizia la narrazione."},
    ]

    segment = 0
    print(f"[narrator] Trama : {args.trama[:80]}{'...' if len(args.trama) > 80 else ''}")
    print(f"[narrator] Modello: {args.model} | Voce: {args.voice} | Lingua: {args.lang}")
    print(f"[narrator] Ollama: {args.ollama_url} | TTS: {args.tts_url}")
    print("[narrator] Ctrl+C per interrompere.\n")

    try:
        while True:
            segment += 1
            print(f"[{segment}] Generazione...", end=" ", flush=True)
            text = call_ollama(messages, args.model, args.ollama_url)
            word_count = len(text.split())
            print(f"{word_count} parole")

            print(f"\n{'─' * 60}")
            print(text)
            print(f"{'─' * 60}\n")

            # Append to history, then trim if needed
            messages.append({"role": "assistant", "content": text})
            messages.append({"role": "user",      "content": "Continua."})
            messages = trim_history(messages, system_prompt, args.max_history)

            chunks = split_tts_chunks(text)
            for ci, chunk in enumerate(chunks, 1):
                print(f"[{segment}.{ci}/{len(chunks)}] Sintesi...", end=" ", flush=True)
                wav = synthesize(chunk, args.voice, args.lang, args.tts_url)
                print(f"{len(wav) // 1024} KB — riproduzione...", flush=True)
                play_wav(wav)

    except KeyboardInterrupt:
        print(f"\n[narrator] Interrotto dopo {segment} segmenti.")


if __name__ == "__main__":
    main()
