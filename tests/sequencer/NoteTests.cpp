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
// 120 BPM: a tick is 25 frames, a 16th-note step (240 ticks) 6000 frames,
// the default 16-step pattern 96000 frames (2 s).
constexpr std::int64_t kStepTicks = vloom::kPpq / 4;
constexpr float kCentre = 0.70710678f;

std::shared_ptr<const vloom::SampleBuffer> constant(int frames, float value = 1.0f)
{
    auto b = std::make_shared<vloom::SampleBuffer>(1, frames, kRate);
    for (int i = 0; i < frames; ++i)
        b->channel(0)[i] = value;
    return b;
}

/// Frame i holds i / 48000: tells how far into the sample playback is.
std::shared_ptr<const vloom::SampleBuffer> ramp(int frames)
{
    auto b = std::make_shared<vloom::SampleBuffer>(1, frames, kRate);
    for (int i = 0; i < frames; ++i)
        b->channel(0)[i] = static_cast<float>(i) / static_cast<float>(kRate);
    return b;
}

void prepare(vloom::Engine& engine)
{
    engine.prepare(kRate, 256);
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

float peak(const std::vector<float>& audio, std::size_t from, std::size_t to)
{
    float p = 0.0f;
    for (std::size_t i = from * 2; i < to * 2 && i < audio.size(); ++i)
        p = std::max(p, std::abs(audio[i]));
    return p;
}

vloom::Note note(std::int64_t start, std::int64_t length, int key = vloom::Note::kMiddleC)
{
    vloom::Note n;
    n.start = start;
    n.length = length;
    n.key = key;
    n.velocity = 1.0f;
    return n;
}

/// One channel playing `sample`, no steps.
std::unique_ptr<vloom::SequencerState> rack(std::shared_ptr<const vloom::SampleBuffer> sample)
{
    auto state = std::make_unique<vloom::SequencerState>();
    vloom::ChannelState c;
    c.sample = std::move(sample);
    c.voices = std::make_shared<vloom::ChannelVoices>();
    state->channels.push_back(std::move(c));
    return state;
}

} // namespace

TEST_CASE("A note starts on its exact frame and is released after its length", "[notes]")
{
    for (const int blockSize : {64, 256, 441, 1024}) {
        vloom::Engine engine;
        prepare(engine);
        auto state = rack(constant(96000));
        state->channels[0].notes.push_back(note(2 * kStepTicks, kStepTicks)); // step 2, one step long
        engine.setSequencerState(std::move(state));
        engine.play();
        const auto audio = render(engine, 1.5, blockSize);

        CAPTURE(blockSize);
        CHECK(peak(audio, 0, 12000) == 0.0f);
        CHECK(left(audio, 12000) == Approx(kCentre).margin(1e-5));
        CHECK(left(audio, 17999) == Approx(kCentre).margin(1e-5));
        // Released at 18000: falls over kReleaseFrames, then silence.
        CHECK(left(audio, 18000 + vloom::Sampler::kReleaseFrames / 2) == Approx(kCentre / 2).margin(0.01));
        CHECK(peak(audio, 18000 + vloom::Sampler::kReleaseFrames, 72000) == 0.0f);
    }
}

TEST_CASE("A note's key sets the pitch, relative to the channel's root key", "[notes]")
{
    const auto playedAt = [](int key, int rootKey) {
        vloom::Engine engine;
        prepare(engine);
        auto state = rack(ramp(96000));
        state->channels[0].rootKey = rootKey;
        state->channels[0].notes.push_back(note(0, 4 * kStepTicks, key));
        engine.setSequencerState(std::move(state));
        engine.play();
        // How far into the sample the note is after 1000 frames.
        return left(render(engine, 0.1), 1000) / kCentre * static_cast<float>(kRate);
    };
    CHECK(playedAt(60, 60) == Approx(1000.0f).margin(0.01f));
    CHECK(playedAt(72, 60) == Approx(2000.0f).margin(0.05f)); // an octave up: twice as fast
    CHECK(playedAt(48, 60) == Approx(500.0f).margin(0.05f));
    CHECK(playedAt(67, 60) == Approx(1000.0f * std::exp2(7.0f / 12.0f)).margin(0.05f)); // a fifth
    CHECK(playedAt(72, 72) == Approx(1000.0f).margin(0.01f));
}

TEST_CASE("Notes come round every time the pattern loops", "[notes]")
{
    vloom::Engine engine;
    prepare(engine);
    auto state = rack(constant(1000));
    state->channels[0].notes.push_back(note(kStepTicks, 10));
    engine.setSequencerState(std::move(state));
    engine.play();
    const auto audio = render(engine, 4.5);
    for (const std::size_t loop : {0u, 1u, 2u}) {
        const std::size_t at = loop * 96000 + 6000;
        CAPTURE(loop);
        CHECK(left(audio, at - 1) == 0.0f);
        CHECK(left(audio, at) == Approx(kCentre).margin(1e-5));
    }
}

TEST_CASE("Out-of-range notes are skipped", "[notes]")
{
    vloom::Engine engine;
    prepare(engine);
    auto state = rack(constant(1000));
    auto& notes = state->channels[0].notes;
    notes.push_back(note(16 * kStepTicks, 10)); // past the pattern's end
    notes.push_back(note(-1, 10));
    notes.push_back(note(0, 0)); // no length
    notes.push_back(note(0, 10, 128)); // no such key
    engine.setSequencerState(std::move(state));
    engine.play();
    CHECK(peak(render(engine, 2.5), 0, 120000) == 0.0f);
}

TEST_CASE("A muted channel starts no notes", "[notes]")
{
    vloom::Engine engine;
    prepare(engine);
    auto state = rack(constant(1000));
    state->channels[0].muted = true;
    state->channels[0].notes.push_back(note(0, 10));
    engine.setSequencerState(std::move(state));
    engine.play();
    CHECK(peak(render(engine, 0.5), 0, 24000) == 0.0f);
}

TEST_CASE("Song mode plays the notes of a pattern clip that start inside it", "[notes][song]")
{
    vloom::Engine engine;
    prepare(engine);
    auto state = rack(constant(1000));
    state->song = true;
    vloom::SongPattern pattern;
    pattern.notes.push_back({note(kStepTicks, 10), note(8 * kStepTicks, 10)});
    state->patterns.push_back(std::move(pattern));
    state->patternClips.push_back({0, 4, 4}); // steps 4 .. 7: the second note falls outside
    engine.setSequencerState(std::move(state));
    engine.play();
    const auto audio = render(engine, 2.5);

    CHECK(peak(audio, 0, 30000) == 0.0f);
    CHECK(left(audio, 30000) == Approx(kCentre).margin(1e-5)); // step 4 + 1
    CHECK(peak(audio, 32000, 120000) == 0.0f);
}

TEST_CASE("Master pitch transposes steps and notes, live, but not audio clips", "[notes]")
{
    SECTION("steps and notes")
    {
        vloom::Engine engine;
        prepare(engine);
        auto state = rack(ramp(96000));
        state->channels[0].steps = {1};
        engine.setSequencerState(std::move(state));
        engine.setMasterPitch(1200.0); // an octave up
        engine.play();
        auto audio = render(engine, 0.05);
        CHECK(left(audio, 1000) / kCentre * kRate == Approx(2000.0).margin(0.05));

        engine.setMasterPitch(-5000.0); // clamped to an octave down
        audio = render(engine, 0.05); // the ringing note follows from the next block
        const float a = left(audio, 1000) / kCentre * static_cast<float>(kRate);
        const float b = left(audio, 1100) / kCentre * static_cast<float>(kRate);
        CHECK(b - a == Approx(50.0f).margin(0.05f));
    }
    SECTION("audio clips keep their speed")
    {
        vloom::Engine engine;
        prepare(engine);
        auto state = rack(ramp(96000));
        state->song = true;
        state->audioClips.push_back({0, 0});
        engine.setSequencerState(std::move(state));
        engine.setMasterPitch(700.0);
        engine.play();
        CHECK(left(render(engine, 0.05), 1000) / kCentre * kRate == Approx(1000.0).margin(0.05));
    }
}

TEST_CASE("Locating releases held notes", "[notes]")
{
    vloom::Engine engine;
    prepare(engine);
    auto state = rack(constant(192000));
    state->channels[0].notes.push_back(note(0, 15 * kStepTicks)); // almost the whole pattern
    engine.setSequencerState(std::move(state));
    engine.play();
    CHECK(left(render(engine, 0.2), 9000) == Approx(kCentre).margin(1e-5));
    engine.locate(8.0 * kStepTicks); // half-way: no note starts there
    const auto audio = render(engine, 0.5);
    CHECK(peak(audio, vloom::Sampler::kReleaseFrames + 256, 24000) == 0.0f);
}

TEST_CASE("A note can follow another of the same key on a one-voice channel", "[notes]")
{
    vloom::Engine engine;
    prepare(engine);
    auto state = rack(ramp(96000));
    state->channels[0].voices = std::make_shared<vloom::ChannelVoices>(1);
    state->channels[0].notes.push_back(note(0, kStepTicks));
    state->channels[0].notes.push_back(note(kStepTicks, kStepTicks)); // right at the first one's end
    engine.setSequencerState(std::move(state));
    engine.play();
    const auto audio = render(engine, 0.5);
    // The second note starts from the top of the sample: the first fades out under it.
    const float value = left(audio, 6000 + 2 * vloom::Sampler::kReleaseFrames) / kCentre;
    CHECK(value * static_cast<float>(kRate) == Approx(2.0f * vloom::Sampler::kReleaseFrames).margin(0.05f));
}

TEST_CASE("Playing notes does not allocate on the audio thread", "[notes]")
{
    vloom::Engine engine;
    prepare(engine);
    auto state = rack(constant(500));
    for (int i = 0; i < 16; ++i)
        state->channels[0].notes.push_back(note(i * kStepTicks + 7, 100, 48 + i));
    engine.setSequencerState(std::move(state));
    engine.setMasterPitch(250.0);
    engine.play();

    std::vector<float> l(256), r(256);
    std::array<float*, 2> out{l.data(), r.data()};
    long long onAudioThread = 0;
    for (int block = 0; block < 800; ++block) {
        if (block == 400)
            engine.locate(kStepTicks * 5.5);
        const vloom::test::AllocationCounter counter;
        engine.process(out.data(), 2, 256);
        onAudioThread += counter.count();
    }
    CHECK(onAudioThread == 0);
}
