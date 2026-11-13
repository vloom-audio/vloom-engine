// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "support/Allocations.hpp"

#include <vloom/engine/Engine.hpp>
#include <vloom/engine/render/OfflineRenderer.hpp>
#include <vloom/engine/sample/SampleBuffer.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <vector>

using Catch::Approx;

namespace {

constexpr double kRate = 48000.0;
constexpr float kCentre = 0.70710678f; // a mono sample in the centre: -3 dB per side

std::shared_ptr<const vloom::SampleBuffer> constant(int frames, float value = 1.0f)
{
    auto b = std::make_shared<vloom::SampleBuffer>(1, frames, kRate);
    for (int i = 0; i < frames; ++i)
        b->channel(0)[i] = value;
    return b;
}

void prepare(vloom::Engine& engine)
{
    engine.prepare(kRate, 256);
    engine.setMetronomeEnabled(false);
}

std::vector<float> render(vloom::Engine& engine, double seconds)
{
    vloom::OfflineRenderOptions options;
    options.seconds = seconds;
    options.blockSize = 256;
    return vloom::renderInterleaved(engine, options);
}

float left(const std::vector<float>& audio, std::size_t frame) { return audio[frame * 2]; }

/// Largest absolute sample from `frame` on.
float peakFrom(const std::vector<float>& audio, std::size_t frame)
{
    float peak = 0.0f;
    for (std::size_t i = frame * 2; i < audio.size(); ++i)
        peak = std::max(peak, std::abs(audio[i]));
    return peak;
}

} // namespace

TEST_CASE("A preview plays while the transport is stopped", "[preview]")
{
    vloom::Engine engine;
    prepare(engine);
    engine.previewSample(constant(4800));
    const auto audio = render(engine, 0.05);
    CHECK_FALSE(engine.isPlaying());
    CHECK(left(audio, 0) == Approx(kCentre).margin(1e-4));
    CHECK(left(audio, 1000) == Approx(kCentre).margin(1e-4));
    CHECK(engine.isPreviewPlaying());
}

TEST_CASE("A preview plays once, then the engine is silent again", "[preview]")
{
    vloom::Engine engine;
    prepare(engine);
    engine.previewSample(constant(1000));
    const auto audio = render(engine, 0.1);
    CHECK(left(audio, 999) == Approx(kCentre).margin(1e-4));
    CHECK(peakFrom(audio, 1000) == 0.0f);
    CHECK_FALSE(engine.isPreviewPlaying());
}

TEST_CASE("The master volume applies to the preview", "[preview]")
{
    vloom::Engine engine;
    prepare(engine);
    engine.setMasterGain(0.5f);
    render(engine, 0.01); // let the gain ramp settle
    engine.previewSample(constant(4800));
    const auto audio = render(engine, 0.05);
    CHECK(left(audio, 1000) == Approx(0.5f * kCentre).margin(1e-4));
}

TEST_CASE("Null and Stop silence the preview", "[preview]")
{
    vloom::Engine engine;
    prepare(engine);
    engine.previewSample(constant(48000));
    render(engine, 0.02);
    REQUIRE(engine.isPreviewPlaying());

    SECTION("null") { engine.previewSample(nullptr); }
    SECTION("Stop") { engine.stop(); }
    const auto audio = render(engine, 0.02);
    CHECK(peakFrom(audio, 256) == 0.0f); // faded out within the first block
    CHECK_FALSE(engine.isPreviewPlaying());
}

TEST_CASE("A replaced preview's sample is freed only after its fade", "[preview]")
{
    vloom::Engine engine;
    prepare(engine);
    auto first = constant(48000);
    const std::weak_ptr<const vloom::SampleBuffer> firstAlive = first;
    engine.previewSample(std::move(first));
    render(engine, 0.01);

    engine.previewSample(constant(48000, 0.5f));
    engine.collectGarbage();
    CHECK_FALSE(firstAlive.expired()); // the audio thread has not let go yet

    const auto audio = render(engine, 0.02); // the old voice fades out in the first block
    engine.collectGarbage();
    CHECK(firstAlive.expired());
    CHECK(left(audio, 500) == Approx(0.5f * kCentre).margin(1e-4)); // only the new one sounds
}

TEST_CASE("Previewing does not allocate on the audio thread", "[preview]")
{
    vloom::Engine engine;
    prepare(engine);
    std::vector<float> l(256), r(256);
    std::array<float*, 2> out{l.data(), r.data()};
    const auto a = constant(2000);
    const auto b = constant(9000, 0.25f);

    long long onAudioThread = 0;
    for (int block = 0; block < 300; ++block) {
        if (block % 7 == 0)
            engine.previewSample(block % 14 == 0 ? a : b); // control side: allocates, not counted
        if (block % 50 == 0)
            engine.collectGarbage();
        const vloom::test::AllocationCounter counter;
        engine.process(out.data(), 2, 256);
        onAudioThread += counter.count();
    }
    CHECK(onAudioThread == 0);
}
