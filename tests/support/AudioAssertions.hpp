// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
//
// Helpers for inspecting rendered audio in tests.
#pragma once

#include <cmath>
#include <cstddef>
#include <vector>

namespace vloom::test {

/// Frame indices where channel 0 becomes non-silent after at least `gap`
/// silent frames. Good for finding click / note onsets.
inline std::vector<std::size_t> findOnsets(
    const std::vector<float>& interleaved, int channels, std::size_t gap)
{
    std::vector<std::size_t> result;
    const auto stride = static_cast<std::size_t>(channels);
    const std::size_t frames = interleaved.size() / stride;
    std::size_t silent = gap;
    for (std::size_t i = 0; i < frames; ++i) {
        const bool loud = std::fabs(interleaved[i * stride]) > 1e-6f;
        if (loud && silent >= gap)
            result.push_back(i);
        silent = loud ? 0 : silent + 1;
    }
    return result;
}

inline bool isSilent(const std::vector<float>& samples)
{
    for (float s : samples)
        if (s != 0.0f)
            return false;
    return true;
}

} // namespace vloom::test
