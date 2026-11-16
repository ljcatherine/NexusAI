// Run on Linux host: c++ -std=c++20 -Wall -Wextra -Werror native/nexus_wav.cpp native/nexus_wav_test.cpp -o /tmp/wavtest
// Derived from PSllama (commit d079717), GPL-3.0-or-later.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "nexus_wav.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static std::string le(uint64_t value, std::size_t count) {
    std::string bytes;
    for (std::size_t i = 0; i < count; ++i) bytes.push_back(static_cast<char>((value >> (8 * i)) & 255));
    return bytes;
}

static std::string s16(int value) { return le(static_cast<uint64_t>(value), 2); }

static bool close_to(float a, float b) { return std::fabs(a - b) < 1e-6f; }

// Minimal canonical RIFF/WAVE: 16-byte fmt chunk, one data chunk, pad byte for odd sizes.
static std::string wav(uint32_t format, uint32_t channels, uint32_t rate, uint32_t bits, const std::string & pcm) {
    const uint32_t block = channels * bits / 8;
    std::string body = "WAVE";
    body += "fmt ";
    body += le(16, 4);
    body += le(format, 2) + le(channels, 2) + le(rate, 4) + le(rate * block, 4) + le(block, 2) + le(bits, 2);
    body += "data";
    body += le(pcm.size(), 4);
    body += pcm;
    if (pcm.size() % 2 != 0) body.push_back('\0');
    return "RIFF" + le(body.size(), 4) + body;
}

static void test_valid_mono_16bit() {
    const std::string file = wav(1, 1, 16000, 16, s16(0) + s16(16384) + s16(-16384) + s16(-32768));
    std::vector<float> samples;
    std::string error;
    assert(ps5lm::decode_wav(file, 16000, samples, error));
    assert(samples.size() == 4);
    assert(close_to(samples[0], 0.0f) && close_to(samples[1], 0.5f));
    assert(close_to(samples[2], -0.5f) && close_to(samples[3], -1.0f));
}

static void test_stereo_downmix() {
    // Frames: (L 0.5, R 0.0) and (L -0.5, R -0.5) average to 0.25 and -0.5.
    const std::string file = wav(1, 2, 16000, 16, s16(16384) + s16(0) + s16(-16384) + s16(-16384));
    std::vector<float> samples;
    std::string error;
    assert(ps5lm::decode_wav(file, 16000, samples, error));
    assert(samples.size() == 2);
    assert(close_to(samples[0], 0.25f) && close_to(samples[1], -0.5f));
}

static void test_other_encodings() {
    std::vector<float> samples;
    std::string error;
    // 8-bit unsigned: 192 is +0.5.
    assert(ps5lm::decode_wav(wav(1, 1, 16000, 8, std::string(1, static_cast<char>(192))), 16000, samples, error));
    assert(samples.size() == 1 && close_to(samples[0], 0.5f));
    // 32-bit float: 0.25 stored as IEEE bits.
    const float quarter = 0.25f;
    uint32_t raw;
    std::memcpy(&raw, &quarter, sizeof(raw));
    assert(ps5lm::decode_wav(wav(3, 1, 16000, 32, le(raw, 4)), 16000, samples, error));
    assert(samples.size() == 1 && close_to(samples[0], 0.25f));
}

static void test_resample_linear() {
    // 8 kHz to 16 kHz doubles the sample count and interpolates between neighbours.
    const std::string file = wav(1, 1, 8000, 16, s16(0) + s16(16384) + s16(0) + s16(-16384));
    std::vector<float> samples;
    std::string error;
    assert(ps5lm::decode_wav(file, 16000, samples, error));
    assert(samples.size() == 8);
    assert(close_to(samples[1], 0.25f) && close_to(samples[7], -0.5f));
}

static void test_rejects_truncated_and_oversized() {
    const std::string valid = wav(1, 1, 16000, 16, s16(1) + s16(2) + s16(3) + s16(4));
    std::vector<float> samples;
    std::string error;
    // Shorter than the RIFF header, inside the fmt chunk, and one byte short of the data chunk.
    for (std::size_t length : {std::size_t(0), std::size_t(5), std::size_t(11), std::size_t(20), valid.size() - 1})
        assert(!ps5lm::decode_wav(valid.substr(0, length), 16000, samples, error));
    // A data chunk that claims 4 GiB must be refused, not read.
    std::string huge = valid;
    huge.replace(valid.find("data") + 4, 4, le(0xFFFFFFFFu, 4));
    assert(!ps5lm::decode_wav(huge, 16000, samples, error));
    assert(!error.empty());
}

static void test_rejects_bad_header_and_format() {
    const std::string valid = wav(1, 1, 16000, 16, s16(1) + s16(2));
    std::vector<float> samples;
    std::string error;
    std::string bad_riff = valid;
    bad_riff.replace(0, 4, "RIFX");
    assert(!ps5lm::decode_wav(bad_riff, 16000, samples, error));
    std::string bad_wave = valid;
    bad_wave.replace(8, 4, "AVI ");
    assert(!ps5lm::decode_wav(bad_wave, 16000, samples, error));
    // Non-PCM (format 2, ADPCM) and a bit depth that is not a whole byte count.
    assert(!ps5lm::decode_wav(wav(2, 1, 16000, 16, s16(1) + s16(2)), 16000, samples, error));
    assert(!ps5lm::decode_wav(wav(1, 1, 16000, 12, s16(1) + s16(2)), 16000, samples, error));
    // Target rate must be positive.
    assert(!ps5lm::decode_wav(valid, 0, samples, error));
}

static void test_base64_round_trip() {
    // RFC 4648 vectors.
    const char * vectors[][2] = {{"", ""},          {"f", "Zg=="},     {"fo", "Zm8="},
                                 {"foo", "Zm9v"},   {"foob", "Zm9vYg=="}, {"fooba", "Zm9vYmE="},
                                 {"foobar", "Zm9vYmFy"}};
    for (const auto & vector : vectors) {
        const std::string plain = vector[0];
        assert(ps5lm::base64_encode(plain) == vector[1]);
        std::string decoded, error;
        assert(ps5lm::base64_decode(vector[1], decoded, error));
        assert(decoded == plain);
    }
    std::string binary;
    for (int i = 0; i < 256; ++i) binary.push_back(static_cast<char>(i));
    std::string decoded, error;
    assert(ps5lm::base64_decode(ps5lm::base64_encode(binary), decoded, error));
    assert(decoded == binary);
    const std::string file = wav(1, 1, 16000, 16, s16(1) + s16(-2));
    assert(ps5lm::base64_decode(ps5lm::base64_encode(file), decoded, error));
    assert(decoded == file);
}

static void test_base64_rejects_invalid() {
    const char * bad[] = {"Zm9v!", "Zg=",   "Zm=v", "Z===", "Zm9*",
                          "Zm9 ",  "Zm9v\n", "Zm9vYg", "Zm9vYg="};
    for (const char * text : bad) {
        std::string decoded, error;
        assert(!ps5lm::base64_decode(text, decoded, error));
        assert(!error.empty());
    }
}

int main() {
    test_valid_mono_16bit();
    test_stereo_downmix();
    test_other_encodings();
    test_resample_linear();
    test_rejects_truncated_and_oversized();
    test_rejects_bad_header_and_format();
    test_base64_round_trip();
    test_base64_rejects_invalid();
    puts("Nexus native WAV and base64 tests passed");
    return 0;
}
