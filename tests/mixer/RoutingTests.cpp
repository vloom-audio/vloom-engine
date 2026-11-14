// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "support/Allocations.hpp"

#include <vloom/engine/Engine.hpp>
#include <vloom/engine/dsp/DelayLine.hpp>
#include <vloom/engine/mixer/MixerGraph.hpp>
#include <vloom/engine/mixer/MixerState.hpp>
#include <vloom/engine/plugin/PluginInstance.hpp>
#include <vloom/engine/render/OfflineRenderer.hpp>
#include <vloom/engine/sequencer/SequencerState.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <vector>

using Catch::Approx;

namespace {

constexpr double kRate = 48000.0;
constexpr float kCentre = 0.70710678f; // a mono sample in the centre: -3 dB per side

/// Delays its input by `latency` frames and says so, like a look-ahead
/// limiter or a linear-phase EQ.
class LatentProcessor final : public vloom::AudioProcessor {
public:
    explicit LatentProcessor(int latency)
        : latency_(latency)
    {
    }
    [[nodiscard]] vloom::PluginInfo info() const override
    {
        return {"test.latent", "Latent", "Tests", "1", {}};
    }
    bool activate(double /*sampleRate*/, int /*maxBlockFrames*/) override
    {
        line_.resize(latency_);
        return true;
    }
    [[nodiscard]] int latencyFrames() const override { return latency_; }
    void process(const vloom::ProcessContext& context) noexcept override
    {
        line_.process(context.channels, context.numChannels, context.numFrames, latency_);
    }
    void reset() noexcept override { line_.clear(); }

private:
    int latency_;
    vloom::DelayLine line_;
};

std::shared_ptr<vloom::PluginInstance> latent(int latency)
{
    auto plugin = std::make_shared<vloom::PluginInstance>(std::make_unique<LatentProcessor>(latency));
    plugin->activate(kRate, vloom::kMaxBlockFrames);
    return plugin;
}

/// A one-frame click.
std::shared_ptr<const vloom::SampleBuffer> click()
{
    auto b = std::make_shared<vloom::SampleBuffer>(1, 1, kRate);
    b->channel(0)[0] = 1.0f;
    return b;
}

std::shared_ptr<const vloom::SampleBuffer> constant(int frames)
{
    auto b = std::make_shared<vloom::SampleBuffer>(1, frames, kRate);
    for (int i = 0; i < frames; ++i)
        b->channel(0)[i] = 1.0f;
    return b;
}

/// Channels playing `sample` once at the start (step 0), one per insert in `inserts`.
std::unique_ptr<vloom::SequencerState> rack(
    std::shared_ptr<const vloom::SampleBuffer> sample, std::vector<int> inserts)
{
    auto state = std::make_unique<vloom::SequencerState>();
    state->stepCount = 64; // 4 bars: plays once in a short render
    for (const int insert : inserts) {
        vloom::ChannelState c;
        c.sample = sample;
        c.voices = std::make_shared<vloom::ChannelVoices>();
        c.steps = {1};
        c.insert = insert;
        state->channels.push_back(std::move(c));
    }
    return state;
}

std::unique_ptr<vloom::MixerState> mixer(std::size_t inserts)
{
    auto state = std::make_unique<vloom::MixerState>();
    state->inserts.resize(inserts);
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

/// Frames where the left channel is not silent.
std::vector<std::size_t> sounding(const std::vector<float>& audio)
{
    std::vector<std::size_t> frames;
    for (std::size_t i = 0; i * 2 < audio.size(); ++i)
        if (audio[i * 2] != 0.0f)
            frames.push_back(i);
    return frames;
}

} // namespace

TEST_CASE("A delay line delays by whole frames and knows when it is empty", "[mixer][dsp]")
{
    vloom::DelayLine line(8, 1);
    std::vector<float> block(6, 0.0f);
    block[0] = 1.0f;
    float* channels[1] = {block.data()};
    line.process(channels, 1, 6, 3);
    CHECK(block == std::vector<float>{0, 0, 0, 1, 0, 0});
    CHECK(line.idle());
    block.assign(6, 0.0f);
    block[5] = 0.5f;
    line.process(channels, 1, 6, 3);
    CHECK_FALSE(line.idle()); // the 0.5 is still inside
    block.assign(6, 0.0f);
    line.process(channels, 1, 6, 3);
    CHECK(block == std::vector<float>{0, 0, 0.5f, 0, 0, 0});
    CHECK(line.idle());

    block.assign(6, 2.0f);
    line.process(channels, 1, 6, 0); // no delay: untouched
    CHECK(block == std::vector<float>(6, 2.0f));

    vloom::DelayLine shortLine(4, 1);
    block.assign(6, 0.0f);
    block[0] = 1.0f;
    shortLine.process(channels, 1, 6, 100); // clamped to its length
    CHECK(block == std::vector<float>{0, 0, 0, 0, 1, 0});
}

TEST_CASE("The mixer graph orders inserts before what they play into and drops loops", "[mixer][graph]")
{
    auto state = mixer(4);
    // Checked in insert order: 1 -> 3 and 2 -> 1 are kept, so 3 -> 2 would
    // close the loop 3 -> 2 -> 1 -> 3 and is left out.
    state->inserts[0].output = 3;
    state->inserts[1].output = 1;
    state->inserts[1].sends.push_back({0, 0.5f, false}); // 2 -> master too
    state->inserts[2].output = 2;
    state->inserts[3].sends.push_back({4, 1.0f, false}); // to itself: left out
    state->inserts[3].sends.push_back({200, 1.0f, false}); // no such insert: left out
    const auto graph = vloom::buildMixerGraph(std::move(state), nullptr);

    const auto at = [&](int insert) {
        return std::find(graph->order.begin(), graph->order.end(), insert) - graph->order.begin();
    };
    REQUIRE(graph->order.size() == static_cast<std::size_t>(vloom::kMaxInserts));
    CHECK(at(2) < at(1));
    CHECK(at(1) < at(3));
    CHECK(graph->routeCount[2] == 2); // into 1, and the send to the master
    CHECK(graph->routeCount[3] == 0); // its output closed the loop: it plays nowhere
    CHECK(graph->routeCount[4] == 1); // its output; both sends dropped
    CHECK(graph->routeCount[5] == 1); // past the state: unity into the master
    CHECK(graph->latency == 0);
}

TEST_CASE("The mixer graph lines up paths with latency", "[mixer][graph][pdc]")
{
    auto state = mixer(3);
    state->inserts[0].effects.push_back(latent(100)); // 1 -> master, 100 late
    state->inserts[1].output = 3; // 2 -> 3 -> master
    state->inserts[2].effects.push_back(latent(30));
    const auto graph = vloom::buildMixerGraph(std::move(state), nullptr);

    CHECK(graph->latency == 100);
    CHECK(graph->inputDelay[0] == 100); // channels straight into the master
    const auto routeOf
        = [&](int insert) { return graph->routes[static_cast<std::size_t>(graph->firstRoute[insert])]; };
    CHECK(routeOf(1).delay == 0);
    CHECK(routeOf(2).delay == 0); // into 3, which itself has latency 30
    CHECK(routeOf(3).delay == 70); // 30 late already
    CHECK(routeOf(4).delay == 100);
}

TEST_CASE("Inserts play into other inserts and send copies", "[mixer][routing]")
{
    vloom::Engine engine;
    prepare(engine);
    engine.setSequencerState(rack(constant(4800), {1}));
    auto state = mixer(3);
    state->inserts[0].output = 2; // 1 -> 2 -> master
    state->inserts[0].gain = 0.5f;
    state->inserts[0].sends.push_back({3, 0.5f, false}); // post-fader: 0.25
    state->inserts[1].gain = 2.0f; // 1.0 through insert 2
    state->inserts[2].gain = 1.0f; // 0.25 through insert 3
    engine.setMixerState(std::move(state));
    engine.play();
    const auto audio = render(engine, 0.05);
    CHECK(left(audio, 100) == Approx(kCentre * (1.0f + 0.25f)).margin(1e-4));
}

TEST_CASE("A pre-fader send ignores the insert's volume", "[mixer][routing]")
{
    vloom::Engine engine;
    prepare(engine);
    engine.setSequencerState(rack(constant(4800), {1}));
    auto state = mixer(2);
    state->inserts[0].gain = 0.0f; // muted
    state->inserts[0].sends.push_back({2, 0.5f, true});
    engine.setMixerState(std::move(state));
    engine.play();
    const auto audio = render(engine, 0.05);
    CHECK(left(audio, 100) == Approx(kCentre * 0.5f).margin(1e-4));
}

TEST_CASE("An insert routed nowhere is silent but still metered", "[mixer][routing]")
{
    vloom::Engine engine;
    prepare(engine);
    engine.setSequencerState(rack(constant(4800), {1}));
    auto state = mixer(1);
    state->inserts[0].output = -1;
    engine.setMixerState(std::move(state));
    engine.play();
    const auto audio = render(engine, 0.05);
    CHECK(sounding(audio).empty());
    CHECK(engine.takeInsertPeak(1, 0) == Approx(kCentre).margin(1e-4));
}

TEST_CASE("Delay compensation lines up a latent insert with the others", "[mixer][pdc]")
{
    constexpr int kLatency = 300;
    vloom::Engine engine;
    prepare(engine);
    engine.setSequencerState(rack(click(), {1, 2, 0})); // a latent insert, a plain one, the master
    auto state = mixer(2);
    state->inserts[0].effects.push_back(latent(kLatency));
    engine.setMixerState(std::move(state));
    engine.play();
    const auto audio = render(engine, 0.05);

    // All three clicks land together, kLatency frames late.
    CHECK(sounding(audio) == std::vector<std::size_t>{kLatency});
    CHECK(left(audio, kLatency) == Approx(3.0f * kCentre).margin(1e-4));
    CHECK(engine.latencyFrames() == kLatency);
}

TEST_CASE("Delay compensation follows a send through a latent insert", "[mixer][pdc]")
{
    constexpr int kLatency = 200;
    vloom::Engine engine;
    prepare(engine);
    engine.setSequencerState(rack(click(), {1}));
    auto state = mixer(2);
    state->inserts[0].sends.push_back({2, 1.0f, false}); // dry to the master, wet through 2
    state->inserts[1].effects.push_back(latent(kLatency));
    engine.setMixerState(std::move(state));
    engine.play();
    const auto audio = render(engine, 0.05);
    CHECK(sounding(audio) == std::vector<std::size_t>{kLatency});
    CHECK(left(audio, kLatency) == Approx(2.0f * kCentre).margin(1e-4));
}

TEST_CASE("The metronome is delayed as much as the music", "[mixer][pdc]")
{
    constexpr int kLatency = 500;
    const auto firstClick = [](int latency) {
        vloom::Engine engine;
        prepare(engine);
        engine.setMetronomeEnabled(true);
        auto state = mixer(1);
        if (latency > 0)
            state->inserts[0].effects.push_back(latent(latency));
        engine.setMixerState(std::move(state));
        engine.play();
        const auto frames = sounding(render(engine, 0.05));
        return frames.empty() ? std::size_t{0} : frames.front();
    };
    const std::size_t plain = firstClick(0);
    CHECK(plain <= 1); // the click's sine starts at 0
    CHECK(firstClick(kLatency) == plain + kLatency);
}

TEST_CASE("A bypassed latent effect keeps its latency", "[mixer][pdc]")
{
    constexpr int kLatency = 300;
    vloom::Engine engine;
    prepare(engine);
    engine.setSequencerState(rack(click(), {1, 0}));
    auto effect = latent(kLatency);
    effect->setBypassed(true);
    auto state = mixer(1);
    state->inserts[0].effects.push_back(effect);
    engine.setMixerState(std::move(state));
    engine.play();
    const auto audio = render(engine, 0.05);
    CHECK(sounding(audio) == std::vector<std::size_t>{kLatency});
}

TEST_CASE("A latent insert's tail plays out after its input stops", "[mixer][pdc]")
{
    constexpr int kLatency = 3000; // longer than a block: the delay holds the click across blocks
    vloom::Engine engine;
    prepare(engine);
    engine.setSequencerState(rack(click(), {2}));
    auto state = mixer(2);
    state->inserts[0].effects.push_back(latent(kLatency)); // makes the master wait
    engine.setMixerState(std::move(state));
    engine.play();
    const auto audio = render(engine, 0.1);
    CHECK(sounding(audio) == std::vector<std::size_t>{kLatency});
}

TEST_CASE("Routing and compensation do not allocate on the audio thread", "[mixer][pdc][alloc]")
{
    vloom::Engine engine;
    prepare(engine);
    engine.setSequencerState(rack(constant(48000), {1, 2}));
    auto state = mixer(3);
    state->inserts[0].effects.push_back(latent(700));
    state->inserts[0].sends.push_back({3, 0.5f, true});
    state->inserts[1].output = 3;
    engine.setMixerState(std::move(state));
    engine.play();
    std::vector<float> left(256);
    std::vector<float> right(256);
    float* out[2] = {left.data(), right.data()};
    engine.process(out, 2, 256); // picks up the state
    const vloom::test::AllocationCounter counter;
    for (int i = 0; i < 50; ++i)
        engine.process(out, 2, 256);
    CHECK(counter.count() == 0);
}
