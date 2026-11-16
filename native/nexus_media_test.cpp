// Host tests for the media HTTP API (native/nexus_media*.cpp). The engines are replaced by the
// link-time fakes in native/nexus_media_fakes.cpp, which record their calls. The tests talk to a
// real MediaServer over loopback sockets. Built and run by test-media.sh <build-tree>.
// Linux host only (sockets, mkdtemp, --wrap=stat). Run on Linux host:
//   c++ -std=c++20 -Wall -Wextra -Werror ... native/nexus_media_test.cpp native/nexus_media_fakes.cpp ...
// SPDX-License-Identifier: GPL-3.0-or-later
#include "nexus_media.hpp"
#include "nexus_wav.hpp"

extern "C" {
#include "hash/sha256/sha256.h"
}
#include <nlohmann/json.hpp>

#include <algorithm>
#include <arpa/inet.h>
#include <chrono>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

// Same contract as assert() in the sibling tests, but always evaluated (no NDEBUG dependence)
// and it prints the failing expression.
#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); std::abort(); } } while (0)

namespace ps5lm {
namespace fake {
void reset();
std::vector<std::string> calls();
void load_engine(const char * engine, const std::string & model);
void set_voice_loaded(bool loaded);
void set_sd_generate_fails(bool fails);
} // namespace fake
} // namespace ps5lm

using namespace ps5lm;
namespace fake = ps5lm::fake;

namespace {
// Public bearer test fixture (length 35, inside the 32..128 bearer rule). Not a secret.
constexpr char kToken[] = "NexusMediaHostTestBearer_0123456789";
constexpr char kWrongToken[] = "NexusMediaHostTestBearer_9999999999";
// Must match the literal returned by the tts_generate fake in nexus_media_fakes.cpp.
const std::string kFakeWav = "RIFF-fake-wav";
const char kImageInvalid[] = "invalid image request";
const char kSttInvalid[] = "audio_b64 (base64 WAV) is required";
const char kSpeechInvalid[] = "input is required";
const char kAuthDenied[] = "valid Bearer API key required";
static_assert(sizeof(kToken) == sizeof(kWrongToken), "wrong token must match the bearer length");
static_assert(sizeof(kToken) - 1 >= 32 && sizeof(kToken) - 1 <= 128, "bearer length rule");

struct Reply {
    int status = -1;
    std::string head;  // status line and headers
    std::string body;  // de-chunked body
};

std::string dechunk(const std::string & encoded) {
    std::string out;
    std::size_t at = 0;
    for (;;) {
        const std::size_t eol = encoded.find("\r\n", at);
        CHECK(eol != std::string::npos);
        const std::size_t size = std::strtoul(encoded.substr(at, eol - at).c_str(), nullptr, 16);
        if (size == 0) return out;
        at = eol + 2;
        CHECK(at + size <= encoded.size());
        out.append(encoded, at, size);
        at += size + 2;  // data, then CRLF
    }
}

// Sends raw bytes to 127.0.0.1 and reads until the server closes the connection.
Reply raw_exchange(unsigned short port, const std::string & wire) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    timeval limit{};
    limit.tv_sec = 10;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof(limit));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &limit, sizeof(limit));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);

    std::size_t sent = 0;
    while (sent < wire.size()) {
        const ssize_t count = send(fd, wire.data() + sent, wire.size() - sent, MSG_NOSIGNAL);
        if (count < 0 && errno == EINTR) continue;
        CHECK(count > 0);
        sent += static_cast<std::size_t>(count);
    }
    std::string raw;
    char buffer[4096];
    for (;;) {
        const ssize_t count = recv(fd, buffer, sizeof(buffer), 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        raw.append(buffer, static_cast<std::size_t>(count));
    }
    close(fd);

    Reply reply;
    const std::size_t split = raw.find("\r\n\r\n");
    if (split == std::string::npos) return reply;  // no response: status stays -1
    reply.head = raw.substr(0, split);
    if (raw.compare(0, 9, "HTTP/1.1 ") == 0) reply.status = std::atoi(raw.c_str() + 9);
    reply.body = raw.substr(split + 4);
    if (reply.head.find("Transfer-Encoding: chunked") != std::string::npos) reply.body = dechunk(reply.body);
    return reply;
}

// Builds a request. An empty auth sends no Authorization header. POST, or any request with a
// body, gets Content-Length.
std::string build_request(const std::string & method, const std::string & path, const std::string & body,
                          const std::string & auth) {
    std::string text = method + " " + path + " HTTP/1.1\r\nHost: localhost\r\n";
    if (!auth.empty()) text += "Authorization: Bearer " + auth + "\r\n";
    if (method == "POST" || !body.empty()) text += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    return text + "Connection: close\r\n\r\n" + body;
}

Reply http_call(unsigned short port, const std::string & method, const std::string & path,
                const std::string & body = std::string(), const std::string & auth = std::string(kToken)) {
    return raw_exchange(port, build_request(method, path, body, auth));
}

nlohmann::json json_of(const Reply & reply) {
    nlohmann::json value = nlohmann::json::parse(reply.body, nullptr, false);
    CHECK(!value.is_discarded());
    return value;
}

// PSllama error shape: {"error":{"message":...,"type":...}}.
void expect_error(const Reply & reply, int status, const char * type, const char * message_part) {
    CHECK(reply.status == status);
    const nlohmann::json body = json_of(reply);
    CHECK(body.contains("error"));
    const nlohmann::json & error = body.at("error");
    CHECK(error.at("type").get<std::string>() == std::string(type));
    CHECK(error.at("message").get<std::string>().find(message_part) != std::string::npos);
}

// Splits a text/event-stream body into the "data: " payloads.
std::vector<std::string> sse_payloads(const std::string & body) {
    std::vector<std::string> payloads;
    std::size_t at = 0;
    while (at < body.size()) {
        const std::size_t end = body.find("\n\n", at);
        CHECK(end != std::string::npos);
        const std::string event = body.substr(at, end - at);
        CHECK(event.compare(0, 6, "data: ") == 0);
        payloads.push_back(event.substr(6));
        at = end + 2;
    }
    return payloads;
}

std::size_t position(const std::vector<std::string> & log, const std::string & entry) {
    const auto found = std::find(log.begin(), log.end(), entry);
    return found == log.end() ? std::string::npos : static_cast<std::size_t>(found - log.begin());
}

std::string le(uint64_t value, std::size_t count) {
    std::string bytes;
    for (std::size_t i = 0; i < count; ++i) bytes.push_back(static_cast<char>((value >> (8 * i)) & 255));
    return bytes;
}

// 160 samples of 16 kHz mono PCM16 silence, as a base64 WAV file.
std::string wav_base64() {
    const std::string pcm(320, '\0');
    const std::string wav = "RIFF" + le(36 + pcm.size(), 4) + "WAVE" + "fmt " + le(16, 4) + le(1, 2) + le(1, 2) +
        le(16000, 4) + le(32000, 4) + le(2, 2) + le(16, 2) + "data" + le(pcm.size(), 4) + pcm;
    return base64_encode(wav);
}

std::string bearer_header() { return std::string("Authorization: Bearer ") + kToken + "\r\n"; }

void write_file(const std::string & path, const std::string & text, mode_t mode) {
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    CHECK(fd >= 0);
    CHECK(write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size()));
    close(fd);
    CHECK(chmod(path.c_str(), mode) == 0);
}

// ---- tests ----------------------------------------------------------------------------------

void test_auth(unsigned short port, const std::string & hex_digest) {
    expect_error(http_call(port, "GET", "/health", "", ""), 401, "authentication_error", kAuthDenied);
    expect_error(http_call(port, "GET", "/health", "", kWrongToken), 401, "authentication_error", kAuthDenied);
    expect_error(http_call(port, "GET", "/health", "", "Bad Token With Spaces 0123456789 abcdef"), 401,
                 "authentication_error", kAuthDenied);
    // The stored digest is not a bearer: it is 64 hex digits, not the token.
    expect_error(http_call(port, "GET", "/health", "", hex_digest), 401, "authentication_error", kAuthDenied);
    expect_error(raw_exchange(port, "GET /health HTTP/1.1\r\nHost: localhost\r\n"
                                    "Authorization: Basic dXNlcjpwYXNz\r\n\r\n"),
                 401, "authentication_error", kAuthDenied);
    expect_error(http_call(port, "POST", "/v1/images/generations", R"({"prompt":"a cat"})", ""), 401,
                 "authentication_error", kAuthDenied);
    CHECK(http_call(port, "GET", "/health").status == 200);
}

void test_framing(unsigned short port) {
    // 411: POST without Content-Length (the header is checked before the body is read).
    expect_error(raw_exchange(port, "POST /v1/images/generations HTTP/1.1\r\nHost: localhost\r\n" +
                                    bearer_header() + "Connection: close\r\n\r\n"),
                 411, "invalid_request_error", "Content-Length required");
    // 413: a declared body one byte over 64 MiB. Only the header is sent.
    expect_error(raw_exchange(port, "POST /v1/images/generations HTTP/1.1\r\nHost: localhost\r\n" +
                                    bearer_header() + "Content-Length: 67108865\r\n\r\n"),
                 413, "invalid_request_error", "request body too large");
    // 431: a header block over 8 KiB, with its terminator, so the whole request is consumed.
    expect_error(raw_exchange(port, "GET /health HTTP/1.1\r\nHost: localhost\r\n" + bearer_header() +
                                    "X-Pad: " + std::string(9000, 'a') + "\r\n\r\n"),
                 431, "invalid_request_error", "request headers too large");
}

void test_routes_and_health(unsigned short port) {
    const Reply health = http_call(port, "GET", "/health");
    CHECK(health.status == 200);
    CHECK(health.head.find("Content-Type: application/json") != std::string::npos);
    const nlohmann::json fields = json_of(health);
    CHECK(fields.at("status").get<std::string>() == "ok");
    CHECK(fields.at("mode").get<std::string>() == "media");
    CHECK(fields.size() == 2);
    CHECK(http_call(port, "GET", "/health?probe=1").status == 200);  // the query string is ignored
    expect_error(http_call(port, "GET", "/nope"), 404, "invalid_request_error", "endpoint not found");
    expect_error(http_call(port, "PUT", "/health"), 405, "invalid_request_error", "method not allowed");
}

void test_json_quote() {
    CHECK(json_quote("a\"b\n\x01") == "\"a\\\"b\\n\\u0001\"");
}

void test_image_requests(unsigned short port) {
    const std::string post = "/v1/images/generations";
    expect_error(http_call(port, "POST", post, ""), 422, "invalid_request_error", kImageInvalid);  // empty body
    expect_error(http_call(port, "POST", post, "{"), 422, "invalid_request_error", kImageInvalid);
    expect_error(http_call(port, "POST", post, "[]"), 422, "invalid_request_error", kImageInvalid);
    expect_error(http_call(port, "POST", post, "{}"), 422, "invalid_request_error", "prompt is required");
    expect_error(http_call(port, "POST", post, R"({"prompt":42})"), 422, "invalid_request_error", "prompt is required");
    expect_error(http_call(port, "POST", post, "{\"prompt\":\"" + std::string(4097, 'a') + "\"}"), 422,
                 "invalid_request_error", "prompt is required");
    // Size rule: multiples of 64 in 256..1024 per side, or both omitted.
    expect_error(http_call(port, "POST", post, R"({"prompt":"a","size":"300x300"})"), 422, "invalid_request_error", kImageInvalid);
    expect_error(http_call(port, "POST", post, R"({"prompt":"a","size":"256"})"), 422, "invalid_request_error", kImageInvalid);
    expect_error(http_call(port, "POST", post, R"({"prompt":"a","width":288,"height":288})"), 422, "invalid_request_error", kImageInvalid);
    expect_error(http_call(port, "POST", post, R"({"prompt":"a","width":192,"height":192})"), 422, "invalid_request_error", kImageInvalid);
    expect_error(http_call(port, "POST", post, R"({"prompt":"a","width":1088,"height":1088})"), 422, "invalid_request_error", kImageInvalid);
    expect_error(http_call(port, "POST", post, R"({"prompt":"a","size":"256x0"})"), 422, "invalid_request_error", kImageInvalid);
    expect_error(http_call(port, "POST", post, R"({"prompt":"a","steps":0})"), 422, "invalid_request_error", kImageInvalid);
    expect_error(http_call(port, "POST", post, R"({"prompt":"a","stream":"yes"})"), 422, "invalid_request_error", kImageInvalid);
    expect_error(http_call(port, "POST", post, R"({"prompt":"a","init_image_b64":"!!"})"), 422, "invalid_request_error", kImageInvalid);

    // Accepted sizes reach the engine and come back as given.
    const char * sizes[][3] = {{"256x256", "256", "256"}, {"1024x1024", "1024", "1024"}, {"256x320", "256", "320"}};
    for (const auto & entry : sizes) {
        const Reply reply = http_call(port, "POST", post,
                                      std::string(R"({"prompt":"a cat","size":")") + entry[0] + "\"}");
        CHECK(reply.status == 200);
        const nlohmann::json image = json_of(reply);
        CHECK(image.at("width").get<int>() == std::atoi(entry[1]));
        CHECK(image.at("height").get<int>() == std::atoi(entry[2]));
    }

    // Success: defaults, explicit seed, base64 PNG data.
    const Reply ok = http_call(port, "POST", post, R"({"prompt":"a cat","seed":7})");
    CHECK(ok.status == 200);
    const nlohmann::json image = json_of(ok);
    CHECK(image.at("model").get<std::string>() == "sd-turbo-fp16");
    CHECK(image.at("seed").get<int>() == 7);
    CHECK(image.at("width").get<int>() == 512);
    std::string png, error;
    CHECK(base64_decode(image.at("data").at(0).at("b64_json").get<std::string>(), png, error));
    CHECK(png.compare(0, 4, std::string("\x89", 1) + "PNG") == 0);

    // Model name rules and folder lookup.
    expect_error(http_call(port, "POST", post, R"({"prompt":"a","model":".."})"), 422, "invalid_request_error", kImageInvalid);
    expect_error(http_call(port, "POST", post, R"({"prompt":"a","model":"a/../b"})"), 422, "invalid_request_error", kImageInvalid);
    expect_error(http_call(port, "POST", post, R"({"prompt":"a","model":"missing-model"})"), 404,
                 "invalid_request_error", "image model not found");
}

void test_video_requests(unsigned short port) {
    const std::string post = "/v1/videos/generations";
    expect_error(http_call(port, "POST", post, "{}"), 422, "invalid_request_error", "prompt is required");
    expect_error(http_call(port, "POST", post, R"({"prompt":"a","frames":6})"), 422, "invalid_request_error", "prompt is required");
    expect_error(http_call(port, "POST", post, R"({"prompt":"a","size":"130x128"})"), 422, "invalid_request_error", "prompt is required");
    expect_error(http_call(port, "POST", post, R"({"prompt":"a","model":".."})"), 422, "invalid_request_error", "prompt is required");

    const Reply reply = http_call(port, "POST", post, R"({"prompt":"a boat","frames":5})");
    CHECK(reply.status == 200);
    const nlohmann::json clip = json_of(reply);
    CHECK(clip.at("frames").get<int>() == 5);
    CHECK(clip.at("data").size() == 5);
    CHECK(clip.at("fps").get<int>() == 16);
    CHECK(clip.at("model").get<std::string>() == "wan2.1-t2v-1.3b");
}

void test_speech_requests(unsigned short port) {
    const std::string post = "/v1/audio/speech";
    expect_error(http_call(port, "POST", post, "{}"), 422, "invalid_request_error", kSpeechInvalid);
    expect_error(http_call(port, "POST", post, "not json"), 422, "invalid_request_error", kSpeechInvalid);
    expect_error(http_call(port, "POST", post, R"({"input":"hi","response_format":"xml"})"), 422, "invalid_request_error", kSpeechInvalid);
    expect_error(http_call(port, "POST", post, R"({"input":"hi","max_frames":5})"), 422, "invalid_request_error", kSpeechInvalid);
    expect_error(http_call(port, "POST", post, R"({"input":"hi","seed":"x"})"), 422, "invalid_request_error", kSpeechInvalid);
    expect_error(http_call(port, "POST", post, R"({"input":"hi","speaker_b64":"!!!!"})"), 422, "invalid_request_error", kSpeechInvalid);
    expect_error(http_call(port, "POST", post, R"({"input":"hi","model":"../x"})"), 422, "invalid_request_error", kSpeechInvalid);
    expect_error(http_call(port, "POST", post, "{\"input\":\"" + std::string(4097, 'a') + "\"}"), 422,
                 "invalid_request_error", "input is longer than 4096 bytes");

    const Reply json_reply = http_call(port, "POST", post, R"({"input":"hello"})");
    CHECK(json_reply.status == 200);
    const nlohmann::json speech = json_of(json_reply);
    CHECK(speech.at("model").get<std::string>() == "qwen3-tts-1.7b");
    CHECK(speech.at("sample_rate").get<int>() == 24000);
    CHECK(speech.at("frames").get<int>() == 3);
    std::string wav, error;
    CHECK(base64_decode(speech.at("audio_b64").get<std::string>(), wav, error));
    CHECK(wav == kFakeWav);

    const Reply wav_reply = http_call(port, "POST", post, R"({"input":"hello","response_format":"wav"})");
    CHECK(wav_reply.status == 200);
    CHECK(wav_reply.head.find("Content-Type: audio/wav") != std::string::npos);
    CHECK(wav_reply.body == kFakeWav);
}

void test_transcription_requests(unsigned short port) {
    const std::string post = "/v1/audio/transcriptions";
    const std::string audio = "{\"audio_b64\":\"" + wav_base64() + "\"";
    expect_error(http_call(port, "POST", post, "{}"), 422, "invalid_request_error", kSttInvalid);
    expect_error(http_call(port, "POST", post, R"({"audio_b64":"!!!!"})"), 422, "invalid_request_error",
                 "audio_b64 is not valid base64");
    expect_error(http_call(port, "POST", post, R"({"audio_b64":"aGVsbG8="})"), 422, "invalid_request_error", "");  // not a WAV
    expect_error(http_call(port, "POST", post, "nope"), 422, "invalid_request_error", kSttInvalid);
    expect_error(http_call(port, "POST", post, audio + ",\"model\":\"whisper\"}"), 422, "invalid_request_error", kSttInvalid);
    expect_error(http_call(port, "POST", post, audio + ",\"model\":\"whisper/..\"}"), 422, "invalid_request_error", kSttInvalid);

    const Reply reply = http_call(port, "POST", post, audio + "}");
    CHECK(reply.status == 200);
    const nlohmann::json transcript = json_of(reply);
    CHECK(transcript.at("text").get<std::string>() == "hello there");
    CHECK(transcript.at("language").get<std::string>() == "en");
    CHECK(transcript.at("segments").size() == 1);
    CHECK(transcript.at("model").get<std::string>() == "whisper/ggml-large-v3-turbo-q5_0.bin");
}

// SSE: the progress steps come first, then one error event carrying http_status, then [DONE].
void test_stream_error_events(unsigned short port) {
    fake::reset();
    fake::set_sd_generate_fails(true);
    const Reply reply = http_call(port, "POST", "/v1/images/generations", R"({"prompt":"a cat","stream":true})");
    CHECK(reply.status == 200);
    CHECK(reply.head.find("Content-Type: text/event-stream") != std::string::npos);
    CHECK(reply.head.find("Transfer-Encoding: chunked") != std::string::npos);
    const std::vector<std::string> events = sse_payloads(reply.body);
    CHECK(events.size() == 4);
    CHECK(events[0].find("\"step\":1") != std::string::npos);
    CHECK(events[1].find("\"step\":2") != std::string::npos);
    const nlohmann::json failure = nlohmann::json::parse(events[2], nullptr, false);
    CHECK(!failure.is_discarded());
    CHECK(failure.at("error").at("type").get<std::string>() == "server_error");
    CHECK(failure.at("error").at("message").get<std::string>() == "fake engine failure");
    CHECK(failure.at("http_status").get<int>() == 500);
    CHECK(events[3] == "[DONE]");

    // Without stream the same failure is a plain 500 JSON response (the fake still fails).
    expect_error(http_call(port, "POST", "/v1/images/generations", R"({"prompt":"a cat"})"), 500,
                 "server_error", "fake engine failure");
    fake::set_sd_generate_fails(false);
}

// GPU coordination through the real media_gpu_acquire and the fake call log.
void test_gpu_coordination(unsigned short port) {
    const std::string sd_model = "sd-turbo-fp16";
    const std::string stt_model = "whisper/ggml-large-v3-turbo-q5_0.bin";
    const std::string tts_model = "qwen3-tts-1.7b";

    // Image: STT and TTS are unloaded before the SD model loads and generates.
    fake::reset();
    fake::load_engine("stt", stt_model);
    fake::load_engine("tts", tts_model);
    CHECK(http_call(port, "POST", "/v1/images/generations", R"({"prompt":"a cat"})").status == 200);
    std::vector<std::string> log = fake::calls();
    const std::size_t stt_off = position(log, "stt_unload");
    const std::size_t tts_off = position(log, "tts_unload");
    const std::size_t sd_load = position(log, "sd_load:" + sd_model);
    const std::size_t sd_gen = position(log, "sd_generate");
    CHECK(stt_off != std::string::npos && tts_off != std::string::npos);
    CHECK(stt_off < sd_load && tts_off < sd_load && sd_load < sd_gen);

    // Speech: SD is unloaded; STT and TTS stay (the TTS model is already the requested one).
    fake::reset();
    fake::load_engine("sd", sd_model);
    fake::load_engine("stt", stt_model);
    fake::load_engine("tts", tts_model);
    CHECK(http_call(port, "POST", "/v1/audio/speech", R"({"input":"hello"})").status == 200);
    log = fake::calls();
    CHECK(position(log, "sd_unload") != std::string::npos);
    CHECK(position(log, "stt_unload") == std::string::npos);
    CHECK(position(log, "tts_unload") == std::string::npos);
    CHECK(position(log, "sd_unload") < position(log, "tts_generate"));

    // Transcription: SD is unloaded; the TTS model stays.
    fake::reset();
    fake::load_engine("sd", sd_model);
    fake::load_engine("tts", tts_model);
    CHECK(http_call(port, "POST", "/v1/audio/transcriptions", "{\"audio_b64\":\"" + wav_base64() + "\"}").status == 200);
    log = fake::calls();
    CHECK(position(log, "sd_unload") != std::string::npos);
    CHECK(position(log, "tts_unload") == std::string::npos);
    CHECK(position(log, "stt_load:" + stt_model) != std::string::npos);
}

void test_gpu_mask() {
    const unsigned all_but_sd = kGpuStt | kGpuTts | kGpuVoiceLlm;
    fake::reset();
    fake::load_engine("sd", "sd-turbo-fp16");
    fake::load_engine("stt", "whisper/x.bin");
    fake::load_engine("tts", "tts");
    fake::set_voice_loaded(true);
    CHECK(media_gpu_acquire(kGpuSd | all_but_sd) == 0);                 // all kept: nothing unloaded
    CHECK(media_gpu_acquire(kGpuSd) == all_but_sd);                     // the rest unloaded, SD kept
    CHECK(media_gpu_acquire(0) == kGpuSd);                              // now SD unloaded
    CHECK(media_gpu_acquire(0) == 0);                                   // idempotent when empty
    CHECK(!media_voice_unload());                                       // voice already gone
}

void test_start_rules(const std::string & token_file, const std::string & directory) {
    MediaServer refused;
    CHECK(!refused.start(directory + "/missing", false, 0));           // no credential file
    const std::string loose = directory + "/loose";
    write_file(loose, "sha256:" + std::string(64, '0') + "\n", 0644);
    CHECK(!refused.start(loose, false, 0));                            // group/world readable
    MediaServer lan;
    CHECK(lan.start(token_file, true, 0));                             // LAN marker: binds 0.0.0.0
    CHECK(http_call(lan.port(), "GET", "/health").status == 200);     // loopback peer admitted
    lan.stop();
    CHECK(unlink(loose.c_str()) == 0);
}

// Host-order IPv4 address from dotted text, for media_peer_allowed.
uint32_t host_ip(const char * text) {
    in_addr address{};
    CHECK(inet_pton(AF_INET, text, &address) == 1);
    return ntohl(address.s_addr);
}

void test_peer_filter() {
    CHECK(media_peer_allowed(host_ip("127.0.0.1"), false));
    CHECK(media_peer_allowed(host_ip("127.255.255.255"), false));
    CHECK(!media_peer_allowed(host_ip("100.63.255.255"), false));
    CHECK(!media_peer_allowed(host_ip("100.63.255.255"), true));
    CHECK(media_peer_allowed(host_ip("100.64.0.0"), false));
    CHECK(media_peer_allowed(host_ip("100.127.255.255"), false));
    CHECK(!media_peer_allowed(host_ip("100.128.0.0"), false));
    CHECK(!media_peer_allowed(host_ip("100.128.0.0"), true));
    CHECK(!media_peer_allowed(host_ip("10.0.0.1"), false));
    CHECK(media_peer_allowed(host_ip("10.0.0.1"), true));
    CHECK(!media_peer_allowed(host_ip("172.15.255.255"), false));
    CHECK(!media_peer_allowed(host_ip("172.15.255.255"), true));
    CHECK(!media_peer_allowed(host_ip("172.16.0.0"), false));
    CHECK(media_peer_allowed(host_ip("172.16.0.0"), true));
    CHECK(!media_peer_allowed(host_ip("172.31.255.255"), false));
    CHECK(media_peer_allowed(host_ip("172.31.255.255"), true));
    CHECK(!media_peer_allowed(host_ip("172.32.0.0"), false));
    CHECK(!media_peer_allowed(host_ip("172.32.0.0"), true));
    CHECK(!media_peer_allowed(host_ip("192.168.1.1"), false));
    CHECK(media_peer_allowed(host_ip("192.168.1.1"), true));
    CHECK(!media_peer_allowed(host_ip("8.8.8.8"), false));
    CHECK(!media_peer_allowed(host_ip("8.8.8.8"), true));
}

// No bearer and a declared 64 MiB body: the 401 must come from the header block alone. The
// client sends only the headers, so a server that waited for the body would stall until its
// 30 s timeout.
void test_unauthenticated_body_not_read(unsigned short port) {
    const auto started = std::chrono::steady_clock::now();
    expect_error(raw_exchange(port, "POST /v1/images/generations HTTP/1.1\r\nHost: localhost\r\n"
                                    "Content-Length: 67108864\r\n\r\n"),
                 401, "authentication_error", kAuthDenied);
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(5));
}

} // namespace

int main() {
    unsigned char digest[32];
    sha256_hash(digest, reinterpret_cast<const unsigned char *>(kToken), sizeof(kToken) - 1);
    const char * hex = "0123456789abcdef";
    std::string hex_digest;
    for (const unsigned char byte : digest) {
        hex_digest += hex[byte >> 4];
        hex_digest += hex[byte & 0x0f];
    }

    char directory_pattern[] = "/tmp/ps5lm-nexus-media-XXXXXX";
    const char * directory = mkdtemp(directory_pattern);
    CHECK(directory != nullptr);
    const std::string token_file = std::string(directory) + "/token";
    write_file(token_file, "sha256:" + hex_digest + "\n", 0600);

    MediaServer server;
    register_media_image_routes(server);
    register_media_audio_routes(server);
    register_media_voice_routes(server);
    CHECK(server.start(token_file, false, 0));
    const unsigned short port = server.port();
    CHECK(port != 0);
    CHECK(!server.route("GET", "/late", handle_speech));              // routes are frozen once started

    test_json_quote();
    test_auth(port, hex_digest);
    test_unauthenticated_body_not_read(port);
    test_peer_filter();
    test_framing(port);
    test_routes_and_health(port);
    test_image_requests(port);
    test_video_requests(port);
    test_speech_requests(port);
    test_transcription_requests(port);
    test_stream_error_events(port);
    test_gpu_coordination(port);
    test_gpu_mask();
    test_start_rules(token_file, directory);

    server.stop();
    CHECK(unlink(token_file.c_str()) == 0);
    CHECK(rmdir(directory) == 0);
    std::puts("media host tests passed");
    return 0;
}
