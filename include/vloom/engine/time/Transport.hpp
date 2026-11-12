// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

#include "vloom/engine/time/MusicalTime.hpp"

#include <cstdint>

namespace vloom {

/// Song-position clock. Owned and advanced by the audio thread only.
///
/// Position = anchor ticks + whole frames since the anchor × ticks per frame.
/// The anchor is reset on tempo change and locate, so rounding error never
/// accumulates block after block.
class Transport {
public:
    /// Furthest position locate() accepts (about 16 000 hours at 120 BPM);
    /// keeps tick and frame arithmetic far from overflow.
    static constexpr double kMaxTicks = 1e11;

    void prepare(double sampleRate) noexcept;

    void play() noexcept { playing_ = true; }
    void stop() noexcept { playing_ = false; }
    void locate(double ticks) noexcept;
    void setTempo(double bpm) noexcept;
    void setBeatsPerBar(int beats) noexcept { beatsPerBar_ = beats < 1 ? 1 : beats; }

    [[nodiscard]] bool isPlaying() const noexcept { return playing_; }
    [[nodiscard]] double positionTicks() const noexcept;
    [[nodiscard]] double tempo() const noexcept { return bpm_; }
    [[nodiscard]] int beatsPerBar() const noexcept { return beatsPerBar_; }
    [[nodiscard]] double sampleRate() const noexcept { return sampleRate_; }
    [[nodiscard]] double ticksPerSample() const noexcept;

    /// Frames from the current position until `ticks` is reached (may be negative).
    [[nodiscard]] double framesUntil(double ticks) const noexcept;

    /// Moves the playhead forward by numFrames if playing.
    void advance(int numFrames) noexcept;

private:
    void reanchor() noexcept;

    double sampleRate_ = 48000.0;
    double bpm_ = 120.0;
    double anchorTicks_ = 0.0;
    std::int64_t framesSinceAnchor_ = 0;
    int beatsPerBar_ = 4;
    bool playing_ = false;
};

} // namespace vloom
