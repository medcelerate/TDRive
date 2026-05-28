#!/usr/bin/env bash
# Build the Rive runtime static libraries needed by TDRiveTOP.
#
# Usage:
#   ./scripts/build_rive.sh                # release build (default)
#   ./scripts/build_rive.sh debug          # debug build
#   ./scripts/build_rive.sh clean release  # clean, then release
#
# On first run this will clone rive-app/rive-runtime into third_party/
# (a recursive clone is required so its dependencies fetch).
#
# The produced static archives live in:
#   third_party/rive-runtime/out/<config>/
#   third_party/rive-runtime/renderer/out/<config>/
#
# TDRiveTOP links against these archives + the system Metal, Foundation
# and QuartzCore frameworks.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RIVE_DIR="$ROOT_DIR/third_party/rive-runtime"
RIVE_REPO="https://github.com/rive-app/rive-runtime.git"

ARGS=("$@")
if [ ${#ARGS[@]} -eq 0 ]; then
    ARGS=(release)
fi

if [ ! -d "$RIVE_DIR/.git" ] && [ ! -f "$RIVE_DIR/premake5_v2.lua" ]; then
    echo ">> Cloning rive-runtime into $RIVE_DIR"
    mkdir -p "$ROOT_DIR/third_party"
    git clone --recursive "$RIVE_REPO" "$RIVE_DIR"
fi

# Make sure premake5 ships into the path Rive expects.
if ! command -v premake5 >/dev/null 2>&1; then
    echo ">> premake5 not on PATH - using the copy in rive-runtime/build if present"
fi
export PATH="$RIVE_DIR/build:$PATH"

# Some macOS environments are missing premake5; fetch a binary if so.
if ! command -v premake5 >/dev/null 2>&1 && [ ! -x "$RIVE_DIR/build/premake5" ]; then
    echo ">> Bootstrapping premake5 via Homebrew"
    if command -v brew >/dev/null 2>&1; then
        brew install premake
    else
        echo "ERROR: install premake5 (e.g. 'brew install premake') and re-run." >&2
        exit 1
    fi
fi

# The renderer's premake5.lua dofiles in the core runtime, decoders, and
# dependencies (harfbuzz / sheenbidi / etc.), so a single invocation from
# renderer/ produces everything we need. There is no premake5.lua at the
# repo root.
echo ""
echo ">> Building rive renderer + core (${ARGS[*]})"
(cd "$RIVE_DIR/renderer" && "$RIVE_DIR/build/build_rive.sh" "${ARGS[@]}")

echo ""
echo ">> Done."
echo "Static libs are in:"
echo "  $RIVE_DIR/renderer/out/<config>/"
