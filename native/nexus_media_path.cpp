// Derived from PSllama (commit d079717), GPL-3.0-or-later.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "nexus_media_path.hpp"

#include <cstddef>
#include <vector>

namespace ps5lm {
namespace {
constexpr std::size_t kMaxSegment = 128;
} // namespace

bool media_segment_ok(std::string_view segment) {
    if (segment.empty() || segment.size() > kMaxSegment || segment == "." || segment == ".." ||
        segment.find("..") != std::string_view::npos)
        return false;
    for (const unsigned char c : segment) {
        if (c < 0x20 || c == 0x7f || c == '/' || c == '\\' || c == ':') return false;
    }
    return true;
}

std::optional<std::string> media_resolve(std::string_view root, std::string_view rel, int max_segments,
                                         bool allow_parent) {
    // Whole-path rules: no drive colon, backslash or control byte.
    for (const unsigned char c : rel) {
        if (c < 0x20 || c == '\\' || c == ':') return std::nullopt;
    }
    // Walk the segments; kept holds the normalized path, ".." pops one entry.
    std::vector<std::string_view> kept;
    bool last_is_parent = false;
    int count = 0;
    std::size_t start = 0;
    while (true) {
        const std::size_t slash = rel.find('/', start);
        const std::string_view segment =
            rel.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
        if (segment.empty() || segment == "." || ++count > max_segments) return std::nullopt;
        last_is_parent = segment == "..";
        if (last_is_parent) {
            // Climbing above root is rejected even when parents are allowed.
            if (!allow_parent || kept.empty()) return std::nullopt;
            kept.pop_back();
        } else {
            kept.push_back(segment);
        }
        if (slash == std::string_view::npos) break;
        start = slash + 1;
    }
    if (last_is_parent) return std::nullopt;  // a path ending in ".." names a folder above the target

    std::string path(root);
    while (!path.empty() && path.back() == '/') path.pop_back();
    for (const std::string_view segment : kept) {
        path += '/';
        path += segment;
    }
    return path;
}
} // namespace ps5lm
