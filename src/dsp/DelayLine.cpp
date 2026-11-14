// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "vloom/engine/dsp/DelayLine.hpp"

#include <algorithm>
#include <bit>

namespace vloom {

DelayLine::DelayLine(int maxDelay, int numChannels) { resize(maxDelay, numChannels); }

void DelayLine::resize(int maxDelay, int numChannels)
{
    maxDelay_ = std::max(maxDelay, 0);
    channels_ = std::clamp(numChannels, 0, kMaxChannels);
    size_ = maxDelay_ > 0 ? std::bit_ceil(static_cast<std::size_t>(maxDelay_) + 1) : 0;
    buffer_.assign(size_ * static_cast<std::size_t>(channels_), 0.0f);
    write_ = 0;
    pending_ = 0;
}

void DelayLine::process(float* const* channels, int numChannels, int numFrames, int delay) noexcept
{
    delay = std::clamp(delay, 0, maxDelay_);
    if (delay == 0 || numFrames <= 0 || size_ == 0) {
        pending_ = 0;
        return;
    }
    numChannels = std::min(numChannels, channels_);
    const std::size_t mask = size_ - 1;
    const auto lag = static_cast<std::size_t>(delay);
    int lastSound = -1; // last frame of the input that was not silent
    for (int ch = 0; ch < numChannels; ++ch) {
        float* ring = buffer_.data() + static_cast<std::size_t>(ch) * size_;
        float* io = channels[ch];
        std::size_t w = write_;
        for (int i = 0; i < numFrames; ++i) {
            const float in = io[i];
            ring[w] = in;
            io[i] = ring[(w - lag) & mask];
            if (in != 0.0f)
                lastSound = std::max(lastSound, i);
            w = (w + 1) & mask;
        }
    }
    write_ = (write_ + static_cast<std::size_t>(numFrames)) & mask;
    pending_ = std::max(pending_ - numFrames, 0);
    if (lastSound >= 0)
        pending_ = std::max(pending_, delay - (numFrames - 1 - lastSound));
}

void DelayLine::clear() noexcept
{
    std::fill(buffer_.begin(), buffer_.end(), 0.0f);
    write_ = 0;
    pending_ = 0;
}

} // namespace vloom
