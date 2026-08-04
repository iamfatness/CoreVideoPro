#!/bin/bash
# Build + run the macOS SwiftUI shell against the full-config native core.
# Usage: scripts/run-mac-shell.sh [--skip-native]
set -euo pipefail
cd "$(dirname "$0")/.."
export PATH="$HOME/Library/Python/3.9/bin:$PATH"
if [ "${1:-}" != "--skip-native" ]; then
  cmake -S native -B native/build-metal -G Ninja \
    -DCOREVIDEO_STUB=OFF -DCOREVIDEO_ENABLE_DEV_ADAPTERS=ON \
    -DCOREVIDEO_WITH_METAL=ON -DCOREVIDEO_WITH_AVF_ENCODER=ON \
    -DCOREVIDEO_WITH_COREAUDIO=ON -DCOREVIDEO_WITH_AVF_CAPTURE=ON \
    -DCOREVIDEO_WITH_SCK=ON -DCOREVIDEO_WITH_RTMP_OUTPUT=ON >/dev/null
  cmake --build native/build-metal --target corevideo-native corevideo-zoom-engine 2>/dev/null \
    || cmake --build native/build-metal --target corevideo-native
  if [ ! -d native/build-engine/corevideo-zoom-engine.app ] && [ -x native/build-engine/corevideo-zoom-engine ]; then
    ./scripts/make-macos-engine-bundle.sh --build-dir native/build-engine --link-sdk
  fi
fi
( cd mac-shell && swift build -c release )
export COREVIDEO_REPO_ROOT="$(pwd)"
exec ./mac-shell/.build/release/CoreVideoProShell
