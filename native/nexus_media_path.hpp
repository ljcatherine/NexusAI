// Shared path rules for the media folder under the media root. Used by the
// image (sd) and audio services so the traversal checks cannot drift apart.
// Derived from PSllama (commit d079717), GPL-3.0-or-later.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace ps5lm {
// One model folder or file name: 1-128 bytes, not "." or "..", no "..",
// no '/', '\\', ':', or control bytes (including DEL).
bool media_segment_ok(std::string_view segment);

// Lexically resolves rel under root and returns root + "/" + normalized rel.
// rel is split on '/'; every segment must be non-empty and not "." (so no
// absolute or trailing-slash paths), with no '\\', ':' or control bytes.
// ".." is accepted only when allow_parent is set, and only while the path
// stays inside root. At most max_segments segments are allowed. A rel whose
// last segment is ".." is rejected. Only lexical: the caller checks existence.
std::optional<std::string> media_resolve(std::string_view root, std::string_view rel, int max_segments,
                                         bool allow_parent);
} // namespace ps5lm
