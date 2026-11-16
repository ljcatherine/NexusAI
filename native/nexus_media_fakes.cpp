// Link-time fakes for the media host tests (test-media.sh). They replace the engine services
// that nexus_media_image.cpp, nexus_media_audio.cpp and nexus_media_gpu.cpp call: SdService
// (stable-diffusion), the whisper STT and TTS functions, and the voice handler, which is not
// built here because its llama.h and chat-template dependencies are not practical to fake.
// Every fake records its call in a log that the tests read through ps5lm::fake; that is how
// GPU coordination is checked. stat() is wrapped (--wrap=stat) so the media root's model
// folders exist without touching /data. Test-only: never linked into the PS5 title.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "nexus_media.hpp"
#include "nexus_audio_service.hpp"
#include "nexus_sd_service.hpp"

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace ps5lm {
namespace fake {
namespace {
std::mutex state_mutex;  // engine state below; taken before log_mutex when both are needed
std::mutex log_mutex;    // call_log only, so record() can run while state_mutex is held
std::vector<std::string> call_log;
std::string sd_loaded, stt_loaded, tts_loaded;
bool voice_loaded = false;
bool sd_generate_fails = false;

void record(const std::string & call) {
    std::lock_guard<std::mutex> lock(log_mutex);
    call_log.push_back(call);
}
} // namespace

void reset() {
    std::lock_guard<std::mutex> state(state_mutex);
    std::lock_guard<std::mutex> log(log_mutex);
    call_log.clear();
    sd_loaded.clear();
    stt_loaded.clear();
    tts_loaded.clear();
    voice_loaded = false;
    sd_generate_fails = false;
}

std::vector<std::string> calls() {
    std::lock_guard<std::mutex> lock(log_mutex);
    return call_log;
}

// Sets an engine as loaded without logging a call. "sd", "stt" or "tts".
void load_engine(const char * engine, const std::string & model) {
    std::lock_guard<std::mutex> lock(state_mutex);
    if (std::strcmp(engine, "sd") == 0) sd_loaded = model;
    else if (std::strcmp(engine, "stt") == 0) stt_loaded = model;
    else tts_loaded = model;
}

void set_voice_loaded(bool loaded) {
    std::lock_guard<std::mutex> lock(state_mutex);
    voice_loaded = loaded;
}

void set_sd_generate_fails(bool fails) {
    std::lock_guard<std::mutex> lock(state_mutex);
    sd_generate_fails = fails;
}
} // namespace fake

// ---- stable-diffusion ----------------------------------------------------------------------

SdService::SdService(std::string) {}
SdService::~SdService() {}

bool SdService::load(const std::string & model, const std::string &, std::string & error,
                     const SdProgress &, double * load_s) {
    if (model.empty()) {
        error = "fake: empty model";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(fake::state_mutex);
        fake::sd_loaded = model;
        fake::record("sd_load:" + model);
    }
    if (load_s) *load_s = 0.25;
    return true;
}

void SdService::unload() {
    std::lock_guard<std::mutex> lock(fake::state_mutex);
    fake::sd_loaded.clear();
    fake::record("sd_unload");
}

SdModelDefaults SdService::defaults() const { return SdModelDefaults{}; }

std::string SdService::loaded_model() const {
    std::lock_guard<std::mutex> lock(fake::state_mutex);
    return fake::sd_loaded;
}

bool SdService::generate(const SdImageRequest & request, SdImageResult & result, std::string & error,
                         const SdProgress & progress) {
    bool fails = false;
    {
        std::lock_guard<std::mutex> lock(fake::state_mutex);
        fails = fake::sd_generate_fails;
        fake::record("sd_generate");
    }
    if (progress) {
        progress("generating", 1, 2);
        progress("generating", 2, 2);
    }
    if (fails) {
        error = "fake engine failure";
        return false;
    }
    result.png = {0x89, 'P', 'N', 'G'};
    result.seed = request.seed < 0 ? 42 : request.seed;
    result.width = request.width > 0 ? request.width : 512;
    result.height = request.height > 0 ? request.height : 512;
    result.steps = 1;
    result.cfg_scale = 1.0f;
    result.generate_s = 0.5;
    return true;
}

bool SdService::generate_video(const SdVideoRequest & request, SdVideoResult & result, std::string & error,
                               const SdProgress & progress) {
    bool fails = false;
    {
        std::lock_guard<std::mutex> lock(fake::state_mutex);
        fails = fake::sd_generate_fails;
        fake::record("sd_generate_video");
    }
    if (progress) progress("generating", 1, 1);
    if (fails) {
        error = "fake engine failure";
        return false;
    }
    const int count = request.frames > 0 ? request.frames : 2;
    result.png_frames.assign(static_cast<std::size_t>(count), std::vector<unsigned char>{0x89, 'P', 'N', 'G'});
    result.seed = request.seed < 0 ? 42 : request.seed;
    result.width = request.width > 0 ? request.width : 512;
    result.height = request.height > 0 ? request.height : 512;
    result.frames = count;
    result.steps = 1;
    result.fps = 16;
    result.generate_s = 0.5;
    return true;
}

// ---- whisper (STT) and TTS ----------------------------------------------------------------

bool stt_load(const std::string & model, std::string & error, double * load_s) {
    if (model.empty()) {
        error = "fake: empty model";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(fake::state_mutex);
        fake::stt_loaded = model;
        fake::record("stt_load:" + model);
    }
    if (load_s) *load_s = 0.25;
    return true;
}

void stt_unload() {
    std::lock_guard<std::mutex> lock(fake::state_mutex);
    fake::stt_loaded.clear();
    fake::record("stt_unload");
}

std::string stt_loaded_model() {
    std::lock_guard<std::mutex> lock(fake::state_mutex);
    return fake::stt_loaded;
}

bool stt_transcribe(const std::vector<float> & samples16k, const std::string &, bool, SttResult & result,
                    std::string &) {
    fake::record("stt_transcribe");
    result.text = "hello there";
    result.language = "en";
    result.segments.assign(1, SttSegment{0.0, 1.0, "hello there"});
    result.audio_s = static_cast<double>(samples16k.size()) / 16000.0;
    result.transcribe_s = 0.1;
    return true;
}

bool tts_load(const std::string & model, std::string & error, double * load_s) {
    if (model.empty()) {
        error = "fake: empty model";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(fake::state_mutex);
        fake::tts_loaded = model;
        fake::record("tts_load:" + model);
    }
    if (load_s) *load_s = 0.25;
    return true;
}

void tts_unload() {
    std::lock_guard<std::mutex> lock(fake::state_mutex);
    fake::tts_loaded.clear();
    fake::record("tts_unload");
}

std::string tts_loaded_model() {
    std::lock_guard<std::mutex> lock(fake::state_mutex);
    return fake::tts_loaded;
}

bool tts_generate(const TtsRequest &, TtsResult & result, std::string &,
                  const std::function<void(int, int)> &) {
    fake::record("tts_generate");
    result.wav = "RIFF-fake-wav";  // the tests compare against this literal
    result.sample_rate = 24000;
    result.frames = 3;
    result.audio_s = 0.2;
    result.generate_s = 0.1;
    return true;
}

// ---- voice (not built in the host tests) ----------------------------------------------------

void handle_voice_chat(int fd, const MediaRequest &) {
    media_send_error(fd, 503, "server_error", "voice chat is not built into the host tests");
}

void register_media_voice_routes(MediaServer & server) {
    server.route("POST", "/v1/voice/chat", handle_voice_chat);
}

bool media_voice_unload() {
    bool was_loaded = false;
    {
        std::lock_guard<std::mutex> lock(fake::state_mutex);
        was_loaded = fake::voice_loaded;
        fake::voice_loaded = false;
        fake::record("voice_unload");
    }
    return was_loaded;
}
} // namespace ps5lm

// ---- stat() seam ----------------------------------------------------------------------------

extern "C" int __real_stat(const char * path, struct stat * info);

// The image handlers stat "/data/PS5LM/media/<model>" to find model folders. On the host every
// folder under that root exists, except names containing "missing" (for the 404 test).
extern "C" int __wrap_stat(const char * path, struct stat * info) {
    static const char kRoot[] = "/data/PS5LM/media/";
    if (std::strncmp(path, kRoot, sizeof(kRoot) - 1) != 0) return __real_stat(path, info);
    if (std::strstr(path, "missing") != nullptr) {
        errno = ENOENT;
        return -1;
    }
    std::memset(info, 0, sizeof(*info));
    info->st_mode = S_IFDIR | 0755;
    return 0;
}
