// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "support/Allocations.hpp"
#include "support/AudioAssertions.hpp"

#include <vloom/engine/Engine.hpp>
#include <vloom/engine/render/OfflineRenderer.hpp>
#include <vloom/engine/sequencer/SequencerState.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using Catch::Approx;

namespace {

constexpr double kRate = 48000.0;
// 120 BPM: a 16th-note step is 0.125 s = 6000 frames; 16 steps = 96000 frames.
constexpr std::size_t kStepFrames = 6000;
constexpr float kCentre = 0.70710678f;

std::shared_ptr<const vloom::SampleBuffer> constant(int frames, float value = 1.0f)
{
    auto b = std::make_shared<vloom::SampleBuffer>(1, frames, kRate);
    for (int i = 0; i < frames; ++i)
        b->channel(0)[i] = value;
    return b;
}

/// "x...x..." -> steps (x = on).
std::vector<std::uint8_t> steps(const std::string& pattern)
{
    std::vector<std::uint8_t> out;
    for (const char c : pattern)
        out.push_back(c == 'x' ? 1 : 0);
    return out;
}

std::unique_ptr<vloom::SequencerState> rack(std::vector<vloom::ChannelState> channels)
{
    auto state = std::make_unique<vloom::SequencerState>();
    state->channels = std::move(channels);
    return state;
}

vloom::ChannelState channel(std::shared_ptr<const vloom::SampleBuffer> sample,
    std::shared_ptr<vloom::ChannelVoices> voices, const std::string& pattern)
{
    vloom::ChannelState c;
    c.sample = std::move(sample);
    c.voices = std::move(voices);
    c.steps = steps(pattern);
    return c;
}

/// An engine at 120 BPM with the metronome off.
void prepare(vloom::Engine& engine, int blockSize = 256)
{
    engine.prepare(kRate, blockSize);
    engine.setTempo(120.0);
    engine.setMetronomeEnabled(false);
}

std::vector<float> render(vloom::Engine& engine, double seconds, int blockSize = 256)
{
    vloom::OfflineRenderOptions options;
    options.seconds = seconds;
    options.blockSize = blockSize;
    return vloom::renderInterleaved(engine, options);
}

float left(const std::vector<float>& interleaved, std::size_t frame) { return interleaved[frame * 2]; }

} // namespace

TEST_CASE("Steps fire on their exact frame and the pattern loops, whatever the block size", "[sequencer]")
{
    const int blockSize = GENERATE(64, 256, 333, 4096);
    vloom::Engine engine;
    prepare(engine, blockSize);
    const auto hit = constant(1); // a one-frame impulse
    engine.setSequencerState(
        rack({channel(hit, std::make_shared<vloom::ChannelVoices>(), "x...x...x...x...")}));
    engine.play();

    const auto audio = render(engine, 2.5, blockSize);
    const std::vector<std::size_t> expected{
        0, 4 * kStepFrames, 8 * kStepFrames, 12 * kStepFrames, 16 * kStepFrames};
    CHECK(vloom::test::findOnsets(audio, 2, 1000) == expected);
    CHECK(left(audio, 0) == Approx(kCentre));
}

TEST_CASE("Every channel plays its own steps", "[sequencer]")
{
    vloom::Engine engine;
    prepare(engine);
    const auto hit = constant(1);
    auto pannedLeft = channel(hit, std::make_shared<vloom::ChannelVoices>(), "x...............");
    pannedLeft.pan = -1.0f;
    auto quiet = channel(hit, std::make_shared<vloom::ChannelVoices>(), "..x.............");
    quiet.gain = 0.5f;
    engine.setSequencerState(rack({pannedLeft, quiet}));
    engine.play();

    const auto audio = render(engine, 0.5);
    // Channel pan is a balance on the channel: hard left keeps the left side.
    CHECK(left(audio, 0) == Approx(kCentre));
    CHECK(audio[1] == Approx(0.0f).margin(1e-7)); // right channel of frame 0
    CHECK(left(audio, 2 * kStepFrames) == Approx(0.5f * kCentre));
}

TEST_CASE("Muted channels and steps past a short pattern stay silent", "[sequencer]")
{
    vloom::Engine engine;
    prepare(engine);
    auto muted = channel(constant(1), std::make_shared<vloom::ChannelVoices>(), "xxxxxxxxxxxxxxxx");
    muted.muted = true;
    auto shortSteps = channel(constant(1), std::make_shared<vloom::ChannelVoices>(), "....");
    engine.setSequencerState(rack({muted, shortSteps}));
    engine.play();
    CHECK(vloom::test::isSilent(render(engine, 2.0)));
}

TEST_CASE("Steps and metronome clicks share the same clock", "[sequencer]")
{
    vloom::Engine engine;
    prepare(engine);
    engine.setMetronomeEnabled(true);
    engine.setSequencerState(
        rack({channel(constant(1, 0.0f), std::make_shared<vloom::ChannelVoices>(), "x")}));
    engine.play();
    const auto audio = render(engine, 1.0);
    // Clicks still on every beat (24000 frames at 120 BPM).
    const std::vector<std::size_t> beats{0, 24000};
    CHECK(vloom::test::findOnsets(audio, 2, 1000) == beats);
}

TEST_CASE("Editing the rack does not cut a ringing note", "[sequencer]")
{
    vloom::Engine engine;
    prepare(engine);
    const auto voices = std::make_shared<vloom::ChannelVoices>();
    const auto pad = constant(48000); // one second
    engine.setSequencerState(rack({channel(pad, voices, "x...............")}));
    engine.play();
    render(engine, 0.1);

    // Same channel (same voices), steps cleared.
    engine.setSequencerState(rack({channel(pad, voices, "................")}));
    const auto after = render(engine, 0.5);
    CHECK(left(after, 0) == Approx(kCentre));
    CHECK(left(after, 20000) == Approx(kCentre)); // still ringing 0.5 s in
}

TEST_CASE("Removing a channel fades its notes out", "[sequencer]")
{
    vloom::Engine engine;
    prepare(engine);
    engine.setSequencerState(rack({channel(constant(48000), std::make_shared<vloom::ChannelVoices>(), "x")}));
    engine.play();
    render(engine, 0.1);

    engine.setSequencerState(rack({}));
    const auto after = render(engine, 0.1);
    CHECK(left(after, 0) > 0.0f); // fading, not cut
    CHECK(left(after, 0) < kCentre);
    CHECK(left(after, vloom::Sampler::kFadeFrames) == 0.0f);
}

TEST_CASE("Stopping the transport fades notes out", "[sequencer]")
{
    vloom::Engine engine;
    prepare(engine);
    engine.setSequencerState(rack({channel(constant(48000), std::make_shared<vloom::ChannelVoices>(), "x")}));
    engine.play();
    render(engine, 0.1);
    engine.stop();
    const auto after = render(engine, 0.1);
    CHECK(left(after, 0) > 0.0f);
    CHECK(left(after, vloom::Sampler::kFadeFrames) == 0.0f);
    CHECK(vloom::test::isSilent(std::vector<float>(after.begin() + 2 * 256, after.end())));
}

TEST_CASE("A replaced sample is freed once its last note ends, not before", "[sequencer]")
{
    vloom::Engine engine;
    prepare(engine);
    const auto voices = std::make_shared<vloom::ChannelVoices>();
    std::weak_ptr<const vloom::SampleBuffer> watch;
    {
        auto old = constant(12000); // 0.25 s
        watch = old;
        engine.setSequencerState(rack({channel(std::move(old), voices, "x")}));
    }
    engine.play();
    render(engine, 0.1);

    engine.setSequencerState(rack({channel(constant(10), voices, "................")}));
    render(engine, 0.05);
    engine.collectGarbage();
    CHECK_FALSE(watch.expired()); // its note is still playing

    render(engine, 0.2); // the old note has ended
    engine.collectGarbage();
    CHECK(watch.expired());
}

TEST_CASE("Playing the rack does not allocate", "[sequencer]")
{
    vloom::Engine engine;
    prepare(engine, 256);
    std::vector<vloom::ChannelState> channels;
    for (int i = 0; i < 8; ++i)
        channels.push_back(
            channel(constant(3000), std::make_shared<vloom::ChannelVoices>(), "x.x.x.x.xxxxx.x."));
    engine.setSequencerState(rack(std::move(channels)));
    engine.play();

    std::vector<float> left(256), right(256);
    std::array<float*, 2> out{left.data(), right.data()};
    engine.process(out.data(), 2, 256); // picks up the state

    const vloom::test::AllocationCounter counter;
    for (int block = 0; block < 400; ++block) // about two seconds
        engine.process(out.data(), 2, 256);
    CHECK(counter.count() == 0);
}

TEST_CASE("A sample two channels share stays alive while either still plays it", "[sequencer]")
{
    // Review finding: an old state was released when only its own channel's
    // sampler had stopped, while another channel's sampler still played the
    // sample through a raw pointer.
    vloom::Engine engine;
    prepare(engine);
    const auto x = std::make_shared<vloom::ChannelVoices>();
    const auto y = std::make_shared<vloom::ChannelVoices>();
    std::weak_ptr<const vloom::SampleBuffer> watch;
    {
        auto shared = constant(24000); // 0.5 s
        watch = shared;
        engine.setSequencerState(rack({channel(shared, x, "x"), channel(shared, y, "")})); // X plays it
        engine.play();
        render(engine, 0.05);
        engine.setSequencerState(rack({channel(constant(10), x, ""), channel(shared, y, "")})); // X moves on
    }
    render(engine, 0.05);
    engine.collectGarbage();
    engine.setSequencerState(rack({channel(constant(10), x, ""), channel(constant(10), y, "")})); // Y too
    render(engine, 0.05);
    engine.collectGarbage();
    CHECK_FALSE(watch.expired()); // X's note of it still sounds

    render(engine, 0.5);
    engine.collectGarbage();
    CHECK(watch.expired());
}

TEST_CASE("Steps between samples fire on the next sample, also across a block boundary", "[sequencer]")
{
    // Review finding: an event in the last fractional frame of a block was
    // deferred, then skipped as "already played". At 44.1 kHz and 120 BPM a
    // step is 5512.5 frames: step k fires on frame ceil(k * 5512.5).
    const int blockSize = GENERATE(64, 333, 512, 5513);
    vloom::Engine engine;
    engine.prepare(44100.0, blockSize);
    engine.setTempo(120.0);
    engine.setMetronomeEnabled(false);
    engine.setSequencerState(
        rack({channel(constant(1), std::make_shared<vloom::ChannelVoices>(), "xxxxxxxxxxxxxxxx")}));
    engine.play();
    const auto audio = render(engine, 3.0, blockSize); // 132300 frames: steps 0..23

    std::vector<std::size_t> expected;
    for (int k = 0; k < 24; ++k)
        expected.push_back(static_cast<std::size_t>(std::ceil(k * 5512.5)));
    INFO("block size " << blockSize);
    CHECK(vloom::test::findOnsets(audio, 2, 1000) == expected);
}

TEST_CASE("Nothing non-finite reaches the output", "[sequencer]")
{
    // Defence in depth behind the WAV reader's own checks.
    auto bad = std::make_shared<vloom::SampleBuffer>(1, 4, kRate);
    bad->channel(0)[0] = std::numeric_limits<float>::infinity();
    bad->channel(0)[1] = -std::numeric_limits<float>::infinity();
    bad->channel(0)[2] = std::numeric_limits<float>::quiet_NaN();
    vloom::Engine engine;
    prepare(engine);
    engine.setSequencerState(rack({channel(bad, std::make_shared<vloom::ChannelVoices>(), "x"),
        channel(bad, std::make_shared<vloom::ChannelVoices>(), "x")})); // +inf and -inf mixed: NaN
    engine.play();
    for (const float v : render(engine, 0.05))
        REQUIRE(std::isfinite(v));
}
