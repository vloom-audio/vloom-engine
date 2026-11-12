// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include <vloom/engine/sample/Resampler.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <numbers>

namespace {

vloom::SampleBuffer sine(double frequency, double rate, std::int64_t frames, int channels = 1)
{
    vloom::SampleBuffer b(channels, frames, rate);
    for (int ch = 0; ch < channels; ++ch)
        for (std::int64_t i = 0; i < frames; ++i)
            b.channel(ch)[i] = static_cast<float>(
                std::sin(2.0 * std::numbers::pi * frequency * static_cast<double>(i) / rate));
    return b;
}

/// Largest difference from the ideal sine, away from the edges.
double maxSineError(const vloom::SampleBuffer& b, double frequency, std::int64_t margin)
{
    double worst = 0.0;
    for (std::int64_t i = margin; i < b.frames() - margin; ++i) {
        const double ideal
            = std::sin(2.0 * std::numbers::pi * frequency * static_cast<double>(i) / b.sampleRate());
        worst = std::max(worst, std::abs(static_cast<double>(b.channel(0)[i]) - ideal));
    }
    return worst;
}

double rms(const vloom::SampleBuffer& b, std::int64_t margin)
{
    double sum = 0.0;
    std::int64_t count = 0;
    for (std::int64_t i = margin; i < b.frames() - margin; ++i, ++count)
        sum += static_cast<double>(b.channel(0)[i]) * static_cast<double>(b.channel(0)[i]);
    return count > 0 ? std::sqrt(sum / static_cast<double>(count)) : 0.0;
}

} // namespace

TEST_CASE("Same rate gives an exact copy", "[sample]")
{
    const auto in = sine(440.0, 48000.0, 1000, 2);
    const auto out = vloom::resample(in, 48000.0);
    REQUIRE(out.frames() == in.frames());
    REQUIRE(out.channels() == 2);
    for (std::int64_t i = 0; i < in.frames(); ++i)
        CHECK(out.channel(1)[i] == in.channel(1)[i]);
}

TEST_CASE("Length scales with the rate ratio", "[sample]")
{
    CHECK(vloom::resample(vloom::SampleBuffer(1, 44100, 44100.0), 48000.0).frames() == 48000);
    CHECK(vloom::resample(vloom::SampleBuffer(2, 48000, 48000.0), 22050.0).frames() == 22050);
    CHECK(vloom::resample(vloom::SampleBuffer(1, 10, 48000.0), 0.0).empty());
    CHECK(vloom::resample(vloom::SampleBuffer(1, 10, 0.0), 48000.0).empty());
}

TEST_CASE("44.1 to 48 kHz keeps a sine wave within -80 dB", "[sample]")
{
    const auto out = vloom::resample(sine(1000.0, 44100.0, 44100), 48000.0);
    CHECK(out.sampleRate() == 48000.0);
    CHECK(maxSineError(out, 1000.0, 200) < 1e-4);
}

TEST_CASE("Upsampling 48 to 96 kHz keeps a high sine wave", "[sample]")
{
    const auto out = vloom::resample(sine(15000.0, 48000.0, 24000), 96000.0);
    CHECK(maxSineError(out, 15000.0, 400) < 1e-3);
}

TEST_CASE("Downsampling removes what the new rate cannot hold", "[sample]")
{
    // 15 kHz is above the 11.025 kHz Nyquist frequency of 22.05 kHz.
    const auto out = vloom::resample(sine(15000.0, 48000.0, 48000), 22050.0);
    CHECK(rms(out, 300) < 0.001); // more than 57 dB down from the input's 0.707
    // ...while a tone inside the new band survives.
    const auto kept = vloom::resample(sine(1000.0, 48000.0, 48000), 22050.0);
    CHECK(maxSineError(kept, 1000.0, 300) < 1e-3);
}

TEST_CASE("Constant signals keep their level", "[sample]")
{
    vloom::SampleBuffer dc(1, 4000, 44100.0);
    for (std::int64_t i = 0; i < dc.frames(); ++i)
        dc.channel(0)[i] = 0.5f;
    const auto out = vloom::resample(dc, 48000.0);
    for (std::int64_t i = 500; i < out.frames() - 500; ++i)
        REQUIRE(std::abs(out.channel(0)[i] - 0.5f) < 1e-4f);
}
