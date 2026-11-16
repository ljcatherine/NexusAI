// Speech endpoints of the media API: POST /v1/audio/transcriptions (speech to text) and
// POST /v1/audio/speech (text to speech). Same JSON fields and 422 messages as PSllama.
// Derived from PSllama (commit d079717), GPL-3.0-or-later.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "nexus_media.hpp"
#include "nexus_audio_service.hpp"
#include "nexus_media_path.hpp"
#include "nexus_wav.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

namespace ps5lm {
namespace {
constexpr std::size_t kMaxAudioBytes = 16u << 20; // decoded WAV: STT input or TTS speaker reference
constexpr std::size_t kMaxSpeechText = 4096;      // bytes of "input"
constexpr char kSttDefaultModel[] = "whisper/ggml-large-v3-turbo-q5_0.bin";
constexpr char kTtsDefaultModel[] = "qwen3-tts-1.7b";
constexpr char kSttInvalid[] = "audio_b64 (base64 WAV) is required; optional model, language, translate";
constexpr char kTtsInvalid[] = "input is required; optional model, language, speaker_b64 (base64 WAV), seed, "
                               "max_frames, response_format json|wav";

// --- Request fields (nlohmann, as in nexus_media_voice.cpp; requests are untrusted) ---------

// Required string member: false when absent or not a JSON string.
bool string_field(const nlohmann::json & body, const char * key, std::string & value) {
    const auto found = body.find(key);
    if (found == body.end() || !found->is_string()) return false;
    value = found->get<std::string>();
    return true;
}

// Optional string member: absent leaves `value` unchanged; present must be a JSON string.
bool optional_string(const nlohmann::json & body, const char * key, std::string & value) {
    if (body.find(key) == body.end()) return true;
    return string_field(body, key, value);
}

// Optional boolean member: present must be true or false.
bool optional_bool(const nlohmann::json & body, const char * key, bool & value) {
    const auto found = body.find(key);
    if (found == body.end()) return true;
    if (!found->is_boolean()) return false;
    value = found->get<bool>();
    return true;
}

// Optional integer member within [minimum, maximum]. Only JSON integers qualify. Negative
// values are allowed so that seed -1 ("random") can be sent explicitly.
bool optional_int(const nlohmann::json & body, const char * key, long long minimum, long long maximum, int & value) {
    const auto found = body.find(key);
    if (found == body.end()) return true;
    if (!found->is_number_integer()) return false;
    long long parsed = 0;
    if (found->is_number_unsigned()) {
        const unsigned long long big = found->get<unsigned long long>();
        if (big > static_cast<unsigned long long>(maximum)) return false;
        parsed = static_cast<long long>(big);
    } else {
        parsed = found->get<long long>();
    }
    if (parsed < minimum || parsed > maximum) return false;
    value = static_cast<int>(parsed);
    return true;
}

// --- Model names and transport -------------------------------------------------------------

// STT model: "folder/file" (both parts media_segment_ok). TTS model: one segment.
// Each part is checked with media_segment_ok; the audio service resolves the path.
bool valid_model(const std::string & model, bool with_file) {
    const std::size_t slash = model.find('/');
    if ((slash != std::string::npos) != with_file) return false;
    if (!media_segment_ok(model.substr(0, slash))) return false;
    return !with_file || media_segment_ok(model.substr(slash + 1));
}
} // namespace

// POST /v1/audio/transcriptions: {"audio_b64": WAV, "model", "language", "translate"}.
void handle_transcription(int fd, const MediaRequest & request) {
    const nlohmann::json body = nlohmann::json::parse(request.body, nullptr, false);
    std::string model = stt_loaded_model().empty() ? std::string(kSttDefaultModel) : stt_loaded_model();
    std::string audio_b64, language, error;
    bool translate = false;
    const bool fields = body.is_object() && string_field(body, "audio_b64", audio_b64) && !audio_b64.empty() &&
        optional_bool(body, "translate", translate) && optional_string(body, "model", model) &&
        valid_model(model, true) && optional_string(body, "language", language);
    if (!fields) {
        media_send_error(fd, 422, "invalid_request_error", kSttInvalid);
        return;
    }
    if (audio_b64.size() / 4 * 3 > kMaxAudioBytes) {
        media_send_error(fd, 413, "invalid_request_error", "audio_b64 decodes to more than 16 MiB");
        return;
    }
    std::string wav;
    if (!base64_decode(audio_b64, wav, error)) {
        media_send_error(fd, 422, "invalid_request_error", "audio_b64 is not valid base64");
        return;
    }
    std::vector<float> samples;
    if (!decode_wav(wav, 16000, samples, error)) {
        media_send_error(fd, 422, "invalid_request_error", error);
        return;
    }

    media_gpu_acquire(kGpuStt | kGpuTts);
    double load_s = 0.0;
    if (stt_loaded_model() != model && !stt_load(model, error, &load_s)) {
        media_send_error(fd, 500, "model_load_error", error);
        return;
    }
    SttResult result;
    if (!stt_transcribe(samples, language, translate, result, error)) {
        media_send_error(fd, 500, "server_error", error);
        return;
    }
    result.load_s = load_s;

    std::string segments;
    for (const SttSegment & segment : result.segments) {
        char head[128];
        std::snprintf(head, sizeof(head), "{\"start\":%.2f,\"end\":%.2f,\"text\":", segment.start_s, segment.end_s);
        segments += (segments.empty() ? "" : ",") + std::string(head) + json_quote(segment.text) + "}";
    }
    char timings[192];
    std::snprintf(timings, sizeof(timings), "{\"audio_s\":%.2f,\"load_s\":%.2f,\"transcribe_s\":%.2f}",
                  result.audio_s, result.load_s, result.transcribe_s);
    media_send_json(fd, 200, "{\"text\":" + json_quote(result.text) + ",\"language\":" + json_quote(result.language) +
                    ",\"model\":" + json_quote(model) + ",\"segments\":[" + segments + "],\"timings\":" + timings + "}");
}

// POST /v1/audio/speech: {"input", "model", "language", "speaker_b64" (WAV),
// "seed", "max_frames", "response_format": "json" (default, audio_b64) | "wav"}.
void handle_speech(int fd, const MediaRequest & request) {
    const nlohmann::json body = nlohmann::json::parse(request.body, nullptr, false);
    TtsRequest tts;
    std::string model = tts_loaded_model().empty() ? std::string(kTtsDefaultModel) : tts_loaded_model();
    std::string speaker_b64, format = "json", error;
    int seed = -1;
    const bool fields = body.is_object() && string_field(body, "input", tts.text) && !tts.text.empty() &&
        optional_int(body, "seed", -1, 0x7fffffff, seed) &&
        optional_int(body, "max_frames", 12, 4096, tts.max_frames) &&
        optional_string(body, "model", model) && valid_model(model, false) &&
        optional_string(body, "language", tts.language) &&
        optional_string(body, "response_format", format) && (format == "json" || format == "wav") &&
        optional_string(body, "speaker_b64", speaker_b64);
    if (!fields) {
        media_send_error(fd, 422, "invalid_request_error", kTtsInvalid);
        return;
    }
    if (tts.text.size() > kMaxSpeechText) {
        media_send_error(fd, 422, "invalid_request_error", "input is longer than 4096 bytes");
        return;
    }
    if (speaker_b64.size() / 4 * 3 > kMaxAudioBytes) {
        media_send_error(fd, 413, "invalid_request_error", "speaker_b64 decodes to more than 16 MiB");
        return;
    }
    if (!speaker_b64.empty() && !base64_decode(speaker_b64, tts.speaker_wav, error)) {
        media_send_error(fd, 422, "invalid_request_error", kTtsInvalid);
        return;
    }
    tts.seed = seed;

    media_gpu_acquire(kGpuStt | kGpuTts);
    double load_s = 0.0;
    if (tts_loaded_model() != model && !tts_load(model, error, &load_s)) {
        media_send_error(fd, 500, "model_load_error", error);
        return;
    }
    TtsResult result;
    if (!tts_generate(tts, result, error, [](int, int) {})) {
        media_send_error(fd, 500, "server_error", error);
        return;
    }
    result.load_s = load_s;
    if (format == "wav") {
        media_send_bytes(fd, 200, "audio/wav", result.wav);
        return;
    }
    char timings[192];
    std::snprintf(timings, sizeof(timings), "{\"audio_s\":%.2f,\"load_s\":%.2f,\"generate_s\":%.2f}",
                  result.audio_s, result.load_s, result.generate_s);
    media_send_json(fd, 200, "{\"model\":" + json_quote(model) + ",\"sample_rate\":" + std::to_string(result.sample_rate) +
                    ",\"frames\":" + std::to_string(result.frames) + ",\"timings\":" + timings +
                    ",\"audio_b64\":\"" + base64_encode(result.wav) + "\"}");
}

void register_media_audio_routes(MediaServer & server) {
    server.route("POST", "/v1/audio/transcriptions", handle_transcription);
    server.route("POST", "/v1/audio/speech", handle_speech);
}
} // namespace ps5lm
