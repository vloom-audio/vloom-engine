// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "support/Allocations.hpp"
#include "support/AudioAssertions.hpp"

#include <vloom/engine/Engine.hpp>
#include <vloom/engine/mixer/MixerState.hpp>
#include <vloom/engine/render/OfflineRenderer.hpp>
#include <vloom/engine/sequencer/SequencerState.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <memory>
#include <vector>

using Catch::Approx;

namespace {

constexpr double kRate = 48000.0;
constexpr float kCentre = 0.70710678f; // a mono sample in the centre: -3 dB per side

std::shared_ptr<const vloom::SampleBuffer> constant(int frames)
{
    auto b = std::make_shared<vloom::SampleBuffer>(1, frames, kRate);
    for (int i = 0; i < frames; ++i)
        b->channel(0)[i] = 1.0f;
    return b;
}

vloom::ChannelState channel(std::shared_ptr<const vloom::SampleBuffer> sample,
    std::shared_ptr<vloom::ChannelVoices> voices, int insert, float gain = 1.0f, float pan = 0.0f)
{
    vloom::ChannelState c;
    c.sample = std::move(sample);
    c.voices = std::move(voices);
    c.steps = {1};
    c.insert = insert;
    c.gain = gain;
    c.pan = pan;
    return c;
}

std::unique_ptr<vloom::SequencerState> rack(std::vector<vloom::ChannelState> channels)
{
    auto state = std::make_unique<vloom::SequencerState>();
    state->channels = std::move(channels);
    return state;
}

std::unique_ptr<const vloom::MixerState> mixer(std::vector<vloom::InsertState> inserts)
{
    auto state = std::make_unique<vloom::MixerState>();
    state->inserts = std::move(inserts);
    return state;
}

void prepare(vloom::Engine& engine)
{
    engine.prepare(kRate, 256);
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

float left(const std::vector<float>& audio, std::size_t frame) { return audio[frame * 2]; }
float right(const std::vector<float>& audio, std::size_t frame) { return audio[frame * 2 + 1]; }

} // namespace

TEST_CASE("A channel plays through the insert it is routed to", "[mixer]")
{
    vloom::Engine engine;
    prepare(engine);
    engine.setSequencerState(rack({channel(constant(48000), std::make_shared<vloom::ChannelVoices>(), 3)}));
    engine.setMixerState(mixer({{}, {}, {0.5f, 0.0f}})); // insert 3 at half volume
    engine.play();
    const auto audio = render(engine, 0.1);
    CHECK(left(audio, 0) == Approx(0.5f * kCentre));
    CHECK(right(audio, 100) == Approx(0.5f * kCentre));
    CHECK(engine.takeInsertPeak(3, 0) == Approx(0.5f * kCentre));
    CHECK(engine.takeInsertPeak(2, 0) == 0.0f);
    CHECK(engine.takeInsertPeak(3, 0) == 0.0f); // taken
}

TEST_CASE("Inserts are panned and silenced by their gain", "[mixer]")
{
    vloom::Engine engine;
    prepare(engine);
    engine.setSequencerState(rack({channel(constant(48000), std::make_shared<vloom::ChannelVoices>(), 1),
        channel(constant(48000), std::make_shared<vloom::ChannelVoices>(), 2)}));
    engine.setMixerState(mixer({{1.0f, 1.0f}, {0.0f, 0.0f}})); // 1: hard right, 2: muted
    engine.play();
    const auto audio = render(engine, 0.05);
    CHECK(left(audio, 10) == Approx(0.0f).margin(1e-7));
    CHECK(right(audio, 10) == Approx(kCentre));
}

TEST_CASE("Channels with no insert, or one out of range, play into the master", "[mixer]")
{
    vloom::Engine engine;
    prepare(engine);
    engine.setSequencerState(rack({channel(constant(48000), std::make_shared<vloom::ChannelVoices>(), 0),
        channel(constant(48000), std::make_shared<vloom::ChannelVoices>(), 999)}));
    engine.play();
    const auto audio = render(engine, 0.05);
    CHECK(left(audio, 10) == Approx(2.0f * kCentre));
}

TEST_CASE("Channel volume and pan change ringing notes, ramped", "[mixer]")
{
    vloom::Engine engine;
    prepare(engine);
    const auto voices = std::make_shared<vloom::ChannelVoices>();
    const auto pad = constant(96000); // two seconds
    engine.setSequencerState(rack({channel(pad, voices, 0)}));
    engine.play();
    render(engine, 0.1);

    vloom::ChannelState quieter = channel(pad, voices, 0, 0.5f, -1.0f); // half volume, hard left
    quieter.steps = {0}; // no new note: the ringing one must change
    engine.setSequencerState(rack({quieter}));
    const auto audio = render(engine, 0.1);
    // A one-block ramp, no jump: the first frame is still near full level...
    CHECK(left(audio, 0) > 0.9f * kCentre);
    // ...and after the block the new setting holds.
    CHECK(left(audio, 1000) == Approx(0.5f * kCentre));
    CHECK(right(audio, 1000) == Approx(0.0f).margin(1e-7));
}

TEST_CASE("Blocks longer than the bus size are rendered in parts", "[mixer]")
{
    vloom::Engine engine;
    engine.prepare(kRate, 20000);
    engine.setTempo(120.0);
    engine.setMetronomeEnabled(false);
    auto hits = channel(constant(1), std::make_shared<vloom::ChannelVoices>(), 2);
    hits.steps = {1, 1, 1, 1}; // every 6000 frames
    engine.setSequencerState(rack({hits}));
    engine.play();
    const auto audio = render(engine, 0.5, 20000);
    const std::vector<std::size_t> expected{0, 6000, 12000, 18000};
    CHECK(vloom::test::findOnsets(audio, 2, 1000) == expected);
}

TEST_CASE("Mixing through inserts does not allocate", "[mixer]")
{
    vloom::Engine engine;
    prepare(engine);
    std::vector<vloom::ChannelState> channels;
    for (int i = 0; i < 8; ++i)
        channels.push_back(channel(constant(3000), std::make_shared<vloom::ChannelVoices>(), i % 4));
    engine.setSequencerState(rack(std::move(channels)));
    engine.setMixerState(mixer({{0.8f, -0.2f}, {1.0f, 0.5f}, {0.3f, 0.0f}}));
    engine.play();
    std::vector<float> l(256), r(256);
    std::array<float*, 2> out{l.data(), r.data()};
    engine.process(out.data(), 2, 256);

    const vloom::test::AllocationCounter counter;
    for (int block = 0; block < 400; ++block)
        engine.process(out.data(), 2, 256);
    CHECK(counter.count() == 0);
}
