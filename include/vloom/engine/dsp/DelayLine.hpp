// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

#include <cstddef>
#include <vector>

namespace vloom {

/// A delay of whole frames for up to two channels, in place. Delay
/// compensation (ADR 0007) and a bypassed plugin's dry path use it.
///
/// Control thread: the constructor and resize() allocate. Audio thread:
/// process() and clear() do not allocate, lock or wait.
class DelayLine {
public:
    static constexpr int kMaxChannels = 2;

    DelayLine() = default;
    /// Room for delays of up to `maxDelay` frames (negative: 0).
    explicit DelayLine(int maxDelay, int numChannels = kMaxChannels);

    /// Reallocates for `maxDelay` frames, silent. Not while process() runs.
    void resize(int maxDelay, int numChannels = kMaxChannels);
    [[nodiscard]] int maxDelay() const noexcept { return maxDelay_; }

    /// Delays `numFrames` of `channels` (at most numChannels given to the
    /// constructor) in place by `delay` frames, clamped to 0 .. maxDelay().
    /// A delay of 0 passes the audio through untouched (and keeps it, so a
    /// later longer delay continues from it).
    void process(float* const* channels, int numChannels, int numFrames, int delay) noexcept;
    /// Forgets what it holds.
    void clear() noexcept;
    /// True when nothing but silence is left inside: processing silence
    /// through it would give silence.
    [[nodiscard]] bool idle() const noexcept { return pending_ == 0; }

private:
    std::vector<float> buffer_; ///< channel-major, `size_` frames each
    std::size_t size_ = 0; ///< a power of two, > maxDelay_ (0 when empty)
    std::size_t write_ = 0;
    int channels_ = 0;
    int maxDelay_ = 0;
    int pending_ = 0; ///< frames until the last non-silent frame written comes out
};

} // namespace vloom
