// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

namespace vloom {

/// A short decaying sine "click". Real-time safe: no allocation after prepare().
class Metronome {
public:
    void prepare(double sampleRate) noexcept;

    /// Starts a new click. Accented clicks (bar starts) are higher and louder.
    void trigger(bool accent) noexcept;

    /// Adds the click signal to `numChannels` buffers in [startFrame, startFrame + numFrames).
    void render(float* const* out, int numChannels, int startFrame, int numFrames) noexcept;

    void setGain(float gain) noexcept { gain_ = gain; }

private:
    double sampleRate_ = 48000.0;
    double phase_ = 0.0;
    double phaseInc_ = 0.0;
    float amp_ = 0.0f;
    float decay_ = 0.0f;
    float gain_ = 0.5f;
    int remaining_ = 0;
    int length_ = 0;
};

} // namespace vloom
