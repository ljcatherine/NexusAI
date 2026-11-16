// WAV decoding and base64 helpers for the Nexus speech path.
// Derived from PSllama (commit d079717), GPL-3.0-or-later.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "nexus_wav.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

namespace ps5lm {
namespace {
uint32_t le32(const unsigned char * p) {
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 |
           static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24;
}
uint32_t le16(const unsigned char * p) {
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8;
}

float sample_value(const unsigned char * s, uint32_t format, uint32_t bits) {
    if (format == 3) {
        const uint32_t raw = le32(s);
        float value;
        std::memcpy(&value, &raw, sizeof(value));
        return value;
    }
    if (bits == 8) return (static_cast<float>(s[0]) - 128.0f) / 128.0f;
    if (bits == 16) return static_cast<float>(static_cast<int16_t>(le16(s))) / 32768.0f;
    if (bits == 24) {
        const uint32_t raw = static_cast<uint32_t>(s[0]) << 8 | static_cast<uint32_t>(s[1]) << 16 |
                             static_cast<uint32_t>(s[2]) << 24;
        return static_cast<float>(static_cast<int32_t>(raw) >> 8) / 8388608.0f;
    }
    return static_cast<float>(static_cast<int32_t>(le32(s))) / 2147483648.0f;
}

int base64_value(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
} // namespace

bool decode_wav(const std::string & bytes, int rate, std::vector<float> & samples, std::string & error) {
    if (rate <= 0) {
        error = "target sample rate must be positive";
        return false;
    }
    const auto * data = reinterpret_cast<const unsigned char *>(bytes.data());
    const std::size_t size = bytes.size();
    if (size < 12 || std::memcmp(data, "RIFF", 4) != 0 || std::memcmp(data + 8, "WAVE", 4) != 0) {
        error = "audio must be a RIFF/WAVE file";
        return false;
    }

    bool have_fmt = false;
    uint32_t format = 0, channels = 0, source_rate = 0, bits = 0;
    const unsigned char * pcm = nullptr;
    std::size_t pcm_size = 0;
    std::size_t at = 12;
    while (at + 8 <= size) {  // the pad byte can push `at` one past the end; this bound stops the walk
        const uint32_t chunk = le32(data + at + 4);
        const std::size_t body_at = at + 8;
        if (chunk > size - body_at) {
            error = "WAV chunk extends past the end of the file";
            return false;
        }
        const unsigned char * body = data + body_at;
        if (std::memcmp(data + at, "fmt ", 4) == 0) {
            if (chunk < 16) {
                error = "WAV fmt chunk is too small";
                return false;
            }
            format = le16(body);
            channels = le16(body + 2);
            source_rate = le32(body + 4);
            bits = le16(body + 14);
            if (format == 0xFFFE) {  // WAVE_FORMAT_EXTENSIBLE: real format is the sub-format GUID's first 2 bytes
                if (chunk < 26) {
                    error = "WAV extensible fmt chunk is too small";
                    return false;
                }
                format = le16(body + 24);
            }
            have_fmt = true;
        } else if (std::memcmp(data + at, "data", 4) == 0) {
            pcm = body;
            pcm_size = chunk;
        }
        // Chunks are word aligned; the pad byte may be missing at end of file, which the loop bound handles.
        at = body_at + chunk + (chunk & 1u);
    }

    const bool pcm_format = format == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32);
    const bool float_format = format == 3 && bits == 32;
    if (!have_fmt || !pcm || !(pcm_format || float_format) || channels < 1 || source_rate == 0) {
        error = "unsupported WAV encoding (PCM 8/16/24/32-bit or float 32 expected)";
        return false;
    }

    const std::size_t bytes_per_sample = bits / 8;
    const std::size_t frame_bytes = bytes_per_sample * channels;
    const std::size_t frames = pcm_size / frame_bytes;
    std::vector<float> mono(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        float sum = 0.0f;
        for (uint32_t c = 0; c < channels; ++c)
            sum += sample_value(pcm + i * frame_bytes + c * bytes_per_sample, format, bits);
        mono[i] = sum / static_cast<float>(channels);
    }

    if (source_rate == static_cast<uint32_t>(rate)) {
        samples = std::move(mono);
        return true;
    }
    // shortcut: linear resampling, enough for speech recognition input; upgrade to a band-limited resampler if quality matters.
    const double step = static_cast<double>(source_rate) / static_cast<double>(rate);
    const std::size_t out = static_cast<std::size_t>(static_cast<double>(frames) / step);
    samples.assign(out, 0.0f);
    for (std::size_t i = 0; i < out; ++i) {
        const double position = static_cast<double>(i) * step;
        std::size_t j = static_cast<std::size_t>(position);
        if (j >= frames) j = frames - 1;  // rounding at the very end; out > 0 implies frames > 0
        if (j + 1 < frames) {
            const float frac = static_cast<float>(position - static_cast<double>(j));
            samples[i] = mono[j] * (1.0f - frac) + mono[j + 1] * frac;
        } else {
            samples[i] = mono[j];
        }
    }
    return true;
}

std::string base64_encode(const std::string & bytes) {
    static constexpr char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const auto * data = reinterpret_cast<const unsigned char *>(bytes.data());
    const std::size_t size = bytes.size();
    std::string out;
    out.reserve((size + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < size; i += 3) {
        const uint32_t v = static_cast<uint32_t>(data[i]) << 16 | static_cast<uint32_t>(data[i + 1]) << 8 | data[i + 2];
        out += table[(v >> 18) & 63];
        out += table[(v >> 12) & 63];
        out += table[(v >> 6) & 63];
        out += table[v & 63];
    }
    const std::size_t rest = size - i;
    if (rest > 0) {
        const uint32_t v = static_cast<uint32_t>(data[i]) << 16 |
                           (rest == 2 ? static_cast<uint32_t>(data[i + 1]) << 8 : 0u);
        out += table[(v >> 18) & 63];
        out += table[(v >> 12) & 63];
        out += rest == 2 ? table[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

bool base64_decode(const std::string & text, std::string & bytes, std::string & error) {
    bytes.clear();
    if (text.size() % 4 != 0) {
        error = "base64 length must be a multiple of 4";
        return false;
    }
    std::size_t padding = 0;
    if (!text.empty() && text.back() == '=') padding = text[text.size() - 2] == '=' ? 2 : 1;

    bytes.reserve(text.size() / 4 * 3);
    uint32_t buffer = 0;
    unsigned bits = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c == '=') {
            if (i < text.size() - padding) {
                error = "base64 padding may only appear at the end";
                return false;
            }
            continue;
        }
        const int value = base64_value(c);
        if (value < 0) {
            error = "invalid base64 character";
            return false;
        }
        buffer = buffer << 6 | static_cast<uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            bytes += static_cast<char>((buffer >> bits) & 0xFFu);
        }
    }
    return true;
}
} // namespace ps5lm
