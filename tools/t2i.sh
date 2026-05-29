#!/usr/bin/env bash
# t2i.sh — Wrapper per t2i.py con configurazione predefinita.
#
# ─── MODIFICA QUI ────────────────────────────────────────────
#   1. Imposta T2I_URL con l'indirizzo del server remoto
#   2. Poi esegui:  ./tools/t2i.sh portrait "una donna con capelli rossi"
# ─────────────────────────────────────────────────────────────
#
# Esempi:
#   ./tools/t2i.sh portrait "ritratto di una donna, capelli castani, luce soffusa"
#   ./tools/t2i.sh portrait "jenny, giovane donna sorridente" --char jenny --save-ref
#   ./tools/t2i.sh portrait "jenny al bar" --char jenny --id-scale 0.9 --show
#   ./tools/t2i.sh portrait "jenny al bar" --char jenny --faceswap
#   ./tools/t2i.sh portrait "jenny al bar" --char jenny --faceswap --faceswap-enhance
#   ./tools/t2i.sh scene "due donne sedute al bar, luce pomeridiana" --chars jenny debbie
#   ./tools/t2i.sh scene "jenny e debbie" --chars jenny debbie --faceswap
#   ./tools/t2i.sh add-ref --char jenny --file jenny.jpg                  # reference singola
#   ./tools/t2i.sh add-ref --char jenny --file foto1.jpg --set            # aggiungi al set
#   ./tools/t2i.sh add-ref --char jenny --files foto1.jpg foto2.jpg       # multi-upload
#   ./tools/t2i.sh add-ref --char jenny --dir ./foto_jenny/               # intera directory (+ auto build)
#   ./tools/t2i.sh add-ref --char jenny --dir ./foto/ --skip-low-quality  # con filtro qualità
#   ./tools/t2i.sh build-ref --char jenny                                 # calcola embedding dal set
#   ./tools/t2i.sh refs
#   ./tools/t2i.sh health
#
# Tutti gli argomenti extra passano direttamente a t2i.py.

set -euo pipefail

# ─── CONFIGURAZIONE ──────────────────────────────────────────
T2I_URL="http://192.168.1.41:8003"
STEPS=20          # Step inference (FLUX.1-dev: 20-28 ottimali)
GUIDANCE=3.5      # Guidance scale (FLUX.1-dev: 3.5-7.0)
WIDTH=512         # Larghezza default per ritratti
HEIGHT=768        # Altezza default per ritratti
ID_SCALE=0.8      # Forza IP-Adapter face conditioning (0.0-1.0)
# ─────────────────────────────────────────────────────────────

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENV_DIR="$SCRIPT_DIR/.venv_t2i"

if [[ $# -lt 1 ]]; then
    echo "Uso: $0 <comando> [argomenti]"
    echo ""
    echo "Comandi:"
    echo "  portrait  \"<prompt>\"                          Genera ritratto NPC"
    echo "  scene     \"<prompt>\"                          Genera scena con più NPC"
    echo "  add-ref   --char <id> --file <img> [--set]    Reference singola (--set = aggiungi al set)"
    echo "  add-ref   --char <id> --files <img...>        Multi-upload nel set"
    echo "  add-ref   --char <id> --dir <cartella>        Intera directory + build automatico"
    echo "  build-ref --char <id>                         Calcola embedding pre-computato dal set"
    echo "  refs                                           Lista reference e embedding salvati"
    echo "  health                                         Stato del server"
    echo ""
    echo "Esempi:"
    echo "  $0 portrait \"ritratto di una donna, capelli castani\""
    echo "  $0 portrait \"jenny sorridente\" --char jenny --save-ref"
    echo "  $0 portrait \"jenny al bar\" --char jenny --id-scale 0.5"
    echo "  $0 scene \"due donne al bar\" --chars jenny debbie"
    echo "  $0 add-ref --char jenny --dir ./foto_jenny/ --skip-low-quality"
    echo "  $0 build-ref --char jenny"
    echo ""
    echo "Opzioni portrait/scene:"
    echo "  --char      ID personaggio (usa reference/embedding se presente)"
    echo "  --chars     Lista ID personaggi (solo scene)"
    echo "  --save-ref  Salva output come reference per --char"
    echo "  --id-scale  Forza IP-Adapter (default: $ID_SCALE, consigliato 0.4-0.6)"
    echo "  --steps     Step inference (default: $STEPS)"
    echo "  --width     Larghezza (default: $WIDTH)"
    echo "  --height    Altezza (default: $HEIGHT)"
    echo "  --lora      Nome LoRA"
    echo "  --seed      Seed (default: random)"
    echo "  --output    Path output PNG"
    echo "  --show      Apri immagine risultato"
    echo "  --url       URL server (default: $T2I_URL)"
    exit 1
fi

# Setup venv se non esiste
if [[ ! -d "$VENV_DIR" ]]; then
    echo "[t2i] Setup venv in $VENV_DIR ..."
    python3 -m venv "$VENV_DIR"
    "$VENV_DIR/bin/pip" install --quiet --upgrade pip
    "$VENV_DIR/bin/pip" install --quiet requests
    echo "[t2i] Venv pronto."
fi

exec "$VENV_DIR/bin/python3" "$SCRIPT_DIR/t2i.py" \
    --url      "$T2I_URL"  \
    --steps    "$STEPS"    \
    --guidance "$GUIDANCE" \
    --width    "$WIDTH"    \
    --height   "$HEIGHT"   \
    --id-scale "$ID_SCALE" \
    "$@"
