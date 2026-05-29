#!/usr/bin/env bash
# qwen_edit.sh — Wrapper per qwen_edit.py con configurazione predefinita.
#
# ─── MODIFICA QUI ────────────────────────────────────────────
#   1. Imposta QWEN_URL con l'indirizzo del server remoto
#   2. Poi esegui:  ./tools/qwen_edit.sh immagine.jpg "modifica"
# ─────────────────────────────────────────────────────────────
#
# Esempi:
#   ./tools/qwen_edit.sh scene.jpg "aggiungi nebbia mattutina"
#   ./tools/qwen_edit.sh scene.jpg "cambia il cielo in tramonto" --strength 0.5 --show
#   ./tools/qwen_edit.sh scene.jpg "stile anime" --lora anime_style
#   ./tools/qwen_edit.sh scene.jpg "rimuovi la persona a sinistra" --output out.png
#
# Tutti gli argomenti extra passano direttamente a qwen_edit.py.

set -euo pipefail

# ─── CONFIGURAZIONE ──────────────────────────────────────────
QWEN_URL="http://192.168.1.41:8000"
STEPS=4           # Step inference (Lightning: 4 step; senza Lightning: 20-30)
STRENGTH=0.75     # 0.0=nessuna modifica, 1.0=rifacimento totale
GUIDANCE=1.0      # Guidance scale (Lightning: 1.0; senza: 3.5-7.0)
# ─────────────────────────────────────────────────────────────

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENV_DIR="$SCRIPT_DIR/.venv_qwen"

if [[ $# -lt 2 ]]; then
    echo "Uso: $0 <immagine> \"<modifica>\" [opzioni extra]"
    echo ""
    echo "Esempi:"
    echo "  $0 scene.jpg \"aggiungi nebbia mattutina\""
    echo "  $0 scene.jpg \"cambia il cielo in tramonto\" --strength 0.5 --show"
    echo "  $0 scene.jpg \"stile anime\" --lora anime_style --lora-scale 0.8"
    echo "  $0 scene.jpg \"rimuovi la persona a sinistra\" --output out.png"
    echo ""
    echo "Opzioni:"
    echo "  --url       URL server (default: $QWEN_URL)"
    echo "  --output    Path PNG output"
    echo "  --lora      Nome LoRA (cartella in ./loras/)"
    echo "  --lora-scale  Scala LoRA (default: 1.0)"
    echo "  --steps     Step inference (default: $STEPS)"
    echo "  --strength  Strength i2i (default: $STRENGTH)"
    echo "  --guidance  Guidance scale (default: $GUIDANCE)"
    echo "  --show      Apri immagine risultato"
    exit 1
fi

# Setup venv se non esiste
if [[ ! -d "$VENV_DIR" ]]; then
    echo "[qwen_edit] Setup venv in $VENV_DIR ..."
    python3 -m venv "$VENV_DIR"
    "$VENV_DIR/bin/pip" install --quiet --upgrade pip
    "$VENV_DIR/bin/pip" install --quiet requests
    echo "[qwen_edit] Venv pronto."
fi

exec "$VENV_DIR/bin/python3" "$SCRIPT_DIR/qwen_edit.py" \
    --url      "$QWEN_URL"  \
    --steps    "$STEPS"     \
    --strength "$STRENGTH"  \
    --guidance "$GUIDANCE"  \
    "$@"
