// Derived from PSllama (commit d079717), GPL-3.0-or-later.
// SPDX-License-Identifier: GPL-3.0-or-later
// Text-to-image and video for the PS5 title: stable-diffusion.cpp on the same
// ggml and Vulkan device as llama.cpp. One model at a time; the caller unloads
// the LLM first (they do not both fit in the GPU heap).
#pragma once

#include "stable-diffusion.h"
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace ps5lm {
// Zero/negative fields mean "the model's default" (SdModelDefaults).
struct SdImageRequest {
    std::string prompt;
    std::string negative_prompt;
    int width = 0;
    int height = 0;
    int steps = 0;
    float cfg_scale = -1.0f;
    int64_t seed = -1;               // -1: random
    std::string sample_method;       // empty: the model's default
    // PNG/JPEG bytes. init_image: image-to-image, redrawn by `strength` (0 keeps
    // it, 1 ignores it). ref_images: reference-image editing where the model supports it.
    std::string init_image;
    float strength = -1.0f;          // negative: 0.75
    std::vector<std::string> ref_images;
};

// Per-model defaults: from the model folder's sd-model.conf, else SD-Turbo's.
struct SdModelDefaults {
    int width = 512;
    int height = 512;
    int steps = 1;
    float cfg_scale = 1.0f;
    std::string sample_method;
    bool video = false;       // sd-model.conf mode=video
    int frames = 17;          // video: 4k+1 frames
    int fps = 16;
    float flow_shift = 0.0f;  // 0: the model's default
};

struct SdVideoRequest {
    std::string prompt;
    std::string negative_prompt;
    int width = 0, height = 0, frames = 0, steps = 0;
    float cfg_scale = -1.0f;
    int64_t seed = -1;
    std::string init_image;   // optional first frame (PNG/JPEG bytes)
};

struct SdVideoResult {
    std::vector<std::vector<unsigned char>> png_frames;
    int64_t seed = 0;
    int width = 0, height = 0, frames = 0, steps = 0, fps = 0;
    double generate_s = 0.0;
};

struct SdImageResult {
    std::vector<unsigned char> png;
    int64_t seed = 0;
    double load_s = 0.0;             // model load, when this request loaded it
    double generate_s = 0.0;
    int width = 0, height = 0, steps = 0;  // as generated, after model defaults
    float cfg_scale = 0.0f;
};

// Progress: phase is "loading" (model load), then per request "preparing"
// (first-use weight loading), "generating" (sampling step of steps), "decoding" (VAE).
// The callback runs with the service lock held: it must not call back into the service.
using SdProgress = std::function<void(const char * phase, int value, int total)>;

class SdService {
  public:
    // Model folders are single path segments directly under media_root.
    explicit SdService(std::string media_root = "/data/PS5LM/media/");
    ~SdService();
    SdService(const SdService &) = delete;
    SdService & operator=(const SdService &) = delete;

    // Loads the model folder `model` unless it is already loaded. A folder may
    // hold sd-model.conf (KEY=VALUE, known keys only); without it the folder is
    // one diffusers-layout model. `prediction` overrides the conf's value.
    bool load(const std::string & model, const std::string & prediction, std::string & error,
              const SdProgress & progress, double * load_s);
    void unload();
    SdModelDefaults defaults() const;
    std::string loaded_model() const;
    bool generate(const SdImageRequest & request, SdImageResult & result, std::string & error,
                  const SdProgress & progress);
    bool generate_video(const SdVideoRequest & request, SdVideoResult & result, std::string & error,
                        const SdProgress & progress);

  private:
    void unload_locked();
    static void on_log(sd_log_level_t level, const char * text, void * self);
    static void on_progress(int step, int steps, float time, void * self);

    std::string media_root_;
    mutable std::mutex mutex_;  // serializes load, unload and generation
    sd_ctx_t * context_ = nullptr;
    std::string loaded_model_;
    SdModelDefaults defaults_;
    const SdProgress * active_progress_ = nullptr;  // set only while a call runs
    const char * active_phase_ = "loading";
};
} // namespace ps5lm
