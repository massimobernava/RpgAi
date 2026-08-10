#!/usr/bin/env bash
# video_assemble.sh — Assemble swapped frames into final video
#
# Usage:
#   ./faceswap_locale/video_assemble.sh <manifest.json> <swapped_dir> <output.mp4> [options]
#
# Options:
#   --audio   <audio.aac>       audio track to mux in (default: <output_dir>/audio.aac if exists)
#   --fps     <N>               frame rate (default: 30)
#   --restore codeformer|gfpgan apply face restoration before encode (default: none)
#   --fidelity <0-1>            CodeFormer fidelity (default: 0.7)
#   --upscale <0|2|4>           upscale factor (default: 0 = disabled)
#
# Examples:
#   ./faceswap_locale/video_assemble.sh saves/video_work/clip1/manifest.json \
#       saves/video_work/clip1/frames_swapped output.mp4
#
#   ./faceswap_locale/video_assemble.sh saves/video_work/clip1/manifest.json \
#       saves/video_work/clip1/frames_swapped output.mp4 \
#       --restore codeformer --fidelity 0.8 --upscale 2

set -euo pipefail

SERVER="${FACESWAP_URL:-http://127.0.0.1:8001}"

usage() {
    echo "Usage: $0 <manifest.json> <swapped_dir> <output.mp4> [--audio path] [--fps N] [--restore codeformer|gfpgan] [--fidelity 0-1] [--upscale 0|2|4]"
    exit 1
}

[[ $# -lt 3 ]] && usage
MANIFEST="$(realpath "$1")"; shift
SWAPPED="$(realpath "$1")";  shift
OUTPUT="$1";                  shift

# Default audio: look for audio.aac next to manifest
DEFAULT_AUDIO="$(dirname "$MANIFEST")/audio.aac"
AUDIO="$([[ -f "$DEFAULT_AUDIO" ]] && echo "$DEFAULT_AUDIO" || echo "")"
FPS="30"; RESTORE=""; FIDELITY="0.7"; UPSCALE="0"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --audio)    AUDIO="$2";    shift 2 ;;
        --fps)      FPS="$2";      shift 2 ;;
        --restore)  RESTORE="$2";  shift 2 ;;
        --fidelity) FIDELITY="$2"; shift 2 ;;
        --upscale)  UPSCALE="$2";  shift 2 ;;
        *) echo "[ERROR] unknown option: $1"; usage ;;
    esac
done

echo "[video_assemble] Manifest:    $MANIFEST"
echo "[video_assemble] Swapped dir: $SWAPPED"
echo "[video_assemble] Output:      $OUTPUT"
[[ -n "$AUDIO"   ]] && echo "[video_assemble] Audio:       $AUDIO"
[[ -n "$RESTORE" ]] && echo "[video_assemble] Restore:     $RESTORE (fidelity=$FIDELITY)"
[[ "$UPSCALE" != "0" ]] && echo "[video_assemble] Upscale:     ${UPSCALE}x"

curl -s -X POST "$SERVER/video/assemble" \
    -F "manifest_path=$MANIFEST" \
    -F "swapped_dir=$SWAPPED" \
    -F "audio_path=$AUDIO" \
    -F "output_path=$OUTPUT" \
    -F "fps=$FPS" \
    -F "restore=$RESTORE" \
    -F "fidelity=$FIDELITY" \
    -F "upscale=$UPSCALE" | python3 -m json.tool
