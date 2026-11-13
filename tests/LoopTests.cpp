// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "support/Allocations.hpp"

#include <vloom/engine/Engine.hpp>
#include <vloom/engine/render/OfflineRenderer.hpp>
#include <vloom/engine/sample/SampleBuffer.hpp>
#include <vloom/engine/sequencer/SequencerState.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

using Catch::Approx;

namespace {

constexpr double kRate = 48000.0;
// 120 BPM: a step (240 ticks) is 6000 frames, a tick 25 frames.
constexpr std::int64_t kStepFrames = 6000;
constexpr double kStepTicks = 240.0;

std::shared_ptr<const vloom::SampleBuffer> constant(int frames, float value = 1.0f)
{
    auto b = std::make_shared<vloom::SampleBuffer>(1, frames, kRate);
    for (int i = 0; i < frames; ++i)
        b->channel(0)[i] = value;
    return b;
}

/// Frame i holds i / 48000.
std::shared_ptr<const vloom::SampleBuffer> ramp(int frames)
{
    auto b = std::make_shared<vloom::SampleBuffer>(1, frames, kRate);
    for (int i = 0; i < frames; ++i)
        b->channel(0)[i] = static_cast<float>(i) / static_cast<float>(kRate);
    return b;
}

/// One channel whose 1-frame click plays on every step.
std::unique_ptr<vloom::SequencerState> clicks()
{
    auto state = std::make_unique<vloom::SequencerState>();
    vloom::ChannelState c;
    c.sample = constant(1);
    c.voices = std::make_shared<vloom::ChannelVoices>();
    c.steps = std::vector<std::uint8_t>(16, 1);
    state->channels.push_back(std::move(c));
    return state;
}

std::vector<float> render(vloom::Engine& engine, double seconds, int blockSize = 256)
{
    vloom::OfflineRenderOptions options;
    options.seconds = seconds;
    options.blockSize = blockSize;
    return vloom::renderInterleaved(engine, options);
}

/// Frames of the left channel that are not silent.
std::vector<std::int64_t> onsets(const std::vector<float>& audio)
{
    std::vector<std::int64_t> out;
    for (std::size_t i = 0; i < audio.size() / 2; ++i)
        if (audio[i * 2] != 0.0f)
            out.push_back(static_cast<std::int64_t>(i));
    return out;
}

} // namespace

TEST_CASE("A loop jumps back on its exact sample and plays its events again", "[loop]")
{
    for (const int blockSize : {37, 256, 441, 4096}) {
        vloom::Engine engine;
        engine.prepare(kRate, blockSize);
        engine.setMetronomeEnabled(false);
        engine.setSequencerState(clicks());
        engine.setLoop(2 * kStepTicks, 4 * kStepTicks); // steps 2 and 3
        engine.play();
        const auto audio = render(engine, 1.0, blockSize); // 8 steps' worth
        CAPTURE(blockSize);
        // Steps 0, 1, 2, 3, then 2, 3 again and again, every 6000 frames.
        const std::vector<std::int64_t> expected{0, 6000, 12000, 18000, 24000, 30000, 36000, 42000};
        CHECK(onsets(audio) == expected);
        CHECK(engine.positionTicks() >= 2 * kStepTicks);
        CHECK(engine.positionTicks() < 4 * kStepTicks);
    }
}

TEST_CASE("A loop that does not fall on a step keeps every pass sample-accurate", "[loop]")
{
    vloom::Engine engine;
    engine.prepare(kRate, 256);
    engine.setMetronomeEnabled(false);
    engine.setSequencerState(clicks());
    // From the middle of step 1 to the middle of step 3: steps 2 and 3 play
    // each pass; a pass is 480 ticks = 12000 frames.
    engine.setLoop(1.5 * kStepTicks, 3.5 * kStepTicks);
    engine.play();
    const auto audio = render(engine, 1.0);
    const std::vector<std::int64_t> expected{
        0, 6000, 12000, 18000, 21000 + 3000, 21000 + 9000, 21000 + 15000, 21000 + 21000};
    // After the first wrap (at 21000) step 2 comes 3000 frames later, then
    // step 3, then the next pass.
    CHECK(onsets(audio) == expected);
}

TEST_CASE("An audio clip under way resumes part-way at the loop start", "[loop][song]")
{
    vloom::Engine engine;
    engine.prepare(kRate, 256);
    engine.setMetronomeEnabled(false);
    auto state = std::make_unique<vloom::SequencerState>();
    state->song = true;
    vloom::ChannelState c;
    c.sample = ramp(96000);
    c.voices = std::make_shared<vloom::ChannelVoices>();
    state->channels.push_back(std::move(c));
    state->audioClips.push_back({0, 0}); // from the song start
    engine.setSequencerState(std::move(state));
    engine.setLoop(2 * kStepTicks, 4 * kStepTicks); // frames 12000 .. 24000
    engine.play();
    const auto audio = render(engine, 1.0); // 48000 frames: two wraps
    const auto value
        = [&](std::size_t frame) { return audio[frame * 2] / 0.70710678f * static_cast<float>(kRate); };
    CHECK(value(23000) == Approx(23000.0f).margin(0.05f)); // first pass: the clip plays on
    // Past the wrap (and the 64-frame fade-in), the clip is back at frame 12000 + n.
    CHECK(value(24000 + 1000) == Approx(12000.0f + 1000.0f).margin(0.05f));
    CHECK(value(36000 + 1000) == Approx(12000.0f + 1000.0f).margin(0.05f));
}

TEST_CASE("A note held across the loop end is released at the wrap", "[loop][notes]")
{
    vloom::Engine engine;
    engine.prepare(kRate, 256);
    engine.setMetronomeEnabled(false);
    auto state = std::make_unique<vloom::SequencerState>();
    vloom::ChannelState c;
    c.sample = constant(480000);
    c.voices = std::make_shared<vloom::ChannelVoices>();
    vloom::Note n;
    n.start = 0;
    n.length = 8 * 240; // reaches far past the loop end
    n.velocity = 1.0f;
    c.notes.push_back(n);
    state->channels.push_back(std::move(c));
    engine.setSequencerState(std::move(state));
    engine.setLoop(0.0, 2 * kStepTicks); // the note starts each pass, frames 0 .. 12000
    engine.play();
    const auto audio = render(engine, 0.6);
    // Released at the wrap (fading over kReleaseFrames) and started again:
    // once the release is over, one voice sounds, not two.
    const auto at = [&](std::size_t frame) { return audio[frame * 2]; };
    CHECK(at(11999) == Approx(0.70710678f).margin(1e-4));
    CHECK(at(12000 + vloom::Sampler::kReleaseFrames + 10) == Approx(0.70710678f).margin(1e-4));
    CHECK(at(24000 + vloom::Sampler::kReleaseFrames + 10) == Approx(0.70710678f).margin(1e-4));
}

TEST_CASE("The metronome clicks on the loop start every pass", "[loop]")
{
    vloom::Engine engine;
    engine.prepare(kRate, 256);
    engine.setLoop(960.0, 2 * 960.0); // beat 1 .. 2: half a second
    engine.play();
    const auto audio = render(engine, 2.0);
    for (const std::size_t pass : {0u, 1u, 2u}) {
        const std::size_t at = 24000 + pass * 24000;
        CAPTURE(pass);
        CHECK(audio[at * 2] != 0.0f);
        CHECK(audio[(at - 1) * 2] == Approx(0.0f).margin(1e-3)); // the previous click has died away
    }
}

TEST_CASE("Playing from past the loop end goes on; a short or cleared loop does nothing", "[loop]")
{
    vloom::Engine engine;
    engine.prepare(kRate, 256);
    engine.setMetronomeEnabled(false);
    engine.setSequencerState(clicks());
    engine.setLoop(0.0, 2 * kStepTicks);
    engine.locate(3 * kStepTicks);
    engine.play();
    render(engine, 0.5);
    CHECK(engine.positionTicks() > 3 * kStepTicks + 900.0);

    engine.setLoop(0.0, vloom::Engine::kMinLoopTicks / 2); // too short: off
    engine.locate(0.0);
    render(engine, 0.5);
    CHECK(engine.positionTicks() > 900.0);

    engine.setLoop(0.0, 2 * kStepTicks);
    engine.clearLoop();
    engine.locate(0.0);
    render(engine, 0.5);
    CHECK(engine.positionTicks() > 900.0);
}

TEST_CASE("The transport stops on its exact sample at the stop point", "[loop]")
{
    for (const int blockSize : {37, 256, 4096}) {
        vloom::Engine engine;
        engine.prepare(kRate, blockSize);
        engine.setMetronomeEnabled(false);
        engine.setSequencerState(clicks());
        engine.setStopAt(3 * kStepTicks); // after steps 0, 1, 2
        engine.play();
        const auto audio = render(engine, 1.0, blockSize);
        CAPTURE(blockSize);
        const std::vector<std::int64_t> expected{0, 6000, 12000};
        CHECK(onsets(audio) == expected);
        CHECK_FALSE(engine.isPlaying());
        CHECK(engine.positionTicks() == Approx(3 * kStepTicks).margin(0.05));
    }
}

TEST_CASE("Stopping at the end fades sounding notes out", "[loop]")
{
    vloom::Engine engine;
    engine.prepare(kRate, 256);
    engine.setMetronomeEnabled(false);
    auto state = clicks();
    state->channels[0].sample = constant(96000); // rings far past the stop
    state->channels[0].steps = {1};
    engine.setSequencerState(std::move(state));
    engine.setStopAt(kStepTicks); // frame 6000
    engine.play();
    const auto audio = render(engine, 0.25);
    CHECK(audio[5999 * 2] == Approx(0.70710678f).margin(1e-4));
    float after = 0.0f;
    for (std::size_t i = (6000 + 64) * 2; i < audio.size(); i += 2)
        after = std::max(after, std::abs(audio[i]));
    CHECK(after == 0.0f);
}

TEST_CASE("Looping does not allocate on the audio thread", "[loop]")
{
    vloom::Engine engine;
    engine.prepare(kRate, 256);
    engine.setSequencerState(clicks());
    engine.setLoop(100.0, 700.0);
    engine.setStopAt(1e6);
    engine.play();
    std::vector<float> l(256), r(256);
    std::array<float*, 2> out{l.data(), r.data()};
    long long onAudioThread = 0;
    for (int block = 0; block < 600; ++block) {
        const vloom::test::AllocationCounter counter;
        engine.process(out.data(), 2, 256);
        onAudioThread += counter.count();
    }
    CHECK(onAudioThread == 0);
}
