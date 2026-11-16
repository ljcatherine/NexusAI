// Derived from PSllama (commit d079717), GPL-3.0-or-later.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "nexus_sd_service.hpp"
#include "nexus_media_path.hpp"
#include <sys/stat.h>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <set>
#include <utility>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include "stb_image_write.h"
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "stb_image.h"

namespace ps5lm {
namespace {
constexpr std::size_t kMaxConfLine = 256;   // sd-model.conf lines longer than this reject the file
// Model folder plus up to 63 segments of a component path (see media_resolve).
constexpr int kMaxComponentSegments = 64;
constexpr std::size_t kMaxPrompt = 4096;
constexpr std::size_t kMaxRefImages = 8;
constexpr long kMaxSteps = 150;
constexpr long kMaxFrames = 257;
constexpr long kMaxSide = 2048;

using ConfMap = std::map<std::string, std::string>;

const std::set<std::string> kKnownKeys = {
    "mode", "model", "diffusion_model", "llm", "vae", "clip_l", "clip_g", "clip_vision", "t5xxl",
    "diffusion_flash_attn", "prediction", "params_backend", "max_vram", "sample_method", "width", "height",
    "steps", "frames", "fps", "cfg_scale", "flow_shift"};
// Values are paths relative to the model folder (or a sibling folder, see load()).
const std::set<std::string> kComponentKeys = {"model", "diffusion_model", "llm", "vae",
                                              "clip_l", "clip_g", "clip_vision", "t5xxl"};

double now_s() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool is_directory(const std::string & path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// Reads KEY=VALUE lines. Over-long lines and unknown keys reject the whole file,
// so a typo cannot silently change a model's setup.
bool read_model_conf(const std::string & folder, ConfMap & conf, std::string & error) {
    FILE * file = std::fopen((folder + "/sd-model.conf").c_str(), "r");
    if (!file) return true;  // no conf: the folder is one diffusers-layout model
    char line[kMaxConfLine + 2];
    bool ok = true;
    while (std::fgets(line, sizeof(line), file)) {
        std::string entry(line);
        if (entry.size() == sizeof(line) - 1 && entry.back() != '\n') {
            error = "sd-model.conf line too long";
            ok = false;
            break;
        }
        while (!entry.empty() && (entry.back() == '\n' || entry.back() == '\r' || entry.back() == ' '))
            entry.pop_back();
        const std::size_t equals = entry.find('=');
        if (entry.empty() || entry[0] == '#' || equals == std::string::npos) continue;
        const std::string key = entry.substr(0, equals);
        if (!kKnownKeys.count(key)) {
            error = "unknown key in sd-model.conf";
            ok = false;
            break;
        }
        conf[key] = entry.substr(equals + 1);
    }
    std::fclose(file);
    return ok;
}

// Strict numeric parse: the whole value must be a number inside [low, high].
bool parse_int(const std::string & text, long low, long high, int & out) {
    char * end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0' || value < low || value > high) return false;
    out = static_cast<int>(value);
    return true;
}

bool parse_float(const std::string & text, float low, float high, float & out) {
    char * end = nullptr;
    const float value = std::strtof(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0' || !(value >= low && value <= high)) return false;
    out = value;
    return true;
}

// Model defaults from the conf; keys absent from the conf keep SdModelDefaults' values.
bool parse_defaults(const ConfMap & conf, SdModelDefaults & d, std::string & error) {
    const auto get = [&conf](const char * key) {
        const auto it = conf.find(key);
        return it == conf.end() ? std::string() : it->second;
    };
    const auto int_key = [&](const char * key, long low, long high, int & field) {
        const std::string value = get(key);
        return value.empty() || parse_int(value, low, high, field);
    };
    const auto float_key = [&](const char * key, float low, float high, float & field) {
        const std::string value = get(key);
        return value.empty() || parse_float(value, low, high, field);
    };
    if (!(int_key("width", 16, kMaxSide, d.width) && int_key("height", 16, kMaxSide, d.height) &&
          int_key("steps", 1, kMaxSteps, d.steps) && int_key("frames", 1, kMaxFrames, d.frames) &&
          int_key("fps", 1, 120, d.fps) && float_key("cfg_scale", 0.0f, 30.0f, d.cfg_scale) &&
          float_key("flow_shift", 0.0f, 20.0f, d.flow_shift))) {
        error = "invalid value in sd-model.conf";
        return false;
    }
    d.sample_method = get("sample_method");
    d.video = get("mode") == "video";
    return true;
}

// Request fields follow the same rule as the conf: zero/negative means the default.
bool optional_in_range(long value, long low, long high) {
    return value <= 0 || (value >= low && value <= high);
}

void png_write(void * context_out, void * data, int size) {
    auto * out = static_cast<std::vector<unsigned char> *>(context_out);
    const auto * bytes = static_cast<const unsigned char *>(data);
    out->insert(out->end(), bytes, bytes + size);
}
} // namespace

SdService::SdService(std::string media_root) : media_root_(std::move(media_root)) {
    while (media_root_.size() > 1 && media_root_.back() == '/') media_root_.pop_back();
}

SdService::~SdService() { unload(); }

void SdService::on_log(sd_log_level_t level, const char * text, void * self) {
    if (level < SD_LOG_INFO || !text) return;
    auto * service = static_cast<SdService *>(self);
    // stable-diffusion.cpp reports weight loading, sampling and VAE decoding
    // through one progress callback; its log lines say which stage follows.
    if (service->active_progress_) {
        if (std::strstr(text, "generating image") || std::strstr(text, "generating video") ||
            std::strstr(text, "sampling using"))
            service->active_phase_ = "generating";
        else if (std::strstr(text, "decoding"))
            service->active_phase_ = "decoding";
    }
    std::printf("ps5lm-app: [sd] %s%s", text, text[0] && text[std::strlen(text) - 1] == '\n' ? "" : "\n");
}

void SdService::on_progress(int step, int steps, float, void * self) {
    auto * service = static_cast<SdService *>(self);
    if (service->active_progress_ && *service->active_progress_)
        (*service->active_progress_)(service->active_phase_, step, steps);
}

void SdService::unload_locked() {
    if (context_) free_sd_ctx(context_);
    context_ = nullptr;
    loaded_model_.clear();
}

void SdService::unload() {
    std::lock_guard<std::mutex> lock(mutex_);
    unload_locked();
}

SdModelDefaults SdService::defaults() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return defaults_;
}

std::string SdService::loaded_model() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return loaded_model_;
}

bool SdService::load(const std::string & model, const std::string & prediction, std::string & error,
                     const SdProgress & progress, double * load_s) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (context_ && loaded_model_ == model) return true;
    if (!media_segment_ok(model)) {
        error = "invalid model name";
        return false;
    }
    const std::string path = media_root_ + "/" + model;
    if (!is_directory(path)) {
        error = "model folder not found";
        return false;
    }
    ConfMap conf;
    if (!read_model_conf(path, conf, error)) return false;
    // Component paths must outlive new_sd_ctx; keep them in conf. They are relative
    // to the model folder, so the model name leads; ".." may climb to a sibling
    // folder (e.g. flux/../flux2-klein-4b/vae.safetensors) but not above media_root.
    for (const std::string & key : kComponentKeys) {
        const auto it = conf.find(key);
        if (it == conf.end()) continue;
        const auto resolved = media_resolve(media_root_, model + "/" + it->second, kMaxComponentSegments, true);
        if (!resolved) {
            error = "invalid component path in sd-model.conf";
            return false;
        }
        it->second = *resolved;
    }
    const auto text = [&conf](const char * key) {
        const auto it = conf.find(key);
        return it == conf.end() ? std::string() : it->second;
    };
    const auto component = [&conf](const char * key) -> const char * {
        const auto it = conf.find(key);
        return it == conf.end() ? nullptr : it->second.c_str();
    };
    SdModelDefaults model_defaults;
    if (!parse_defaults(conf, model_defaults, error)) return false;
    if (!model_defaults.sample_method.empty() &&
        str_to_sample_method(model_defaults.sample_method.c_str()) == SAMPLE_METHOD_COUNT) {
        error = "unknown sample_method in sd-model.conf";
        return false;
    }
    const std::string model_prediction = prediction.empty() ? text("prediction") : prediction;
    if (!model_prediction.empty() && str_to_prediction(model_prediction.c_str()) == PREDICTION_COUNT) {
        error = "unknown prediction type";
        return false;
    }

    sd_ctx_params_t params;
    sd_ctx_params_init(&params);
    if (conf.empty()) {
        params.model_path = path.c_str();
    } else {
        params.model_path = component("model");
        params.diffusion_model_path = component("diffusion_model");
        params.llm_path = component("llm");
        params.vae_path = component("vae");
        params.clip_l_path = component("clip_l");
        params.clip_g_path = component("clip_g");
        params.clip_vision_path = component("clip_vision");
        params.t5xxl_path = component("t5xxl");
        params.diffusion_flash_attn = text("diffusion_flash_attn") == "1";
    }
    params.n_threads = 4;
    params.enable_mmap = false;
    params.backend = "Vulkan0";
    // Where weights live: on the GPU by default; sd-model.conf can move modules
    // to disk (loaded for their stage, then released), e.g.
    // params_backend=all=Vulkan0,te=disk,vae=disk for models larger than the heap.
    const std::string params_backend = conf.count("params_backend") ? text("params_backend") : "Vulkan0";
    params.params_backend = params_backend.c_str();
    const std::string max_vram = text("max_vram");
    if (!max_vram.empty()) params.max_vram = max_vram.c_str();
    if (!model_prediction.empty()) params.prediction = str_to_prediction(model_prediction.c_str());

    unload_locked();  // the previous model's VRAM must be free before the next one loads
    sd_set_log_callback(on_log, this);
    sd_set_progress_callback(on_progress, this);
    const double started = now_s();
    active_progress_ = &progress;
    active_phase_ = "loading";
    context_ = new_sd_ctx(&params);
    active_progress_ = nullptr;
    if (load_s) *load_s = now_s() - started;
    if (!context_ || !(sd_ctx_supports_image_generation(context_) || sd_ctx_supports_video_generation(context_))) {
        unload_locked();
        error = "image model failed to load (see klog)";
        return false;
    }
    loaded_model_ = model;
    defaults_ = model_defaults;
    return true;
}

bool SdService::generate(const SdImageRequest & request, SdImageResult & result, std::string & error,
                         const SdProgress & progress) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!context_) {
        error = "no image model is loaded";
        return false;
    }
    if (request.prompt.size() > kMaxPrompt || request.negative_prompt.size() > kMaxPrompt ||
        request.ref_images.size() > kMaxRefImages || request.strength > 1.0f || request.cfg_scale > 30.0f ||
        !optional_in_range(request.width, 16, kMaxSide) || !optional_in_range(request.height, 16, kMaxSide) ||
        !optional_in_range(request.steps, 1, kMaxSteps)) {
        error = "request out of range";
        return false;
    }
    SdImageRequest filled = request;
    if (filled.width <= 0) filled.width = defaults_.width;
    if (filled.height <= 0) filled.height = defaults_.height;
    if (filled.steps <= 0) filled.steps = defaults_.steps;
    if (filled.cfg_scale < 0.0f) filled.cfg_scale = defaults_.cfg_scale;
    if (filled.sample_method.empty()) filled.sample_method = defaults_.sample_method;
    const SdImageRequest & request_used = filled;
    sd_img_gen_params_t params;
    sd_img_gen_params_init(&params);
    params.prompt = request_used.prompt.c_str();
    params.negative_prompt = request_used.negative_prompt.c_str();
    params.width = request_used.width;
    params.height = request_used.height;
    params.sample_params.sample_steps = request_used.steps;
    params.sample_params.guidance.txt_cfg = request_used.cfg_scale;
    if (!request_used.sample_method.empty()) {
        params.sample_params.sample_method = str_to_sample_method(request_used.sample_method.c_str());
        if (params.sample_params.sample_method == SAMPLE_METHOD_COUNT) {
            error = "unknown sample_method";
            return false;
        }
    }
    // Decoded start and reference images; freed when this function returns.
    struct Pixels {
        std::vector<unsigned char *> owned;
        ~Pixels() {
            for (unsigned char * p : owned) stbi_image_free(p);
        }
        bool decode(const std::string & bytes, sd_image_t & image) {
            int width = 0, height = 0, channels = 0;
            unsigned char * pixels = stbi_load_from_memory(reinterpret_cast<const unsigned char *>(bytes.data()),
                                                           static_cast<int>(bytes.size()), &width, &height,
                                                           &channels, 3);
            if (!pixels) return false;
            owned.push_back(pixels);
            image = sd_image_t{static_cast<uint32_t>(width), static_cast<uint32_t>(height), 3, pixels};
            return true;
        }
    } pixels;
    std::vector<sd_image_t> references(request_used.ref_images.size());
    if (!request_used.init_image.empty()) {
        if (!pixels.decode(request_used.init_image, params.init_image)) {
            error = "init image could not be decoded (PNG or JPEG expected)";
            return false;
        }
        params.strength = request_used.strength >= 0.0f ? request_used.strength : 0.75f;
    }
    for (std::size_t i = 0; i < references.size(); ++i) {
        if (!pixels.decode(request_used.ref_images[i], references[i])) {
            error = "a reference image could not be decoded (PNG or JPEG expected)";
            return false;
        }
    }
    if (!references.empty()) {
        params.ref_images = references.data();
        params.ref_images_count = static_cast<int>(references.size());
    }
    result.width = request_used.width;
    result.height = request_used.height;
    result.steps = request_used.steps;
    result.cfg_scale = request_used.cfg_scale;
    result.seed = request_used.seed >= 0 ? request_used.seed
                                         : static_cast<int64_t>(std::random_device{}() & 0x7fffffff);
    params.seed = result.seed;
    params.batch_count = 1;
    // Large VAE decodes as one dispatch could approach the 10 s GPU watchdog.
    params.vae_tiling_params.enabled = request_used.width * request_used.height > 768 * 768;

    const double started = now_s();
    active_progress_ = &progress;
    active_phase_ = "preparing";  // first-use weight loading and text encoding
    sd_image_t * images = nullptr;
    int count = 0;
    const bool ok = generate_image(context_, &params, &images, &count);
    active_progress_ = nullptr;
    result.generate_s = now_s() - started;
    if (!ok || count < 1 || !images || !images[0].data) {
        free_sd_images(images, count);
        error = "image generation failed (see klog)";
        return false;
    }
    const sd_image_t & image = images[0];
    result.png.clear();
    stbi_write_png_to_func(png_write, &result.png, static_cast<int>(image.width), static_cast<int>(image.height),
                           static_cast<int>(image.channel), image.data,
                           static_cast<int>(image.width * image.channel));
    free_sd_images(images, count);
    if (result.png.empty()) {
        error = "PNG encoding failed";
        return false;
    }
    return true;
}

bool SdService::generate_video(const SdVideoRequest & request, SdVideoResult & result, std::string & error,
                               const SdProgress & progress) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!context_ || !defaults_.video) {
        error = context_ ? "the loaded model is not a video model" : "no video model is loaded";
        return false;
    }
    if (request.prompt.size() > kMaxPrompt || request.negative_prompt.size() > kMaxPrompt ||
        request.cfg_scale > 30.0f || !optional_in_range(request.width, 16, kMaxSide) ||
        !optional_in_range(request.height, 16, kMaxSide) || !optional_in_range(request.frames, 1, kMaxFrames) ||
        !optional_in_range(request.steps, 1, kMaxSteps)) {
        error = "request out of range";
        return false;
    }
    sd_vid_gen_params_t params;
    sd_vid_gen_params_init(&params);
    params.prompt = request.prompt.c_str();
    params.negative_prompt = request.negative_prompt.c_str();
    int image_width = 0, image_height = 0, image_channels = 0;
    unsigned char * image_pixels = nullptr;
    if (!request.init_image.empty()) {
        image_pixels = stbi_load_from_memory(reinterpret_cast<const unsigned char *>(request.init_image.data()),
                                             static_cast<int>(request.init_image.size()), &image_width,
                                             &image_height, &image_channels, 3);
        if (!image_pixels) {
            error = "init image could not be decoded (PNG or JPEG expected)";
            return false;
        }
        params.init_image = sd_image_t{static_cast<uint32_t>(image_width), static_cast<uint32_t>(image_height), 3,
                                       image_pixels};
    }
    // With a start image and no explicit size, follow the image (multiples of 16).
    const bool size_from_image = image_pixels && request.width <= 0;
    result.width = params.width = size_from_image ? image_width / 16 * 16
                                                  : request.width > 0 ? request.width : defaults_.width;
    result.height = params.height = size_from_image ? image_height / 16 * 16
                                                    : request.height > 0 ? request.height : defaults_.height;
    result.frames = params.video_frames = request.frames > 0 ? request.frames : defaults_.frames;
    result.steps = params.sample_params.sample_steps = request.steps > 0 ? request.steps : defaults_.steps;
    params.sample_params.guidance.txt_cfg = request.cfg_scale >= 0.0f ? request.cfg_scale : defaults_.cfg_scale;
    if (!defaults_.sample_method.empty())
        params.sample_params.sample_method = str_to_sample_method(defaults_.sample_method.c_str());
    if (defaults_.flow_shift > 0.0f) params.sample_params.flow_shift = defaults_.flow_shift;
    params.fps = defaults_.fps;
    result.seed = request.seed >= 0 ? request.seed : static_cast<int64_t>(std::random_device{}() & 0x7fffffff);
    params.seed = result.seed;
    // The Wan VAE decodes all frames at once; tile it to bound memory and
    // the length of any single GPU submission.
    params.vae_tiling_params.enabled = true;

    const double started = now_s();
    active_progress_ = &progress;
    active_phase_ = "preparing";
    sd_image_t * frames = nullptr;
    int count = 0;
    int fps = 0;
    const bool ok = ::generate_video(context_, &params, &frames, &count, nullptr, &fps);  // the sd.cpp C API, not the member
    active_progress_ = nullptr;
    stbi_image_free(image_pixels);
    result.generate_s = now_s() - started;
    if (!ok || count < 1 || !frames) {
        free_sd_images(frames, count);
        error = "video generation failed (see klog)";
        return false;
    }
    result.fps = fps > 0 ? fps : defaults_.fps;
    result.png_frames.clear();
    for (int i = 0; i < count; ++i) {
        std::vector<unsigned char> png;
        stbi_write_png_to_func(png_write, &png, static_cast<int>(frames[i].width),
                               static_cast<int>(frames[i].height), static_cast<int>(frames[i].channel),
                               frames[i].data, static_cast<int>(frames[i].width * frames[i].channel));
        result.png_frames.push_back(std::move(png));
    }
    free_sd_images(frames, count);
    return true;
}
} // namespace ps5lm
