#!/usr/bin/env bash
# faceswap_locale/start.sh — setup venv (first run) and start the server
set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENV="$SCRIPT_DIR/.venv"

if [ ! -d "$VENV" ]; then
    echo "[faceswap_locale] Creating venv..."
    python3 -m venv "$VENV"
    source "$VENV/bin/activate"
    echo "[faceswap_locale] Installing dependencies..."
    pip install --upgrade pip -q
    pip install -r "$SCRIPT_DIR/requirements.txt"
    echo "[faceswap_locale] Setup complete."
else
    source "$VENV/bin/activate"
fi

mkdir -p "$SCRIPT_DIR/models"
mkdir -p "$SCRIPT_DIR/faces"

exec python "$SCRIPT_DIR/server.py" \
    --host 127.0.0.1 \
    --port 8001 \
    "$@"
