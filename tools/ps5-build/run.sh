#!/usr/bin/env bash
# Builds the PS5 title in Docker: tools/ps5-build/run.sh [pin-header]
# The title lands in build/title/<TITLE_ID>; set TEST_TITLE_ID to install under a test ID.
# The first run builds the toolchain (hours); later runs only rebuild the app.
set -euo pipefail
root="$(cd -- "$(dirname -- "$0")/../.." && pwd)"
docker build -q -t nexusai-ps5build "$root/tools/ps5-build" >/dev/null
mkdir -p "$root/build"
pin=()
if [[ -n "${1:-}" ]]; then pin=(-v "$(realpath "$1"):/pin.h:ro"); fi
docker run --rm -e TEST_TITLE_ID="${TEST_TITLE_ID:-}" -v nexusai-ps5build:/work -v "$root:/repo:ro" \
    -v "$root/build:/out" "${pin[@]}" nexusai-ps5build bash /repo/tools/ps5-build/build.sh 2>&1 |
    tee "$root/build/ps5-build.log"
