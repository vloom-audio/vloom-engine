// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "vloom/engine/dsp/Metronome.hpp"

#include <cmath>
#include <numbers>

namespace vloom {

namespace {
constexpr double kClickSeconds = 0.035;
constexpr double kAccentHz = 1500.0;
constexpr double kNormalHz = 1000.0;
} // namespace

void Metronome::prepare(double sampleRate) noexcept
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    length_ = static_cast<int>(kClickSeconds * sampleRate_);
    // Decay to about -60 dB (x0.001) over the click length.
    decay_ = static_cast<float>(std::pow(0.001, 1.0 / static_cast<double>(length_)));
    remaining_ = 0;
    amp_ = 0.0f;
}

void Metronome::trigger(bool accent) noexcept
{
    const double hz = accent ? kAccentHz : kNormalHz;
    // Start at the peak (cosine) so the click has a sharp, audible attack
    // and its first sample is already non-zero.
    phase_ = std::numbers::pi / 2.0;
    phaseInc_ = 2.0 * std::numbers::pi * hz / sampleRate_;
    amp_ = accent ? 1.0f : 0.7f;
    remaining_ = length_;
}

void Metronome::render(float* const* out, int numChannels, int startFrame, int numFrames) noexcept
{
    const int frames = remaining_ < numFrames ? remaining_ : numFrames;
    for (int i = 0; i < frames; ++i) {
        const float s = static_cast<float>(std::sin(phase_)) * amp_ * gain_;
        for (int ch = 0; ch < numChannels; ++ch)
            out[ch][startFrame + i] += s;
        phase_ += phaseInc_;
        amp_ *= decay_;
    }
    remaining_ -= frames;
}

} // namespace vloom
