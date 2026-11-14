// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "support/Allocations.hpp"

#include <vloom/engine/Engine.hpp>
#include <vloom/engine/mixer/MixerState.hpp>
#include <vloom/engine/plugin/GainProcessor.hpp>
#include <vloom/engine/plugin/PluginInstance.hpp>
#include <vloom/engine/render/OfflineRenderer.hpp>
#include <vloom/engine/sequencer/SequencerState.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <thread>
#include <vector>

using Catch::Approx;

namespace {

constexpr double kRate = 48000.0;
constexpr float kCentre = 0.70710678f; // a mono sample in the centre: -3 dB per side

/// A test processor: adds `level` to every sample (a tone generator that
/// also shows whether it ran), and records what the host did with it.
class ProbeProcessor final : public vloom::AudioProcessor {
public:
    struct Log {
        std::atomic<int> activations{0};
        std::atomic<double> lastRate{0.0};
        std::atomic<int> largestBlock{0};
        std::atomic<int> resets{0};
        std::atomic<bool> destroyed{false};
        std::thread::id destroyedOn;
        std::atomic<double> lastParameter{-1.0};
    };

    ProbeProcessor(Log& log, float level, bool refuse = false)
        : log_(log)
        , level_(level)
        , refuse_(refuse)
    {
    }
    ~ProbeProcessor() override
    {
        log_.destroyedOn = std::this_thread::get_id();
        log_.destroyed = true;
    }

    [[nodiscard]] vloom::PluginInfo info() const override
    {
        return {"test.probe", "Probe", "Tests", "1", {}};
    }
    bool activate(double sampleRate, int /*maxBlockFrames*/) override
    {
        if (refuse_)
            return false;
        ++log_.activations;
        log_.lastRate = sampleRate;
        return true;
    }
    void process(const vloom::ProcessContext& context) noexcept override
    {
        for (const vloom::ParameterEvent& e : context.parameters)
            log_.lastParameter = e.value;
        log_.largestBlock = std::max(log_.largestBlock.load(), context.numFrames);
        for (int ch = 0; ch < context.numChannels; ++ch)
            for (int i = 0; i < context.numFrames; ++i)
                context.channels[ch][i] += level_;
    }
    void reset() noexcept override { ++log_.resets; }

private:
    Log& log_;
    float level_;
    bool refuse_;
};

std::shared_ptr<vloom::PluginInstance> instance(
    std::unique_ptr<vloom::AudioProcessor> processor, bool activate = true)
{
    auto plugin = std::make_shared<vloom::PluginInstance>(std::move(processor));
    if (activate)
        plugin->activate(kRate, vloom::kMaxBlockFrames);
    return plugin;
}

std::shared_ptr<const vloom::SampleBuffer> constant(int frames)
{
    auto b = std::make_shared<vloom::SampleBuffer>(1, frames, kRate);
    for (int i = 0; i < frames; ++i)
        b->channel(0)[i] = 1.0f;
    return b;
}

/// One channel holding a constant sample, routed to `insert`.
std::unique_ptr<vloom::SequencerState> rackInto(int insert)
{
    auto state = std::make_unique<vloom::SequencerState>();
    vloom::ChannelState c;
    c.sample = constant(96000);
    c.voices = std::make_shared<vloom::ChannelVoices>();
    c.steps = {1};
    c.insert = insert;
    state->channels.push_back(std::move(c));
    return state;
}

std::unique_ptr<const vloom::MixerState> withEffects(
    int insert, std::vector<std::shared_ptr<vloom::PluginInstance>> effects)
{
    auto state = std::make_unique<vloom::MixerState>();
    state->inserts.resize(static_cast<std::size_t>(insert));
    state->inserts.back().effects = std::move(effects);
    return state;
}

void prepare(vloom::Engine& engine)
{
    engine.prepare(kRate, 256);
    engine.setTempo(120.0);
    engine.setMetronomeEnabled(false);
}

std::vector<float> render(vloom::Engine& engine, double seconds)
{
    vloom::OfflineRenderOptions options;
    options.seconds = seconds;
    options.blockSize = 256;
    return vloom::renderInterleaved(engine, options);
}

/// Processes one block of `frames` of 1.0 through `plugin`; the left channel.
std::vector<float> runOnes(vloom::PluginInstance& plugin, int frames)
{
    std::vector<float> left(static_cast<std::size_t>(frames), 1.0f);
    std::vector<float> right(static_cast<std::size_t>(frames), 1.0f);
    plugin.process(left.data(), right.data(), frames, vloom::TransportInfo{});
    return left;
}

} // namespace

TEST_CASE("The built-in gain ramps to a new value over one block", "[plugin]")
{
    vloom::PluginInstance gain(std::make_unique<vloom::GainProcessor>());
    REQUIRE(gain.activate(kRate, 256));
    CHECK(gain.processor().info().id == "vloom.gain");
    REQUIRE(gain.processor().parameters().size() == 1);

    CHECK(runOnes(gain, 64).back() == Approx(1.0f)); // 0 dB
    REQUIRE(gain.setParameter(vloom::GainProcessor::kGain, -6.0));
    const auto ramp = runOnes(gain, 100);
    CHECK(ramp.front() < 1.0f);
    CHECK(ramp.front() > 0.5f); // ramped, not a jump
    CHECK(ramp.back() == Approx(0.501187f).margin(1e-5));
    CHECK(runOnes(gain, 10).front() == Approx(0.501187f).margin(1e-5)); // and stays

    // Its state is the gain.
    const auto saved = gain.processor().saveState();
    vloom::GainProcessor other;
    REQUIRE(other.loadState(saved));
    CHECK(other.gainDb() == Approx(-6.0));
    CHECK_FALSE(other.loadState(std::vector<std::uint8_t>(3, 0)));
}

TEST_CASE("An inactive, refused or bypassed processor passes audio through", "[plugin]")
{
    ProbeProcessor::Log log;
    vloom::PluginInstance never(std::make_unique<ProbeProcessor>(log, 0.25f));
    CHECK(runOnes(never, 16).front() == 1.0f); // not activated

    vloom::PluginInstance refused(std::make_unique<ProbeProcessor>(log, 0.25f, true));
    CHECK_FALSE(refused.activate(kRate, 256));
    CHECK_FALSE(refused.isActive());
    CHECK(runOnes(refused, 16).front() == 1.0f);

    vloom::PluginInstance probe(std::make_unique<ProbeProcessor>(log, 0.25f));
    REQUIRE(probe.activate(kRate, 256));
    CHECK(runOnes(probe, 16).front() == Approx(1.25f));
    probe.setBypassed(true);
    REQUIRE(probe.setParameter(7, 0.5)); // made while bypassed: kept
    CHECK(runOnes(probe, 16).front() == 1.0f);
    CHECK(log.lastParameter == -1.0);
    CHECK(log.resets == 1); // as bypass started: held notes end
    CHECK(runOnes(probe, 16).front() == 1.0f);
    CHECK(log.resets == 1); // once
    probe.setBypassed(false);
    CHECK(runOnes(probe, 16).front() == Approx(1.25f));
    CHECK(log.lastParameter == 0.5); // delivered once it processes again
}

TEST_CASE("Blocks longer than the processor's maximum are split", "[plugin]")
{
    ProbeProcessor::Log log;
    vloom::PluginInstance probe(std::make_unique<ProbeProcessor>(log, 0.0f));
    REQUIRE(probe.activate(kRate, 64));
    (void)runOnes(probe, 200);
    CHECK(log.largestBlock == 64);
    CHECK_FALSE(probe.setParameter(1, std::numeric_limits<double>::quiet_NaN()));
}

TEST_CASE("An insert's effects process what is routed to it", "[plugin][mixer]")
{
    vloom::Engine engine;
    prepare(engine);
    engine.setSequencerState(rackInto(2));
    auto gain = instance(std::make_unique<vloom::GainProcessor>());
    REQUIRE(gain->setParameter(vloom::GainProcessor::kGain, vloom::GainProcessor::kMinDb)); // silent
    engine.setMixerState(withEffects(2, {gain}));
    engine.play();
    auto audio = render(engine, 0.1);
    float loudest = 0.0f;
    for (std::size_t i = 256 * 2; i < audio.size(); ++i) // past the first block's ramp
        loudest = std::max(loudest, std::abs(audio[i]));
    CHECK(loudest == 0.0f);

    // Back to 0 dB: the channel as it is.
    REQUIRE(gain->setParameter(vloom::GainProcessor::kGain, 0.0));
    audio = render(engine, 0.1);
    CHECK(audio[(2000) * 2] == Approx(kCentre).margin(1e-4));
}

TEST_CASE("Effects run on an insert nothing plays into, for their tails", "[plugin][mixer]")
{
    ProbeProcessor::Log log;
    vloom::Engine engine;
    prepare(engine);
    engine.setMixerState(withEffects(5, {instance(std::make_unique<ProbeProcessor>(log, 0.25f))}));
    const auto audio = render(engine, 0.02); // transport stopped, no channels
    CHECK(audio[100] == Approx(0.25f));
    CHECK(engine.takeInsertPeak(5, 0) == Approx(0.25f));
}

TEST_CASE("A removed effect is destroyed on the control thread, after the audio thread is done", "[plugin]")
{
    ProbeProcessor::Log log;
    vloom::Engine engine;
    prepare(engine);
    engine.setMixerState(withEffects(1, {instance(std::make_unique<ProbeProcessor>(log, 0.0f))}));
    (void)render(engine, 0.01);
    CHECK_FALSE(log.destroyed); // the engine still holds it

    engine.setMixerState(withEffects(1, {})); // removed
    CHECK_FALSE(log.destroyed); // the audio thread has not moved on yet
    (void)render(engine, 0.01);
    engine.collectGarbage();
    CHECK(log.destroyed);
    CHECK(log.destroyedOn == std::this_thread::get_id());
}

TEST_CASE("prepare() activates the hosted effects again at the new rate", "[plugin]")
{
    ProbeProcessor::Log log;
    vloom::Engine engine;
    prepare(engine);
    engine.setMixerState(withEffects(1, {instance(std::make_unique<ProbeProcessor>(log, 0.0f))}));
    (void)render(engine, 0.01); // the audio thread picks the state up
    REQUIRE(log.activations == 1);
    engine.prepare(96000.0, 512);
    CHECK(log.activations == 2);
    CHECK(log.lastRate == 96000.0);
}

TEST_CASE("prepare() activates effects of a state the audio thread has not picked up yet", "[plugin]")
{
    ProbeProcessor::Log log;
    vloom::Engine engine;
    prepare(engine);
    engine.setMixerState(withEffects(1, {instance(std::make_unique<ProbeProcessor>(log, 0.0f))}));
    engine.prepare(96000.0, 512); // no block in between
    CHECK(log.activations == 2);
    CHECK(log.lastRate == 96000.0);
}

TEST_CASE("Effects do not allocate on the audio thread", "[plugin]")
{
    vloom::Engine engine;
    prepare(engine);
    engine.setSequencerState(rackInto(1));
    auto gain = instance(std::make_unique<vloom::GainProcessor>());
    engine.setMixerState(withEffects(1, {gain, nullptr, instance(std::make_unique<vloom::GainProcessor>())}));
    engine.play();
    std::vector<float> l(256), r(256);
    std::array<float*, 2> out{l.data(), r.data()};
    long long onAudioThread = 0;
    for (int block = 0; block < 200; ++block) {
        if (block % 10 == 0)
            gain->setParameter(vloom::GainProcessor::kGain, -0.1 * block); // control side: outside the count
        const vloom::test::AllocationCounter counter;
        engine.process(out.data(), 2, 256);
        onAudioThread += counter.count();
    }
    CHECK(onAudioThread == 0);
}
