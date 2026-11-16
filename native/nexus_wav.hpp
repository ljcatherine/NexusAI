// WAV decoding and base64 helpers for the Nexus speech path. Pure C++20: no
// engine, platform or PS5 SDK includes, so the host tests build on any compiler.
// Derived from PSllama (commit d079717), GPL-3.0-or-later.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

namespace ps5lm {
// Decodes a RIFF/WAVE file (PCM 8/16/24/32-bit or float 32, any channel
// count and sample rate) to mono float samples at `rate` Hz. Returns false
// with `error` set for any malformed or unsupported input; never reads past `bytes`.
bool decode_wav(const std::string & bytes, int rate, std::vector<float> & samples, std::string & error);

// Standard alphabet with '=' padding.
std::string base64_encode(const std::string & bytes);
// Strict: length must be a multiple of 4, only alphabet characters, and '='
// only as the final one or two characters. Whitespace is rejected.
bool base64_decode(const std::string & text, std::string & bytes, std::string & error);
} // namespace ps5lm
