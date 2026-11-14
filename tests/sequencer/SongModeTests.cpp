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

/// A pattern whose channel 0 plays `row`.
vloom::SongPattern stepPattern(std::vector<std::uint8_t> row)
{
    vloom::SongPattern pattern;
    pattern.steps.push_back(std::move(row));
    return pattern;
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
    state->patterns.push_back(stepPattern({1, 0, 0, 0})); // channel 0: step 0
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
    state->patterns.push_back(stepPattern({1}));
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
    state->patterns.push_back(stepPattern({1, 0, 1, 0}));
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

TEST_CASE("A pattern clip with an offset starts that far into its pattern", "[song][clips]")
{
    vloom::Engine engine;
    prepare(engine);
    auto state = song(constant(100));
    // Steps 0 and 5 play; the clip starts 4 steps in, so step 5 lands on its
    // step 1, and with a 8-step pattern the next pass's step 0 on its step 4.
    auto pattern = stepPattern({1, 0, 0, 0, 0, 1, 0, 0});
    pattern.stepCount = 8;
    state->patterns.push_back(std::move(pattern));
    vloom::SongPatternClip clip{0, 2, 8};
    clip.offsetSteps = 4;
    state->patternClips.push_back(clip); // steps 2 .. 9
    engine.setSequencerState(std::move(state));
    engine.play();
    const auto audio = render(engine, 1.5);

    std::vector<std::size_t> onsets;
    for (std::size_t s = 0; s < 12; ++s)
        if (left(audio, s * kStepFrames) != 0.0f)
            onsets.push_back(s);
    CHECK(onsets == std::vector<std::size_t>{3, 6});
}

TEST_CASE("A pattern clip's offset moves its notes too, without a repeat", "[song][clips]")
{
    vloom::Engine engine;
    prepare(engine);
    auto state = song(constant(48000));
    vloom::SongPattern pattern;
    pattern.notes.emplace_back();
    vloom::Note note;
    note.start = 3 * kTicksPerStep;
    note.length = kTicksPerStep;
    note.velocity = 1.0f;
    pattern.notes[0].push_back(note);
    state->patterns.push_back(std::move(pattern)); // stepCount 0: no repeat
    vloom::SongPatternClip clip{0, 0, 4};
    clip.offsetSteps = 2;
    state->patternClips.push_back(clip);
    engine.setSequencerState(std::move(state));
    engine.play();
    const auto audio = render(engine, 1.0);

    CHECK(peak(audio, 0, kStepFrames) == 0.0f);
    CHECK(left(audio, kStepFrames) == Approx(kCentre).margin(1e-4)); // pattern step 3 = clip step 1
    // Held one step, then released: silent once the release is over.
    CHECK(peak(audio, 2 * kStepFrames + vloom::Sampler::kReleaseFrames + 10, 4 * kStepFrames) == 0.0f);
}

TEST_CASE("An audio clip with an offset plays its sample from there", "[song][clips]")
{
    vloom::Engine engine;
    prepare(engine);
    auto state = song(ramp(48000));
    vloom::SongAudioClip clip{0, 1};
    clip.offsetSteps = 2; // 12000 frames into the sample
    state->audioClips.push_back(clip);
    engine.setSequencerState(std::move(state));
    engine.play();
    const auto audio = render(engine, 0.5);

    CHECK(peak(audio, 0, kStepFrames) == 0.0f);
    // After the fade-in, frame 12000 + n of the sample.
    CHECK(left(audio, kStepFrames + 200) / kCentre == Approx((12000.0 + 200.0) / 48000.0).margin(1e-3));
    CHECK(std::abs(left(audio, kStepFrames)) < 0.02f); // starts part-way: faded in
}

TEST_CASE("An audio clip with a length fades out at its end", "[song][clips]")
{
    vloom::Engine engine;
    prepare(engine);
    auto state = song(constant(48000, 0.5f));
    vloom::SongAudioClip clip{0, 0};
    clip.lengthSteps = 2;
    state->audioClips.push_back(clip);
    engine.setSequencerState(std::move(state));
    engine.play();
    const auto audio = render(engine, 0.6);

    CHECK(left(audio, 2 * kStepFrames - 1) == Approx(0.5f * kCentre).margin(1e-4));
    CHECK(peak(audio, 2 * kStepFrames + vloom::Sampler::kReleaseFrames + 1, audio.size() / 2) == 0.0f);
}

TEST_CASE("Playing from inside a cut audio clip keeps its offset and end", "[song][clips]")
{
    vloom::Engine engine;
    prepare(engine);
    auto state = song(ramp(48000));
    vloom::SongAudioClip clip{0, 0};
    clip.offsetSteps = 1; // 6000 frames in
    clip.lengthSteps = 3;
    state->audioClips.push_back(clip);
    engine.setSequencerState(std::move(state));
    engine.locate(1.0 * kTicksPerStep); // one step into the clip
    engine.play();
    const auto audio = render(engine, 0.5);

    CHECK(left(audio, 200) / kCentre == Approx((12000.0 + 200.0) / 48000.0).margin(1e-3));
    // The clip ends 2 steps after the locate.
    CHECK(peak(audio, 2 * kStepFrames + vloom::Sampler::kReleaseFrames + 1, audio.size() / 2) == 0.0f);

    // Located past its end: silent.
    engine.locate(3.5 * kTicksPerStep);
    CHECK(peak(render(engine, 0.2), 0, 9600) == 0.0f);
}

TEST_CASE("An audio clip's rate plays its sample faster, offset included", "[song][clips]")
{
    vloom::Engine engine;
    prepare(engine);
    auto state = song(ramp(96000));
    vloom::SongAudioClip clip{0, 0};
    clip.rate = 2.0; // fitted to the tempo by resampling
    clip.offsetSteps = 1; // one step of the song: 6000 frames played, 12000 of the sample
    state->audioClips.push_back(clip);
    engine.setSequencerState(std::move(state));
    engine.play();
    const auto audio = render(engine, 0.2);
    // After the fade-in, frame n of the song plays frame 12000 + 2n of the sample.
    CHECK(left(audio, 1000) / kCentre == Approx((12000.0 + 2000.0) / 96000.0).margin(2e-3));
    CHECK(left(audio, 5000) / kCentre == Approx((12000.0 + 10000.0) / 96000.0).margin(2e-3));
}

TEST_CASE("An audio clip's own sample replaces its channel's", "[song][clips]")
{
    vloom::Engine engine;
    prepare(engine);
    auto state = song(constant(48000, 0.25f));
    vloom::SongAudioClip clip{0, 0};
    clip.sample = constant(48000, 0.5f); // e.g. time-stretched to the tempo
    state->audioClips.push_back(clip);
    engine.setSequencerState(std::move(state));
    engine.play();
    const auto audio = render(engine, 0.1);
    CHECK(left(audio, 1000) == Approx(0.5f * kCentre).margin(1e-4));
}

TEST_CASE("A clip's own sample stays alive while it sounds, then is freed", "[song][clips]")
{
    vloom::Engine engine;
    prepare(engine);
    std::weak_ptr<const vloom::SampleBuffer> watch;
    std::shared_ptr<vloom::ChannelVoices> voices;
    {
        auto state = song(constant(10));
        voices = state->channels[0].voices;
        vloom::SongAudioClip clip{0, 0};
        auto own = constant(12000); // 0.25 s
        watch = own;
        clip.sample = std::move(own);
        state->audioClips.push_back(clip);
        engine.setSequencerState(std::move(state));
    }
    engine.play();
    render(engine, 0.1);

    // The clip is gone from the song, but the same channel still plays its
    // sample: the old state, which alone holds it, must stay.
    auto next = song(constant(10));
    next->channels[0].voices = voices;
    engine.setSequencerState(std::move(next));
    render(engine, 0.05);
    engine.collectGarbage();
    CHECK_FALSE(watch.expired());

    render(engine, 0.2);
    engine.collectGarbage();
    CHECK(watch.expired());
}
