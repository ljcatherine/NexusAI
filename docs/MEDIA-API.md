# Media API (:8083)

This documents the media listener as implemented in `native/nexus_media*.cpp`, `native/nexus_media.hpp`
and the engine endpoints in `native/nexus_control.cpp`. Where the integration plan differs from the code,
this follows the code.

**Not a client API.** The Nexus gateway (NexusBackend) is the only intended client. Do not call this
listener directly from apps, scripts or browsers. It has no CORS, no per-user identity and no rate limit of
its own; the gateway owns quotas, validation and logging.

Source of the media engines: PSllama commit `d079717` (GPL-3.0-or-later), see `NOTICE`.

## 1. Listener and peer rules

| Setting | Behaviour |
|---|---|
| Port | `8083` (`kMediaDefaultPort`) |
| Default bind | `127.0.0.1` (loopback only) |
| With the `nexus-control.lan` marker in `/data/PS5LM/` | bind `0.0.0.0`, LAN peers admitted |
| Admitted peers, default | `127.0.0.0/8`, Tailscale CGNAT `100.64.0.0/10` |
| Admitted peers, with marker | the above plus RFC1918: `10.0.0.0/8`, `172.16.0.0/12`, `192.168.0.0/16` |
| Everything else | closed at `accept()` without a response |

The peer filter runs on the accepted socket's IPv4 address (`media_peer_allowed`). The marker is the same
`nexus-control.lan` file that controls the control API on :8082. If the token file does not load, the listener
does not start: the log line is `ps5lm-media: credential unavailable, media listener not started`, and the app
shows the notification "the media API did not start".

Each accepted connection gets 30 s receive and send timeouts. The server handles one connection at a time on
a single thread, and the listen backlog is 4. A long generation therefore holds up any other client.
Every response carries `Connection: close`; one request per connection.

## 2. Authentication

- Header: `Authorization: Bearer <token>`. The scheme string `Bearer ` (capital B, one space) is required.
- Token: 32 to 128 characters from `A-Z a-z 0-9 - _`. Anything else is rejected as 401.
- Storage: the same file as the control API, `/data/PS5LM/nexus-control.token`, one line `sha256:<64 lowercase hex>`.
  The device stores only the SHA-256 of the token. Per request it hashes the supplied token and compares the
  digests in constant time.
- Order: the 401 check runs **after the header block is read and before any body byte is read**. An
  unauthenticated peer cannot make the server buffer a body. Every route is protected, including `GET /health`.
- A duplicate `Authorization` header is a 400 (checked during header parsing, before the 401 check).
- With the provisioning-pin build (`NEXUS_CONTROL_REQUIRE_PROVISIONING_PIN`, PS5 title only), the token file
  must also be mode `0600`, end in a newline, and match the pinned digest. Otherwise the listener does not start.

## 3. Request limits and transport errors

Checks run in this order. The first failure ends the request.

| Step | Limit or rule | Status | `message` |
|---|---|---|---|
| Header block | 8192 bytes, from the request line up to the blank line | 431 | `request headers too large` |
| Request line | `METHOD SP path SP HTTP/1.1`, path starts with `/`, exactly three tokens | 400 | `invalid or oversized HTTP request` |
| Header lines | every line has a colon; no duplicate `Content-Length`; no duplicate `Authorization`; `Transfer-Encoding` only `identity` | 400 | same |
| Bearer token | see section 2 | 401 | `valid Bearer API key required` |
| Body length | `Content-Length` digits only, at most 67108864 (64 MiB) | 413 | `request body too large` |
| Body presence | `POST` without `Content-Length` | 411 | `Content-Length required` |
| Body framing | bytes already received after the head must not exceed `Content-Length` | 400 | `invalid or oversized HTTP request` |
| Method | only `GET` and `POST` | 405 | `method not allowed` |
| Route | the query string is stripped; `METHOD path` must be registered | 404 | `endpoint not found` |

A connection that closes before any byte arrives gets no response.

Other limits are applied by the handlers and are listed per endpoint in section 6.

## 4. Error JSON

Every non-streamed error is:

```json
{"error":{"message":"...","type":"..."}}
```

| `type` | Used for |
|---|---|
| `authentication_error` | 401 |
| `invalid_request_error` | 400, 404, 405, 411, 413, 422, 431 and most validation errors |
| `model_load_error` | 500 when a model cannot be loaded |
| `server_error` | 500 when generation or transcription fails |

Validation errors (422) and the 404 for a missing model folder are plain JSON **even when the request set
`stream: true`**. Streaming begins only after validation and the model-folder check have passed.

Within a stream, a failure is the final data event with `http_status` set (image and video). Voice chat uses its
own error event (see its section).

## 5. GPU coordination

Only one engine set may hold GPU memory. Each request first calls `media_gpu_acquire(keep_mask)`, which
unloads every loaded engine not in `keep_mask` (`native/nexus_media_gpu.cpp`). The engine bits are
`SD`, `STT` (whisper), `TTS` (qwen3-tts) and `VoiceLLM`.

| Route | Unloads before running | Keeps loaded |
|---|---|---|
| `POST /v1/images/generations` | STT, TTS, voice LLM | SD |
| `POST /v1/videos/generations` | STT, TTS, voice LLM | SD |
| `POST /v1/audio/transcriptions` | SD, voice LLM | STT, TTS |
| `POST /v1/audio/speech` | SD, voice LLM | STT, TTS |
| `POST /v1/voice/chat` | SD | STT, TTS, voice LLM (each loaded as needed) |
| `GET /health` | nothing | nothing |

Rules that follow from the code:

- A model already loaded for the requested engine is reused. A load happens only when the requested model name
  differs from the loaded one. A different voice LLM is freed before the new one loads.
- Image and video streams announce each unload with a `{"status":"unloading",...}` event before the work starts.
  Non-streamed requests and the audio routes do not report unloads.
- Engines are never unloaded by a timer. They stay resident until another route claims the memory or the media
  API stops. Stopping it (app quit or restart) calls `media_gpu_acquire(0)`, which frees everything.
- Voice chat holds a mutex for the whole turn, so the voice LLM is never used by two turns at once.
- Voice prompt evaluation is submitted in 128-token chunks so that one submit stays well under the 10 s GPU
  watchdog (comment in `nexus_media_voice.cpp`).

## 6. Endpoints

### GET /health

Auth required. Returns the media mode marker.

Response `200`:

```json
{"status":"ok","mode":"media"}
```

### POST /v1/images/generations

Text to image, with optional image-to-image and reference images. The model is a folder under
`/data/PS5LM/media/` containing an `sd-model.conf` (layout in `docs/MODELS-LICENSES.md`).

**Request fields.** Any other JSON type for a field is a 422.

| Field | Type | Range or rule | Default |
|---|---|---|---|
| `prompt` | string | required, 1 to 4096 bytes | none |
| `model` | string | folder name, 1 to 128 bytes, no `/`, `\`, `:`, `..` or control bytes | loaded image model, else `sd-turbo-fp16` |
| `negative_prompt` | string | up to 4096 bytes | empty |
| `size` | string `"WxH"` | digits only, up to 5 per side | model default |
| `width` | integer | 256 to 1024, multiple of 64; send with `height`, or both 0 | model default |
| `height` | integer | 256 to 1024, multiple of 64; send with `width`, or both 0 | model default |
| `steps` | integer | 1 to 100 | model default |
| `cfg_scale` | number | 0 to 30, finite | model default |
| `seed` | integer | -1 (random) to 2147483647 | -1 |
| `sample_method` | string | passed to the SD service; an unknown value fails at generation (500) | model default |
| `prediction` | string | passed to the SD service on model load | model default |
| `stream` | boolean | | `false` |
| `strength` | number | 0 to 1; used with `init_image_b64` | 0.75 (SD service default) |
| `init_image_b64` | string | base64 PNG or JPEG, non-empty | none (text to image) |
| `ref_images_b64` | array of strings | at most 4, each base64 and non-empty | none |

`size` sets width and height; an explicit `width` or `height` replaces that value. Setting only one of
`width` and `height` is a 422.

422 message: `invalid image request; prompt is required; optional: model (folder in the media folder), size WxH or width/height (multiples of 64, 256..1024 per side), steps 1..100, cfg_scale, seed, negative_prompt, sample_method, prediction, stream, init_image_b64 with strength 0..1, ref_images_b64 (up to 4); omitted values use the model folder defaults (sd-model.conf)`

**Response, non-streamed, `200`:**

```json
{"created":0,"model":"sd-turbo-fp16","seed":123,"width":512,"height":512,"steps":1,"cfg_scale":1.00,
 "timings":{"load_s":0.00,"generate_s":1.23},"data":[{"b64_json":"<base64 PNG>"}]}
```

**Streaming** (`stream: true`): `200` with `text/event-stream`, chunked. Each `data:` line is one JSON object,
and the stream ends with `data: [DONE]`.

| Event | Shape |
|---|---|
| unloading | `{"status":"unloading","model":"<name>","elapsed_s":0.0}` for each unloaded engine; `"voice LLM"` for the voice LLM |
| loading | `{"status":"loading","model":"<name>","progress":0-100,"elapsed_s":0.0}`, sent on each new whole percent |
| step | `{"status":"<phase>","step":n,"steps":N,"elapsed_s":0.0}` for the phases `generating` and `decoding` (`nexus_sd_service.hpp`) |
| final | the non-streamed body with `"http_status":200` added, or `{"error":{...},"http_status":500}` |
| end | `[DONE]`, then the chunked stream ends |

Errors: `404` `invalid_request_error` `image model not found in the media folder` (plain JSON, before streaming);
`500` `model_load_error` and `500` `server_error`, both with the message from the SD service.

### POST /v1/videos/generations

Text to video, returning PNG frames. It uses the same SD service as images. The model must be a video model
(`mode=video` in its `sd-model.conf`).

**Request fields:**

| Field | Type | Range or rule | Default |
|---|---|---|---|
| `prompt` | string | required, 1 to 4096 bytes | none |
| `model` | string | folder name, as for images | `wan2.1-t2v-1.3b` |
| `negative_prompt` | string | up to 4096 bytes | empty |
| `size` | string `"WxH"` | each side 128 to 1280 and a multiple of 16, or 0 | model default |
| `width` | integer | 128 to 1280, multiple of 16, or 0 | model default |
| `height` | integer | 128 to 1280, multiple of 16, or 0 | model default |
| `frames` | integer | 1 to 121 and `frames % 4 == 1` (1, 5, 9 ... 121), or 0 | model default |
| `steps` | integer | 1 to 100 | model default |
| `cfg_scale` | number | 0 to 30 | model default |
| `seed` | integer | -1 to 2147483647 | -1 |
| `init_image_b64` | string | base64 PNG or JPEG first frame, non-empty | none |
| `stream` | boolean | | `false` |

Unlike images, a 0 for one side is allowed, and `width` and `height` do not have to be sent together.

**Response, non-streamed, `200`:**

```json
{"model":"wan2.1-t2v-1.3b","seed":7,"width":832,"height":480,"frames":17,"steps":20,"fps":16,
 "timings":{"load_s":0.00,"generate_s":60.00},"data":[{"b64_png":"<base64 PNG>"},"..."]}
```

`frames` is the number of PNG frames returned, and `fps` comes from the model's `sd-model.conf`.

**Streaming:** the same SSE framing as images, with the same `unloading` events and the final event carrying
`http_status`. The progress callback sends the `step` shape for every phase, including `loading`; video streams
have no `progress` percent event.

Errors: `404` `invalid_request_error` `video model not found in the media folder` (plain JSON); `500`
`model_load_error`; `500` `server_error`.

### POST /v1/audio/transcriptions

Speech to text with Whisper (ggml).

**Request fields:**

| Field | Type | Range or rule | Default |
|---|---|---|---|
| `audio_b64` | string | required; base64 of a RIFF/WAVE file; decoded at most 16 MiB | none |
| `model` | string | `folder/file`, both parts pass the folder rule | loaded STT model, else `whisper/ggml-large-v3-turbo-q5_0.bin` |
| `language` | string | `""` or `"auto"` to detect, else a language code | detect |
| `translate` | boolean | | `false` |

WAV handling (`nexus_wav.cpp`): PCM 8, 16, 24 or 32 bit, or float 32 bit. Any channel count, downmixed to mono.
Resampled to 16 kHz with linear interpolation.

**Response, `200`:**

```json
{"text":" hello there","language":"en","model":"whisper/ggml-large-v3-turbo-q5_0.bin",
 "segments":[{"start":0.00,"end":1.20,"text":" hello there"}],
 "timings":{"audio_s":1.20,"load_s":0.00,"transcribe_s":0.30}}
```

`load_s` is non-zero only when this request loaded the model.

Errors: `422` `invalid_request_error` with the message `audio_b64 (base64 WAV) is required; optional model, language, translate`
when a field is wrong; `422` `audio_b64 is not valid base64`; `422` with the WAV reader's message (for example
`audio must be a RIFF/WAVE file`, or `unsupported WAV encoding (PCM 8/16/24/32-bit or float 32 expected)`);
`413` `audio_b64 decodes to more than 16 MiB`; `500` `model_load_error`; `500` `server_error`.

### POST /v1/audio/speech

Text to speech with qwen3-tts (a GGUF backbone plus an `mmproj-*.gguf` projector).

**Request fields:**

| Field | Type | Range or rule | Default |
|---|---|---|---|
| `input` | string | required, 1 to 4096 bytes | none |
| `model` | string | one folder name | loaded TTS model, else `qwen3-tts-1.7b` |
| `language` | string | model-specific (for example `"en"`) | empty |
| `speaker_b64` | string | base64 WAV reference voice, decoded at most 16 MiB | none (model default voice) |
| `seed` | integer | -1 to 2147483647 | -1 |
| `max_frames` | integer | 12 to 4096 | 1024 |
| `response_format` | string | `"json"` or `"wav"` | `"json"` |

Sampling is fixed in the request path: top_k 50, top_p 0.9, temperature 0.9.

**Response, `json`, `200`:**

```json
{"model":"qwen3-tts-1.7b","sample_rate":24000,"frames":120,
 "timings":{"audio_s":2.50,"load_s":0.00,"generate_s":1.10},"audio_b64":"<base64 WAV, 16-bit mono>"}
```

**Response, `wav`, `200`:** the raw WAV bytes with `Content-Type: audio/wav`, no JSON.

Errors: `422` `input is required; optional model, language, speaker_b64 (base64 WAV), seed, max_frames, response_format json|wav`
(also used for an invalid `speaker_b64`); `422` `input is longer than 4096 bytes`; `413` `speaker_b64 decodes to more than 16 MiB`;
`500` `model_load_error`; `500` `server_error`.

### POST /v1/voice/chat

One spoken turn: speech to text, a chat LLM reply, and text to speech for each sentence. Once validation passes,
the answer is **always an SSE stream**. Validation errors before that are plain JSON `422`.

**Request fields:**

| Field | Type | Range or rule | Default |
|---|---|---|---|
| `audio_b64` | string | required; base64 WAV; at most 120 s after conversion to 16 kHz mono (1 920 000 samples) | none |
| `speaker_b64` | string | base64 WAV reference voice; limited only by the 64 MiB body | none (the first sentence's voice is reused) |
| `system` | string | up to 2048 bytes | "You are a helpful voice assistant. Answer in one to three short spoken sentences. Plain words only: no lists, markdown, emoji or code." |
| `language` | string | up to 16 bytes | empty |
| `llm` | string | `folder/file.gguf`, file not starting with `mmproj`; up to 128 bytes | `qwen3-4b/Qwen3-4B-Q4_K_M.gguf` |
| `stt_model` | string | `folder/file`; up to 128 bytes | `whisper/ggml-large-v3-turbo-q5_0.bin` |
| `tts_model` | string | one folder name; up to 128 bytes | `qwen3-tts-1.7b` |
| `messages` | array | at most 20 objects `{role: system\|user\|assistant, content: string}`; total content at most 16384 bytes | none |
| `max_tokens` | integer | 1 to 1024 | 200 |

Chat settings are fixed: context 4096 tokens, top_k 40, top_p 0.9, temperature 0.7. Prompt tokens plus
`max_tokens` must fit the context, otherwise the turn fails with `conversation is too long for the context`.

Sentence split: a sentence ends at `.`, `!` or `?` followed by whitespace, once at least 12 bytes are pending.
Each sentence is spoken with `max_frames` 600 and seed 7. A sentence whose TTS fails is skipped silently
(logged only).

**SSE events.** Each is `data: <json>`; the stream ends with `data: [DONE]`.

| `type` or `status` | Shape |
|---|---|
| loading | `{"status":"loading","model":"<name>","elapsed_s":0.0}` for the LLM, and for STT or TTS when that model changes |
| transcript | `{"type":"transcript","text":"...","audio_s":1.2,"at_s":0.4}` |
| text | `{"type":"text","delta":"..."}` for each generated piece |
| audio | `{"type":"audio","index":0,"audio_s":1.8,"tts_s":0.6,"at_s":1.9,"text":"...","audio_b64":"<WAV>"}` |
| done | `{"type":"done","transcript":"...","reply":"...","tokens":57,"sentences":3,"timings":{"ready_s":0,"transcript_s":0,"first_text_s":0,"first_audio_s":0,"tts_s":0,"speech_s":0,"total_s":0}}` |
| error | `{"type":"error","message":"..."}`, then `[DONE]` |

Pre-stream `422` `invalid_request_error` messages include `audio_b64 (base64 WAV) is required`,
`llm, stt_model or tts_model is not a valid model name`, `max_tokens must be 1 to 1024`,
`audio_b64 is not a readable WAV file`, `audio_b64 is longer than 120 seconds`, `audio_b64 has no audio`,
`messages must be an array of at most 20 objects {role: system|user|assistant, content: string}`, and
`messages are longer than 16384 bytes in total`.

Model-load and transcription failures during the turn arrive as the `error` event, not as an HTTP status.

### POST /v1/nexus/engine/media

**Control API, port 8082**, not 8083. Switches the app into media mode.

- Auth: the same bearer token as the control API (section 7).
- `409` `{"error":"model switch already pending"}` if a switch is already queued; `409`
  `{"error":"inference is active"}` while generation is running.
- Success: `202` `{"status":"switching","restart_required":true}`. The app restarts about three frames later.
  `model.txt` becomes `media` on line 1 and the LLM to return to on line 2, and `app-args.txt` is removed.
  The dashboard shows "Switching to media mode", with "about 30 seconds".
- Media mode starts on a later launch only when `model.txt` begins with `media` and `app-args.txt` is absent.

### POST /v1/nexus/engine/llm

**Control API, port 8082.** Returns to the LLM engine, with the same refusals and `202` body as above.
`model.txt` gets the LLM model path (or `none`), and the app restarts with llama-server.

### GET /v1/nexus/status

**Control API, port 8082.** Returns the existing status fields plus `"engine"`:

- `"engine":"llm"` by default, and `"engine":"media"` when the app started in media mode. The value is set from
  `main.cpp` before the control API begins serving.

## 7. Control API (:8082) notes

The control API controls the same app, and its rules differ from the media listener's:

- Binds `127.0.0.1:8082`, or `0.0.0.0:8082` with `nexus-control.lan`. The accept path applies the same peer rule as
  the media listener (`peer_allowed_ipv4` in `native/nexus_peer.hpp`): loopback and CGNAT always, RFC1918 only with
  the marker. Refused peers are closed before any byte is read. The bearer token is still required.
- Auth: `Authorization: Bearer <token>`, the same token file and SHA-256 rule as the media API.
- The error shape differs: `{"error":"<message>"}`, a plain string, not the nested `error` object.
  Bad or missing auth: `401` `{"error":"unauthorized"}`.
- Headers over 8192 bytes: `413` `{"error":"headers too large"}`. A body other than `Content-Length: 0`: `400`
  `{"error":"body not supported"}`. `Transfer-Encoding` is refused with `400`.
- Socket timeouts: 2 s.
- Unknown routes: `404` `{"error":"unknown endpoint"}`. A method other than GET or POST: `405`.

## 8. Vulkan tuning switches (patch 0104)

`patches/llama.cpp/0104-ps5-vulkan-tuning-env.patch`. All switches are opt-in: with none set, the build behaves like
upstream. The variables are read with `getenv` at device init, so they must be in the environment before the app
starts. Nothing in this repository sets them.

| Variable | Value | Default (unset) | Effect |
|---|---|---|---|
| `GGML_VK_RM_KQ` | integer, clamped to at least 1 | upstream heuristic (4 rows on RDNA3/4, else the heuristic value) | rows per workgroup for k-quant mat-vec shaders; also sets `RM_IQ` to `2 * RM_KQ` |
| `GGML_VK_RM_IQ` | integer, clamped to at least 1 | `2 * RM_KQ` | rows per workgroup for i-quant mat-vec shaders; overrides the value from `RM_KQ` when both are set |
| `GGML_VK_DMMV_WG` | `large` only | heuristic (subgroup size) | any other value is ignored; `large` forces the 4-subgroup dequant mat-vec workgroup |
| `PS5_OUTPUT_CPU_BUFFER` | any value (presence test) | output logits in the device host buffer | keeps the output logits in plain CPU memory instead of the device host buffer; the CPU sampler reads every logit each token |

`RM_KQ` and `RM_IQ` are parsed with `atoi`, so a non-numeric value becomes 1.

## 9. Media-mode environment (patch 0011)

`patches/0011-nexus-media-mode.patch` sets two variables at start-up, only when media mode is requested
(`model.txt` first line `media`, no `app-args.txt`):

| Variable | Value | Why |
|---|---|---|
| `GGML_VK_IGNORE_MEMORY_BUDGET` | `1` | media mode creates the ggml-vulkan device itself, so the budget check is skipped |
| `GGML_VK_SUBALLOCATION_BLOCK_SIZE` | `268435456` (256 MiB) | larger suballocation blocks for the multi-engine GPU heap |

Both are set before the first Vulkan init. Media mode also calls `llama_backend_init()` itself, because it runs no
llama-server.

## 10. Known gaps

- Single-threaded: one generation blocks all other media requests, and clients queue in the 4-slot backlog.
- The control API applies the same peer filter as the media listener (section 7). Keep `nexus-control.lan` off unless a firewall limits :8082.
- Image and video `sample_method` and `prediction` are only type-checked here. Bad values surface as `500 server_error`.
- No request IDs and no access log beyond the `ps5lm-media:` lines in the app log.
