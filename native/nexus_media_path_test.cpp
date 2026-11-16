// Run on Linux host: c++ -std=c++20 -Wall -Wextra -Werror native/nexus_media_path.cpp native/nexus_media_path_test.cpp -o /tmp/pathtest && /tmp/pathtest
// SPDX-License-Identifier: GPL-3.0-or-later
#include "nexus_media_path.hpp"

#include <cassert>
#include <cstdio>
#include <string>

using ps5lm::media_resolve;
using ps5lm::media_segment_ok;

int main() {
    // Single segments: model folder and file names.
    assert(!media_segment_ok(""));
    assert(!media_segment_ok("."));
    assert(!media_segment_ok(".."));
    assert(!media_segment_ok("a/../b"));
    assert(!media_segment_ok("/abs"));
    assert(!media_segment_ok("C:x"));
    assert(!media_segment_ok("a\\b"));
    assert(!media_segment_ok("a/"));
    assert(!media_segment_ok("a//b"));
    assert(!media_segment_ok("a..b"));
    assert(!media_segment_ok(std::string(129, 'a')));
    assert(media_segment_ok(std::string(128, 'a')));
    assert(!media_segment_ok(std::string("a\x01" "b")));
    assert(!media_segment_ok(std::string("a\x7f" "b")));
    assert(media_segment_ok("flux2-klein-4b"));
    assert(media_segment_ok("vae.safetensors"));

    // Resolution under root "/media". Valid single and folder/file paths.
    assert(media_resolve("/media", "flux", 1, false) == "/media/flux");
    assert(media_resolve("/media/", "a/b", 2, false) == "/media/a/b");
    assert(media_resolve("/media", "a/b", 1, false) == std::nullopt);  // too many segments

    // Malformed paths.
    assert(!media_resolve("/media", "", 1, false));
    assert(!media_resolve("/media", ".", 1, false));
    assert(!media_resolve("/media", "..", 1, true));
    assert(!media_resolve("/media", "/abs", 2, true));
    assert(!media_resolve("/media", "C:x", 1, true));
    assert(!media_resolve("/media", "a\\b", 1, true));
    assert(!media_resolve("/media", "a/", 2, true));
    assert(!media_resolve("/media", "a//b", 2, true));
    assert(!media_resolve("/media", std::string("a\x01" "b"), 1, true));

    // ".." needs allow_parent and must stay inside root.
    assert(!media_resolve("/media", "a/../b", 3, false));
    assert(media_resolve("/media", "a/../b", 3, true) == "/media/b");
    // Model-relative component paths: rel is "<model>/<component>", so ".." climbs from the model folder.
    assert(!media_resolve("/media", "../sibling/file", 3, true));      // climbs above root
    assert(media_resolve("/media", "flux/../sibling/file", 4, true) == "/media/sibling/file");
    assert(!media_resolve("/media", "flux/../sibling/file", 4, false));
    assert(!media_resolve("/media", "flux/../../x", 4, true));         // escapes root
    assert(!media_resolve("/media", "../../x", 3, true));              // escapes root
    assert(!media_resolve("/media", "flux/x/..", 3, true));            // ends in ".."

    std::puts("nexus_media_path_test: ok");
    return 0;
}
