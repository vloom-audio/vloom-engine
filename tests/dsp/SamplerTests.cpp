// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "support/Allocations.hpp"

#include <vloom/engine/dsp/Sampler.hpp>
#include <vloom/engine/sample/SampleBuffer.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <vector>

using Catch::Approx;

namespace {

/// Two planar output channels of `frames` samples, zeroed.
struct Output {
    std::vector<float> left, right;
    std::array<float*, 2> ptrs{};
    explicit Output(int frames)
        : left(static_cast<std::size_t>(frames), 0.0f)
        , right(static_cast<std::size_t>(frames), 0.0f)
        , ptrs{left.data(), right.data()}
    {
    }
};

/// Mono sample whose frame i holds `start + i * step`.
vloom::SampleBuffer ramp(int frames, float start = 1.0f, float step = 0.0f)
{
    vloom::SampleBuffer b(1, frames, 48000.0);
    for (int i = 0; i < frames; ++i)
        b.channel(0)[i] = start + step * static_cast<float>(i);
    return b;
}

constexpr float kCentre = 0.70710678f; // cos(pi / 4): -3 dB

} // namespace

TEST_CASE("A triggered mono sample plays from its first frame, -3 dB per side in the centre", "[dsp]")
{
    const auto sample = ramp(4, 0.1f, 0.1f); // 0.1 0.2 0.3 0.4
    vloom::Sampler sampler;
    sampler.trigger(&sample);
    Output out(6);
    sampler.render(out.ptrs.data(), 2, 0, 6);

    for (int i = 0; i < 4; ++i) {
        CHECK(out.left[static_cast<std::size_t>(i)] == Approx(0.1f * static_cast<float>(i + 1) * kCentre));
        CHECK(out.right[static_cast<std::size_t>(i)] == Approx(0.1f * static_cast<float>(i + 1) * kCentre));
    }
    CHECK(out.left[4] == 0.0f); // the sample has ended
    CHECK(sampler.activeVoices() == 0);
}

TEST_CASE("Rendering in segments starts a sample on the exact frame", "[dsp]")
{
    const auto sample = ramp(3);
    vloom::Sampler sampler;
    Output out(8);
    sampler.render(out.ptrs.data(), 2, 0, 5); // nothing yet
    sampler.trigger(&sample);
    sampler.render(out.ptrs.data(), 2, 5, 3);
    CHECK(out.left[4] == 0.0f);
    CHECK(out.left[5] == Approx(kCentre));
    CHECK(out.left[7] == Approx(kCentre));
}

TEST_CASE("Pan and gain", "[dsp]")
{
    const auto mono = ramp(1);
    vloom::Sampler sampler;
    {
        Output out(1);
        sampler.trigger(&mono, 0.5f, -1.0f); // hard left
        sampler.render(out.ptrs.data(), 2, 0, 1);
        CHECK(out.left[0] == Approx(0.5f));
        CHECK(out.right[0] == Approx(0.0f).margin(1e-7));
    }

    vloom::SampleBuffer stereo(2, 1, 48000.0);
    stereo.channel(0)[0] = 1.0f;
    stereo.channel(1)[0] = 1.0f;
    {
        Output out(1);
        sampler.trigger(&stereo); // centre: stereo keeps full level
        sampler.render(out.ptrs.data(), 2, 0, 1);
        CHECK(out.left[0] == 1.0f);
        CHECK(out.right[0] == 1.0f);
    }
    {
        Output out(1);
        sampler.trigger(&stereo, 1.0f, 0.5f); // balance right: left at half
        sampler.render(out.ptrs.data(), 2, 0, 1);
        CHECK(out.left[0] == Approx(0.5f));
        CHECK(out.right[0] == 1.0f);
    }
}

TEST_CASE("Overlapping triggers add up", "[dsp]")
{
    const auto sample = ramp(10);
    vloom::Sampler sampler;
    Output out(4);
    sampler.trigger(&sample);
    sampler.render(out.ptrs.data(), 2, 0, 2);
    sampler.trigger(&sample);
    sampler.render(out.ptrs.data(), 2, 2, 2);
    CHECK(out.left[1] == Approx(kCentre));
    CHECK(out.left[2] == Approx(2.0f * kCentre));
    CHECK(sampler.activeVoices() == 2);
}

TEST_CASE("Beyond the voice limit the oldest voice fades out instead of clicking", "[dsp]")
{
    const auto sample = ramp(100000);
    vloom::Sampler sampler;
    for (int i = 0; i < vloom::Sampler::kDefaultVoices; ++i)
        sampler.trigger(&sample);
    Output warm(1);
    sampler.render(warm.ptrs.data(), 2, 0, 1);
    const float full = warm.left[0];

    sampler.trigger(&sample); // one too many
    Output out(vloom::Sampler::kFadeFrames + 2);
    sampler.render(out.ptrs.data(), 2, 0, static_cast<int>(out.left.size()));

    // The new voice starts at once; the stolen one ramps down linearly
    // instead of stopping dead, then the level is back to kDefaultVoices voices.
    constexpr int kFade = vloom::Sampler::kFadeFrames;
    for (int i = 0; i <= kFade + 1; ++i) {
        const float stolen
            = i < kFade ? kCentre * static_cast<float>(kFade - i) / static_cast<float>(kFade + 1) : 0.0f;
        INFO("frame " << i);
        CHECK(out.left[static_cast<std::size_t>(i)] == Approx(full + stolen).epsilon(1e-5));
    }
    CHECK(sampler.activeVoices() == vloom::Sampler::kDefaultVoices);
}

TEST_CASE("stopAll fades every voice to silence", "[dsp]")
{
    const auto sample = ramp(100000);
    vloom::Sampler sampler;
    sampler.trigger(&sample);
    sampler.trigger(&sample);
    sampler.stopAll();
    Output out(vloom::Sampler::kFadeFrames + 10);
    sampler.render(out.ptrs.data(), 2, 0, static_cast<int>(out.left.size()));
    CHECK(out.left[0] < 2.0f * kCentre);
    CHECK(out.left[0] > 1.9f * kCentre);
    CHECK(out.left[vloom::Sampler::kFadeFrames] == 0.0f);
    CHECK(sampler.activeVoices() == 0);
    CHECK_FALSE(sampler.uses(&sample));
}

TEST_CASE("uses() reports samples still being played", "[dsp]")
{
    const auto a = ramp(10);
    const auto b = ramp(10);
    vloom::Sampler sampler;
    sampler.trigger(&a);
    CHECK(sampler.uses(&a));
    CHECK_FALSE(sampler.uses(&b));
    CHECK_FALSE(sampler.uses(nullptr));
    Output out(10);
    sampler.render(out.ptrs.data(), 2, 0, 10);
    CHECK_FALSE(sampler.uses(&a));
}

TEST_CASE("Bad input is ignored", "[dsp]")
{
    const vloom::SampleBuffer empty;
    const auto sample = ramp(4);
    vloom::Sampler sampler;
    sampler.trigger(nullptr);
    sampler.trigger(&empty);
    sampler.trigger(&sample, std::nanf(""));
    sampler.trigger(&sample, 1.0f, std::nanf(""));
    CHECK(sampler.activeVoices() == 0);
}

TEST_CASE("Mono output mixes both sides", "[dsp]")
{
    const auto sample = ramp(1);
    vloom::Sampler sampler;
    sampler.trigger(&sample);
    std::vector<float> mono(1, 0.0f);
    float* ptr = mono.data();
    sampler.render(&ptr, 1, 0, 1);
    CHECK(mono[0] == Approx(kCentre));
}

TEST_CASE("The sampler does not allocate while playing", "[dsp]")
{
    const auto sample = ramp(1000);
    vloom::Sampler sampler;
    Output out(256);
    const vloom::test::AllocationCounter counter;
    for (int i = 0; i < 40; ++i) {
        sampler.trigger(&sample, 0.8f, 0.3f);
        sampler.render(out.ptrs.data(), 2, 0, 256);
    }
    sampler.stopAll();
    sampler.render(out.ptrs.data(), 2, 0, 256);
    CHECK(counter.count() == 0);
}

TEST_CASE("Polyphony is set per sampler and clamped", "[dsp]")
{
    CHECK(vloom::Sampler().maxVoices() == vloom::Sampler::kDefaultVoices);
    CHECK(vloom::Sampler(4).maxVoices() == 4);
    CHECK(vloom::Sampler(0).maxVoices() == 1);
    CHECK(vloom::Sampler(1000).maxVoices() == vloom::Sampler::kMaxPolyphony);
}

TEST_CASE("With one voice each trigger cuts the previous note with a fade", "[dsp]")
{
    const auto sample = ramp(100000);
    vloom::Sampler sampler(1);
    Output out(vloom::Sampler::kFadeFrames + 4);
    sampler.trigger(&sample);
    sampler.render(out.ptrs.data(), 2, 0, 2);
    sampler.trigger(&sample); // closes the first note
    sampler.render(out.ptrs.data(), 2, 2, static_cast<int>(out.left.size()) - 2);

    CHECK(out.left[1] == Approx(kCentre)); // one note
    CHECK(out.left[2] > kCentre); // the new note plus the fading old one
    CHECK(out.left.back() == Approx(kCentre)); // one note again
    CHECK(sampler.activeVoices() == 1);
}

TEST_CASE("A sample can start part-way, fading in", "[dsp]")
{
    const auto sample = ramp(1000, 0.0f, 0.001f); // frame i holds i / 1000
    vloom::Sampler sampler;
    sampler.trigger(&sample, 1.0f, 0.0f, 400);
    Output out(700);
    sampler.render(out.ptrs.data(), 2, 0, 700);

    // The first frame is frame 400, nearly silent; after the fade it is exact.
    CHECK(out.left[0] == Approx(0.4f * kCentre / (vloom::Sampler::kFadeFrames + 1)).margin(1e-5));
    CHECK(out.left[100] == Approx(0.5f * kCentre).margin(1e-5));
    CHECK(out.left[599] == Approx(0.999f * kCentre).margin(1e-5));
    CHECK(out.left[600] == 0.0f); // ended: 600 frames were left
    CHECK(sampler.activeVoices() == 0);
}

TEST_CASE("A start at or past the end, or before the beginning, is ignored", "[dsp]")
{
    const auto sample = ramp(100);
    vloom::Sampler sampler;
    sampler.trigger(&sample, 1.0f, 0.0f, 100);
    sampler.trigger(&sample, 1.0f, 0.0f, -1);
    CHECK(sampler.activeVoices() == 0);
}
