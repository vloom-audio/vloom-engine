// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include <vloom/engine/Engine.hpp>
#include <vloom/engine/mixer/MixerState.hpp>
#include <vloom/engine/render/Bounce.hpp>
#include <vloom/engine/render/OfflineRenderer.hpp>
#include <vloom/engine/render/SongExport.hpp>
#include <vloom/engine/sample/SampleBuffer.hpp>
#include <vloom/engine/sample/WavReader.hpp>
#include <vloom/engine/sequencer/SequencerState.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using Catch::Approx;

namespace {

constexpr double kRate = 48000.0;
constexpr std::int64_t kStep = vloom::kPpq / 4; ///< ticks per step (the default ticksPerStep)
// 120 BPM: a 16th-note step (240 ticks) is 6000 frames.
constexpr std::int64_t kStepFrames = 6000;
constexpr float kCentre = 0.70710678f;

std::shared_ptr<const vloom::SampleBuffer> constant(int frames, float value)
{
    auto b = std::make_shared<vloom::SampleBuffer>(1, frames, kRate);
    for (int i = 0; i < frames; ++i)
        b->channel(0)[i] = value;
    return b;
}

vloom::ChannelState channel(std::shared_ptr<const vloom::SampleBuffer> sample, int insert = 0)
{
    vloom::ChannelState c;
    c.sample = std::move(sample);
    c.voices = std::make_shared<vloom::ChannelVoices>();
    c.insert = insert;
    return c;
}

/// Song mode: two channels (0.5 and 0.25), pattern 0 plays both on its
/// first step and channel 0 on its third; pattern 1 plays channel 1 on
/// every step. The playlist has pattern 1 at the start and pattern 0 later.
vloom::SequencerState twoChannels()
{
    vloom::SequencerState state;
    state.song = true;
    state.channels.push_back(channel(constant(3000, 0.5f), 1));
    state.channels.push_back(channel(constant(3000, 0.25f), 2));
    vloom::SongPattern first;
    first.steps = {{1, 0, 1, 0}, {1, 0, 0, 0}};
    first.stepCount = 4;
    vloom::SongPattern second;
    second.steps = {{}, {1, 1, 1, 1, 1, 1, 1, 1}};
    second.stepCount = 8;
    state.patterns = {first, second};
    state.patternClips.push_back({1, 0, 8 * kStep});
    state.patternClips.push_back({0, 16 * kStep, 4 * kStep});
    return state;
}

float leftAt(const vloom::BounceResult& result, std::int64_t frame)
{
    return result.audio[static_cast<std::size_t>(2 * frame)];
}

struct TempFolder {
    std::filesystem::path path;
    TempFolder()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() / ("vloom_bounce_" + std::to_string(stamp));
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

} // namespace

TEST_CASE("A bounced pattern is as long as the pattern, and its tail", "[render][bounce]")
{
    const auto state = twoChannels();
    CHECK(vloom::patternLengthTicks(state, 0) == 4 * kStep);
    CHECK(vloom::patternLengthTicks(state, 1) == 8 * kStep);
    CHECK(vloom::patternLengthTicks(state, vloom::kChannelRackPattern) == 16 * kStep);
    CHECK(vloom::patternLengthTicks(state, 2) == 0);

    vloom::BounceOptions options;
    options.maxTailSeconds = 0.0;
    auto result = vloom::bouncePattern(state, 0, nullptr, kRate, options);
    REQUIRE(result.ok);
    CHECK(result.lengthTicks == 4 * kStep);
    CHECK(result.bodyFrames == 4 * kStepFrames);
    CHECK(result.frames == 4 * kStepFrames);
    CHECK(result.audio.size() == 2 * 4 * kStepFrames);

    // A sample that rings past the pattern's end: the tail goes on until it has died away.
    auto ringing = state;
    ringing.channels[0].sample = constant(30000, 0.5f); // from the third step (12000) to 42000
    options.maxTailSeconds = 10.0;
    result = vloom::bouncePattern(ringing, 0, nullptr, kRate, options);
    REQUIRE(result.ok);
    CHECK(result.bodyFrames == 4 * kStepFrames);
    CHECK(result.frames >= 42000 + 2400);
    CHECK(result.frames < 42000 + 2400 + 2 * 512);
    CHECK(leftAt(result, 41000) != 0.0f);

    options.maxTailSeconds = 0.25; // capped like an export's
    result = vloom::bouncePattern(ringing, 0, nullptr, kRate, options);
    REQUIRE(result.ok);
    CHECK(result.frames == 4 * kStepFrames + 12000);
}

TEST_CASE("A bounced pattern sounds exactly like the pattern played by the engine", "[render][bounce]")
{
    const auto state = twoChannels();
    vloom::BounceOptions options;
    options.mix = vloom::BounceMix::Dry;
    const auto result = vloom::bouncePattern(state, 0, nullptr, kRate, options);
    REQUIRE(result.ok);

    // The same pattern as the only clip of a song, played the usual way.
    auto alone = state;
    alone.patternClips = {{0, 0, 4 * kStep}};
    vloom::Engine live;
    live.prepare(kRate, 512);
    live.setMetronomeEnabled(false);
    live.setSequencerState(vloom::copyWithOwnVoices(alone));
    live.play();
    vloom::OfflineRenderOptions render;
    render.seconds = static_cast<double>(result.frames) / kRate;
    const auto audio = vloom::renderInterleaved(live, render);
    REQUIRE(audio.size() == result.audio.size());
    for (std::size_t i = 0; i < audio.size(); ++i)
        if (audio[i] != result.audio[i]) {
            FAIL("sample " << i << " differs");
            break;
        }
    CHECK(leftAt(result, 0) == Approx(0.75f * kCentre).margin(1e-6));
    CHECK(leftAt(result, 2 * kStepFrames) == Approx(0.5f * kCentre).margin(1e-6));

    // And twice the same.
    const auto again = vloom::bouncePattern(state, 0, nullptr, kRate, options);
    REQUIRE(again.ok);
    CHECK(again.audio == result.audio);
}

TEST_CASE("A bounce can keep only some channels", "[render][bounce]")
{
    const auto state = twoChannels();
    vloom::BounceOptions options;
    options.mix = vloom::BounceMix::Dry;
    options.maxTailSeconds = 0.0;
    options.channels = {1};
    auto result = vloom::bouncePattern(state, 0, nullptr, kRate, options);
    REQUIRE(result.ok);
    CHECK(leftAt(result, 0) == Approx(0.25f * kCentre).margin(1e-6));
    CHECK(leftAt(result, 2 * kStepFrames) == 0.0f); // channel 0's step is not heard

    options.channels = {0};
    result = vloom::bouncePattern(state, 0, nullptr, kRate, options);
    REQUIRE(result.ok);
    CHECK(leftAt(result, 0) == Approx(0.5f * kCentre).margin(1e-6));

    options.channels = {0, 5};
    result = vloom::bouncePattern(state, 0, nullptr, kRate, options);
    CHECK_FALSE(result.ok);
    CHECK_FALSE(result.error.empty());
    CHECK(result.audio.empty());
}

TEST_CASE("A bounce goes dry, through the inserts, or through the whole mixer", "[render][bounce][mixer]")
{
    const auto state = twoChannels();
    vloom::MixerState mixer;
    mixer.inserts.resize(2);
    mixer.inserts[0].gain = 0.5f; // channel 0's insert
    mixer.inserts[1].gain = 0.0f; // channel 1's: silent
    vloom::BounceOptions options;
    options.maxTailSeconds = 0.0;
    options.masterGain = 0.5f;

    options.mix = vloom::BounceMix::Dry;
    auto result = vloom::bouncePattern(state, 0, &mixer, kRate, options);
    REQUIRE(result.ok);
    CHECK(leftAt(result, 0) == Approx(0.75f * kCentre).margin(1e-6));

    options.mix = vloom::BounceMix::Inserts; // the master's gain is left out
    result = vloom::bouncePattern(state, 0, &mixer, kRate, options);
    REQUIRE(result.ok);
    CHECK(leftAt(result, 1000) == Approx(0.25f * kCentre).margin(1e-6));

    options.mix = vloom::BounceMix::Master;
    result = vloom::bouncePattern(state, 0, &mixer, kRate, options);
    REQUIRE(result.ok);
    CHECK(leftAt(result, 1000) == Approx(0.125f * kCentre).margin(1e-6));
}

TEST_CASE("The channel rack's own pattern bounces too, and to a WAV file", "[render][bounce]")
{
    TempFolder folder;
    auto state = twoChannels();
    state.song = false;
    state.stepCount = 4;
    state.channels[0].steps = {0, 1, 0, 0};
    vloom::BounceOptions options;
    options.mix = vloom::BounceMix::Dry;
    options.maxTailSeconds = 0.0;
    options.path = folder.file("rack.wav");
    options.format = vloom::WavFormat::Float32;
    const auto result = vloom::bouncePattern(state, vloom::kChannelRackPattern, nullptr, kRate, options);
    REQUIRE(result.ok);
    CHECK(result.audio.empty());
    CHECK(result.frames == 4 * kStepFrames);

    const auto file = vloom::readWavFile(folder.file("rack.wav"));
    REQUIRE(file.buffer.has_value());
    CHECK(file.buffer->channels() == 2);
    CHECK(file.buffer->sampleRate() == kRate);
    CHECK(file.buffer->frames() == 4 * kStepFrames);
    CHECK(file.buffer->channel(0)[0] == 0.0f);
    CHECK(file.buffer->channel(0)[kStepFrames] == Approx(0.5f * kCentre).margin(1e-6));

    const auto missing = vloom::bouncePattern(state, 7, nullptr, kRate, options);
    CHECK_FALSE(missing.ok);
    CHECK_FALSE(missing.error.empty());
}

TEST_CASE("A range of the song bounces like the export of that range", "[render][bounce]")
{
    TempFolder folder;
    const auto state = twoChannels();
    vloom::BounceOptions options;
    options.mix = vloom::BounceMix::Master;
    options.channels = {1};
    const auto result = vloom::bounceRange(state, 2 * kStep, 18 * kStep, nullptr, kRate, options);
    REQUIRE(result.ok);
    CHECK(result.lengthTicks == 16 * kStep);
    CHECK(result.bodyFrames == 16 * kStepFrames);

    auto only = state;
    only.channels[0].muted = true;
    vloom::SongExportOptions exportOptions;
    exportOptions.format = vloom::WavFormat::Float32;
    exportOptions.startTicks = 2 * kStep;
    exportOptions.endTicks = 18 * kStep;
    const auto exported = vloom::exportSong(folder.file("range.wav"), only, nullptr, kRate, exportOptions);
    REQUIRE(exported.ok);
    const auto file = vloom::readWavFile(folder.file("range.wav"));
    REQUIRE(file.buffer.has_value());
    REQUIRE(static_cast<std::uint64_t>(file.buffer->frames()) == result.frames);
    for (std::int64_t i = 0; i < file.buffer->frames(); ++i)
        if (file.buffer->channel(0)[i] != leftAt(result, i)) {
            FAIL("frame " << i << " differs");
            break;
        }
    // Pattern 1's steps 2 .. 7, then pattern 0's first step (channel 1 only).
    CHECK(leftAt(result, 0) == Approx(0.25f * kCentre).margin(1e-6));
    CHECK(leftAt(result, 14 * kStepFrames) == Approx(0.25f * kCentre).margin(1e-6));
    CHECK(leftAt(result, 14 * kStepFrames + 3500) == 0.0f);
}

TEST_CASE("A bounce leaves the live engine's playback alone", "[render][bounce]")
{
    const auto state = twoChannels();
    // Two live engines playing the same song; one of them shares its voices
    // with the state the bounce is made from.
    auto shared = vloom::copyWithOwnVoices(state);
    const vloom::SequencerState source = *shared;
    vloom::Engine live;
    live.prepare(kRate, 256);
    live.setMetronomeEnabled(false);
    live.setSequencerState(std::move(shared));
    live.play();
    vloom::Engine reference;
    reference.prepare(kRate, 256);
    reference.setMetronomeEnabled(false);
    reference.setSequencerState(vloom::copyWithOwnVoices(state));
    reference.play();

    vloom::OfflineRenderOptions render;
    render.seconds = 6144.0 / kRate; // whole blocks, so both engines render the same blocks
    render.blockSize = 256;
    auto a = vloom::renderInterleaved(live, render);
    const auto bounced = vloom::bouncePattern(source, 0, nullptr, kRate);
    REQUIRE(bounced.ok);
    const auto rest = vloom::renderInterleaved(live, render);
    a.insert(a.end(), rest.begin(), rest.end());
    render.seconds = 2.0 * 6144.0 / kRate;
    const auto b = vloom::renderInterleaved(reference, render);
    CHECK(a == b);
}

TEST_CASE("A bounce too long for memory fails with an error, not an exception", "[render][bounce]")
{
    const auto state = twoChannels();
    vloom::BounceOptions options;
    options.maxTailSeconds = 0.0;
    // 120 BPM: two beats a second, so 31 minutes is 3720 beats, past the cap.
    const auto longer = static_cast<std::int64_t>(31.0 * 60.0 * 2.0) * vloom::kPpq;
    vloom::BounceResult result;
    REQUIRE_NOTHROW(result = vloom::bounceRange(state, 0, longer, nullptr, kRate, options));
    CHECK_FALSE(result.ok);
    CHECK_FALSE(result.error.empty());
    CHECK(result.audio.empty());

    // Under the cap, but not with its longest tail: refused before rendering.
    const auto almost = static_cast<std::int64_t>(29.0 * 60.0 * 2.0) * vloom::kPpq; // 29 min
    options.maxTailSeconds = 120.0;
    REQUIRE_NOTHROW(result = vloom::bounceRange(state, 0, almost, nullptr, kRate, options));
    CHECK_FALSE(result.ok);
    CHECK_FALSE(result.error.empty());
    CHECK(result.frames == 0); // nothing was rendered
    options.maxTailSeconds = 0.0;

    // A short range still bounces into memory.
    const auto shorter = static_cast<std::int64_t>(2.0 * 2.0) * vloom::kPpq; // 2 s
    result = vloom::bounceRange(state, 0, shorter, nullptr, kRate, options);
    REQUIRE(result.ok);
    CHECK(result.frames == static_cast<std::uint64_t>(2.0 * kRate));
}
