// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

#include <cstdint>

namespace vloom {

/// Musical time resolution: ticks per quarter note.
inline constexpr int kPpq = 960;

/// Musical position split into bar / beat / tick. Bar and beat are 1-based.
struct BarBeatTick {
    std::int64_t bar = 1;
    int beat = 1;
    int tick = 0;
};

/// Converts an absolute tick position to bar / beat / tick.
BarBeatTick toBarBeatTick(double ticks, int beatsPerBar) noexcept;

} // namespace vloom
