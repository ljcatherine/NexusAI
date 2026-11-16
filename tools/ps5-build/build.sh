#!/usr/bin/env bash
# Builds the NexusAI PS5 title and its whole toolchain from source, each
# dependency at a pinned commit. Run through tools/ps5-build/run.sh: /work is a
# persistent volume (toolchain stages leave a marker and are skipped on reruns),
# /repo the NexusAI checkout, /out where the title is copied, and /pin.h the
# optional private provisioning header. TEST_TITLE_ID installs under a test ID.
set -euo pipefail
W=/work
stage() { [[ -e "$W/.done-$1" ]] && { echo "== $1: done earlier"; return 1; }; echo "== $1"; }
done_() { touch "$W/.done-$1"; }
pin() { # repo-url dir commit: clone and check out an exact commit, refusing any other
    [[ -d "$2/.git" ]] || git clone -q "$1" "$2"
    git -C "$2" checkout -q --detach "$3"
    [[ "$(git -C "$2" rev-parse HEAD)" == "$3" ]]
}
git config --global core.autocrlf false
git config --global --add safe.directory '*'

# The original blackbearreloaded repository is gone; this fork holds the pinned commit.
if stage boilerplate; then
    pin https://github.com/leolobato/ps5-native-app-boilerplate "$W/ps5-native-app-boilerplate" \
        4f531c4b517f80bcb6b1267135848168d2250047
    (cd "$W/ps5-native-app-boilerplate" && bash tools/setup-native-dependencies.sh)
    done_ boilerplate
fi

# ps5-opengl records Clang 21.1.8 for its SDK; newer clang rejects code it only warns about.
if stage llvm21; then
    f="$W/LLVM-21.1.8-Linux-X64.tar.xz"
    curl -fsSL -o "$f" https://github.com/llvm/llvm-project/releases/download/llvmorg-21.1.8/LLVM-21.1.8-Linux-X64.tar.xz
    echo "b3b7f2801d15d50736acea3c73982994d025b01c2f035b91ae3b49d1b575732b  $f" | sha256sum --check --strict
    mkdir -p "$W/llvm21" && tar -xJf "$f" -C "$W/llvm21" --strip-components=1 && rm -f "$f"
    done_ llvm21
fi
llvm21="$W/llvm21/bin/llvm-config"

# The SDK the app's screen links: mihawk-99's copy of ps5-opengl, tag v1.0.0.
if stage opengl-1.0.0; then
    rm -rf "$W/ps5-opengl"  # a failed attempt's objects must not be reused
    pin https://github.com/mihawk-99/ps5-opengl "$W/ps5-opengl" dce74910af7f4274c48d05e0ae001b3038462588
    (cd "$W/ps5-opengl" && export PS5_NATIVE_APP_TEMPLATE="$W/ps5-native-app-boilerplate" LLVM_CONFIG="$llvm21" &&
        make source-fetch && make sdk-gl46)
    done_ opengl-1.0.0
fi

# PS5_Vulkan reads the 0.3.0 layout for its shader compiler.
if stage opengl-0.3.0; then
    tag=$(git -C "$W/ps5-opengl" rev-parse 'v0.3.0^{commit}')
    pin https://github.com/mihawk-99/ps5-opengl "$W/ps5-opengl-0.3.0" "$tag"
    (cd "$W/ps5-opengl-0.3.0" && export PS5_NATIVE_APP_TEMPLATE="$W/ps5-native-app-boilerplate" LLVM_CONFIG="$llvm21" &&
        make source-fetch)
    done_ opengl-0.3.0
fi

VK="$W/src/PS5_Vulkan"
if stage vulkan; then
    pin https://github.com/mihawk-99/PS5_Vulkan "$VK" 7f492b4c6f67a4e8c8c67a4272cfeef22abea9cc
    (cd "$VK" && make init && make deps && tools/adapt-opengl-sdk.sh "$W/ps5-opengl-0.3.0" &&
        tools/setup-native-dependencies.sh && tools/build-radv.sh release && make app)
    done_ vulkan
fi

# The app: always from the current checkout.
T="$W/nexusai"
rm -rf "$T"
bash /repo/prepare-build.sh "$T"
git -C "$T" submodule update --init --depth 1 third_party/llama.cpp
git -C "$T" submodule update --init third_party/ps5-homebrew-ui
[[ "$(git -C "$T/third_party/ps5-homebrew-ui" rev-parse HEAD)" == bf44d80384d5b8f8f4f213cf98d748887211328d ]]
# sd.cpp and whisper.cpp are plain clones (not submodules); their pins live in prepare-build.sh.
sdcpp_pin=$(sed -n "s/^sdcpp_revision='\([0-9a-f]\{40\}\)'\$/\1/p" /repo/prepare-build.sh)
whisper_pin=$(sed -n "s/^whisper_revision='\([0-9a-f]\{40\}\)'\$/\1/p" /repo/prepare-build.sh)
[[ -n "$sdcpp_pin" && -n "$whisper_pin" ]] || { echo "cannot read sdcpp_revision/whisper_revision from /repo/prepare-build.sh" >&2; exit 1; }
[[ "$(git -C "$T/third_party/stable-diffusion.cpp" rev-parse HEAD)" == "$sdcpp_pin" ]]
[[ "$(git -C "$T/third_party/whisper.cpp" rev-parse HEAD)" == "$whisper_pin" ]]
[[ -z "${TEST_TITLE_ID:-}" ]] || python3 /repo/prepare-test-identity.py "$T" --test-title-id "$TEST_TITLE_ID"
if [[ -f /pin.h ]]; then  # build-app.sh requires an owner-only header owned by the builder
    install -m 600 /pin.h /tmp/nexus-pin.h
    export NEXUS_PS5_CONTROL_PIN_HEADER=/tmp/nexus-pin.h
else
    echo "warning: no pin header; the control API on :8082 will not start on the console" >&2
fi
sdk=$(find "$W/ps5-opengl/build/sdk" -name libPS5OpenGL.a -path '*/lib/*' | head -1)
[[ -n "$sdk" ]] || { echo "no libPS5OpenGL.a in the built SDK" >&2; exit 1; }
(cd "$T" && PS5_VULKAN_ROOT="$VK" PS5_OPENGL_SDK="$(dirname "$(dirname "$sdk")")" bash scripts/build-app.sh)
rm -f /tmp/nexus-pin.h
rm -rf /out/title && cp -r "$T/build/app-ps5/title" /out/title
ls -la /out/title
