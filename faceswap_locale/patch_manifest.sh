#!/usr/bin/env bash
# patch_manifest.sh — Update NPC assignments or swap settings in manifest.json
#
# Usage (assign NPCs to clusters — run once after /video/analyze):
#   ./faceswap_locale/patch_manifest.sh <manifest.json> \
#       --assign '{"face_0":"jenny","face_1":"marco"}'
#
# Usage (refine flagged sequence):
#   ./faceswap_locale/patch_manifest.sh <manifest.json> \
#       --start 000043 --end 000051 \
#       --mask-parts face,skin --expand 3 --blur 5
#
# Usage (skip a range — keep original frames):
#   ./faceswap_locale/patch_manifest.sh <manifest.json> \
#       --start 000100 --end 000115 --skip
#
# Options:
#   --start <frame_id>           first frame (zero-padded, e.g. 000043); default: all frames
#   --end   <frame_id>           last frame (inclusive); default: all frames
#   --assign <json>              JSON map cluster_id→npc_id, e.g. '{"face_0":"jenny"}'
#   --cluster <cluster_id>       filter: only patch faces of this cluster
#   --mask-parts <parts>         comma-separated FaceParser parts (face,skin,lips,eye,hair,...)
#   --expand <N>                 dilate mask by N pixels
#   --blur   <N>                 Gaussian feather radius in pixels
#   --skip                       mark frames as skip (use original in assembly)
#   --status <status>            set frame status (default: pending_retry)

set -euo pipefail

SERVER="${FACESWAP_URL:-http://127.0.0.1:8001}"

usage() {
    echo "Usage: $0 <manifest.json> [--start F] [--end F] [--assign JSON] [--cluster ID] [--mask-parts p,q] [--expand N] [--blur N] [--skip] [--status S]"
    exit 1
}

[[ $# -lt 1 ]] && usage
MANIFEST="$(realpath "$1")"; shift

START=""; END=""; ASSIGN=""; CLUSTER=""; MASK_PARTS=""
EXPAND="-1"; BLUR="-1"; SKIP="false"; STATUS="pending_retry"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --start)      START="$2";      shift 2 ;;
        --end)        END="$2";        shift 2 ;;
        --assign)     ASSIGN="$2";     shift 2 ;;
        --cluster)    CLUSTER="$2";    shift 2 ;;
        --mask-parts) MASK_PARTS="$2"; shift 2 ;;
        --expand)     EXPAND="$2";     shift 2 ;;
        --blur)       BLUR="$2";       shift 2 ;;
        --skip)       SKIP="true";     shift ;;
        --status)     STATUS="$2";     shift 2 ;;
        *) echo "[ERROR] unknown option: $1"; usage ;;
    esac
done

[[ -f "$MANIFEST" ]] || { echo "[ERROR] manifest not found: $MANIFEST"; exit 1; }

echo "[patch_manifest] $MANIFEST"
[[ -n "$START" ]]      && echo "  Range:      $START – ${END:-end}"
[[ -n "$ASSIGN" ]]     && echo "  NPC assign: $ASSIGN"
[[ -n "$CLUSTER" ]]    && echo "  Cluster:    $CLUSTER"
[[ -n "$MASK_PARTS" ]] && echo "  Mask parts: $MASK_PARTS  expand=$EXPAND  blur=$BLUR"
[[ "$SKIP" == "true" ]] && echo "  SKIP frames"

curl -s -X POST "$SERVER/video/patch_manifest" \
    -F "manifest_path=$MANIFEST" \
    -F "frame_start=$START" \
    -F "frame_end=$END" \
    -F "npc_assign=$ASSIGN" \
    -F "cluster_id=$CLUSTER" \
    -F "mask_parts=$MASK_PARTS" \
    -F "expand=$EXPAND" \
    -F "blur=$BLUR" \
    -F "skip=$SKIP" \
    -F "status=$STATUS" | python3 -m json.tool
