#!/usr/bin/env bash
# Clone pinned upstream and apply the Nexus model-control extension reproducibly.
# Does not contact, update, or reboot a PlayStation 5.
set -euo pipefail
shopt -s nullglob
root="$(cd -- "$(dirname -- "$0")" && pwd)"
repository="$root"
destination="${1:-$repository/build/nexusai}"
upstream='https://github.com/abhi245y/PS5LM-Plus.git'
revision='6729b4acbaf65e23b2e8bc8708b2acba524a7aa8'
sdcpp_upstream='https://github.com/leejet/stable-diffusion.cpp'
sdcpp_revision='f89d9b13d730eabeede7314ce49dacd18d3c90c2'
whisper_upstream='https://github.com/ggml-org/whisper.cpp'
whisper_revision='d1be6fde11ac6e0407606b4e42fe72d34add8037'
if [[ -e "$destination" ]]; then
    echo "Refusing to overwrite existing folder: $destination" >&2
    exit 2
fi
mkdir -p -- "$(dirname "$destination")"
git -c core.autocrlf=false clone --filter=blob:none --no-checkout "$upstream" "$destination"
git -C "$destination" checkout --detach "$revision"
# The original blackbearreloaded/ps5-homebrew-ui is gone; this mirror holds the pinned commit.
git -C "$destination" submodule set-url third_party/ps5-homebrew-ui https://github.com/abhi245y/ps5-homebrew-ui
# sd.cpp and whisper.cpp are plain pinned clones, not submodules (sd.cpp uses the system ggml).
fetch_pinned() {
    local url=$1 dir=$2 pin=$3 actual
    git -c core.autocrlf=false clone --filter=blob:none --no-checkout "$url" "$dir"
    git -C "$dir" fetch --filter=blob:none origin "$pin"
    git -C "$dir" -c core.autocrlf=false checkout --detach "$pin"
    actual="$(git -C "$dir" rev-parse HEAD)"
    if [[ "$actual" != "$pin" ]]; then
        echo "Pinned checkout mismatch in $dir: HEAD is $actual, expected $pin" >&2
        exit 2
    fi
}
mkdir -p -- "$destination/third_party"
fetch_pinned "$sdcpp_upstream" "$destination/third_party/stable-diffusion.cpp" "$sdcpp_revision"
fetch_pinned "$whisper_upstream" "$destination/third_party/whisper.cpp" "$whisper_revision"
# Shim that builds sd.cpp against third_party/llama.cpp/ggml; its defaults resolve from this folder.
mkdir -p -- "$destination/third_party/sd-ps5"
cp -- "$root/native/sd/CMakeLists.txt" "$destination/third_party/sd-ps5/CMakeLists.txt"
git -C "$destination" apply --unidiff-zero --check "$root/patches/0001-nexus-control.patch"
git -C "$destination" apply --unidiff-zero "$root/patches/0001-nexus-control.patch"
git -C "$destination" apply --check "$root/patches/0003-nexusai-branding.patch"
git -C "$destination" apply "$root/patches/0003-nexusai-branding.patch"
git -C "$destination" apply --check "$root/patches/0004-nexus-model-metadata.patch"
git -C "$destination" apply "$root/patches/0004-nexus-model-metadata.patch"
git -C "$destination" apply --check "$root/patches/0005-nexus-executable-modes.patch"
git -C "$destination" apply "$root/patches/0005-nexus-executable-modes.patch"
git -C "$destination" apply --check "$root/patches/0006-nexus-provisioning-pin.patch"
git -C "$destination" apply "$root/patches/0006-nexus-provisioning-pin.patch"
git -C "$destination" apply --check "$root/patches/0007-nexus-midnight-palette.patch"
git -C "$destination" apply "$root/patches/0007-nexus-midnight-palette.patch"
git -C "$destination" apply --check "$root/patches/0008-nexus-control-logs.patch"
git -C "$destination" apply "$root/patches/0008-nexus-control-logs.patch"
git -C "$destination" apply --check "$root/patches/0009-nexus-launch-animation.patch"
git -C "$destination" apply "$root/patches/0009-nexus-launch-animation.patch"
git -C "$destination" apply --check "$root/patches/0010-nexus-ggml-max-name.patch"
git -C "$destination" apply "$root/patches/0010-nexus-ggml-max-name.patch"
git -C "$destination" apply --check "$root/patches/0011-nexus-media-mode.patch"
git -C "$destination" apply "$root/patches/0011-nexus-media-mode.patch"
git -C "$destination" apply --check "$root/patches/0012-nexus-media-build.patch"
git -C "$destination" apply "$root/patches/0012-nexus-media-build.patch"
cp -- "$root"/native/nexus_*.cpp "$root"/native/nexus_*.hpp "$destination/ps5/app/"
# The upstream build scripts apply patches in the native clone's patches/
# directory to the pinned llama.cpp submodule before compiling. Keep the
# PS5-only numeric JSON compatibility guard reproducible for new clones.
cp -- "$root/patches/0002-ps5-empty-locale-decimal.patch" "$destination/patches/nexus-ps5-empty-locale-decimal.patch"
# NexusAI llama.cpp patches, staged under nexus- names as check-patches.sh does.
for p in "$root"/patches/llama.cpp/*.patch; do
    cp -- "$p" "$destination/patches/nexus-$(basename -- "$p")"
done
echo "NexusAI build tree prepared at: $destination"
echo "Upstream revision: $revision"
echo "Run: $root/test-control.sh '$destination'"
echo "To compile for PS5, first initialize upstream submodules and install its documented PS5_Vulkan/SDK toolchain."
