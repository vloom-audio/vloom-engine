// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
//
// Regression tests for the engine audit: each case reproduces a finding.
#include <vloom/engine/Engine.hpp>
#include <vloom/engine/dsp/Sampler.hpp>
#include <vloom/engine/dsp/Synth.hpp>
#include <vloom/engine/render/OfflineRenderer.hpp>
#include <vloom/engine/render/WavWriter.hpp>
#include <vloom/engine/sample/SampleBuffer.hpp>
#include <vloom/engine/sample/SampleLoader.hpp>
#include <vloom/engine/sequencer/SequencerState.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using Catch::Approx;

namespace {

constexpr double kRate = 48000.0;

std::shared_ptr<const vloom::SampleBuffer> constant(int frames, float value = 1.0f)
{
    auto b = std::make_shared<vloom::SampleBuffer>(1, frames, kRate);
    for (int i = 0; i < frames; ++i)
        b->channel(0)[i] = value;
    return b;
}

/// A folder of its own under the temp directory, removed afterwards.
struct TempFolder {
    std::filesystem::path path;
    TempFolder()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() / ("vloom_audit_" + std::to_string(stamp));
        std::filesystem::create_directories(path);
    }
    ~TempFolder()
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    TempFolder(const TempFolder&) = delete;
    TempFolder& operator=(const TempFolder&) = delete;
};

/// A stereo output buffer for driving Engine::process() block by block.
struct Blocks {
    std::vector<float> left, right;
    std::array<float*, 2> out{};
    explicit Blocks(int frames)
        : left(static_cast<std::size_t>(frames))
        , right(static_cast<std::size_t>(frames))
        , out{left.data(), right.data()}
    {
    }
};

int filesIn(const std::filesystem::path& folder)
{
    int count = 0;
    for ([[maybe_unused]] const auto& entry : std::filesystem::directory_iterator(folder))
        ++count;
    return count;
}

} // namespace

TEST_CASE("Tempo changes between blocks neither repeat nor drop a step", "[audit][sequencer]")
{
    // Audit finding: each block worked out afresh which events had fired, so
    // a tempo change moved the fraction of a sample before an event across
    // the "already fired" line: events fired twice or never.
    for (const int blockSize : {37, 64, 512}) {
        vloom::Engine engine;
        engine.prepare(kRate, blockSize);
        engine.setMetronomeEnabled(false);
        auto state = std::make_unique<vloom::SequencerState>();
        vloom::ChannelState c;
        c.sample = constant(1); // one frame: one click per step
        c.voices = std::make_shared<vloom::ChannelVoices>();
        c.steps = std::vector<std::uint8_t>(16, 1);
        state->channels.push_back(std::move(c));
        state->ticksPerStep = 7; // many steps, at odd places between samples
        engine.setSequencerState(std::move(state));
        engine.play();

        Blocks blocks(blockSize);
        int clicks = 0;
        for (int block = 0; block < 3000; ++block) {
            engine.setTempo(block % 2 == 0 ? 61.3 : 187.9);
            engine.process(blocks.out.data(), 2, blockSize);
            for (const float s : blocks.left)
                if (s != 0.0f)
                    ++clicks;
        }
        // Every step up to the last frame rendered fires once: the last
        // block ran at 187.9 BPM, so that frame is one sample before the end.
        const double ticks = engine.positionTicks();
        const double ticksPerSample = 187.9 / 60.0 * vloom::kPpq / kRate;
        const double lastFrame = ticks - ticksPerSample * (1.0 - 1e-6);
        const auto expected = static_cast<int>(std::floor(lastFrame / 7.0)) + 1;
        CAPTURE(blockSize, ticks);
        CHECK(clicks == expected);
    }
}

TEST_CASE("A preview of the same sample, asked for again and again, can still be stopped", "[audit][preview]")
{
    // Audit finding: old preview states playing the same sample as the
    // current one were never freed; after 16 the exchange took no new ones.
    vloom::Engine engine;
    engine.prepare(kRate, 256);
    const auto sample = constant(480000); // 10 s
    Blocks blocks(256);
    for (int i = 0; i < 40; ++i) {
        engine.previewSample(sample);
        engine.process(blocks.out.data(), 2, 256);
        engine.collectGarbage();
    }
    CHECK(engine.isPreviewPlaying());
    engine.previewSample(nullptr);
    for (int i = 0; i < 4; ++i)
        engine.process(blocks.out.data(), 2, 256);
    CHECK_FALSE(engine.isPreviewPlaying());
}

TEST_CASE("A preview asked for in the same block as Stop still plays", "[audit][preview]")
{
    vloom::Engine engine;
    engine.prepare(kRate, 256);
    engine.play();
    Blocks blocks(256);
    engine.process(blocks.out.data(), 2, 256);
    engine.stop();
    engine.previewSample(constant(48000));
    engine.process(blocks.out.data(), 2, 256);
    CHECK(engine.isPreviewPlaying());
}

TEST_CASE("Offline rendering refuses lengths that make no sense", "[audit][render]")
{
    vloom::Engine engine;
    engine.prepare(kRate, 256);
    vloom::OfflineRenderOptions options;
    for (const double seconds :
        {-1.0, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
        options.seconds = seconds;
        CHECK(vloom::offlineRenderFrames(engine, options) == 0);
        CHECK(vloom::renderInterleaved(engine, options).empty());
    }

    TempFolder folder;
    const std::string path = (folder.path / "long.wav").string();
    options.seconds = 6.0 * 3600.0; // 6 hours of stereo float: over 4 GiB
    std::string error;
    const auto started = std::chrono::steady_clock::now();
    CHECK_FALSE(vloom::renderToWav(engine, path, options, &error));
    CHECK_FALSE(error.empty());
    // Refused up front, not after rendering hours of audio.
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
    CHECK_FALSE(std::filesystem::exists(folder.path / "long.wav"));
}

TEST_CASE("renderToWav streams to a temporary file and leaves no trace on failure", "[audit][render]")
{
    TempFolder folder;
    vloom::Engine engine;
    engine.prepare(kRate, 256);
    engine.setMetronomeEnabled(true);
    engine.play();
    vloom::OfflineRenderOptions options;
    options.seconds = 1.0;
    const std::string path = (folder.path / "out.wav").string();
    std::string error;
    REQUIRE(vloom::renderToWav(engine, path, options, &error));
    CHECK(std::filesystem::file_size(folder.path / "out.wav") == 58 + 48000 * 2 * 4);
    CHECK(filesIn(folder.path) == 1); // no temporary file left

    // An abandoned writer removes its temporary file and keeps the old one.
    {
        vloom::WavFileWriter writer;
        REQUIRE(writer.open(path, 10, 2, 48000, &error));
        const std::vector<float> some(4, 0.5f);
        REQUIRE(writer.write(some, &error));
        CHECK_FALSE(writer.finish(&error)); // 2 of 10 frames
    }
    CHECK(filesIn(folder.path) == 1);
    CHECK(std::filesystem::file_size(folder.path / "out.wav") == 58 + 48000 * 2 * 4);
}

TEST_CASE("A WAV header whose byte rate would overflow is refused", "[audit][render]")
{
    CHECK(vloom::encodeWavFloat32Header(10, vloom::kMaxWavChannels, 96000).empty());
    CHECK_FALSE(vloom::encodeWavFloat32Header(10, 2, 48000).empty());
}

TEST_CASE("Loading a very low-rate file does not blow it up at the engine's rate", "[audit][sample]")
{
    // Audit finding: a 1 kHz file upsampled to 48 kHz grew 48 times.
    TempFolder folder;
    const std::string path = (folder.path / "low.wav").string();
    REQUIRE(vloom::writeWavFloat32(path, std::vector<float>(400000, 0.1f), 1, 1000)); // 1.6 MB of float
    std::string error;
    CHECK(vloom::loadSample(path, 48000.0, &error) == nullptr); // would be 76.8 MB
    CHECK_FALSE(error.empty());
    CHECK(vloom::loadSample(path, 8000.0, &error) != nullptr); // 12.8 MB: fine
}

TEST_CASE("A file name that is not UTF-8 is reported, not thrown", "[audit][sample]")
{
    std::string error;
    CHECK(vloom::loadSample(std::string("bad-\xFF\xFE.wav"), 48000.0, &error) == nullptr);
    CHECK_FALSE(error.empty());
    TempFolder folder;
    const std::string bad = folder.path.string() + "/bad-\xFF\xFE.wav";
    CHECK_NOTHROW(vloom::writeWavFloat32(bad, std::vector<float>{0.0f}, 1, 48000, &error));
}

TEST_CASE("Stealing more voices than fade slots in one go cuts none", "[audit][dsp]")
{
    // Audit finding: with only 4 fade slots, a 5th steal in one block cut the
    // voice it had just started fading (the one with the most fade left was
    // picked by "smallest fadeLeft" only among the others).
    const auto sample = constant(10000);
    vloom::Sampler sampler(4);
    for (int i = 0; i < 4; ++i)
        sampler.trigger(sample.get());
    for (int i = 0; i < 6; ++i) // a 6-note chord
        sampler.trigger(sample.get());
    CHECK(sampler.activeVoices() == 10); // 4 + 6 sounding, 6 of them fading out

    vloom::Synth synth(4);
    for (int i = 0; i < 10; ++i)
        synth.noteOn(220.0 * (i + 1), 1.0f, vloom::Synth::kNoRelease);
    CHECK(synth.activeVoices() == 10);
}

TEST_CASE("Clip and note values far out of range are ignored, not overflowed", "[audit][sequencer]")
{
    vloom::Engine engine;
    engine.prepare(kRate, 256);
    engine.setMetronomeEnabled(false);
    auto state = std::make_unique<vloom::SequencerState>();
    state->song = true;
    vloom::ChannelState c;
    c.sample = constant(100);
    c.voices = std::make_shared<vloom::ChannelVoices>();
    state->channels.push_back(std::move(c));
    vloom::SongPattern pattern;
    pattern.steps.push_back({1});
    vloom::Note huge;
    huge.start = 0;
    huge.length = std::numeric_limits<std::int64_t>::max(); // held "forever", without overflowing
    huge.velocity = 1.0f;
    pattern.notes.push_back({huge});
    state->patterns.push_back(std::move(pattern));
    constexpr auto kMax = std::numeric_limits<std::int64_t>::max();
    state->patternClips.push_back({0, kMax / 2, kMax / 2});
    state->patternClips.push_back({0, 0, kMax});
    state->patternClips.push_back({0, 0, 4}); // the one that plays
    state->audioClips.push_back({0, kMax / 3});
    state->audioClips.push_back({0, -5});
    engine.setSequencerState(std::move(state));
    engine.play();
    const auto audio = vloom::renderInterleaved(engine, {});
    CHECK(std::all_of(audio.begin(), audio.end(), [](float s) { return std::isfinite(s); }));
    CHECK(audio[0] != 0.0f); // step 0 of the sane clip
}

TEST_CASE("Notes in any order play: the engine sorts them", "[audit][sequencer]")
{
    vloom::Engine engine;
    engine.prepare(kRate, 256);
    engine.setMetronomeEnabled(false);
    auto state = std::make_unique<vloom::SequencerState>();
    vloom::ChannelState c;
    c.sample = constant(10);
    c.voices = std::make_shared<vloom::ChannelVoices>();
    for (const std::int64_t start : {720, 0, 480, 240}) {
        vloom::Note n;
        n.start = start;
        n.length = 10;
        n.velocity = 1.0f;
        c.notes.push_back(n);
    }
    state->channels.push_back(std::move(c));
    engine.setSequencerState(std::move(state));
    engine.play();
    vloom::OfflineRenderOptions options;
    options.seconds = 0.5;
    const auto audio = vloom::renderInterleaved(engine, options);
    for (const std::size_t frame : {0u, 6000u, 12000u, 18000u}) { // 240 ticks = 6000 frames
        CAPTURE(frame);
        CHECK(audio[frame * 2] != 0.0f);
    }
}

TEST_CASE("prepare() keeps the sample rate in a range the scheduler can work with", "[audit][engine]")
{
    vloom::Engine engine;
    engine.prepare(std::numeric_limits<double>::infinity(), 256);
    CHECK(engine.sampleRate() == 48000.0);
    engine.prepare(1.0, 256);
    CHECK(engine.sampleRate() == vloom::Engine::kMinSampleRate);
    engine.prepare(1e9, 256);
    CHECK(engine.sampleRate() == vloom::Engine::kMaxSampleRate);

    vloom::Synth synth;
    synth.setSampleRate(10.0); // its cutoff range stays valid
    synth.noteOn(100.0, 1.0f, vloom::Synth::kNoRelease);
    std::vector<float> l(64), r(64);
    std::array<float*, 2> out{l.data(), r.data()};
    synth.render(vloom::SynthSettings{}, out.data(), 2, 0, 64);
    CHECK(std::all_of(l.begin(), l.end(), [](float s) { return std::isfinite(s); }));
}

TEST_CASE("Playing from just past an audio clip's start plays it once", "[audit][song]")
{
    // Audit finding: resumeAt() started the clip at offset 0 and the step at
    // its start, less than a frame earlier, started it again: +6 dB.
    vloom::Engine engine;
    engine.prepare(kRate, 256);
    engine.setMetronomeEnabled(false);
    auto state = std::make_unique<vloom::SequencerState>();
    state->song = true;
    vloom::ChannelState c;
    c.sample = constant(4800);
    c.voices = std::make_shared<vloom::ChannelVoices>();
    state->channels.push_back(std::move(c));
    state->audioClips.push_back({0, 4});
    engine.setSequencerState(std::move(state));
    engine.locate(4.0 * vloom::kPpq / 4 + 0.01); // 0.01 tick = a quarter of a frame past the start
    engine.play();
    vloom::OfflineRenderOptions options;
    options.seconds = 0.01;
    const auto audio = vloom::renderInterleaved(engine, options);
    CHECK(audio[200] == Approx(0.70710678f).margin(1e-4)); // one voice, centred
}

TEST_CASE("A locked target fails when opened, not after the render", "[audit][render]")
{
    TempFolder folder;
    const std::string path = (folder.path / "busy.wav").string();
    REQUIRE(vloom::writeWavFloat32(path, std::vector<float>{0.0f}, 1, 48000));
    std::filesystem::permissions(folder.path / "busy.wav",
        std::filesystem::perms::owner_write | std::filesystem::perms::group_write
            | std::filesystem::perms::others_write,
        std::filesystem::perm_options::remove);
    vloom::WavFileWriter writer;
    std::string error;
    CHECK_FALSE(writer.open(path, 10, 1, 48000, &error));
    CHECK_FALSE(error.empty());
    std::filesystem::permissions(folder.path / "busy.wav",
        std::filesystem::perms::owner_write | std::filesystem::perms::group_write
            | std::filesystem::perms::others_write,
        std::filesystem::perm_options::add);
}
