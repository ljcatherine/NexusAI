#!/usr/bin/env bash
# Clone pinned upstream and apply the Nexus model-control extension reproducibly.
# Does not contact, update, or reboot a PlayStation 5.
set -euo pipefail
root="$(cd -- "$(dirname -- "$0")" && pwd)"
repository="$root"
destination="${1:-$repository/build/nexusai}"
upstream='https://github.com/abhi245y/PS5LM-Plus.git'
revision='6729b4acbaf65e23b2e8bc8708b2acba524a7aa8'
if [[ -e "$destination" ]]; then
    echo "Refusing to overwrite existing folder: $destination" >&2
    exit 2
fi
mkdir -p -- "$(dirname "$destination")"
git -c core.autocrlf=false clone --filter=blob:none --no-checkout "$upstream" "$destination"
git -C "$destination" checkout --detach "$revision"
# The original blackbearreloaded/ps5-homebrew-ui is gone; this mirror holds the pinned commit.
git -C "$destination" submodule set-url third_party/ps5-homebrew-ui https://github.com/abhi245y/ps5-homebrew-ui
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
cp -- "$root/native/nexus_control.cpp" "$root/native/nexus_control.hpp" "$root/native/nexus_control_test.cpp" "$destination/ps5/app/"
# The upstream build scripts apply patches in the native clone's patches/
# directory to the pinned llama.cpp submodule before compiling. Keep the
# PS5-only numeric JSON compatibility guard reproducible for new clones.
cp -- "$root/patches/0002-ps5-empty-locale-decimal.patch" "$destination/patches/nexus-ps5-empty-locale-decimal.patch"
echo "NexusAI build tree prepared at: $destination"
echo "Upstream revision: $revision"
echo "Run: $root/test-control.sh '$destination'"
echo "To compile for PS5, first initialize upstream submodules and install its documented PS5_Vulkan/SDK toolchain."
