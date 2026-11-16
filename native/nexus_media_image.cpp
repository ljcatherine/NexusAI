// Derived from PSllama (commit d079717), GPL-3.0-or-later.
// SPDX-License-Identifier: GPL-3.0-or-later
// POST /v1/images/generations and POST /v1/videos/generations for the media listener.
//
// Service access: media_image_service() is the one SdService of the media process
// (function-local static, model root kMediaModelRoot), declared in nexus_media.hpp.
// Before a generation the other engines (STT, TTS, voice LLM) are unloaded through
// media_gpu_acquire(kGpuSd), as PSllama's free_gpu_for did.
#include "nexus_media.hpp"
#include "nexus_audio_service.hpp"
#include "nexus_media_path.hpp"
#include "nexus_sd_service.hpp"
#include "nexus_wav.hpp"

#include <nlohmann/json.hpp>

#include <sys/stat.h>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace ps5lm {
namespace {
constexpr char kMediaModelRoot[] = "/data/PS5LM/media/";
constexpr char kDefaultImageModel[] = "sd-turbo-fp16";
constexpr char kDefaultVideoModel[] = "wan2.1-t2v-1.3b";
constexpr std::size_t kMaxPromptBytes = 4096;  // same limit SdService applies
constexpr std::size_t kMaxRefImages = 4;

using Clock = std::chrono::steady_clock;

double seconds_since(Clock::time_point started) {
    return std::chrono::duration<double>(Clock::now() - started).count();
}

std::string fixed(double value, int decimals) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
    return buffer;
}

std::string bytes_of(const std::vector<unsigned char> & data) {
    if (data.empty()) return {};
    return std::string(reinterpret_cast<const char *>(data.data()), data.size());
}

// ---- Request fields (nlohmann, as in nexus_media_voice.cpp; requests are untrusted) ----

// True when the key is a member of the request object.
bool present(const nlohmann::json & body, const char * key) {
    return body.find(key) != body.end();
}

// String member. False when absent or not a JSON string.
bool string_member(const nlohmann::json & body, const char * key, std::string & value) {
    const auto found = body.find(key);
    if (found == body.end() || !found->is_string()) return false;
    value = found->get<std::string>();
    return true;
}

// Optional integer in [low, high]. Absent: returns true and leaves value unchanged.
// Only JSON integers qualify; floats, strings and booleans are a type error.
template <typename T>
bool optional_int(const nlohmann::json & body, const char * key, long long low, long long high, T & value) {
    const auto found = body.find(key);
    if (found == body.end()) return true;
    if (!found->is_number_integer()) return false;
    long long parsed = 0;
    if (found->is_number_unsigned()) {
        const unsigned long long big = found->get<unsigned long long>();
        if (big > static_cast<unsigned long long>(high)) return false;
        parsed = static_cast<long long>(big);
    } else {
        parsed = found->get<long long>();
    }
    if (parsed < low || parsed > high) return false;
    value = static_cast<T>(parsed);
    return true;
}

// Optional finite number in [low, high], checked after conversion to float.
// Absent: returns true and leaves value unchanged.
bool optional_float(const nlohmann::json & body, const char * key, float low, float high, float & value) {
    const auto found = body.find(key);
    if (found == body.end()) return true;
    if (!found->is_number()) return false;
    const float parsed = static_cast<float>(found->get<double>());
    if (!std::isfinite(parsed) || parsed < low || parsed > high) return false;
    value = parsed;
    return true;
}

// Optional true/false. Absent: returns true and leaves value unchanged.
bool optional_bool(const nlohmann::json & body, const char * key, bool & value) {
    const auto found = body.find(key);
    if (found == body.end()) return true;
    if (!found->is_boolean()) return false;
    value = found->get<bool>();
    return true;
}

// Optional array of base64 strings, decoded strictly, each non-empty, at most max_items.
// Absent: returns true and leaves out unchanged.
bool optional_base64_list(const nlohmann::json & body, const char * key, std::size_t max_items,
                          std::vector<std::string> & out) {
    const auto found = body.find(key);
    if (found == body.end()) return true;
    if (!found->is_array() || found->size() > max_items) return false;
    for (const nlohmann::json & item : *found) {
        std::string bytes, error;
        if (!item.is_string() || !base64_decode(item.get<std::string>(), bytes, error) || bytes.empty()) return false;
        out.push_back(std::move(bytes));
    }
    return true;
}

bool parse_dimension(const std::string & text, int & out) {
    if (text.empty() || text.size() > 5 || text.find_first_not_of("0123456789") != std::string::npos) return false;
    out = std::atoi(text.c_str());
    return true;
}

// "WxH", as PSllama's "size" property.
bool parse_size(const std::string & text, int & width, int & height) {
    const std::size_t x = text.find('x');
    if (x == std::string::npos) return false;
    return parse_dimension(text.substr(0, x), width) && parse_dimension(text.substr(x + 1), height);
}

// ---- Responses ----

std::string error_body(const char * type, const std::string & message) {
    return "{\"error\":{\"message\":" + json_quote(message) + ",\"type\":" + json_quote(type) + "}}";
}

// Sends the final JSON: a plain response, or (streaming) the last SSE event with
// "http_status" added, then [DONE].
void finish_response(int fd, MediaSse & sse, bool streaming, int code, const std::string & json) {
    if (!streaming) {
        media_send_json(fd, code, json);
        return;
    }
    std::string last = json;
    last.insert(last.size() - 1, ",\"http_status\":" + std::to_string(code));
    sse.event(last);
    sse.event("[DONE]");
    sse.end();
}

// Unloads the STT, TTS and voice LLM models (they share the GPU heap) through the GPU
// coordinator. Each unload is announced; the names are read before the unload.
void unload_other_engines(MediaSse & sse, Clock::time_point started) {
    const std::string stt = stt_loaded_model();
    const std::string tts = tts_loaded_model();
    const unsigned unloaded = media_gpu_acquire(kGpuSd);
    auto announce = [&](const std::string & model) {
        sse.event("{\"status\":\"unloading\",\"model\":" + json_quote(model) + ",\"elapsed_s\":" +
                  fixed(seconds_since(started), 1) + "}");
    };
    if (unloaded & kGpuStt) announce(stt);
    if (unloaded & kGpuTts) announce(tts);
    if (unloaded & kGpuVoiceLlm) announce("voice LLM");
}

std::string step_event(const char * phase, int value, int total, Clock::time_point started) {
    return "{\"status\":" + json_quote(phase) + ",\"step\":" + std::to_string(value) + ",\"steps\":" +
        std::to_string(total) + ",\"elapsed_s\":" + fixed(seconds_since(started), 1) + "}";
}
} // namespace

// The media process's Stable Diffusion service. Constructed on first use; it unloads
// its model in its destructor at exit.
SdService & media_image_service() {
    static SdService service(kMediaModelRoot);
    return service;
}

// POST /v1/images/generations: OpenAI-style text to image, PSllama's request fields.
void handle_image_generation(int fd, const MediaRequest & request) {
    const nlohmann::json body = nlohmann::json::parse(request.body, nullptr, false);
    SdService & service = media_image_service();
    SdImageRequest image;
    std::string model = service.loaded_model();
    if (model.empty()) model = kDefaultImageModel;
    std::string prediction, size;
    int64_t seed = -1;
    bool want_stream = false;

    bool ok = body.is_object() && string_member(body, "prompt", image.prompt) && !image.prompt.empty() &&
        image.prompt.size() <= kMaxPromptBytes;
    if (ok && present(body, "model")) ok = string_member(body, "model", model) && media_segment_ok(model);
    if (ok && present(body, "negative_prompt")) ok = string_member(body, "negative_prompt", image.negative_prompt) &&
        image.negative_prompt.size() <= kMaxPromptBytes;
    if (ok && present(body, "sample_method")) ok = string_member(body, "sample_method", image.sample_method);
    if (ok && present(body, "prediction")) ok = string_member(body, "prediction", prediction);
    if (ok && present(body, "size")) ok = string_member(body, "size", size) &&
        parse_size(size, image.width, image.height);
    // Omitted size/steps/cfg_scale (0 / -1) take the model's defaults.
    ok = ok && optional_int(body, "width", 256, 1024, image.width) &&
         optional_int(body, "height", 256, 1024, image.height) &&
         optional_int(body, "steps", 1, 100, image.steps) &&
         optional_float(body, "cfg_scale", 0.0f, 30.0f, image.cfg_scale) &&
         optional_int(body, "seed", -1, 0x7fffffff, seed) &&
         optional_bool(body, "stream", want_stream) &&
         optional_float(body, "strength", 0.0f, 1.0f, image.strength) &&
         image.width % 64 == 0 && image.height % 64 == 0 &&
         (image.width == 0) == (image.height == 0) &&
         (image.width == 0 || (image.width >= 256 && image.width <= 1024 &&
                               image.height >= 256 && image.height <= 1024));
    // Editing: "init_image_b64" (image to image, with "strength") and "ref_images_b64" (up to 4).
    if (ok && present(body, "init_image_b64")) {
        std::string encoded, error;
        ok = string_member(body, "init_image_b64", encoded) &&
            base64_decode(encoded, image.init_image, error) && !image.init_image.empty();
    }
    ok = ok && optional_base64_list(body, "ref_images_b64", kMaxRefImages, image.ref_images);
    if (!ok) {
        media_send_error(fd, 422, "invalid_request_error",
            "invalid image request; prompt is required; optional: model (folder in the media folder), size WxH or width/height (multiples of 64, 256..1024 per side), steps 1..100, cfg_scale, seed, negative_prompt, sample_method, prediction, stream, init_image_b64 with strength 0..1, ref_images_b64 (up to 4); omitted values use the model folder defaults (sd-model.conf)");
        return;
    }
    image.seed = seed;

    struct stat info{};
    if (::stat((std::string(kMediaModelRoot) + model).c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) {
        media_send_error(fd, 404, "invalid_request_error", "image model not found in the media folder");
        return;
    }

    MediaSse sse(fd);
    if (want_stream) sse.begin();
    const Clock::time_point started = Clock::now();
    unload_other_engines(sse, started);

    int last_percent = -1, last_step = -1;
    const SdProgress progress = [&](const char * phase, int value, int total) {
        if (!sse.open()) return;
        if (std::strcmp(phase, "loading") == 0) {
            const int percent = total > 0 ? value * 100 / total : 0;
            if (percent == last_percent) return;
            last_percent = percent;
            sse.event("{\"status\":\"loading\",\"model\":" + json_quote(model) + ",\"progress\":" +
                      std::to_string(percent) + ",\"elapsed_s\":" + fixed(seconds_since(started), 1) + "}");
        } else {
            if (value == last_step) return;
            last_step = value;
            sse.event(step_event(phase, value, total, started));
        }
    };

    std::string error;
    double load_s = 0.0;
    if (service.loaded_model() != model &&
        !service.load(model, prediction, error, progress, &load_s)) {
        finish_response(fd, sse, want_stream, 500, error_body("model_load_error", error));
        return;
    }
    SdImageResult result;
    if (!service.generate(image, result, error, progress)) {
        finish_response(fd, sse, want_stream, 500, error_body("server_error", error));
        return;
    }
    const std::string json = "{\"created\":0,\"model\":" + json_quote(model) +
        ",\"seed\":" + std::to_string(static_cast<long long>(result.seed)) +
        ",\"width\":" + std::to_string(result.width) + ",\"height\":" + std::to_string(result.height) +
        ",\"steps\":" + std::to_string(result.steps) + ",\"cfg_scale\":" + fixed(result.cfg_scale, 2) +
        ",\"timings\":{\"load_s\":" + fixed(load_s, 2) + ",\"generate_s\":" + fixed(result.generate_s, 2) +
        "},\"data\":[{\"b64_json\":\"" + base64_encode(bytes_of(result.png)) + "\"}]}";
    finish_response(fd, sse, want_stream, 200, json);
}

// POST /v1/videos/generations: like the image route for video models (sd-model.conf
// mode=video), plus "frames"; returns PNG frames and fps.
void handle_video_generation(int fd, const MediaRequest & request) {
    const nlohmann::json body = nlohmann::json::parse(request.body, nullptr, false);
    SdService & service = media_image_service();
    SdVideoRequest video;
    std::string model = kDefaultVideoModel, size;
    int64_t seed = -1;
    bool want_stream = false;

    bool ok = body.is_object() && string_member(body, "prompt", video.prompt) && !video.prompt.empty() &&
        video.prompt.size() <= kMaxPromptBytes;
    if (ok && present(body, "model")) ok = string_member(body, "model", model) && media_segment_ok(model);
    if (ok && present(body, "negative_prompt")) ok = string_member(body, "negative_prompt", video.negative_prompt) &&
        video.negative_prompt.size() <= kMaxPromptBytes;
    if (ok && present(body, "size")) ok = string_member(body, "size", size) &&
        parse_size(size, video.width, video.height);
    ok = ok && optional_int(body, "width", 128, 1280, video.width) &&
         optional_int(body, "height", 128, 1280, video.height) &&
         optional_int(body, "frames", 1, 121, video.frames) &&
         optional_int(body, "steps", 1, 100, video.steps) &&
         optional_float(body, "cfg_scale", 0.0f, 30.0f, video.cfg_scale) &&
         optional_int(body, "seed", -1, 0x7fffffff, seed) &&
         optional_bool(body, "stream", want_stream) &&
         video.width % 16 == 0 && video.height % 16 == 0 &&
         (video.width == 0 || (video.width >= 128 && video.width <= 1280)) &&
         (video.height == 0 || (video.height >= 128 && video.height <= 1280)) &&
         (video.frames == 0 || video.frames % 4 == 1);
    if (ok && present(body, "init_image_b64")) {
        std::string encoded, error;
        ok = string_member(body, "init_image_b64", encoded) &&
            base64_decode(encoded, video.init_image, error) && !video.init_image.empty();
    }
    if (!ok) {
        media_send_error(fd, 422, "invalid_request_error",
            "prompt is required; optional model, size WxH (multiples of 16), frames (4k+1), steps, cfg_scale, seed, negative_prompt, init_image_b64 (PNG/JPEG first frame for image-to-video models), stream");
        return;
    }
    video.seed = seed;

    struct stat info{};
    if (::stat((std::string(kMediaModelRoot) + model).c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) {
        media_send_error(fd, 404, "invalid_request_error", "video model not found in the media folder");
        return;
    }

    MediaSse sse(fd);
    if (want_stream) sse.begin();
    const Clock::time_point started = Clock::now();
    unload_other_engines(sse, started);

    int last_value = -1;
    const SdProgress progress = [&](const char * phase, int value, int total) {
        if (!sse.open() || value == last_value) return;
        last_value = value;
        sse.event(step_event(phase, value, total, started));
    };

    std::string error;
    double load_s = 0.0;
    if (service.loaded_model() != model &&
        !service.load(model, "", error, progress, &load_s)) {
        finish_response(fd, sse, want_stream, 500, error_body("model_load_error", error));
        return;
    }
    SdVideoResult result;
    if (!service.generate_video(video, result, error, progress)) {
        finish_response(fd, sse, want_stream, 500, error_body("server_error", error));
        return;
    }
    std::string json = "{\"model\":" + json_quote(model) +
        ",\"seed\":" + std::to_string(static_cast<long long>(result.seed)) +
        ",\"width\":" + std::to_string(result.width) + ",\"height\":" + std::to_string(result.height) +
        ",\"frames\":" + std::to_string(result.png_frames.size()) +
        ",\"steps\":" + std::to_string(result.steps) + ",\"fps\":" + std::to_string(result.fps) +
        ",\"timings\":{\"load_s\":" + fixed(load_s, 2) + ",\"generate_s\":" + fixed(result.generate_s, 2) +
        "},\"data\":[";
    for (std::size_t i = 0; i < result.png_frames.size(); ++i) {
        if (i) json += ',';
        json += "{\"b64_png\":\"" + base64_encode(bytes_of(result.png_frames[i])) + "\"}";
    }
    json += "]}";
    finish_response(fd, sse, want_stream, 200, json);
}

void register_media_image_routes(MediaServer & server) {
    server.route("POST", "/v1/images/generations", handle_image_generation);
    server.route("POST", "/v1/videos/generations", handle_video_generation);
}
} // namespace ps5lm
