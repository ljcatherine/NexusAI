// Derived from PSllama (commit d079717), GPL-3.0-or-later.
// SPDX-License-Identifier: GPL-3.0-or-later
// POST /v1/voice/chat: one spoken turn. Whisper (STT), the TTS model and a small chat
// LLM run in sequence, and every spoken sentence of the reply is sent as audio as soon
// as it is ready. The answer is always an SSE stream, as in PSllama.
#include "nexus_media.hpp"
#include "nexus_audio_service.hpp"
#include "nexus_media_path.hpp"
#include "nexus_wav.hpp"

#include "chat.h"
#include "llama.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace ps5lm {
namespace {
constexpr char kMediaRoot[] = "/data/PS5LM/media/";
constexpr char kDefaultSystem[] = "You are a helpful voice assistant. Answer in one to three short spoken "
                                  "sentences. Plain words only: no lists, markdown, emoji or code.";
constexpr char kDefaultLlm[] = "qwen3-4b/Qwen3-4B-Q4_K_M.gguf";        // Needs Verification: folder vs M-3
constexpr char kDefaultStt[] = "whisper/ggml-large-v3-turbo-q5_0.bin";
constexpr char kDefaultTts[] = "qwen3-tts-1.7b";
constexpr std::size_t kVoiceContext = 4096;
constexpr std::size_t kVoiceBatch = 128;            // one submit stays well under the 10 s GPU watchdog
constexpr std::size_t kMaxHistoryTurns = 20;
constexpr std::size_t kMaxHistoryBytes = 16384;
constexpr std::size_t kMaxSystemBytes = 2048;
constexpr std::size_t kMaxLanguageBytes = 16;
constexpr std::size_t kMaxModelNameBytes = 128;
constexpr std::size_t kMaxAudioSamples = 120 * 16000; // 120 s of 16 kHz mono
constexpr int kMaxMaxTokens = 1024;

// One voice chat LLM at a time. The lock is held for a whole turn, and the
// model is kept loaded between turns like the STT and TTS models.
std::mutex voice_mutex;
llama_model * llm_model = nullptr;
llama_context * llm_context = nullptr;
std::string llm_name;

using SamplerPtr = std::unique_ptr<llama_sampler, void (*)(llama_sampler *)>;

struct ChatTurn {
    std::string role, content;
};

struct VoiceRequest {
    std::string speaker_wav;  // decoded reference voice, WAV bytes; empty when not sent
    std::string system = kDefaultSystem;
    std::string language;
    std::string llm = kDefaultLlm, stt_model = kDefaultStt, tts_model = kDefaultTts;
    std::vector<ChatTurn> history;
    int max_tokens = 200;
};

double now_s() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

double round2(double value) {
    return std::round(value * 100.0) / 100.0;
}

// Invalid UTF-8 is replaced, so a multi-byte piece split across tokens cannot break an event.
std::string json_text(const nlohmann::json & value) {
    return value.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

// "folder/file" with both parts media_segment_ok. Used for STT and as the base of the LLM check.
bool valid_folder_file(const std::string & model) {
    const std::size_t slash = model.find('/');
    return slash != std::string::npos && media_segment_ok(model.substr(0, slash)) &&
        media_segment_ok(model.substr(slash + 1));
}

// The chat LLM is a .gguf file in a folder under the media root. The mmproj-*
// projector is not a chat model. Only this shape is ever opened, so the request
// cannot name any other file.
bool valid_llm(const std::string & model) {
    if (!valid_folder_file(model)) return false;
    const std::string file = model.substr(model.find('/') + 1);
    return file.size() > 5 && file.compare(file.size() - 5, 5, ".gguf") == 0 && file.rfind("mmproj", 0) != 0;
}

// TTS model: one folder name under the media root.
bool valid_tts(const std::string & model) {
    return media_segment_ok(model);
}

// Length of the prefix of `text` that ends on a complete UTF-8 sequence.
std::size_t complete_utf8_length(const std::string & text) {
    for (std::size_t back = 1; back <= 4 && back <= text.size(); ++back) {
        const unsigned char c = static_cast<unsigned char>(text[text.size() - back]);
        if ((c & 0xC0) == 0x80) continue;  // continuation byte: keep looking for the lead byte
        const std::size_t need = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        return need > back ? text.size() - back : text.size();
    }
    return text.size();
}

// Optional string field: absent leaves `value` unchanged; present must be a string within `limit` bytes.
bool optional_string(const nlohmann::json & body, const char * key, std::size_t limit, std::string & value,
                     std::string & error) {
    const auto found = body.find(key);
    if (found == body.end()) return true;
    if (!found->is_string() || found->get<std::string>().size() > limit) {
        error = std::string(key) + " must be a string of at most " + std::to_string(limit) + " bytes";
        return false;
    }
    value = found->get<std::string>();
    return true;
}

bool parse_history(const nlohmann::json & body, std::vector<ChatTurn> & turns, std::string & error) {
    const auto found = body.find("messages");
    if (found == body.end()) return true;
    const std::string shape = "messages must be an array of at most 20 objects {role: system|user|assistant, content: string}";
    if (!found->is_array() || found->size() > kMaxHistoryTurns) {
        error = shape;
        return false;
    }
    std::size_t bytes = 0;
    for (const nlohmann::json & item : *found) {
        const auto role = item.find("role");
        const auto content = item.find("content");
        if (!item.is_object() || role == item.end() || content == item.end() || !role->is_string() ||
            !content->is_string()) {
            error = shape;
            return false;
        }
        const std::string name = role->get<std::string>();
        if (name != "system" && name != "user" && name != "assistant") {
            error = shape;
            return false;
        }
        bytes += content->get<std::string>().size();
        if (bytes > kMaxHistoryBytes) {
            error = "messages are longer than 16384 bytes in total";
            return false;
        }
        turns.push_back({name, content->get<std::string>()});
    }
    return true;
}

// Parses and validates the whole request, decodes the audio to 16 kHz mono.
// On failure `error` is the 422 message.
bool parse_voice_request(const std::string & body, VoiceRequest & out, std::vector<float> & samples,
                         std::string & error) {
    const nlohmann::json json = nlohmann::json::parse(body, nullptr, false);
    if (json.is_discarded() || !json.is_object()) {
        error = "request body must be a JSON object";
        return false;
    }
    std::string audio_b64, speaker_b64;
    if (!optional_string(json, "audio_b64", kMediaMaxBody, audio_b64, error)) return false;
    if (audio_b64.empty()) {
        error = "audio_b64 (base64 WAV) is required";
        return false;
    }
    if (!optional_string(json, "speaker_b64", kMediaMaxBody, speaker_b64, error) ||
        !optional_string(json, "system", kMaxSystemBytes, out.system, error) ||
        !optional_string(json, "language", kMaxLanguageBytes, out.language, error) ||
        !optional_string(json, "llm", kMaxModelNameBytes, out.llm, error) ||
        !optional_string(json, "stt_model", kMaxModelNameBytes, out.stt_model, error) ||
        !optional_string(json, "tts_model", kMaxModelNameBytes, out.tts_model, error)) {
        return false;
    }
    if (!valid_llm(out.llm) || !valid_folder_file(out.stt_model) || !valid_tts(out.tts_model)) {
        error = "llm, stt_model or tts_model is not a valid model name";
        return false;
    }
    if (!parse_history(json, out.history, error)) return false;
    const auto tokens = json.find("max_tokens");
    if (tokens != json.end()) {
        if (!tokens->is_number_integer() || tokens->get<int64_t>() < 1 || tokens->get<int64_t>() > kMaxMaxTokens) {
            error = "max_tokens must be 1 to 1024";
            return false;
        }
        out.max_tokens = tokens->get<int>();
    }

    std::string bytes, ignored;
    if (!base64_decode(audio_b64, bytes, ignored)) {
        error = "audio_b64 is not valid base64";
        return false;
    }
    if (!decode_wav(bytes, 16000, samples, ignored)) {
        error = "audio_b64 is not a readable WAV file";
        return false;
    }
    if (samples.empty()) {
        error = "audio_b64 has no audio";
        return false;
    }
    if (samples.size() > kMaxAudioSamples) {
        error = "audio_b64 is longer than 120 seconds";
        return false;
    }
    if (!speaker_b64.empty() && !base64_decode(speaker_b64, out.speaker_wav, ignored)) {
        error = "speaker_b64 is not valid base64";
        return false;
    }
    return true;
}

// Progress event while a model loads: {"status":...,"model":...,"elapsed_s":...}.
std::string phase_json(const char * status, const std::string & model, double started) {
    return json_text({{"status", status}, {"model", model}, {"elapsed_s", round2(now_s() - started)}});
}

// Loads the chat LLM unless it is already loaded. A different LLM is freed first.
// Caller holds voice_mutex.
bool ensure_llm(const std::string & model, double started, std::string & error, MediaSse & sse) {
    if (llm_model && llm_name == model) return true;
    if (llm_context) llama_free(llm_context);
    if (llm_model) llama_model_free(llm_model);
    llm_context = nullptr;
    llm_model = nullptr;
    llm_name.clear();
    const std::string path = std::string(kMediaRoot) + model;
    struct stat info{};
    if (stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) {
        error = "voice LLM file is not found under the media root";
        return false;
    }
    sse.event(phase_json("loading", model, started));
    llama_model_params model_params = llama_model_default_params();
    model_params.n_gpu_layers = 999;
    llama_model * model_handle = llama_model_load_from_file(path.c_str(), model_params);
    if (!model_handle) {
        error = "voice LLM failed to load (see app log)";
        return false;
    }
    llama_context_params context_params = llama_context_default_params();
    context_params.n_ctx = static_cast<uint32_t>(kVoiceContext);
    context_params.n_batch = static_cast<uint32_t>(kVoiceBatch);
    context_params.n_ubatch = static_cast<uint32_t>(kVoiceBatch);
    context_params.n_threads = 4;
    context_params.n_threads_batch = 4;
    context_params.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_ENABLED;  // required for the quantized V cache
    context_params.type_k = GGML_TYPE_Q4_0;
    context_params.type_v = GGML_TYPE_Q4_0;
    llama_context * context = llama_init_from_model(model_handle, context_params);
    if (!context) {
        llama_model_free(model_handle);
        error = "voice LLM context could not be created (see app log)";
        return false;
    }
    llm_model = model_handle;
    llm_context = context;
    llm_name = model;
    return true;
}

// Appends the tokens of one prompt part. Only template text may spell special
// tokens; user text is tokenized with parse_special off.
bool tokenize_part(const llama_vocab * vocab, const std::string & text, bool add_special, bool parse_special,
                   std::vector<llama_token> & out) {
    std::vector<llama_token> buffer(text.size() + 16);
    int32_t count = llama_tokenize(vocab, text.data(), static_cast<int32_t>(text.size()), buffer.data(),
                                   static_cast<int32_t>(buffer.size()), add_special, parse_special);
    if (count < 0) {  // buffer too small: count is the negated size needed
        buffer.resize(static_cast<std::size_t>(-count));
        count = llama_tokenize(vocab, text.data(), static_cast<int32_t>(text.size()), buffer.data(),
                               static_cast<int32_t>(buffer.size()), add_special, parse_special);
    }
    if (count < 0) return false;
    out.insert(out.end(), buffer.begin(), buffer.begin() + count);
    return true;
}

// Renders system, history and what was just heard with the model's chat template, then tokenizes it.
// Caller holds voice_mutex and has loaded the LLM.
bool build_prompt(const VoiceRequest & params, const std::string & heard, std::vector<llama_token> & tokens,
                  std::string & error) {
    common_chat_prompt prompt;
    try {
        auto templates = common_chat_templates_init(llm_model, "");
        common_chat_templates_inputs inputs;
        inputs.add_generation_prompt = true;
        inputs.use_jinja = true;
        inputs.enable_thinking = false;
        auto add = [&](const std::string & role, const std::string & content) {
            common_chat_msg message;
            message.role = role;
            message.content = content;
            inputs.messages.push_back(std::move(message));
        };
        add("system", params.system);
        for (const ChatTurn & turn : params.history) add(turn.role, turn.content);
        add("user", heard);
        prompt = common_chat_templates_apply(templates.get(), inputs).prompt;
    } catch (const std::exception &) {
        error = "chat template failed";  // fixed text: the exception may echo template or user content
        return false;
    }
    const llama_vocab * vocab = llama_model_get_vocab(llm_model);
    bool first = true;
    for (const common_chat_prompt_part & part : prompt.parts) {
        if (!tokenize_part(vocab, part.text, first, !part.is_input, tokens)) {
            error = "prompt could not be tokenized";
            return false;
        }
        first = false;
    }
    return true;
}
} // namespace

bool media_voice_unload() {
    std::lock_guard<std::mutex> lock(voice_mutex);
    if (!llm_model && !llm_context) return false;
    if (llm_context) llama_free(llm_context);
    if (llm_model) llama_model_free(llm_model);
    llm_context = nullptr;
    llm_model = nullptr;
    llm_name.clear();
    return true;
}

void handle_voice_chat(int fd, const MediaRequest & request) {
    const double started = now_s();
    VoiceRequest params;
    std::vector<float> samples;
    std::string error;
    if (!parse_voice_request(request.body, params, samples, error)) {
        media_send_error(fd, 422, "invalid_request_error", error);
        std::printf("ps5lm-media: BAD VOICE REQUEST: %s\n", error.c_str());
        return;
    }

    // Before voice_mutex is taken: media_gpu_acquire may call media_voice_unload, which locks it.
    media_gpu_acquire(kGpuStt | kGpuTts | kGpuVoiceLlm);
    std::lock_guard<std::mutex> lock(voice_mutex);
    MediaSse sse(fd);
    if (!sse.begin()) return;
    auto fail = [&](const std::string & message) {
        sse.event(json_text({{"type", "error"}, {"message", message}}));
        sse.event("[DONE]");
        sse.end();
        std::printf("ps5lm-media: VOICE TURN FAILED: %s\n", message.c_str());
    };

    if (!ensure_llm(params.llm, started, error, sse)) {
        fail(error);
        return;
    }
    if (stt_loaded_model() != params.stt_model) sse.event(phase_json("loading", params.stt_model, started));
    if (!stt_load(params.stt_model, error, nullptr)) {
        fail(error);
        return;
    }
    if (tts_loaded_model() != params.tts_model) sse.event(phase_json("loading", params.tts_model, started));
    if (!tts_load(params.tts_model, error, nullptr)) {
        fail(error);
        return;
    }
    const double ready_s = now_s() - started;

    // 1. Speech to text.
    SttResult heard;
    if (!stt_transcribe(samples, params.language, false, heard, error)) {
        fail(error);
        return;
    }
    const double transcript_s = now_s() - started;
    sse.event(json_text({{"type", "transcript"}, {"text", heard.text}, {"audio_s", round2(heard.audio_s)},
                         {"at_s", round2(transcript_s)}}));
    std::printf("ps5lm-media: voice heard %.1f s in %.2f s\n", heard.audio_s, heard.transcribe_s);

    // 2. Prompt, then check it fits the context with room for the reply.
    std::vector<llama_token> tokens;
    if (!build_prompt(params, heard.text, tokens, error)) {
        fail(error);
        return;
    }
    if (tokens.size() + static_cast<std::size_t>(params.max_tokens) > llama_n_ctx(llm_context)) {
        fail("conversation is too long for the context");
        return;
    }
    llama_memory_clear(llama_get_memory(llm_context), true);
    for (std::size_t from = 0; from < tokens.size(); from += kVoiceBatch) {
        const std::size_t count = std::min(kVoiceBatch, tokens.size() - from);
        if (llama_decode(llm_context, llama_batch_get_one(tokens.data() + from, static_cast<int32_t>(count))) != 0) {
            fail("prompt evaluation failed");
            return;
        }
    }
    const llama_vocab * vocab = llama_model_get_vocab(llm_model);
    SamplerPtr sampler(llama_sampler_chain_init(llama_sampler_chain_default_params()), llama_sampler_free);
    llama_sampler_chain_add(sampler.get(), llama_sampler_init_top_k(40));
    llama_sampler_chain_add(sampler.get(), llama_sampler_init_top_p(0.9f, 1));
    llama_sampler_chain_add(sampler.get(), llama_sampler_init_temp(0.7f));
    llama_sampler_chain_add(sampler.get(), llama_sampler_init_dist(LLAMA_DEFAULT_SEED));

    // 3. Speak each sentence as soon as it is complete.
    std::string reply, pending, carry, voice_reference = params.speaker_wav;
    int spoken = 0, generated = 0;
    double first_text_s = 0.0, first_audio_s = 0.0, tts_s = 0.0, speech_s = 0.0;
    bool speak_failed = false;
    auto speak = [&](const std::string & sentence) {
        if (!sse.open()) return;
        std::string clean;
        bool has_word = false;
        for (const unsigned char c : sentence) {
            if (c == '*' || c == '#' || c == '`' || c == '_') continue;
            clean += (c == '\n' || c == '\r') ? ' ' : static_cast<char>(c);
            has_word |= std::isalnum(c) != 0;
        }
        if (!has_word) return;
        TtsRequest tts;
        tts.text = clean;
        tts.language = params.language;
        tts.speaker_wav = voice_reference;
        tts.max_frames = 600;
        tts.seed = 7;
        TtsResult audio;
        std::string tts_error;
        if (!tts_generate(tts, audio, tts_error, nullptr)) {
            speak_failed = true;
            std::printf("ps5lm-media: VOICE TTS FAILED: %s\n", tts_error.c_str());
            return;
        }
        // Without a reference voice, the first sentence's voice is reused for the rest of the reply.
        if (voice_reference.empty()) voice_reference = audio.wav;
        tts_s += audio.generate_s;
        speech_s += audio.audio_s;
        const double now = now_s() - started;
        if (first_audio_s == 0.0) first_audio_s = now;
        sse.event(json_text({{"type", "audio"}, {"index", spoken}, {"audio_s", round2(audio.audio_s)},
                             {"tts_s", round2(audio.generate_s)}, {"at_s", round2(now)}, {"text", clean},
                             {"audio_b64", base64_encode(audio.wav)}}));
        ++spoken;
    };
    for (; generated < params.max_tokens && sse.open(); ++generated) {
        llama_token token = llama_sampler_sample(sampler.get(), llm_context, -1);
        if (llama_vocab_is_eog(vocab, token)) break;
        char piece[256];
        const int32_t n = llama_token_to_piece(vocab, token, piece, static_cast<int32_t>(sizeof(piece)), 0, false);
        if (n > 0) {
            if (first_text_s == 0.0) first_text_s = now_s() - started;
            carry.append(piece, static_cast<std::size_t>(n));
            const std::size_t complete = complete_utf8_length(carry);
            const std::string text = carry.substr(0, complete);
            carry.erase(0, complete);
            if (!text.empty()) {
                reply += text;
                pending += text;
                sse.event(json_text({{"type", "text"}, {"delta", text}}));
                // A sentence ends at . ! ? followed by whitespace (so "3.5" stays whole).
                std::size_t cut = std::string::npos;
                for (std::size_t i = 0; i + 1 < pending.size(); ++i) {
                    if ((pending[i] == '.' || pending[i] == '!' || pending[i] == '?') &&
                        std::isspace(static_cast<unsigned char>(pending[i + 1])) && i >= 12)
                        cut = i + 1;
                }
                if (cut != std::string::npos) {
                    speak(pending.substr(0, cut));
                    pending.erase(0, cut);
                }
            }
        }
        if (llama_decode(llm_context, llama_batch_get_one(&token, 1)) != 0) break;
    }
    if (!carry.empty()) {  // an incomplete UTF-8 tail at the end of the reply
        reply += carry;
        pending += carry;
        carry.clear();
    }
    speak(pending);
    if (speak_failed) std::printf("ps5lm-media: voice turn had TTS errors\n");

    // 4. Final event and end of stream.
    const double total_s = now_s() - started;
    sse.event(json_text({{"type", "done"}, {"transcript", heard.text}, {"reply", reply}, {"tokens", generated},
                         {"sentences", spoken},
                         {"timings", {{"ready_s", round2(ready_s)}, {"transcript_s", round2(transcript_s)},
                                      {"first_text_s", round2(first_text_s)},
                                      {"first_audio_s", round2(first_audio_s)}, {"tts_s", round2(tts_s)},
                                      {"speech_s", round2(speech_s)}, {"total_s", round2(total_s)}}}}));
    sse.event("[DONE]");
    sse.end();
    std::printf("ps5lm-media: voice turn first audio %.2f s, %.1f s speech, total %.2f s\n", first_audio_s,
                speech_s, total_s);
}

void register_media_voice_routes(MediaServer & server) {
    if (!server.route("POST", "/v1/voice/chat", handle_voice_chat))
        std::printf("ps5lm-media: POST /v1/voice/chat was not registered\n");
}
} // namespace ps5lm
