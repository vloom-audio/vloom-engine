// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "support/Allocations.hpp"

#include <vloom/engine/Engine.hpp>
#include <vloom/engine/dsp/Synth.hpp>
#include <vloom/engine/render/OfflineRenderer.hpp>
#include <vloom/engine/sequencer/SequencerState.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <vector>

using Catch::Approx;

namespace {

constexpr int kRate = 48000;

/// Renders `synth` for `frames` frames into a left/right pair.
struct Rendered {
    std::vector<float> left, right;
};

Rendered render(vloom::Synth& synth, const vloom::SynthSettings& settings, int frames, int block = 256)
{
    Rendered r{std::vector<float>(static_cast<std::size_t>(frames), 0.0f),
        std::vector<float>(static_cast<std::size_t>(frames), 0.0f)};
    std::array<float*, 2> out{r.left.data(), r.right.data()};
    for (int done = 0; done < frames; done += block)
        synth.render(settings, out.data(), 2, done, std::min(block, frames - done));
    return r;
}

/// Rising zero crossings in [from, to), a frequency estimate.
int crossings(const std::vector<float>& audio, std::size_t from, std::size_t to)
{
    int count = 0;
    for (std::size_t i = from + 1; i < to; ++i)
        if (audio[i - 1] < 0.0f && audio[i] >= 0.0f)
            ++count;
    return count;
}

float peak(const std::vector<float>& audio, std::size_t from, std::size_t to)
{
    float p = 0.0f;
    for (std::size_t i = from; i < to && i < audio.size(); ++i)
        p = std::max(p, std::abs(audio[i]));
    return p;
}

vloom::SynthSettings sine()
{
    vloom::SynthSettings s;
    s.waveform = vloom::Waveform::Sine;
    s.cutoff = 20000.0f;
    s.resonance = 0.0f;
    s.sustain = 1.0f;
    s.level = 1.0f;
    return s;
}

} // namespace

TEST_CASE("A synth note sounds at its frequency, the same on both sides", "[dsp][synth]")
{
    vloom::Synth synth;
    synth.setSampleRate(kRate);
    synth.noteOn(440.0, 1.0f, vloom::Synth::kNoRelease);
    const auto out = render(synth, sine(), kRate);
    CHECK(crossings(out.left, 0, kRate) == Approx(440).margin(1)); // the last one may fall on the end
    CHECK(out.left == out.right);
    CHECK(peak(out.left, 1000, kRate) == Approx(0.7071f).margin(0.01f)); // -3 dB, like a centred sample
}

TEST_CASE("setPitch() and the sample rate change the frequency", "[dsp][synth]")
{
    vloom::Synth synth;
    synth.setSampleRate(kRate / 2);
    synth.noteOn(100.0, 1.0f, vloom::Synth::kNoRelease);
    synth.setPitch(2.0);
    const auto out = render(synth, sine(), kRate / 2); // one second at 24 kHz
    CHECK(crossings(out.left, 0, kRate / 2) == Approx(200).margin(1));
}

TEST_CASE("Every waveform is in range, and band-limited ones stay so at high notes", "[dsp][synth]")
{
    for (const auto wave :
        {vloom::Waveform::Sine, vloom::Waveform::Triangle, vloom::Waveform::Saw, vloom::Waveform::Square}) {
        for (const double frequency : {55.0, 3520.0, 15000.0}) {
            vloom::Synth synth;
            synth.setSampleRate(kRate);
            auto settings = sine();
            settings.waveform = wave;
            synth.noteOn(frequency, 1.0f, vloom::Synth::kNoRelease);
            const auto out = render(synth, settings, 4800);
            CAPTURE(static_cast<int>(wave), frequency);
            CHECK(peak(out.left, 0, 4800) > 0.1f);
            CHECK(peak(out.left, 0, 4800) < 1.2f);
        }
    }
}

TEST_CASE("The envelope: attack, decay to sustain, then release on time", "[dsp][synth]")
{
    vloom::Synth synth;
    synth.setSampleRate(kRate);
    auto settings = sine();
    settings.attack = 0.01f; // 480 frames
    settings.decay = 0.01f;
    settings.sustain = 0.5f;
    settings.release = 0.1f; // 4800 frames
    synth.noteOn(1000.0, 1.0f, 100);
    CHECK(synth.nextRelease() == 100);

    auto out = render(synth, settings, 4800);
    CHECK(peak(out.left, 0, 48) < 0.1f); // starts from silence
    CHECK(peak(out.left, 400, 600) > 0.6f); // near the top
    CHECK(peak(out.left, 2400, 4800) == Approx(0.5f * 0.7071f).margin(0.01f)); // sustain

    synth.release(99);
    CHECK(synth.activeVoices() == 1);
    synth.release(100);
    CHECK(synth.nextRelease() == vloom::Synth::kNoRelease);
    out = render(synth, settings, 9600);
    CHECK(peak(out.left, 2300, 2500) == Approx(0.25f * 0.7071f).margin(0.01f)); // half-way down
    CHECK(peak(out.left, 4800, 9600) == 0.0f);
    CHECK(synth.activeVoices() == 0);
}

TEST_CASE("The low-pass filter takes the top off", "[dsp][synth]")
{
    const auto energy = [](float cutoff) {
        vloom::Synth synth;
        synth.setSampleRate(kRate);
        auto settings = sine();
        settings.waveform = vloom::Waveform::Saw;
        settings.cutoff = cutoff;
        synth.noteOn(220.0, 1.0f, vloom::Synth::kNoRelease);
        const auto out = render(synth, settings, 9600);
        // Energy of the sample-to-sample difference: the high partials.
        double sum = 0.0;
        for (std::size_t i = 4801; i < 9600; ++i)
            sum += std::pow(out.left[i] - out.left[i - 1], 2.0f);
        return sum;
    };
    CHECK(energy(500.0f) < 0.3 * energy(20000.0f));
}

TEST_CASE("Synth polyphony: a note beyond it fades out the oldest", "[dsp][synth]")
{
    vloom::Synth synth(2);
    synth.setSampleRate(kRate);
    for (int i = 0; i < 3; ++i)
        synth.noteOn(200.0 + 100.0 * i, 1.0f, vloom::Synth::kNoRelease);
    CHECK(synth.activeVoices() == 3); // the oldest still fading
    render(synth, sine(), vloom::Synth::kFadeFrames + 1);
    CHECK(synth.activeVoices() == 2);
    synth.releaseHeld();
    synth.stopAll();
    render(synth, sine(), vloom::Synth::kFadeFrames + 1);
    CHECK(synth.activeVoices() == 0);
}

TEST_CASE("Non-finite settings and notes cannot break the synth", "[dsp][synth]")
{
    constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
    vloom::Synth synth;
    synth.setSampleRate(kRate);
    synth.noteOn(std::numeric_limits<double>::infinity(), 1.0f, 0);
    synth.noteOn(-5.0, 1.0f, 0);
    synth.noteOn(440.0, kNaN, 0);
    CHECK(synth.activeVoices() == 0);
    synth.noteOn(440.0, 1.0f, vloom::Synth::kNoRelease);
    vloom::SynthSettings bad;
    bad.attack = kNaN;
    bad.decay = -1.0f;
    bad.release = std::numeric_limits<float>::infinity();
    bad.cutoff = 1e9f;
    bad.resonance = 7.0f;
    bad.level = kNaN;
    bad.sustain = 3.0f;
    const auto out = render(synth, bad, 4800);
    CHECK(std::all_of(out.left.begin(), out.left.end(), [](float x) { return std::isfinite(x); }));
    CHECK(peak(out.left, 0, 4800) < 2.0f);
}

TEST_CASE("A synth channel plays steps and notes through the sequencer", "[synth][sequencer]")
{
    vloom::Engine engine;
    engine.prepare(kRate, 256);
    engine.setMetronomeEnabled(false);
    auto state = std::make_unique<vloom::SequencerState>();
    vloom::ChannelState c;
    c.instrument = vloom::Instrument::Synth; // no sample needed
    c.synth = sine();
    c.steps = {1}; // step 0: root key (C5, 261.63 Hz) for one step, 6000 frames
    vloom::Note n;
    n.start = 4 * vloom::kPpq / 4; // step 4
    n.length = 2 * vloom::kPpq; // one second
    n.key = 69; // A4
    n.velocity = 1.0f;
    c.notes.push_back(n);
    c.voices = std::make_shared<vloom::ChannelVoices>();
    state->channels.push_back(std::move(c));
    engine.setSequencerState(std::move(state));
    engine.play();

    vloom::OfflineRenderOptions options;
    options.seconds = 1.6;
    const auto audio = vloom::renderInterleaved(engine, options);
    std::vector<float> left(audio.size() / 2);
    for (std::size_t i = 0; i < left.size(); ++i)
        left[i] = audio[2 * i];

    CHECK(crossings(left, 0, 6000) == Approx(32.7).margin(1)); // 261.63 Hz * 0.125 s
    // Released at 6000: silent after the default 0.2 s release.
    CHECK(peak(left, 6000 + 9700, 24000) == 0.0f);
    CHECK(crossings(left, 24000, 72000) == Approx(440).margin(1));
    CHECK(peak(left, 72000 + 9700, 76800) == 0.0f);
}

TEST_CASE("A synth channel does not allocate on the audio thread", "[synth][sequencer]")
{
    vloom::Engine engine;
    engine.prepare(kRate, 256);
    auto state = std::make_unique<vloom::SequencerState>();
    vloom::ChannelState c;
    c.instrument = vloom::Instrument::Synth;
    c.steps = {1, 1, 0, 1};
    for (int i = 0; i < 8; ++i) {
        vloom::Note n;
        n.start = i * 480;
        n.length = 700;
        n.key = 40 + 5 * i;
        c.notes.push_back(n);
    }
    c.voices = std::make_shared<vloom::ChannelVoices>(4);
    state->channels.push_back(std::move(c));
    engine.setSequencerState(std::move(state));
    engine.setMasterPitch(-300.0);
    engine.play();

    std::vector<float> l(256), r(256);
    std::array<float*, 2> out{l.data(), r.data()};
    long long onAudioThread = 0;
    for (int block = 0; block < 800; ++block) {
        if (block == 400)
            engine.locate(1000.0);
        const vloom::test::AllocationCounter counter;
        engine.process(out.data(), 2, 256);
        onAudioThread += counter.count();
    }
    CHECK(onAudioThread == 0);
}
