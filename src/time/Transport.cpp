// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "vloom/engine/time/Transport.hpp"

#include <algorithm>

namespace vloom {

namespace {
constexpr double kMinBpm = 10.0;
constexpr double kMaxBpm = 999.0;
} // namespace

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
    anchorTicks_ = ticks < 0.0 ? 0.0 : ticks;
    framesSinceAnchor_ = 0;
}

void Transport::setTempo(double bpm) noexcept
{
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
