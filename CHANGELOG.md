# Changelog

NexusAI follows [semantic versioning](https://semver.org).

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
