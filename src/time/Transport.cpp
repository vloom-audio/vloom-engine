// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "vloom/engine/time/Transport.hpp"

#include <algorithm>
#include <cmath>

namespace vloom {

void Transport::prepare(double sampleRate) noexcept
{
    reanchor();
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
}

void Transport::reanchor() noexcept
{
    anchorTicks_ = positionTicks();
    framesSinceAnchor_ = 0;
}

void Transport::locate(double ticks) noexcept
{
    if (!std::isfinite(ticks))
        return;
    anchorTicks_ = std::clamp(ticks, 0.0, kMaxTicks);
    framesSinceAnchor_ = 0;
}

void Transport::setTempo(double bpm) noexcept
{
    if (!std::isfinite(bpm))
        return; // std::clamp would keep NaN
    reanchor();
    bpm_ = std::clamp(bpm, kMinBpm, kMaxBpm);
}

double Transport::ticksPerSample() const noexcept
{
    return (bpm_ / 60.0) * static_cast<double>(kPpq) / sampleRate_;
}

double Transport::positionTicks() const noexcept
{
    return anchorTicks_ + static_cast<double>(framesSinceAnchor_) * ticksPerSample();
}

double Transport::framesUntil(double ticks) const noexcept
{
    const double fromAnchor
        = (ticks - anchorTicks_) * sampleRate_ * 60.0 / (bpm_ * static_cast<double>(kPpq));
    return fromAnchor - static_cast<double>(framesSinceAnchor_);
}

void Transport::advance(int numFrames) noexcept
{
    if (playing_ && numFrames > 0)
        framesSinceAnchor_ += numFrames;
}

} // namespace vloom
