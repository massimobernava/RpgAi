#!/usr/bin/env bash
# build.sh — configure and build rpgai
#
# Usage:
#   ./build.sh            # Release build (default)
#   ./build.sh debug      # Debug build
#   ./build.sh clean      # Wipe build dir and rebuild from scratch
#
# If cmake complains a dependency is not found even though you just added it,
# run:  ./build.sh clean
# CMake caches find_path results — a clean build forces a fresh search.
#
# Custom header paths (if not in vendor/ or system locations):
#   SOL2_INCLUDE_DIR=/path ./build.sh
#   NLOHMANN_JSON_INCLUDE_DIR=/path ./build.sh
#   ASIO_INCLUDE_DIR=/path ./build.sh

set -e

BUILD_TYPE="Release"
BUILD_DIR="build"

# Parse arguments
for arg in "$@"; do
    case "$arg" in
        debug|Debug)   BUILD_TYPE="Debug" ;;
        clean)         rm -rf "$BUILD_DIR" ;;
        *)             echo "Unknown argument: $arg"; exit 1 ;;
    esac
done

# Pass optional path overrides via environment variables
CMAKE_EXTRA_ARGS=()
[ -n "$SOL2_INCLUDE_DIR"         ] && CMAKE_EXTRA_ARGS+=("-DSOL2_INCLUDE_DIR=$SOL2_INCLUDE_DIR")
[ -n "$ASIO_INCLUDE_DIR"         ] && CMAKE_EXTRA_ARGS+=("-DASIO_INCLUDE_DIR=$ASIO_INCLUDE_DIR")
[ -n "$CROW_INCLUDE_DIR"         ] && CMAKE_EXTRA_ARGS+=("-DCROW_INCLUDE_DIR=$CROW_INCLUDE_DIR")
[ -n "$NLOHMANN_JSON_INCLUDE_DIR"] && CMAKE_EXTRA_ARGS+=("-DNLOHMANN_JSON_INCLUDE_DIR=$NLOHMANN_JSON_INCLUDE_DIR")
[ -n "$OLLAMA_INCLUDE_DIR"       ] && CMAKE_EXTRA_ARGS+=("-DOLLAMA_INCLUDE_DIR=$OLLAMA_INCLUDE_DIR")
[ -n "$STB_INCLUDE_DIR"          ] && CMAKE_EXTRA_ARGS+=("-DSTB_INCLUDE_DIR=$STB_INCLUDE_DIR")

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

cmake .. \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    "${CMAKE_EXTRA_ARGS[@]}"

cmake --build . -- -j"$(nproc 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 4)"

echo ""
echo "Build complete: $BUILD_DIR/rpgai  ($BUILD_TYPE)"
[ -f rpgai-gui ] && echo "               $BUILD_DIR/rpgai-gui  ($BUILD_TYPE)" || true
