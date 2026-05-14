#!/usr/bin/env python3
"""
Test script for tts_locale server.

Usage:
    # Health check only
    python3 test_tts.py

    # Add a voice profile and synthesize test text
    python3 test_tts.py --voice-file my_voice.wav --voice-id elena

    # Synthesize custom text with existing profile
    python3 test_tts.py --voice-id elena --text "Cosa fai qui di notte?"

    # Change language
    python3 test_tts.py --voice-id elena --text "Hello there." --lang en
"""

import argparse
import subprocess
import sys
import time

try:
    import requests
except ImportError:
    print("Missing: pip install requests")
    sys.exit(1)

BASE = "http://127.0.0.1:8004"
DEFAULT_TEXT = "Ciao, sono Elena. Cosa fai qui di notte? Non dovresti essere in giro a quest'ora."


def print_section(title: str):
    print(f"\n{'─' * 50}")
    print(f"  {title}")
    print(f"{'─' * 50}")


def health_check():
    print_section("Health")
    try:
        r = requests.get(f"{BASE}/health", timeout=5)
        r.raise_for_status()
        d = r.json()
        print(f"  Status : {d.get('status')}")
        print(f"  Model  : {d.get('model')}")
        print(f"  Device : {d.get('device')}")
        print(f"  Voices : {d.get('voices')}")
        return True
    except requests.exceptions.ConnectionError:
        print("  ERROR: Server not running. Start with: python3 server.py")
        return False
    except Exception as e:
        print(f"  ERROR: {e}")
        return False


def list_voices():
    print_section("Available voices")
    r = requests.get(f"{BASE}/voices")
    voices = r.json().get("voices", [])
    if voices:
        for v in voices:
            print(f"  - {v}")
    else:
        print("  (none — add one with --voice-file)")


def add_voice(wav_path: str, voice_id: str):
    print_section(f"Adding voice '{voice_id}' from {wav_path}")
    with open(wav_path, "rb") as f:
        r = requests.post(
            f"{BASE}/voices/add",
            files={"file": (wav_path, f, "audio/wav")},
            data={"voice_id": voice_id},
        )
    d = r.json()
    if r.status_code == 200:
        print(f"  OK → {d.get('path')}")
        print(f"  Note: {d.get('note')}")
    else:
        print(f"  ERROR {r.status_code}: {d}")
        sys.exit(1)


def synthesize(voice_id: str, text: str, lang: str, output: str):
    print_section(f"Synthesizing with voice '{voice_id}'")
    print(f"  Text     : {text[:80]}{'...' if len(text) > 80 else ''}")
    print(f"  Language : {lang}")

    t0 = time.perf_counter()
    r = requests.post(
        f"{BASE}/synthesize",
        data={"text": text, "voice_id": voice_id, "language": lang},
        timeout=120,
    )
    elapsed = time.perf_counter() - t0

    if r.status_code != 200:
        print(f"  ERROR {r.status_code}: {r.text}")
        sys.exit(1)

    with open(output, "wb") as f:
        f.write(r.content)

    size_kb = len(r.content) / 1024
    print(f"  Time     : {elapsed:.2f}s")
    print(f"  Output   : {output} ({size_kb:.1f} KB)")
    print(f"  Chars    : {len(text)} → {elapsed/len(text)*1000:.1f} ms/char")

    # Try to play
    played = False
    for player in ("afplay", "aplay", "ffplay"):
        try:
            if player == "ffplay":
                subprocess.run(["ffplay", "-nodisp", "-autoexit", output],
                               capture_output=True, timeout=30)
            else:
                subprocess.run([player, output], capture_output=True, timeout=30)
            print(f"  Played via {player}")
            played = True
            break
        except (FileNotFoundError, subprocess.TimeoutExpired):
            continue

    if not played:
        print(f"  (no audio player found — open {output} manually)")


def main():
    parser = argparse.ArgumentParser(description="Test tts_locale server")
    parser.add_argument("--url",        default="http://127.0.0.1:8004", help="Server base URL")
    parser.add_argument("--voice-file", help="WAV reference file to upload")
    parser.add_argument("--voice-id",   help="Voice profile name")
    parser.add_argument("--text",       default=DEFAULT_TEXT, help="Text to synthesize")
    parser.add_argument("--lang",       default="it", help="Language code (default: it)")
    parser.add_argument("--output",     default="test_output.wav", help="Output WAV filename")
    args = parser.parse_args()

    global BASE
    BASE = args.url.rstrip("/")

    if not health_check():
        sys.exit(1)

    list_voices()

    if args.voice_file:
        if not args.voice_id:
            print("ERROR: --voice-id required when using --voice-file")
            sys.exit(1)
        add_voice(args.voice_file, args.voice_id)

    if args.voice_id:
        synthesize(args.voice_id, args.text, args.lang, args.output)


if __name__ == "__main__":
    main()
