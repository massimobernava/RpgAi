#!/usr/bin/env bash
# video_extract.sh — Extract frames and audio from a video clip
#
# Usage:
#   ./faceswap_locale/video_extract.sh <video> <output_dir> [--start HH:MM:SS] [--end HH:MM:SS] [--fps 30]
#
# Examples:
#   ./faceswap_locale/video_extract.sh movie.mp4 saves/video_work/clip1
#   ./faceswap_locale/video_extract.sh movie.mp4 saves/video_work/clip1 --start 00:01:23 --end 00:01:45
#   ./faceswap_locale/video_extract.sh movie.mp4 saves/video_work/clip1 --start 83 --end 105.5 --fps 25

set -euo pipefail

SERVER="${FACESWAP_URL:-http://127.0.0.1:8001}"

usage() {
    echo "Usage: $0 <video_path> <output_dir> [--start HH:MM:SS] [--end HH:MM:SS] [--fps N]"
    exit 1
}

[[ $# -lt 2 ]] && usage
VIDEO="$(realpath "$1")"; shift
mkdir -p "$1"
OUTDIR="$(realpath "$1")"; shift

START=""; END=""; FPS=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --start) START="$2"; shift 2 ;;
        --end)   END="$2";   shift 2 ;;
        --fps)   FPS="$2";   shift 2 ;;
        *) echo "[ERROR] unknown option: $1"; usage ;;
    esac
done

[[ -f "$VIDEO" ]] || { echo "[ERROR] video not found: $VIDEO"; exit 1; }

echo "[video_extract] Video:      $VIDEO"
echo "[video_extract] Output dir: $OUTDIR"
[[ -n "$START" ]] && echo "[video_extract] Start:      $START"
[[ -n "$END"   ]] && echo "[video_extract] End:        $END"
[[ -n "$FPS"   ]] && echo "[video_extract] FPS:        $FPS"

curl -s -X POST "$SERVER/video/extract" \
    -F "video_path=$VIDEO" \
    -F "output_dir=$OUTDIR" \
    -F "start_time=$START" \
    -F "end_time=$END" \
    -F "fps=$FPS" | python3 -m json.tool
