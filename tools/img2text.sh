#!/usr/bin/env bash
# img2text.sh — Wrapper per img2text.py con defaults per questo progetto.
#
# Utilizzo rapido:
#   ./tools/img2text.sh <image>              # descrizione generica
#   ./tools/img2text.sh <image> --scene      # analisi scena VN
#   ./tools/img2text.sh <image> --debug      # debug layer compositing
#
# Override da env:
#   OLLAMA_URL=http://192.168.1.10:11434 ./tools/img2text.sh <image>
#   VISION_MODEL=llava ./tools/img2text.sh <image>
#
# Esempi concreti:
#   ./tools/img2text.sh scripts/images/scene.png --debug
#   ./tools/img2text.sh scripts/images/background.png --scene

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"

OLLAMA_URL="${OLLAMA_URL:-http://192.168.1.41:11434}"
VISION_MODEL="${VISION_MODEL:-Fermi/Cydonia-24B-v4.3-heretic-vision:Q4_K_M}"

if [[ $# -lt 1 ]]; then
    echo "Uso: $0 <image_path> [--scene | --debug | --prompt '...' | --json]"
    echo "     OLLAMA_URL=http://host:11434 $0 <image>  (per server remoto)"
    echo "     VISION_MODEL=llava $0 <image>            (per modello diverso)"
    exit 1
fi

IMAGE="$1"
shift

# Risolvi path relativo dalla root del progetto se il file non esiste as-is
if [[ ! -f "$IMAGE" && -f "$ROOT_DIR/$IMAGE" ]]; then
    IMAGE="$ROOT_DIR/$IMAGE"
fi

if [[ ! -f "$IMAGE" ]]; then
    echo "[ERROR] File non trovato: $IMAGE" >&2
    exit 1
fi

exec python3 "$SCRIPT_DIR/img2text.py" \
    "$IMAGE" \
    --model "$VISION_MODEL" \
    --url   "$OLLAMA_URL" \
    "$@"
