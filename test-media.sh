#!/usr/bin/env bash
set -euo pipefail
root="$(cd -- "$(dirname -- "$0")" && pwd)"
repo="$root"
fork="${1:-$repo/build/nexusai}"
[[ -r "$fork/ps5/app/stats.hpp" ]] || { echo 'NexusAI build tree not found; run ./prepare-build.sh first' >&2; exit 1; }
for header in "$fork/third_party/llama.cpp/vendor/nlohmann/json.hpp" \
    "$fork/third_party/stable-diffusion.cpp/include/stable-diffusion.h"; do
    [[ -r "$header" ]] || { echo "Missing $header; run ./prepare-build.sh first" >&2; exit 1; }
done
output="$(mktemp -d)"
trap 'rm -rf -- "$output"' EXIT
cc -std=c11 -O2 -Wall -Wextra -Werror -I "$fork/third_party/llama.cpp/vendor" \
  -I "$fork/third_party/llama.cpp/vendor/hash" \
  -c "$fork/third_party/llama.cpp/vendor/hash/sha256/sha256.c" -o "$output/sha256.o"
# Third-party headers (nlohmann, stable-diffusion.h) are system includes so their warnings do not
# fail the build. nexus_media_voice.cpp is not built: its llama.h dependencies are not faked.
c++ -std=c++20 -O2 -pthread -Wall -Wextra -Werror \
  -isystem "$fork/third_party/llama.cpp/vendor" \
  -isystem "$fork/third_party/stable-diffusion.cpp/include" \
  -I "$root/native" \
  "$root/native/nexus_media_test.cpp" \
  "$root/native/nexus_media_fakes.cpp" \
  "$root/native/nexus_media_http.cpp" \
  "$root/native/nexus_media_image.cpp" \
  "$root/native/nexus_media_audio.cpp" \
  "$root/native/nexus_media_gpu.cpp" \
  "$root/native/nexus_media_path.cpp" \
  "$root/native/nexus_wav.cpp" \
  "$output/sha256.o" -Wl,--wrap=stat -o "$output/nexus-media-tests"
"$output/nexus-media-tests"
