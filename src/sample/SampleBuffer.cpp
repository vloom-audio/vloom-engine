// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "vloom/engine/sample/SampleBuffer.hpp"

#include <algorithm>

namespace vloom {

SampleBuffer::SampleBuffer(int channels, std::int64_t frames, double sampleRate)
    : channels_(std::max(channels, 0))
    , frames_(std::max<std::int64_t>(frames, 0))
    , sampleRate_(sampleRate)
    , data_(static_cast<std::size_t>(channels_) * static_cast<std::size_t>(frames_), 0.0f)
{
}

} // namespace vloom
