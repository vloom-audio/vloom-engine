// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace vloom {

/// Decoded audio in memory: `channels` planar float channels of `frames`
/// samples each, at `sampleRate`.
///
/// Built on a control or background thread; once handed to the audio thread
/// (as a shared_ptr<const SampleBuffer>) it is only read.
class SampleBuffer {
public:
    SampleBuffer() = default;
    /// Silent buffer. Negative sizes count as 0.
    SampleBuffer(int channels, std::int64_t frames, double sampleRate);

    [[nodiscard]] int channels() const noexcept { return channels_; }
    [[nodiscard]] std::int64_t frames() const noexcept { return frames_; }
    [[nodiscard]] double sampleRate() const noexcept { return sampleRate_; }
    [[nodiscard]] bool empty() const noexcept { return frames_ == 0 || channels_ == 0; }
    [[nodiscard]] double seconds() const noexcept
    {
        return sampleRate_ > 0.0 ? static_cast<double>(frames_) / sampleRate_ : 0.0;
    }

    /// Samples of `channel` (0-based); `frames()` long. No bounds check.
    [[nodiscard]] const float* channel(int channel) const noexcept
    {
        return data_.data() + static_cast<std::size_t>(channel) * static_cast<std::size_t>(frames_);
    }
    [[nodiscard]] float* channel(int channel) noexcept
    {
        return data_.data() + static_cast<std::size_t>(channel) * static_cast<std::size_t>(frames_);
    }

private:
    int channels_ = 0;
    std::int64_t frames_ = 0;
    double sampleRate_ = 0.0;
    std::vector<float> data_;
};

} // namespace vloom
