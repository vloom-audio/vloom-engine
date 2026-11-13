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
#include <memory>
#include <vector>

using Catch::Approx;

namespace {

constexpr double kRate = 48000.0;
// 120 BPM: a 16th-note step is 0.125 s = 6000 frames.
constexpr std::size_t kStepFrames = 6000;
constexpr int kTicksPerStep = vloom::kPpq / 4;
constexpr float kCentre = 0.70710678f; // a mono sample in the centre: -3 dB per side

std::shared_ptr<const vloom::SampleBuffer> constant(int frames, float value = 1.0f)
{
    auto b = std::make_shared<vloom::SampleBuffer>(1, frames, kRate);
    for (int i = 0; i < frames; ++i)
        b->channel(0)[i] = value;
    return b;
}

/// Frame i holds i / frames: tells which part of the sample is playing.
std::shared_ptr<const vloom::SampleBuffer> ramp(int frames)
{
    auto b = std::make_shared<vloom::SampleBuffer>(1, frames, kRate);
    for (int i = 0; i < frames; ++i)
        b->channel(0)[i] = static_cast<float>(i) / static_cast<float>(frames);
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

/// Largest absolute sample in frames [from, to).
float peak(const std::vector<float>& audio, std::size_t from, std::size_t to)
{
    float p = 0.0f;
    for (std::size_t i = from * 2; i < to * 2 && i < audio.size(); ++i)
        p = std::max(p, std::abs(audio[i]));
    return p;
}

/// One channel playing `sample`; song mode with the given clips.
std::unique_ptr<vloom::SequencerState> song(
    std::shared_ptr<const vloom::SampleBuffer> channelSample = nullptr)
{
    auto state = std::make_unique<vloom::SequencerState>();
    state->song = true;
    vloom::ChannelState c;
    c.sample = std::move(channelSample);
    c.voices = std::make_shared<vloom::ChannelVoices>();
    state->channels.push_back(std::move(c));
    return state;
}

} // namespace

TEST_CASE("Song mode plays a pattern clip where it is placed", "[song]")
{
    vloom::Engine engine;
    prepare(engine);
    auto state = song(constant(1000));
    state->patterns.push_back({{{1, 0, 0, 0}}}); // channel 0: step 0
    state->patternClips.push_back({0, 4, 4}); // steps 4 .. 7
    engine.setSequencerState(std::move(state));
    engine.play();
    const auto audio = render(engine, 1.5);

    CHECK(peak(audio, 0, 4 * kStepFrames) == 0.0f); // nothing before the clip
    CHECK(left(audio, 4 * kStepFrames) == Approx(kCentre).margin(1e-4));
    CHECK(peak(audio, 4 * kStepFrames + 1000, audio.size() / 2) == 0.0f); // played once: no loop
}

TEST_CASE("Song mode plays an audio clip from its step, once", "[song]")
{
    vloom::Engine engine;
    prepare(engine);
    auto state = song(constant(3000, 0.5f));
    state->audioClips.push_back({0, 2});
    engine.setSequencerState(std::move(state));
    engine.play();
    const auto audio = render(engine, 1.0);

    CHECK(peak(audio, 0, 2 * kStepFrames) == 0.0f);
    CHECK(left(audio, 2 * kStepFrames) == Approx(0.5f * kCentre).margin(1e-4));
    CHECK(left(audio, 2 * kStepFrames + 2999) == Approx(0.5f * kCentre).margin(1e-4));
    CHECK(peak(audio, 2 * kStepFrames + 3000, audio.size() / 2) == 0.0f);
}

TEST_CASE("Pattern mode ignores the playlist", "[song]")
{
    vloom::Engine engine;
    prepare(engine);
    auto state = song(constant(3000));
    state->song = false;
    state->audioClips.push_back({0, 0});
    state->patterns.push_back({{{1}}});
    state->patternClips.push_back({0, 0, 16});
    engine.setSequencerState(std::move(state));
    engine.play();
    CHECK(peak(render(engine, 0.5), 0, 24000) == 0.0f);
}

TEST_CASE("Playing from inside an audio clip starts it part-way", "[song]")
{
    vloom::Engine engine;
    prepare(engine);
    auto state = song(ramp(48000)); // one second long
    state->audioClips.push_back({0, 0}); // from the start
    engine.setSequencerState(std::move(state));
    // Half a second in: one beat at 120 BPM.
    engine.locate(1.0 * vloom::kPpq);
    engine.play();
    const auto audio = render(engine, 0.1);

    // After the fade-in, the sample continues from frame 24000.
    CHECK(left(audio, 200) / kCentre == Approx((24000.0 + 200.0) / 48000.0).margin(1e-3));
    CHECK(std::abs(left(audio, 0)) < 0.02f); // faded in, no click
}

TEST_CASE("Locating while playing restarts the clips at the new place", "[song]")
{
    vloom::Engine engine;
    prepare(engine);
    auto state = song(ramp(96000));
    state->audioClips.push_back({0, 0});
    engine.setSequencerState(std::move(state));
    engine.play();
    render(engine, 0.1);
    engine.locate(kTicksPerStep * 8.0); // 1 s in
    const auto audio = render(engine, 0.05);
    CHECK(left(audio, 500) / kCentre == Approx((48000.0 + 500.0) / 96000.0).margin(1e-3));
}

TEST_CASE("Stop fades the audio clips out", "[song]")
{
    vloom::Engine engine;
    prepare(engine);
    auto state = song(constant(96000));
    state->audioClips.push_back({0, 0});
    engine.setSequencerState(std::move(state));
    engine.play();
    render(engine, 0.1);
    engine.stop();
    const auto audio = render(engine, 0.05);
    CHECK(peak(audio, 256, audio.size() / 2) == 0.0f);
}

TEST_CASE("An audio clip plays through its channel: volume, mute, insert", "[song]")
{
    vloom::Engine engine;
    prepare(engine);
    const auto play = [&](float gain, bool muted, int insert) {
        auto state = song(constant(3000));
        state->channels[0].gain = gain;
        state->channels[0].muted = muted;
        state->channels[0].insert = insert;
        state->audioClips.push_back({0, 0});
        state->audioClips.push_back({5, 0}); // no such channel: ignored
        engine.setSequencerState(std::move(state));
        engine.stop();
        engine.locate(0.0);
        engine.play();
        return render(engine, 0.05);
    };
    CHECK(left(play(0.5f, false, 0), 1000) == Approx(0.5f * kCentre).margin(1e-3));
    CHECK(peak(play(1.0f, true, 0), 256, 2400) == 0.0f); // after the last run's fade-out
    play(1.0f, false, 3);
    CHECK(engine.takeInsertPeak(3, 0) == Approx(kCentre).margin(1e-3));
}

TEST_CASE("Song mode does not allocate on the audio thread", "[song]")
{
    vloom::Engine engine;
    prepare(engine);
    auto state = song(constant(500));
    state->patterns.push_back({{{1, 0, 1, 0}}});
    state->patternClips.push_back({0, 0, 64});
    for (int i = 0; i < 8; ++i)
        state->audioClips.push_back({0, i * 3});
    engine.setSequencerState(std::move(state));
    engine.play();

    std::vector<float> l(256), r(256);
    std::array<float*, 2> out{l.data(), r.data()};
    long long onAudioThread = 0;
    for (int block = 0; block < 600; ++block) {
        if (block == 300)
            engine.locate(kTicksPerStep * 5.5); // resume inside clips
        const vloom::test::AllocationCounter counter;
        engine.process(out.data(), 2, 256);
        onAudioThread += counter.count();
    }
    CHECK(onAudioThread == 0);
}
