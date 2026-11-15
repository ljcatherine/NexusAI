# Changelog

NexusAI follows [semantic versioning](https://semver.org).

## Unreleased

- DeepSeek Harness: `scripts/nexusai-dsh.py` writes a DSH overlay that makes NexusAI (:8081) the model of DSH agents, with an optional live smoke test. See `docs/DSH.md`.
- Dashboard in the Nexus Midnight palette.
- Launch animation: the Orbit N draws itself, then the dashboard enters. Cross skips it.
- Control API: `GET /v1/nexus/logs/app` and `/logs/llama`, with an `after` cursor for following a log.
- `prepare-test-identity.py` names the title "NexusAI" and installs the Orbit N icon (`assets/nexus-icon0.png`), with the content ID the PPSA99743 test title uses. It now works on `prepare-build.sh`'s `build/nexusai` and the Docker build's `/work/nexusai`.
- `verify-test-title.py` accepts any Nexus-named title, and reports the real `titleName` (it reported `sce_module/libc.prx`).

## 1.0.0

First standalone release, moved out of the NexusiOS repository into its own.

- Opt-in, bearer-protected control API on :8082: status, installed models, load and unload.
- NexusAI dashboard branding, showing the NexusAI release on the console.
- Model metadata in `GET /v1/nexus/models`: file size, architecture and quantization type.
- PS5 JSON numeric-locale fix, executable-mode and provisioning patches.
- Safe test-title tooling, benchmark, security preflight and service provenance checks.
- Reproducible build: `prepare-build.sh` clones the pinned upstream revision and applies everything.
  It now clones with `core.autocrlf=false`, so the patches apply on Windows too.

### Renamed for this release

| Before | Now |
|---|---|
| `clone-nexus-fork.sh`, `test-nexus-control.sh` | `prepare-build.sh`, `test-control.sh` |
| Default build folder `build/ps5lm-nexus-fork` | `build/nexusai` |
| `NEXUS_PS5LM_URL`, `NEXUS_PS5LM_MODEL`, `NEXUS_PS5LM_API_KEY` | `NEXUSAI_URL`, `NEXUSAI_MODEL`, `NEXUSAI_API_KEY` |
| `PS5LM_BENCH_BEARER_TOKEN` | `NEXUSAI_BENCH_BEARER_TOKEN` |
| Test title content ID `…-NEXUSPS5LM000001`, name `NexusAI Test (PS5LM)` | `…-NEXUSAI000000001`, `NexusAI Test` |
