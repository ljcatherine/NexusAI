// Speech to text (whisper.cpp) and text to speech (llama.cpp mtmd audio
// generation) for the Nexus title.
// Derived from PSllama (commit d079717), GPL-3.0-or-later.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "nexus_audio_service.hpp"
#include "nexus_media_path.hpp"

#include "llama.h"
#include "mtmd.h"
#include "mtmd-helper.h"
#include "whisper.h"

#include <chrono>
#include <climits>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <mutex>
#include <random>
#include <utility>

namespace ps5lm {
namespace {
constexpr char kMediaRoot[] = "/data/PS5LM/media/";
constexpr int kMaxTtsFrames = 4096; // matches the TTS context size below

// Each engine has its own lock: the public calls hold it for their whole run,
// and the *_unload helpers used inside a locked call do not lock again.
std::mutex stt_mutex;
whisper_context *stt = nullptr;
std::string stt_name;

std::mutex tts_mutex;
llama_model *tts_model = nullptr;
llama_context *tts_context = nullptr;
mtmd_context *tts_mtmd = nullptr;
std::string tts_name;

double now_s() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Engine log lines go to the same stdout log as the rest of the app. Never
// called with prompt or audio contents.
void engine_log(const char *engine, const char *text) {
    if (!text || !*text) return;
    std::printf("ps5lm-app: %s %s%s", engine, text, text[std::strlen(text) - 1] == '\n' ? "" : "\n");
}

void whisper_log(ggml_log_level level, const char *text, void *) {
    if (level >= GGML_LOG_LEVEL_INFO) engine_log("[whisper]", text);
}

void mtmd_log(ggml_log_level level, const char *text, void *) {
    if (level >= GGML_LOG_LEVEL_INFO) engine_log("[mtmd]", text);
}

// Resolves a model name under kMediaRoot. with_file: "folder/file" (STT);
// otherwise a single folder name (TTS). Rejects traversal and absolute paths.
bool media_path(const std::string &model, bool with_file, std::string &path, std::string &error) {
    const std::size_t slash = model.find('/');
    const bool has_file = slash != std::string::npos;
    const std::string folder = model.substr(0, slash);
    const bool names_ok = media_segment_ok(folder) && (!has_file || media_segment_ok(model.substr(slash + 1)));
    std::optional<std::string> resolved;
    if (names_ok && has_file == with_file) resolved = media_resolve(kMediaRoot, model, with_file ? 2 : 1, false);
    if (!resolved) {
        error = with_file ? "speech-to-text model must be folder/file under the media root"
                          : "text-to-speech model must be a folder name under the media root";
        return false;
    }
    path = *resolved;
    return true;
}

void stt_unload_locked() {
    if (stt) whisper_free(stt);
    stt = nullptr;
    stt_name.clear();
}

void tts_unload_locked() {
    if (tts_mtmd) mtmd_free(tts_mtmd);
    if (tts_context) llama_free(tts_context);
    if (tts_model) llama_model_free(tts_model);
    tts_mtmd = nullptr;
    tts_context = nullptr;
    tts_model = nullptr;
    tts_name.clear();
}
} // namespace

std::string stt_loaded_model() {
    std::lock_guard<std::mutex> lock(stt_mutex);
    return stt_name;
}

void stt_unload() {
    std::lock_guard<std::mutex> lock(stt_mutex);
    stt_unload_locked();
}

bool stt_load(const std::string &model, std::string &error, double *load_s) {
    std::string path;
    if (!media_path(model, true, path, error)) return false;
    std::lock_guard<std::mutex> lock(stt_mutex);
    if (stt && stt_name == model) return true;
    stt_unload_locked();
    whisper_log_set(whisper_log, nullptr);
    whisper_context_params params = whisper_context_default_params();
    params.use_gpu = true;
    params.flash_attn = true;
    params.gpu_device = 0;
    const double started = now_s();
    stt = whisper_init_from_file_with_params(path.c_str(), params);
    if (load_s) *load_s = now_s() - started;
    if (!stt) {
        error = "speech-to-text model failed to load (see app log)";
        return false;
    }
    stt_name = model;
    return true;
}

bool stt_transcribe(const std::vector<float> &samples16k, const std::string &language, bool translate,
                    SttResult &result, std::string &error) {
    std::lock_guard<std::mutex> lock(stt_mutex);
    if (!stt) {
        error = "no speech-to-text model is loaded";
        return false;
    }
    if (samples16k.empty() || samples16k.size() > static_cast<std::size_t>(INT_MAX)) {
        error = "audio has no samples or is too long";
        return false;
    }
    whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    params.n_threads = 4;
    params.translate = translate;
    params.language = language.empty() ? "auto" : language.c_str();
    params.print_progress = false;
    params.print_realtime = false;
    params.print_timestamps = false;
    params.print_special = false;
    const double started = now_s();
    if (whisper_full(stt, params, samples16k.data(), static_cast<int>(samples16k.size())) != 0) {
        error = "transcription failed (see app log)";
        return false;
    }
    result.transcribe_s = now_s() - started;
    result.audio_s = samples16k.size() / static_cast<double>(WHISPER_SAMPLE_RATE);
    result.language = whisper_lang_str(whisper_full_lang_id(stt));
    result.text.clear();
    result.segments.clear();
    for (int i = 0; i < whisper_full_n_segments(stt); ++i) {
        SttSegment segment;
        segment.start_s = whisper_full_get_segment_t0(stt, i) / 100.0;
        segment.end_s = whisper_full_get_segment_t1(stt, i) / 100.0;
        segment.text = whisper_full_get_segment_text(stt, i);
        result.text += segment.text;
        result.segments.push_back(std::move(segment));
    }
    return true;
}

std::string tts_loaded_model() {
    std::lock_guard<std::mutex> lock(tts_mutex);
    return tts_name;
}

void tts_unload() {
    std::lock_guard<std::mutex> lock(tts_mutex);
    tts_unload_locked();
}

bool tts_load(const std::string &model, std::string &error, double *load_s) {
    std::string path;
    if (!media_path(model, false, path, error)) return false;
    std::lock_guard<std::mutex> lock(tts_mutex);
    if (tts_model && tts_name == model) return true;
    tts_unload_locked();
    std::string backbone, projector;
    if (DIR *dir = opendir(path.c_str())) {
        while (dirent *entry = readdir(dir)) {
            const std::string name = entry->d_name;
            if (name.size() < 5 || name.compare(name.size() - 5, 5, ".gguf") != 0) continue;
            (name.rfind("mmproj", 0) == 0 ? projector : backbone) = path + "/" + name;
        }
        closedir(dir);
    }
    if (backbone.empty() || projector.empty()) {
        error = "text-to-speech folder needs a model .gguf and an mmproj-*.gguf";
        return false;
    }
    mtmd_helper_log_set(mtmd_log, nullptr);
    const double started = now_s();
    llama_model_params model_params = llama_model_default_params();
    model_params.n_gpu_layers = 999;
    tts_model = llama_model_load_from_file(backbone.c_str(), model_params);
    if (tts_model) {
        llama_context_params context_params = llama_context_default_params();
        context_params.n_ctx = 4096;
        context_params.n_batch = 128;   // one submit stays well under the 10 s GPU watchdog
        context_params.n_ubatch = 128;
        context_params.n_threads = 4;
        context_params.n_threads_batch = 4;
        context_params.embeddings = true;  // the audio stages read the backbone's hidden states
        tts_context = llama_init_from_model(tts_model, context_params);
    }
    if (tts_context) {
        mtmd_context_params mtmd_params = mtmd_context_params_default();
        mtmd_params.use_gpu = true;
        mtmd_params.n_threads = 4;
        mtmd_params.print_timings = false;
        tts_mtmd = mtmd_init_from_file(projector.c_str(), tts_model, mtmd_params);
    }
    if (load_s) *load_s = now_s() - started;
    if (!tts_mtmd || mtmd_gen_audio_get_info(tts_mtmd).type == MTMD_GEN_AUDIO_TYPE_NONE) {
        tts_unload_locked();
        error = "text-to-speech model failed to load or cannot generate audio (see app log)";
        return false;
    }
    tts_name = model;
    return true;
}

bool tts_generate(const TtsRequest &request, TtsResult &result, std::string &error,
                  const std::function<void(int, int)> &progress) {
    std::lock_guard<std::mutex> lock(tts_mutex);
    if (!tts_mtmd) {
        error = "no text-to-speech model is loaded";
        return false;
    }
    if (request.text.empty()) {
        error = "text-to-speech input is empty";
        return false;
    }
    if (request.max_frames < 1 || request.max_frames > kMaxTtsFrames) {
        error = "text-to-speech max_frames must be between 1 and 4096";
        return false;
    }
    llama_memory_clear(llama_get_memory(tts_context), true);
    mtmd::bitmap_ptr speaker;
    if (!request.speaker_wav.empty()) {
        auto wrapper = mtmd_helper_bitmap_init_from_buf(
            tts_mtmd, reinterpret_cast<const unsigned char *>(request.speaker_wav.data()), request.speaker_wav.size(),
            false, mtmd_helper_init_opt_default());
        if (!wrapper.bitmap) {
            error = "speaker reference audio could not be decoded";
            return false;
        }
        speaker.reset(wrapper.bitmap);
    }
    const uint32_t seed = request.seed >= 0 ? static_cast<uint32_t>(request.seed) : std::random_device{}();
    mtmd_helper::gen_audio gen(tts_context, tts_mtmd);
    mtmd_helper_gen_audio_inp input{};
    input.seq_id = 0;
    input.prompt = request.text.c_str();
    input.prompt_len = request.text.size();
    input.speaker_ref = speaker.get();
    input.lang = request.language.empty() ? nullptr : request.language.c_str();
    input.top_k = request.top_k;
    input.top_p = request.top_p;
    input.seed = seed;
    input.out_type = MTMD_HELPER_GEN_AUDIO_OUTTYPE_WAV;
    const double started = now_s();
    if (gen.set_input(&input) != 0) {
        error = "text-to-speech input rejected";
        return false;
    }
    for (;;) {
        const int32_t remaining = gen.step_prompt(128);
        if (remaining < 0) {
            error = "text-to-speech prompt processing failed";
            return false;
        }
        if (remaining == 0) break;
    }
    // Semantic codes are sampled from the backbone like the llama.cpp tts tool does.
    llama_sampler *sampler = llama_sampler_chain_init(llama_sampler_chain_default_params());
    llama_sampler_chain_add(sampler, llama_sampler_init_top_k(request.top_k));
    llama_sampler_chain_add(sampler, llama_sampler_init_top_p(request.top_p, 1));
    llama_sampler_chain_add(sampler, llama_sampler_init_temp(request.temperature));
    llama_sampler_chain_add(sampler, llama_sampler_init_dist(seed));
    llama_token sampled = llama_sampler_sample(sampler, tts_context, -1);
    const float *h_state = llama_get_embeddings_ith(tts_context, -1);
    bool stop = false;
    int frames = 0;
    while (!stop && frames < request.max_frames) {
        const float *h_next = nullptr;
        if (gen.step_gen(sampled, h_state, &h_next, &stop) != 0) {
            llama_sampler_free(sampler);
            error = "text-to-speech generation failed";
            return false;
        }
        if (!h_next) break;
        ++frames;
        h_state = h_next;
        sampled = llama_sampler_sample(sampler, tts_context, -1);
        if (progress && frames % 12 == 0) progress(frames, request.max_frames);
    }
    llama_sampler_free(sampler);
    int32_t rate = 0;
    const char *wav = nullptr;
    size_t wav_size = 0;
    int64_t samples = 0;
    if (gen.get_output(&rate, &wav, &wav_size, &samples) != 0 || !wav || wav_size == 0) {
        error = "text-to-speech produced no audio";
        return false;
    }
    result.wav.assign(wav, wav_size);
    result.sample_rate = rate;
    result.audio_s = rate > 0 ? samples / static_cast<double>(rate) : 0.0;
    result.generate_s = now_s() - started;
    result.frames = frames;
    return true;
}
} // namespace ps5lm
