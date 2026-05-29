#!/usr/bin/env bash
# narrator.sh — Wrapper per narrator.py con configurazioni.
#
# ─── MODIFICA QUI ────────────────────────────────────────────
#   1. Imposta OLLAMA_URL, TEXT_MODEL, TTS_URL, VOICE (obbligatori)
#   2. Poi esegui:  ./tools/narrator.sh "trama della storia"
# ─────────────────────────────────────────────────────────────
#
# Esempio:
#   ./tools/narrator.sh "Un detective cinico indaga su un omicidio in una città piovosa degli anni '40"
#
# Ctrl+C per interrompere.
#
# Override da riga di comando (tutti gli argomenti extra passano a narrator.py):
#   ./tools/narrator.sh "trama" --lang en --words 200

set -euo pipefail

# ─── CONFIGURAZIONE ──────────────────────────────────────────
OLLAMA_URL="http://192.168.1.41:11434"
TEXT_MODEL="Fermi/Cydonia-24B-v4.3-heretic-vision:Q4_K_M"       # Modello testo (non vision)
TTS_URL="http://192.168.1.41:8004"
VOICE="F"                   # Voice profile registrato nel TTS server (GET /voices per lista)
LANG="it"                   # Lingua: it, en, fr, de, es, pt, ...
WORDS_PER_SEGMENT=120       # Parole target per segmento narrato
MAX_HISTORY=10              # Scambi in memoria (più alto = più coerenza, più token)
# ─────────────────────────────────────────────────────────────

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENV_DIR="$SCRIPT_DIR/.venv_narrator"

if [[ $# -lt 1 ]]; then
    echo "Uso: $0 \"trama della storia\" [opzioni extra]"
    echo ""
    echo "Esempi:"
    echo "  $0 \"Un detective cinico indaga su un omicidio in una città piovosa\""
    echo "  $0 \"Una strega solitaria scopre un portale nel bosco\" --words 80"
    echo "  $0 \"Storia fantasy\" --lang en --voice my_voice"
    exit 1
fi

# Setup venv se non esiste
if [[ ! -d "$VENV_DIR" ]]; then
    echo "[narrator] Setup venv in $VENV_DIR ..."
    python3 -m venv "$VENV_DIR"
    "$VENV_DIR/bin/pip" install --quiet --upgrade pip
    "$VENV_DIR/bin/pip" install --quiet requests
    echo "[narrator] Venv pronto."
fi

exec "$VENV_DIR/bin/python3" "$SCRIPT_DIR/narrator.py" \
    --ollama-url  "$OLLAMA_URL"          \
    --model       "$TEXT_MODEL"          \
    --tts-url     "$TTS_URL"             \
    --voice       "$VOICE"               \
    --lang        "$LANG"                \
    --words       "$WORDS_PER_SEGMENT"   \
    --max-history "$MAX_HISTORY"         \
    "$@"
