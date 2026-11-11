// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "vloom/engine/time/MusicalTime.hpp"

#include <cmath>

namespace vloom {

BarBeatTick toBarBeatTick(double ticks, int beatsPerBar) noexcept
{
    if (ticks < 0.0)
        ticks = 0.0;
    if (beatsPerBar < 1)
        beatsPerBar = 1;

    const auto whole = static_cast<std::int64_t>(std::floor(ticks));
    const auto totalBeats = whole / kPpq;

    BarBeatTick out;
    out.bar = totalBeats / beatsPerBar + 1;
    out.beat = static_cast<int>(totalBeats % beatsPerBar) + 1;
    out.tick = static_cast<int>(whole % kPpq);
    return out;
}

} // namespace vloom
