# Models and licences (media API)

Candidate models for the media engines, from `docs/PSLLAMA-INTEGRATION-PLAN.md` section 1.7. Nothing here was
looked up online. Values the plan does not confirm are marked **Needs Verification** (NV). Sizes and "fits"
values are the plan's claims, which rest on PSllama's documentation and on-console runs.

The heap budget is about 12 GiB, as the plan states. "Fits" means the plan says the model ran on the console within
that heap, with the caveats noted in the plan.

**The final model choice is the user's (decision M-3 in the plan).** Nothing below is approved for release.

## Candidate models

Flag column: **restrictive** means a licence with use, commercial or territory limits that the plan calls out.
**exclude** means the plan recommends not shipping it.

| Use | Model | File size | Fits ~12 GiB heap | Licence | Flag |
|---|---|---|---|---|---|
| Chat and voice LLM | Qwen3-4B Q4_K_M | 2.5 GB | yes | Needs Verification (plan: published as Apache-2.0) | |
| Chat and vision | Gemma 3 12B Q4_K_M, plus mmproj | 7.3 GB | yes, context up to 30,720 | Needs Verification (Gemma Terms of Use) | **restrictive** (use policy) |
| Chat | Qwen3-30B-A3B UD-IQ2_XXS | 10.4 GB | yes, little headroom | Needs Verification | |
| Vision | Qwen2.5-VL 7B Q4_K_M | 4.7 GB | yes (q8_0 KV cache) | Needs Verification | |
| Speech to text | Whisper large-v3-turbo q5_0 | Needs Verification | yes (author ran it) | Needs Verification (plan: Whisper weights published as MIT) | |
| Text to speech | Qwen3-TTS-12Hz-1.7B Q4_K_M, plus mmproj Q8_0 | 1.04 GB + 0.45 GB | yes | Needs Verification | voice cloning needs consent (not a licence flag) |
| Voice stack | Whisper + Qwen3-4B + Qwen3-TTS together | about 6.5 GB | yes | as above | |
| Image | FLUX.2-klein-4B Q8_0, plus Qwen3-4B and VAE | Needs Verification (total) | yes (plan: README) | Apache-2.0 per the model's `sd-model.conf` (plan) | |
| Image | Z-Image-Turbo Q8_0, plus Qwen3-4B-Instruct and Flux VAE | 9.4 GB | yes | Apache-2.0 (plan) | |
| Image (draft) | SD-Turbo FP16 | 2.6 GB | yes | Stability AI Community License (plan) | **restrictive** (commercial threshold) |
| Image | ERNIE-Image-Turbo Q6_K, plus Ministral-3-3B | Needs Verification | yes | Apache-2.0 per conf; Ministral licence NV | |
| Image | Z-Image-Turbo with abliterated encoder | n/a | yes | Apache-2.0 per conf | **exclude** (safety-removed encoder conflicts with the content policy) |
| Video | Wan2.1 T2V 1.3B fp16, plus umt5 Q4_K_M and VAE | 2.84 GB DiT; the rest Needs Verification | yes (encoders and VAE read from disk) | Apache-2.0 per the model's `sd-model.conf` (plan) | |
| Video | Wan2.2 TI2V 5B Q8_0 | 5.4 GB | yes | Apache-2.0 per conf | poor quality at small sizes (plan) |
| Video | HunyuanVideo 1.5 I2V 480p Q4_K_M, plus encoders | 11.7 GB weights | only with encoders and VAE on disk | Tencent Hunyuan Community License (plan) | **restrictive** (territory and use limits; details NV) |
| Robot (VLA) | OpenVLA (Llama 2 7B) | n/a | n/a | Needs Verification (Llama 2 Community License) | not ported |

Notes from the plan:

- The plan flags Gemma (use policy), SD-Turbo (commercial threshold) and HunyuanVideo (territory and use limits;
  details Needs Verification) as restrictive. The full licence texts were not read for this document.
- The Z-Image-Turbo abliterated-encoder row is excluded for the reason given.

## Defaults hard-coded in code

These are the values that apply when a caller omits a model. Each one is a choice that the user still has to
confirm under M-3.

| Engine | Default | Where |
|---|---|---|
| Image allow-list | `sd-turbo-fp16`, `flux2-klein-4b` | `NexusBackend/gateway/media_image.go:16` (`mediaImageModels`); the comment says the final list depends on M-3 |
| Image picker (client) | `sd-turbo-fp16` ("SD Turbo (fast)"), `flux2-klein-4b` ("FLUX.2 klein 4B") | `NexusOnline/src/app/image-core.ts:8-9`; must match the gateway allow-list (U-22) |
| Image, device | `sd-turbo-fp16` when no model is loaded and the request names none | `native/nexus_media_image.cpp:32` |
| Video | `wan2.1-t2v-1.3b` | gateway `NexusBackend/gateway/media_video.go:16,19` (the only allowed video model); device `native/nexus_media_image.cpp:33` |
| Speech to text | `whisper/ggml-large-v3-turbo-q5_0.bin` | gateway `NexusBackend/gateway/media_stt.go:14` (client cannot choose); device `native/nexus_media_audio.cpp:21` |
| Text to speech | `qwen3-tts-1.7b` | gateway `NexusBackend/gateway/media_tts.go:12` (client cannot choose); device `native/nexus_media_audio.cpp:22` |
| Voice chat LLM | `qwen3-4b/Qwen3-4B-Q4_K_M.gguf` | device only, `native/nexus_media_voice.cpp:34`, marked Needs Verification in code (folder naming vs M-3). The gateway's voice request sets no model field |

The gateway's image allow-list rejects any other image model. Video, STT and TTS names are fixed or limited on
the gateway side; the device defaults matter for direct callers and for the voice-chat LLM.

## Folder layout under `/data/PS5LM/media/`

The media root is `/data/PS5LM/media/` (`kMediaModelRoot`, `kMediaRoot` in the native sources). Every model name a
request sends is checked as one or two path segments under that root. Names may not contain `..`, `/` (except the
folder/file form), `\`, `:` or control bytes, and may be at most 128 bytes per segment. Nothing may resolve above the root.

```
/data/PS5LM/media/
  <image-or-video-model>/       one folder per SD or video model
    sd-model.conf               keys: mode (image|video), width, height, steps, frames, fps, cfg_scale,
                                flow_shift, sample_method, prediction, params_backend, max_vram,
                                and component paths: model, diffusion_model, llm, vae, clip_l, clip_g,
                                clip_vision, t5xxl
                                component paths are relative to the folder (or a sibling folder, not above the root)
  whisper/                      STT: model addressed as "whisper/<file>"
    ggml-large-v3-turbo-q5_0.bin
  qwen3-tts-1.7b/               TTS: one folder with exactly one backbone .gguf and one mmproj-*.gguf
    <backbone>.gguf
    mmproj-<name>.gguf
  qwen3-4b/                     voice LLM: addressed as "qwen3-4b/<file>.gguf"; the file must not start with "mmproj"
    Qwen3-4B-Q4_K_M.gguf
```

Image and video model folders are addressed by folder name alone. The `sd-model.conf` file, not the folder name,
decides whether a folder is an image or a video model (`mode=video`). An `sd-model.conf` line longer than 256 bytes,
or an unknown key, makes the model fail to load.

The exact file names inside each SD folder depend on the model and are not checked here. Confirm them against the
plan's conf examples before shipping a model (Needs Verification).
