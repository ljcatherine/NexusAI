#!/usr/bin/env bash
# Check that the NexusAI patch stack still applies at the pinned upstream revision.
# Git only: clones PS5LM-Plus at the pin from prepare-build.sh, inits llama.cpp, applies patches.
# Builds nothing and does not contact a PlayStation 5.
set -euo pipefail
shopt -s nullglob

root="$(cd -- "$(dirname -- "$0")/.." && pwd)"
prepare="$root/prepare-build.sh"

# Reuse the pins from prepare-build.sh so they are defined in one place.
upstream="$(sed -n "s/^upstream='\(.*\)'$/\1/p" "$prepare")"
revision="$(sed -n "s/^revision='\([0-9a-f]\{40\}\)'$/\1/p" "$prepare")"
if [[ -z "$upstream" || -z "$revision" ]]; then
    echo "Cannot read upstream/revision pins from $prepare" >&2
    exit 2
fi

# CRLF conversion would change patch context, so force it off for every git call.
export GIT_CONFIG_COUNT=2 GIT_CONFIG_KEY_0=core.autocrlf GIT_CONFIG_VALUE_0=false GIT_CONFIG_KEY_1=core.longpaths GIT_CONFIG_VALUE_1=true

tmp=""
if [[ $# -ge 1 ]]; then
    work="$1"
    if [[ -e "$work" ]]; then
        echo "Refusing to overwrite existing folder: $work" >&2
        exit 2
    fi
else
    tmp="$(mktemp -d)"
    trap 'rm -rf -- "$tmp"' EXIT
    work="$tmp/PS5LM-Plus"
fi

mkdir -p -- "$(dirname -- "$work")"
git -c core.autocrlf=false clone --filter=blob:none --no-checkout "$upstream" "$work"
git -C "$work" checkout --detach "$revision"

# Only llama.cpp is needed; the superproject records the exact commit to use.
llama_rel=third_party/llama.cpp
llama_sha="$(git -C "$work" ls-tree HEAD -- "$llama_rel" | awk '{print $3}')"
if [[ ! "$llama_sha" =~ ^[0-9a-f]{40}$ ]]; then
    echo "No $llama_rel gitlink at $revision" >&2
    exit 2
fi
git -C "$work" submodule update --init --depth 1 -- "$llama_rel"
llama="$work/$llama_rel"
if [[ "$(git -C "$llama" rev-parse HEAD)" != "$llama_sha" ]]; then
    echo "llama.cpp checkout is not $llama_sha" >&2
    exit 2
fi

# Same check-then-apply pair as prepare-build.sh. Exits on the first failing patch.
apply_patch() {
    local dir=$1 patch=$2 name=${2##*/}
    shift 2
    if ! git -C "$dir" apply --check "$@" "$patch" || ! git -C "$dir" apply "$@" "$patch"; then
        echo "FAILED $name" >&2
        exit 1
    fi
    echo "applied $name"
}

# NexusAI app patches, in the order prepare-build.sh applies them (read from that file).
while IFS= read -r line; do
    flags=()
    [[ "$line" == *--unidiff-zero* ]] && flags=(--unidiff-zero)
    apply_patch "$work" "$root/patches/${line##*/patches/}" "${flags[@]}"
done < <(grep -E '^git -C "\$destination" apply (--unidiff-zero )?"\$root/patches/[^"]+"$' "$prepare" | sed 's/"$//')

# Stage the llama.cpp patch set as prepare-build.sh leaves it in the clone:
# upstream patches/, then NexusAI's 0002 and patches/llama.cpp/*.patch under nexus- names.
stage="$work/patches"
cp -- "$root/patches/0002-ps5-empty-locale-decimal.patch" "$stage/nexus-ps5-empty-locale-decimal.patch"
for p in "$root"/patches/llama.cpp/*.patch; do
    cp -- "$p" "$stage/nexus-$(basename -- "$p")"
done

# build-app.sh applies "$root"/patches/*.patch to llama.cpp in glob order.
llama_patches=("$stage"/*.patch)
if (( ${#llama_patches[@]} == 0 )); then
    echo "No llama.cpp patches found" >&2
    exit 1
fi
for p in "${llama_patches[@]}"; do
    apply_patch "$llama" "$p"
done

echo "All patches apply at $revision (llama.cpp $llama_sha)"
