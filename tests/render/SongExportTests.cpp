// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include <vloom/engine/Engine.hpp>
#include <vloom/engine/render/OfflineRenderer.hpp>
#include <vloom/engine/render/SongExport.hpp>
#include <vloom/engine/render/WavWriter.hpp>
#include <vloom/engine/sample/SampleBuffer.hpp>
#include <vloom/engine/sample/WavReader.hpp>
#include <vloom/engine/sequencer/SequencerState.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using Catch::Approx;

namespace {

constexpr double kRate = 48000.0;
constexpr float kCentre = 0.70710678f;
// 120 BPM: a 16th-note step (240 ticks) is 6000 frames.
constexpr std::int64_t kStepFrames = 6000;

std::shared_ptr<const vloom::SampleBuffer> constant(int frames, float value = 1.0f, double rate = kRate)
{
    auto b = std::make_shared<vloom::SampleBuffer>(1, frames, rate);
    for (int i = 0; i < frames; ++i)
        b->channel(0)[i] = value;
    return b;
}

/// Song mode, one channel playing `sample`.
vloom::SequencerState song(std::shared_ptr<const vloom::SampleBuffer> sample)
{
    vloom::SequencerState state;
    state.song = true;
    vloom::ChannelState c;
    c.sample = std::move(sample);
    c.voices = std::make_shared<vloom::ChannelVoices>();
    state.channels.push_back(std::move(c));
    return state;
}

vloom::SongPattern pattern(std::vector<std::uint8_t> row, int stepCount = 0)
{
    vloom::SongPattern p;
    p.steps.push_back(std::move(row));
    p.stepCount = stepCount;
    return p;
}

struct TempFolder {
    std::filesystem::path path;
    TempFolder()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() / ("vloom_export_" + std::to_string(stamp));
        std::filesystem::create_directories(path);
    }
    ~TempFolder()
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    TempFolder(const TempFolder&) = delete;
    TempFolder& operator=(const TempFolder&) = delete;
    [[nodiscard]] std::string file(const char* name) const { return (path / name).string(); }
};

std::vector<std::uint8_t> bytesOf(const std::string& path)
{
    std::ifstream in(std::filesystem::path(path), std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

} // namespace

TEST_CASE("The song ends with its last pattern or audio clip", "[export]")
{
    auto state = song(constant(48000)); // 1 s
    state.patterns.push_back(pattern({1}));
    state.patternClips.push_back({0, 4, 8}); // steps 4 .. 11
    CHECK(vloom::songEndTicks(state, 120.0) == 12 * 240);

    // At 120 BPM one second is 1920 ticks; at 60 BPM 960.
    state.audioClips.push_back({0, 16}); // from tick 3840
    CHECK(vloom::songEndTicks(state, 120.0) == 3840 + 1920);
    CHECK(vloom::songEndTicks(state, 60.0) == 3840 + 960); // the same second is fewer ticks
    state.audioClips.push_back({0, 20});
    CHECK(vloom::songEndTicks(state, 60.0) == 20 * 240 + 960);

    state.song = false; // pattern mode: one pass of the pattern
    CHECK(vloom::songEndTicks(state, 120.0) == 16 * 240);
    CHECK(vloom::songEndTicks(vloom::SequencerState{}, 120.0) == 16 * 240);
    state.ticksPerStep = 0;
    CHECK(vloom::songEndTicks(state, 120.0) == 0);
}

TEST_CASE("An export sounds exactly like the live engine", "[export]")
{
    TempFolder folder;
    auto state = song(constant(3000, 0.5f));
    state.patterns.push_back(pattern({1, 0, 1, 0}));
    state.patternClips.push_back({0, 0, 4});

    vloom::SongExportOptions options;
    options.format = vloom::WavFormat::Float32;
    const auto result = vloom::exportSong(folder.file("song.wav"), state, nullptr, kRate, options);
    REQUIRE(result.ok);
    CHECK(result.error.empty());

    const auto read = vloom::readWavFile(folder.file("song.wav"));
    REQUIRE(read.buffer);
    const vloom::SampleBuffer& wav = *read.buffer;
    CHECK(wav.channels() == 2);
    CHECK(wav.sampleRate() == kRate);
    // Four steps, then 50 ms of silence ends the tail (rounded up to blocks).
    CHECK(wav.frames() >= 4 * kStepFrames + 2400);
    CHECK(wav.frames() < 4 * kStepFrames + 2400 + 2 * 512);
    CHECK(static_cast<std::uint64_t>(wav.frames()) == result.frames);

    // The same song through an engine played live (offline here).
    vloom::Engine live;
    live.prepare(kRate, 512);
    live.setMetronomeEnabled(false);
    live.setSequencerState(std::make_unique<vloom::SequencerState>(state));
    live.play();
    vloom::OfflineRenderOptions render;
    render.seconds = static_cast<double>(wav.frames()) / kRate;
    const auto audio = vloom::renderInterleaved(live, render);
    for (std::int64_t i = 0; i < wav.frames(); ++i) {
        const auto at = static_cast<std::size_t>(i);
        if (wav.channel(0)[i] != audio[at * 2] || wav.channel(1)[i] != audio[at * 2 + 1]) {
            FAIL("frame " << i << " differs");
            break;
        }
    }
    CHECK(wav.channel(0)[0] == Approx(0.5f * kCentre).margin(1e-6));
    CHECK(wav.channel(0)[2 * kStepFrames] == Approx(0.5f * kCentre).margin(1e-6));
}

TEST_CASE("An export renders the tail after the song's end", "[export]")
{
    TempFolder folder;
    auto state = song(constant(48000, 0.25f)); // 1 s, far past the 1-step clip
    state.patterns.push_back(pattern({1}));
    state.patternClips.push_back({0, 0, 1});
    vloom::SongExportOptions options;
    options.format = vloom::WavFormat::Float32;

    auto result = vloom::exportSong(folder.file("tail.wav"), state, nullptr, kRate, options);
    REQUIRE(result.ok);
    CHECK(result.frames >= 48000); // the whole sample, then a little silence
    CHECK(result.frames < 48000 + 2400 + 1024);

    options.maxTailSeconds = 0.25; // the tail is capped
    result = vloom::exportSong(folder.file("short.wav"), state, nullptr, kRate, options);
    REQUIRE(result.ok);
    CHECK(result.frames == static_cast<std::uint64_t>(kStepFrames + 12000));
}

TEST_CASE("A cancelled export writes no file", "[export]")
{
    TempFolder folder;
    auto state = song(constant(100));
    state.patterns.push_back(pattern({1}, 1));
    state.patternClips.push_back({0, 0, 64});
    std::vector<double> seen;
    const auto result = vloom::exportSong(folder.file("x.wav"), state, nullptr, kRate, {}, [&](double done) {
        seen.push_back(done);
        return done < 0.5;
    });
    CHECK_FALSE(result.ok);
    CHECK(result.cancelled);
    CHECK(std::is_sorted(seen.begin(), seen.end()));
    CHECK(
        std::distance(std::filesystem::directory_iterator(folder.path), std::filesystem::directory_iterator{})
        == 0);
}

TEST_CASE("Export refuses what it cannot render right", "[export]")
{
    TempFolder folder;
    auto state = song(constant(100, 1.0f, 44100.0)); // loaded for another rate
    state.patterns.push_back(pattern({1}));
    state.patternClips.push_back({0, 0, 4});
    auto result = vloom::exportSong(folder.file("a.wav"), state, nullptr, kRate);
    CHECK_FALSE(result.ok);
    CHECK_FALSE(result.error.empty());

    result = vloom::exportSong(folder.file("b.wav"), song(constant(100)), nullptr, kRate);
    CHECK_FALSE(result.ok); // no clips: nothing to export

    vloom::SongExportOptions options;
    options.channels = 6;
    result = vloom::exportSong(folder.file("c.wav"), state, nullptr, 44100.0, options);
    CHECK_FALSE(result.ok);

    // A range far too long is refused up front, not rendered until the file
    // is full (and the frame count never overflows: review finding).
    auto fine = song(constant(100));
    fine.patterns.push_back(pattern({1}));
    fine.patternClips.push_back({0, 0, 4});
    options = {};
    for (const std::int64_t end : {std::numeric_limits<std::int64_t>::max(), std::int64_t{100'000'000'000},
             std::int64_t{2'000'000'000}}) {
        options.endTicks = end;
        const auto started = std::chrono::steady_clock::now();
        result = vloom::exportSong(folder.file("d.wav"), fine, nullptr, kRate, options);
        CAPTURE(end);
        CHECK_FALSE(result.ok);
        CHECK_FALSE(result.error.empty());
        CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
    }

    // The possible tail counts too: a song that fits a 16-bit mono file only
    // without its tail is refused up front (review finding).
    options = {};
    options.format = vloom::WavFormat::Pcm16;
    options.channels = 1;
    options.maxTailSeconds = 600.0;
    const double fitFrames = (4294967295.0 - 64.0) / 2.0; // mono 16-bit
    const double ticksPerFrame = 120.0 / 60.0 * vloom::kPpq / kRate;
    options.endTicks = static_cast<std::int64_t>((fitFrames - 600.0 * kRate / 2.0) * ticksPerFrame);
    const auto started = std::chrono::steady_clock::now();
    result = vloom::exportSong(folder.file("e.wav"), fine, nullptr, kRate, options);
    CHECK_FALSE(result.ok);
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
}

TEST_CASE("Export uses voices of its own", "[export]")
{
    auto state = song(constant(100));
    const auto copy = vloom::copyWithOwnVoices(state);
    REQUIRE(copy->channels[0].voices != nullptr);
    CHECK(copy->channels[0].voices != state.channels[0].voices);
    CHECK(copy->channels[0].sample == state.channels[0].sample); // samples are shared
}

TEST_CASE("A pattern clip longer than its pattern repeats it", "[export][song]")
{
    vloom::Engine engine;
    engine.prepare(kRate, 256);
    engine.setMetronomeEnabled(false);
    auto state = std::make_unique<vloom::SequencerState>(song(constant(100)));
    auto looped = pattern({1, 0, 0, 0}, 4); // a 4-step pattern ...
    vloom::Note note;
    note.start = 2 * 240; // ... with a note on its step 2
    note.length = 10;
    note.velocity = 1.0f;
    looped.notes.push_back({note});
    state->patterns.push_back(std::move(looped));
    state->patternClips.push_back({0, 0, 10}); // ... in a 10-step clip: steps 0, 4, 8; notes 2, 6
    engine.setSequencerState(std::move(state));
    engine.play();
    vloom::OfflineRenderOptions render;
    render.seconds = 1.6;
    const auto audio = vloom::renderInterleaved(engine, render);
    const auto at
        = [&](std::int64_t step) { return audio[static_cast<std::size_t>(step * kStepFrames) * 2]; };
    for (const std::int64_t step : {0, 2, 4, 6, 8}) {
        CAPTURE(step);
        CHECK(at(step) != 0.0f);
    }
    for (const std::int64_t step :
        {1, 3, 5, 7, 9, 10, 12}) { // step 10: past the clip (note at 10 would be step 2 again)
        CAPTURE(step);
        CHECK(at(step) == 0.0f);
    }
}

TEST_CASE("PCM WAV: 16 and 24 bits, clamped, dithered by choice", "[export][render]")
{
    TempFolder folder;
    const std::vector<float> samples{0.0f, 0.5f, -0.5f, 1.5f, -1.5f, 0.0f};

    vloom::WavFileWriter writer;
    vloom::WavFileWriter::Spec spec;
    spec.channels = 1;
    spec.format = vloom::WavFormat::Pcm16;
    spec.dither = false;
    std::string error;
    REQUIRE(writer.open(folder.file("16.wav"), spec, &error));
    REQUIRE(writer.write(samples, &error));
    REQUIRE(writer.finish(&error));
    const auto bytes = bytesOf(folder.file("16.wav"));
    REQUIRE(bytes.size() == 44 + 6 * 2);
    const auto s16 = [&](std::size_t i) {
        return static_cast<std::int16_t>(bytes[44 + 2 * i] | (bytes[44 + 2 * i + 1] << 8));
    };
    CHECK(s16(0) == 0);
    CHECK(s16(1) == 16384);
    CHECK(s16(2) == -16384);
    CHECK(s16(3) == 32767); // clamped
    CHECK(s16(4) == -32768);

    // Our own reader understands both, 24-bit included.
    spec.format = vloom::WavFormat::Pcm24;
    REQUIRE(writer.open(folder.file("24.wav"), spec, &error));
    REQUIRE(writer.write(samples, &error));
    REQUIRE(writer.finish(&error));
    const auto read = vloom::readWavFile(folder.file("24.wav"));
    REQUIRE(read.buffer);
    CHECK(read.buffer->frames() == 6);
    CHECK(read.buffer->channel(0)[1] == Approx(0.5f).margin(1e-6));
    CHECK(read.buffer->channel(0)[3] == Approx(1.0f).margin(1e-6));

    // Dithered silence: noise of at most one step either way, not all zero.
    spec.format = vloom::WavFormat::Pcm16;
    spec.dither = true;
    REQUIRE(writer.open(folder.file("dither.wav"), spec, &error));
    REQUIRE(writer.write(std::vector<float>(1000, 0.0f), &error));
    REQUIRE(writer.finish(&error));
    const auto noisy = bytesOf(folder.file("dither.wav"));
    int nonZero = 0;
    for (std::size_t i = 0; i < 1000; ++i) {
        const auto v = static_cast<std::int16_t>(noisy[44 + 2 * i] | (noisy[44 + 2 * i + 1] << 8));
        CHECK(std::abs(static_cast<int>(v)) <= 1);
        nonZero += v != 0 ? 1 : 0;
    }
    CHECK(nonZero > 100);
}

TEST_CASE("A WAV file of unknown length gets its real length in the header", "[render]")
{
    TempFolder folder;
    vloom::WavFileWriter writer;
    vloom::WavFileWriter::Spec spec; // stereo float, length unknown
    std::string error;
    REQUIRE(writer.open(folder.file("open.wav"), spec, &error));
    const std::vector<float> samples{0.25f, -0.25f, 0.5f, -0.5f};
    REQUIRE(writer.write(samples, &error));
    REQUIRE(writer.write(samples, &error));
    REQUIRE(writer.finish(&error));
    CHECK(bytesOf(folder.file("open.wav"))
        == vloom::encodeWavFloat32(
            std::vector<float>{0.25f, -0.25f, 0.5f, -0.5f, 0.25f, -0.25f, 0.5f, -0.5f}, 2, 48000));
}
