# Safe first test: NexusAI controller

Before title verification/transfer, normalize `eboot.bin` and
`sce_module/libc.prx` to mode **0755**. Verify both modes after deployment,
in addition to raw SELF hashes. The title verifier rejects missing execute
bits and records file modes in its manifest. A valid binary hash does not
prove that the console can execute a file uploaded as 0644. For new test
builds, remove and reinstall the isolated title using supported operations;
preserve originals, rollback and shared model/settings files.

## Run order

Everything in the NixOS and gateway sections runs on the NixOS build host, where the PS5 SDK and toolchain are installed. Do not build on Windows. Commit and push the NexusAI changes first: the build host only sees committed state, and the Windows working copy has uncommitted changes.

Work top to bottom. Do not start a step until the previous step's expected result holds:

1. NixOS build: patch check, build tree, host tests, PS5 title build, title checks.
2. Gateway on Linux: Go and Python suites.
3. Safe first install and network setup (the sections after this one).
4. Hardware acceptance: console steps H-0 to H-20.
5. Rollback, if any hardware step fails.

Values in `<angle brackets>` are placeholders you fill in. Some are not known to this guide yet (plan M-4): the PS5_Vulkan tree, the PS5 OpenGL SDK, and whether LLVM 21 and Docker are on the build host. Fill them in from the host and do not guess paths.

## NixOS build

Run from the repository root unless a step says otherwise. The build tree `build/nexusai` is ignored by Git.

### Inputs

| Input | Placeholder | Status |
|---|---|---|
| NexusAI checkout | `<nexusai-checkout>` | Remote URL `<nexusai-remote>` (not recorded here) |
| PS5_Vulkan tree | `<ps5-vulkan-root>` | Unknown (M-4) |
| PS5 OpenGL SDK | `<ps5-opengl-sdk>` | Unknown (M-4). Must contain `bin/llvm-nm` and `lib/libPS5OpenGL.a` |
| LLVM 21 on the host | none | Unknown (M-4) |
| Test title ID | `<unused-test-id>` | Unknown (M-2). Format `PPSA#####`, confirmed unused on the console, never `PPSA99581` |
| Pin header | `build/private/nexus-pin.h` | Made on the gateway host (step 5b). Never committed |

### 1. Clone or pull

```bash
git clone <nexusai-remote> <nexusai-checkout>   # first time only
cd <nexusai-checkout>
git pull --ff-only
git status --short
```

Expected: `git status --short` prints nothing, so the checkout matches the pushed commit.

If `pull --ff-only` refuses, the host has local changes or diverged history. Resolve that on the host. Do not force.

### 2. Check the patch stack

```bash
bash scripts/check-patches.sh
```

Expected: one `applied <patch>` line for each patch in `patches/` (0001 to 0012, and the llama.cpp patches 0101 to 0104), then exit 0. The script clones the pinned upstream, so it needs network access.

If it fails: it prints `FAILED <patch>` and exits 1. The pinned upstream no longer matches that patch. Stop, note the patch name, and do not change the pin.

### 3. Prepare the build tree

```bash
./prepare-build.sh
git -C build/nexusai submodule update --init --depth 1 third_party/llama.cpp
git -C build/nexusai submodule update --init third_party/ps5-homebrew-ui
git -C build/nexusai/third_party/ps5-homebrew-ui rev-parse HEAD        # bf44d80384d5b8f8f4f213cf98d748887211328d
git -C build/nexusai/third_party/stable-diffusion.cpp rev-parse HEAD   # f89d9b13d730eabeede7314ce49dacd18d3c90c2
git -C build/nexusai/third_party/whisper.cpp rev-parse HEAD            # d1be6fde11ac6e0407606b4e42fe72d34add8037
```

Expected:

- `prepare-build.sh` ends with `NexusAI build tree prepared at: .../build/nexusai`.
- Each `rev-parse` line prints the hash in its comment. The script also checks the sd.cpp and whisper.cpp pins and aborts on a mismatch.
- `build/nexusai/third_party/sd-ps5/CMakeLists.txt` exists (copied from `native/sd/`).
- `build/nexusai/ps5/app/` contains the `nexus_*.cpp` and `nexus_*.hpp` files, tests included.
- Every patch applies without error output.

If it fails: `Refusing to overwrite existing folder` means a tree is already there. To start over, remove only `build/nexusai`. Do not delete `build/` itself, because it may hold packages you have already verified.

### 4. Host tests

These need no PS5 SDK. Run them after step 3, because `test-control.sh` and `test-media.sh` read headers from the tree.

```bash
c++ -std=c++20 -Wall -Wextra -Werror native/nexus_wav.cpp native/nexus_wav_test.cpp -o /tmp/wavtest && /tmp/wavtest
c++ -std=c++20 -Wall -Wextra -Werror native/nexus_media_path.cpp native/nexus_media_path_test.cpp -o /tmp/pathtest && /tmp/pathtest
./test-control.sh build/nexusai
./test-media.sh build/nexusai
```

| Test | Expected | If it fails |
|---|---|---|
| WAV (U-34) | Prints `Nexus native WAV and base64 tests passed`, exit 0 | Compile error: compare `native/nexus_wav*.cpp` with the copies in `build/nexusai/ps5/app/`. Assertion failure: keep the output, it names the failing check |
| Media path | Prints `nexus_media_path_test: ok`, exit 0 | As above, for `nexus_media_path*.cpp` |
| `test-control.sh` | The controller tests run twice (default build, then with the provisioning pin), then `Provisioning policy negative compilation checks passed`, exit 0 | Missing `stats.hpp` or vendor headers: repeat step 3. Missing python3 breaks the pin header generation. The script fails if an invalid pin configuration compiles |
| `test-media.sh` | Exit 0 | Missing `nlohmann/json.hpp` or `stable-diffusion.h`: the submodule or sd.cpp checkout from step 3 is incomplete. A link error on `stat` means the `--wrap=stat` link failed on this host, which is expected only with glibc (NixOS host, not the PS5 toolchain) |

Notes for `test-media.sh`:

- One test binds `0.0.0.0`. Run it on the build host only, not on a shared or untrusted network.
- The voice handler is stubbed, and `nexus_media_voice.cpp` is not compiled here. Voice is covered by the title build (step 5) and the hardware tests.

### 5. Build the PS5 title

The toolchain install is documented upstream in `build/nexusai/docs/CONSOLE.md`, and this guide does not repeat it. Install PS5_Vulkan and the SDK as that document says, record the versions you used, and keep the build log.

**5a. Give the build tree the test identity.** Run this before the build:

```bash
python3 prepare-test-identity.py build/nexusai --test-title-id <unused-test-id>
```

Expected: a JSON description of the title ID change, exit 0. It changes only the ignored build tree. It refuses the original `PPSA99581` and any tracked repository.

**5b. Create the pin header.** Generate it on the gateway host, where the bearer lives. Do not copy the bearer to NixOS. The header contains only the digest:

```bash
TOKEN="$(cat <private-bearer-file>)"   # on the gateway host; never echo it
printf '#define NEXUS_CONTROL_PROVISIONING_DIGEST "sha256:%s"\n' \
  "$(printf '%s' "$TOKEN" | sha256sum | cut -d' ' -f1)" > nexus-pin.h
unset TOKEN
chmod 0600 nexus-pin.h
```

Copy `nexus-pin.h` to the NixOS checkout, then:

```bash
mkdir -p build/private && cp <copied-pin-header> build/private/nexus-pin.h && chmod 0600 build/private/nexus-pin.h
stat -c '%a %U %n' build/private/nexus-pin.h
grep -c 'NEXUS_CONTROL_PROVISIONING_DIGEST "sha256:[0-9a-f]\{64\}"' build/private/nexus-pin.h
```

Expected: `stat` prints `600 <you> build/private/nexus-pin.h`, and `grep -c` prints `1`. The digest must come from the bearer in the gateway's private configuration, never from the console's `nexus-control.token`. `build/private/` sits outside `build/nexusai`, so removing the build tree keeps the header.

**5c. Build the title.**

```bash
set -o pipefail
cd build/nexusai
NEXUS_PS5_CONTROL_PIN_HEADER=<absolute-path-to-checkout>/build/private/nexus-pin.h \
PS5_VULKAN_ROOT=<ps5-vulkan-root> PS5_OPENGL_SDK=<ps5-opengl-sdk> \
  bash scripts/build-app.sh 2>&1 | tee ../ps5-build.log
cd ../..
```

Expected:

- Exit 0. The last line reports the title folder and its size in bytes.
- `build/nexusai/build/app-ps5/title/<unused-test-id>/` exists.
- `build/nexusai/build/app-ps5/llama/ggml/src/libggml.a` exists.
- The sd.cpp configure warns that INT8 tensorwise is disabled and that FP8 is converted to F16. This is expected with `SD_USE_UPSTREAM_GGML=ON` (`native/sd/CMakeLists.txt`). It costs memory on the PS5. Decide whether to accept it before H-5. This guide does not decide it.
- `grep -in error build/ps5-build.log` prints nothing. The Mamba-1 shader (`ssm_scan_mamba1.comp`) and patches 0101, 0102 and 0104 compile without error.

If it fails:

- The pin header is rejected: it must be a regular, non-symlink file you own, with mode 0600. Fix the file. Do not loosen the check.
- `build-app: unexpected unresolved weak symbols`: see step 6.4. Do not add `--defsym` by hand.
- The sd configure fails: read `build/nexusai/build/app-ps5.sd-configure.log`. Llama configure or compile errors are in `build/nexusai/build/app-ps5.configure.log`.
- A voice compile error mentions `common_chat_params` or `chat.h`: the worker report ties the voice handler to the chat-params API at commit 620d2225. Check `git -C build/nexusai/third_party/llama.cpp rev-parse --short=8 HEAD` against it before you edit any code.
- The pin or the bearer changed: remove `build/nexusai` and redo steps 3 and 5, so no stale controller object is reused.

**Docker alternative**, if Docker is on the build host (M-4):

```bash
TEST_TITLE_ID=<unused-test-id> tools/ps5-build/run.sh build/private/nexus-pin.h
```

Expected: the first run builds the whole toolchain, which takes hours, into the `nexusai-ps5build` volume. Later runs rebuild only the app. The title is copied to `build/title/<unused-test-id>`, and the log is `build/ps5-build.log`. Without a pin header the script only warns, and the console's :8082 controller will not start. Always pass the header for a PS5 test build.

### 6. Check the built title

Set these once. The Docker output is in `build/title/<unused-test-id>` instead of the path below.

```bash
SDK=<ps5-opengl-sdk>
TREE=build/nexusai/build/app-ps5
TITLE=$TREE/title/<unused-test-id>
```

**6.1 Ggml name size.** `GGML_MAX_NAME` must be 160 in every translation unit that includes `ggml.h` (patch 0010). A mix would give two struct layouts for one ggml.

```bash
grep -o 'GGML_MAX_NAME=[0-9]*' $TREE/llama/CMakeCache.txt | sort | uniq -c
```

Expected: only `GGML_MAX_NAME=160`. Any `=64`, or no output at all, is a stop. Rebuild from a clean tree.

**6.2 Undefined symbols.** Collect the undefined names of the new objects and of `libstable-diffusion.a`:

```bash
find "$TREE" \( -name whisper-aneforge.o -o -name 'nexus_*.o' -o -name libstable-diffusion.a \) \
  -exec "$SDK/bin/llvm-nm" -u {} + | awk '{print $NF}' | sort -u > "${TMPDIR:-/tmp}/undefined.txt"
```

Expected:

- `dlopen`, `dlsym`, `dlerror` and `dlclose` appear as undefined in `whisper-aneforge.o`. They must resolve from the PS5 SDK libc, and the link succeeds only if they do. If the link fails on undefined `dl*` names, stop. Do not link a stub in their place.
- Every other libc or POSIX name in `undefined.txt` must come from `compat_app.c` (the app compat layer) or from the SDK. A name nothing provides is a finding to report.

**6.3 No duplicate SHA-256.** `vendor-hash.a` and the loose `sha256.o` must not both be linked:

```bash
find "$TREE" \( -name sha256.o -o -name '*.a' \) -exec "$SDK/bin/llvm-nm" -A --defined-only {} + 2>/dev/null | grep -w sha256_hash
grep -n 'sha256_hash' build/nexusai/third_party/llama.cpp/vendor/hash/sha256/sha256.h
```

Expected: `sha256_hash` is defined by one linked input. Several lines can come from archives that the link does not use. If two inputs on the link line both define it, stop. The header declares `sha256_hash(uint8_t*, const uint8_t*, size_t)`; parameter names may differ.

**6.4 Weak symbols.** The build writes `weak-unresolved.txt` next to the title and links the Mesa and `_ZTH` names to zero:

```bash
find "$TREE" -name weak-unresolved.txt -exec cat {} +
test -x "$SDK/bin/llvm-nm" && echo "llvm-nm present"
```

Expected: the file is empty, or lists only names with the Mesa prefixes or `_ZTH` (patch 0012). Any other name makes `build-app.sh` fail, so a successful build means this file is clean.

**6.5 Size.** Record the sizes:

```bash
du -b $TITLE/eboot.bin $TITLE/sce_module/libc.prx
```

The scripts enforce no size limit. The worker measured that the title still fits, but nothing checks it. Put the numbers next to the package and compare them with the manifest of the last verified package. Review any large increase before installing.

**6.6 Verify the title.** Run the verifier with a new manifest path, because it never overwrites:

```bash
mkdir -p build/ps5lm-nexus-test-<unused-test-id>
python3 verify-test-title.py $TITLE --manifest build/ps5lm-nexus-test-<unused-test-id>/manifest.json
stat -c '%a %n' $TITLE/eboot.bin $TITLE/sce_module/libc.prx
sha256sum $TITLE/eboot.bin
```

Expected: the verifier passes and writes the manifest. Both files show `755`, which patch 0005 sets. Save the manifest and the SHA-256 with the package. A passing file check is not a hardware test.

**6.7 Media linked.** The media code must be in the binary:

```bash
"$SDK/bin/llvm-nm" -C $TREE/llvm-pie.elf | grep -c -E 'sd_service_load|stt_load|tts_load|NexusMedia'
```

Expected: 4 or more. Fewer means the media objects were not linked. Check the link line in `build/ps5-build.log`.

## Gateway on Linux

This needs no PS5. Run it in the NexusBackend checkout on the NixOS host (or any Linux host with Go 1.25 and gcc, since `go.mod` says `go 1.25.0`). NexusBackend is a separate repository.

```bash
cd <nexusbackend-checkout>/gateway
CGO_ENABLED=1 go test -race -count=1 ./...
go vet ./...
CGO_ENABLED=0 go test -count=1 ./...
cd ..
python3 -I -m unittest discover -s scripts -p 'test_*.py'
```

Expected:

- Each `go test` run prints `ok` for every package, no `FAIL` lines, exit 0. The `-race` run reports no `WARNING: DATA RACE`.
- `go vet ./...` prints nothing.
- The Python run ends with `OK` and exit 0. `-I` runs Python in isolated mode, so the directory's own files cannot shadow standard modules.

Notes: on Windows, 11 existing tests fail because they check Unix directory modes and Unix absolute paths, and `python3` is missing. Those are environment failures. On NixOS they must pass. If one fails, investigate the host (file modes, PATH) and do not skip the test. If `-race` reports a race, fix the race. Do not drop `-race`.

## PS5 title provisioning pin

Some observed title sandboxes return owner 0/link count 0 from an open file
and deny path-based status calls, even though the private credential's true
owner is 1. Do not reinterpret those values as verified filesystem ownership
or fix this by widening permissions.

For that measured PS5 context, the build recipe accepts an explicit
`NEXUS_PS5_CONTROL_PIN_HEADER` pointing to a regular, non-symlink, owner-owned
0600 header. Generate its `NEXUS_CONTROL_PROVISIONING_DIGEST` definition from
the SHA-256 of the strong bearer in Debian's verified private configuration,
never from the mutable console digest. Keep the header in an ignored private
build directory. It contains only the verifier digest, not the bearer; never
commit the header or transmit the raw bearer to the console.

An explicitly pinned PS5 build requires the exact canonical 72-byte private
credential record and matching embedded digest on **every** startup. Only
the observed effective-UID 1 / file-UID 0 / zero-link context can use the pin
instead of unverifiable owner metadata, and it additionally requires exact
0600 mode with no special bits, a regular descriptor and no symlink. No
listener exists until validation completes. Missing/wrong pin configuration
fails compilation; a substituted or malformed file fails startup. Unpinned
builds keep the existing ownership checks. HTTP still requires the raw bearer;
the digest is never a bearer credential.

The pin establishes approved credential identity, not actual file ownership
or confidentiality. Its trust boundary includes reviewed build inputs and
the installed executable. Changing the bearer requires a coordinated new pin
build, private file provisioning and gateway configuration; old rollback
executables retain their old pin. The build always recompiles the controller
with the explicitly selected flags; do not reuse stale controller objects or
auto-discover an old header. Inspect the actual patched build recipe used.

The experimental PS5-native build has passed **offline** inspection and a minimal
PS5 hardware startup and inference test; **controller switching and Nexus iOS
integration remain untested**. The locally verified
test archive is `build/NexusAI-Test-PPSA99742.zip`, with its `.sha256`
sidecar and `build/ps5lm-nexus-test-PPSA99742/manifest.json`. Follow this
checklist before installing or launching that package.

## Stage and inspect before touching the console

1. Build from the pinned `./prepare-build.sh` build tree;
   avoid the original title ID **PPSA99581**. In the ignored test clone,
   run `prepare-test-identity.py` with a distinct, owner-verified unused
   nine-character `PPSA#####` title ID **before** invoking the builder.
2. Build the PS5 title using the upstream PS5_Vulkan cross-toolchain and
   its verified SDK, RADV release archive, native-tool and OpenGL files.
   Preserve the build logs. Do not substitute any unsigned host executable
   or empty DLL/archive for a required PS5 dependency.
3. Verify `build/app-ps5/title/<TEST_ID>` with `verify-test-title.py`.
   Save the manifest and SHA-256 hash alongside the package. A passing file
   validator is **not** a real PS5 hardware/firmware test.

   The current ZIP uses the provisional test ID **PPSA99742**. Confirm that
   this ID is genuinely unused on the target console before installation; if
   not, regenerate the package with another distinct ID and reverify it.

## Preserve the original upstream app

1. Before transferring anything, back up the installed original
   `PPSA99581` title folder **and** `/data/PS5LM`, or the corresponding
   `/download0/PS5LM` sandbox state (settings, model.txt, model list,
   downloads and any conversations). Check that backup content and size are
   plausible before continuing. Avoid copying huge GGUF files unless needed;
   record their exact locations so they remain available to restore.
2. Test under a **different title ID**. This protects the original title
   files, **not** the global model/settings directory. Both application
   variants can still write `/data/PS5LM`; do not run both variants at once
   and never perform a model switch until state is backed up.
3. Keep the original files and title install intact. Launch and test the
   experimental title only after confirming that the chosen ID is unused.

## Start with the safest network configuration

1. Without `nexus-control.token`, the controller must remain **disabled**.
   Test this first. The controller never replaces llama-server on port 8081.
2. Create a *separate* random 32–128-character token on a trusted machine.
   Store only its `sha256:<64 lowercase hex digits>` digest in
   `nexus-control.token` with mode 0600 (or stricter); never place the raw
   token on the console. Store client-side copies in the Debian gateway's
   owner-only config and Nexus's iOS Keychain; never commit or log them.
3. The controller defaults to **127.0.0.1:8082**. Only create the
   `nexus-control.lan` opt-in marker when a protected private network/gateway
   and firewall policy are ready. Port 8082 is **HTTP without TLS** and must
   not be exposed to the internet or shared with untrusted Wi-Fi devices.
   **Current owner policy:** FTP :2121 and payload loader :9021 are accepted
   risks on the owner's trusted private LAN. Their LAN reachability does not
   alone block further experimental controller work. Hash storage mitigates
   simple FTP token disclosure, but FTP writes/loader code execution/plaintext
   interception remain possible; no network probe proves router WAN safety.
   Verify router mappings separately, preserve the original title and rollback,
   and never forward these services or unauthenticated management publicly.
   Dedicated controller authentication and the 401/200 checks below remain
   mandatory before model switching. Do not provision real console settings
   without authorized experimental-title preparation.

4. From a trusted client through the approved authenticated/private ingress,
   verify unauthorized requests return HTTP 401, and authenticated
   `/v1/nexus/status` and `/v1/nexus/models` reflect **actual** installed
   PS5 models. Confirm IDs are 16 lowercase hex characters.
5. Test a model switch only after saving ongoing conversations. `POST`
   `/v1/nexus/models/<id>/load` returning HTTP 202 means **queued**, not
   loaded; the native app restarts to reclaim GPU memory. Poll status until
   it reports the correct model as ready. A failed restart should expose
   `restart_failed`, rather than blocking subsequent requests forever.
6. Test Nexus native iOS controls and separately configured workspace
   bridges only after both sides authenticate successfully. Debian and NixOS
   agents are separate processes and must use distinct bearer tokens and
   explicit allowlisted roots. Workspaces are read-only by default; managed edits and fixed-profile commands require their separate grants, human review and explicit execution.

## Hardware acceptance

Run this only after the NixOS build and gateway checks pass, and after the safe first install below is complete. For every step, do the action, then compare with the expected result. For any failure, save `app.log` and `llama.log` before you change anything.

Preconditions:

- The title verified in step 6 is installed under the unused test ID, never `PPSA99581`. See "Stage and inspect before touching the console".
- The original `PPSA99581` title and `/data/PS5LM` (or `/download0/PS5LM`) are backed up and checked. See "Preserve the original upstream app".
- Only the test title runs. Never run both variants at once.
- The gateway's control URL points to `<ps5-tailnet-ip>:8082`, and its media URL to `:8083` (port proposed, D-4). Real addresses stay in the gateway's private configuration, not in this guide.
- Conversations are saved before any model switch.
- Timings below are targets from PSllama's measurements, including its 10 s GPU timeout and ~12 GiB heap. They are not guarantees for this console. Record what you measure.

Set these in your shell. Keep the token file outside the repository, with mode 0600:

```bash
PS5=<ps5-tailnet-ip>
TOKEN="$(cat <private-token-file>)"   # never paste the value into a command or a log
```

| # | Steps | Expected |
|---|---|---|
| H-0 | Safe first install: start the test title with no `nexus-control.token` present | Controller disabled: `curl -m5 http://$PS5:8082/v1/nexus/status` fails to connect. The original title is still installed |
| H-1 | From a non-gateway tailnet device: `curl -m5 http://$PS5:8082/v1/nexus/status`, then the same for `:8081` and `:8083`. From the gateway host without the bearer: `curl -s -o /dev/null -w '%{http_code}\n' http://$PS5:8082/v1/nexus/status`, and the same for `:8083` | From the non-gateway device all three time out (Tailscale ACL). From the gateway host without the bearer: `401` on :8082 and :8083 |
| H-2 | LLM mode, `GGML_MAX_NAME=160` build: load the usual chat model, send a greedy prompt through the gateway | Same output as the pre-port build for the same prompt, tok/s within 5 % of it (PSllama saw no regression) |
| H-3 | Vision: chat with a JPEG on a model that has an mmproj file | The answer refers to the image content |
| H-4 | Engine switch to media, using the round trip below (or `POST /v1/nexus/engine/media` through the gateway) | `202`. Status goes `switching` then `ready` with `"engine":"media"`. Record the time: expect about 30 s, with no LLM loaded |
| H-5 | Image: FLUX.2-klein-4B, 768×768, 4 steps (model choice pending M-3) | PNG returned. Warm time about 17.6 s (PSllama target). Record the cold load time separately. Decide the FP8-to-F16 memory cost first (step 5c) |
| H-6 | STT: an 8 s WAV | Transcript returned. Warm time about 0.8 s (target) |
| H-7 | TTS: one sentence; then again with a reference voice and consent | The WAV plays. Without consent the gateway returns 403 |
| H-8 | Voice: two turns | First audio about 2.3 to 2.7 s warm (target). A cold turn after an image takes 60 s or less |
| H-9 | Video: Wan2.1 1.3B, 9 frames at 416×240 (model and output format pending M-3, M-10) | About 60 s (target). The MP4 plays in NexusOnline |
| H-10 | Fragmentation: voice ×2, then Z-Image 512², then voice, then image, then `engine/llm` | No `ErrorOutOfDeviceMemory` in `app.log`. LLM mode returns to `ready` |
| H-11 | Cut power to the network, or stop the title during a job | The gateway marks the PS5 offline within its breaker window. The UI shows "PS5 offline" and the alert fires (M-8). It recovers after the title restarts |
| H-12 | 30-minute soak with mixed queued jobs from two test subjects | No crash. Fairness order holds. The usage log has counts and durations, and no prompt text |
| H-13 | Boot in media mode: set `model.txt` to `media`, then restart the title | Media mode boots, and it stays sticky until `engine:llm`. The tuning variables `GGML_VK_IGNORE_MEMORY_BUDGET` and `GGML_VK_SUBALLOCATION_BLOCK_SIZE` are read when the Vulkan device is created. Check the order of lines in `app.log`: no Vulkan device is created before the media setup |
| H-14 | Media listener auth: call a media route on :8083 (routes are in `native/nexus_media_http.cpp`) without the bearer, then with it | Without the bearer: `401`. With the bearer: the route answers. The listener starts only with the provisioned token (pinned build) |
| H-15 | Large unauthenticated POST: `head -c 5000000 /dev/zero \| curl -s -X POST --data-binary @- -o /dev/null -w '%{http_code}\n' http://$PS5:8082/v1/nexus/models/unload` | `401`, or the connection resets. A reset is acceptable on Linux |
| H-16 | GPU order: speech then image, image then speech, image then voice. Then `engine/llm` | No out-of-memory error in any order. Free memory in `/status` recovers after leaving media mode (`media_gpu_acquire(0)` frees it on exit). Compare `/status` before and after |
| H-17 | Voice LLM: the default voice model `qwen3-4b/Qwen3-4B-Q4_K_M.gguf` is in the media model folder (location pending D-4). Run a voice turn | The file exists at that path. The voice turn answers while port :8081 (llama-server) is not running, so the voice path calls `llama_backend_init()` directly |
| H-18 | Dashboard in media mode | Shows "Media mode". Known cosmetic issue: the state word reads "NO MODEL". Do not fix it as part of this test |
| H-19 | Mamba-1 model: load it and run a prompt | It runs on the GPU (`llama.log` shows the Vulkan device). The default boot sets no tuning variables; they are opt-in only (D-3) |
| H-20 | Rollback drill, after all steps: follow "Rollback" below | The original title starts. The test title is stopped. `app.log` and `llama.log` are kept |

### Engine switch round trip (H-4, H-13)

Each request returns `202`, which means queued, not done. Poll `/status` until the engine and state show the target. A `restart_failed` state means stop and read the logs.

```bash
# to media
curl -sS -o /dev/null -w '%{http_code}\n' -X POST -H "Authorization: Bearer $TOKEN" http://$PS5:8082/v1/nexus/engine/media
timeout 300 bash -c 'until curl -sS -H "Authorization: Bearer $TOKEN" http://$PS5:8082/v1/nexus/status | grep -q "\"engine\":\"media\""; do sleep 5; done'
curl -sS -H "Authorization: Bearer $TOKEN" http://$PS5:8082/v1/nexus/status

# back to LLM
curl -sS -o /dev/null -w '%{http_code}\n' -X POST -H "Authorization: Bearer $TOKEN" http://$PS5:8082/v1/nexus/engine/llm
timeout 300 bash -c 'until curl -sS -H "Authorization: Bearer $TOKEN" http://$PS5:8082/v1/nexus/status | grep -q "\"engine\":\"llm\""; do sleep 5; done'
curl -sS -H "Authorization: Bearer $TOKEN" http://$PS5:8082/v1/nexus/status
unset TOKEN
```

Expected: each POST prints `202`. Each poll finishes within the timeout (about 30 s for the restart, plus model load time). The final status shows the target engine and `ready`. Record both restart times. A timeout with no `restart_failed` state means the restart is stuck: read `llama.log`.

### Confirm before any media test

These are not hardware steps, but do not ship media features until they are settled:

- The gateway sends PSllama's `translate` field as a boolean.
- The whisper default model path matches the file on the console.
- The image and video model IDs (`sd-turbo-fp16`, `flux2-klein-4b`, `wan2.1-t2v-1.3b`) are not final. The M-3 choice and the licences are pending. HunyuanVideo and Gemma are flagged.
- The voice system prompt in the gateway is a placeholder. Replace it before release.
- Voice cloning UI stays hidden (`VOICE_CLONE_VISIBLE=false`) until a client-visible permission exists. Expected: the control is hidden in NexusOnline and on iOS.

## Rollback

Stop the experimental title. Preserve logs, especially `app.log` and
`llama.log`; disable the experimental API by removing its token/opt-in marker
from the test state, restore the original upstream settings from the verified
backup if changed, then launch the original title. Do **not** delete or
overwrite the original `PPSA99581` binary as part of the test.

Nexus source changes and host tests are tracked in the Git repository; PS5
console acceptance and Vulkan inference behavior must be measured on the
actual device before labeling the new build production-ready.
