// Derived from PSllama (commit d079717), GPL-3.0-or-later.
// SPDX-License-Identifier: GPL-3.0-or-later
// GPU coordinator for the media engines: unloads every engine that is not needed for the
// current request, as PSllama's free_gpu_for did, so that the GPU heap holds one engine set.
#include "nexus_media.hpp"
#include "nexus_audio_service.hpp"
#include "nexus_sd_service.hpp"

namespace ps5lm {
unsigned media_gpu_acquire(unsigned keep_mask) {
    unsigned unloaded = 0;
    if (!(keep_mask & kGpuSd) && !media_image_service().loaded_model().empty()) {
        media_image_service().unload();
        unloaded |= kGpuSd;
    }
    if (!(keep_mask & kGpuStt) && !stt_loaded_model().empty()) {
        stt_unload();
        unloaded |= kGpuStt;
    }
    if (!(keep_mask & kGpuTts) && !tts_loaded_model().empty()) {
        tts_unload();
        unloaded |= kGpuTts;
    }
    if (!(keep_mask & kGpuVoiceLlm) && media_voice_unload()) unloaded |= kGpuVoiceLlm;
    return unloaded;
}
} // namespace ps5lm
