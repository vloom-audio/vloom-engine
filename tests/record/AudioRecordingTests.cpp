// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "support/Allocations.hpp"

#include <vloom/engine/Engine.hpp>
#include <vloom/engine/core/SpscRing.hpp>
#include <vloom/engine/io/AudioInput.hpp>
#include <vloom/engine/record/AudioTake.hpp>
#include <vloom/engine/sample/WavReader.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

using Catch::Approx;

namespace {

constexpr double kRate = 48000.0;
constexpr int kBlock = 256;
// 120 BPM at 48 kHz: 0.04 ticks a frame (kPpq = 960), 24000 frames a beat.
constexpr double kTicksPerFrame = 0.04;

struct TempFolder {
    std::filesystem::path path;
    TempFolder()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() / ("vloom_record_" + std::to_string(stamp));
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

/// An engine with nothing to play: what comes out is the input monitored.
struct Rig {
    vloom::Engine engine;
    std::vector<float> left = std::vector<float>(kBlock);
    std::vector<float> right = std::vector<float>(kBlock);
    std::array<float*, 2> out{left.data(), right.data()};
    std::vector<float> inLeft = std::vector<float>(kBlock);
    std::vector<float> inRight = std::vector<float>(kBlock);

    Rig()
    {
        engine.prepare(kRate, kBlock);
        engine.setTempo(120.0);
        engine.setMetronomeEnabled(false);
    }

    /// One block of input at `l` / `r` in, one block out.
    void block(float l, float r)
    {
        std::fill(inLeft.begin(), inLeft.end(), l);
        std::fill(inRight.begin(), inRight.end(), r);
        engine.pushAudioInput(inLeft.data(), inRight.data(), kBlock);
        engine.process(out.data(), 2, kBlock);
    }
    void silentBlock() { engine.process(out.data(), 2, kBlock); }
};

} // namespace

TEST_CASE("A ring of floats passes what one thread writes to another, in order", "[core][record]")
{
    vloom::SpscRing ring(5); // rounded up to 8
    CHECK(ring.capacity() == 8);
    const std::array<float, 6> data{1, 2, 3, 4, 5, 6};
    CHECK(ring.write(data.data(), 6) == 6);
    CHECK(ring.write(data.data(), 6) == 2); // only two more fit
    CHECK(ring.available() == 8);
    std::array<float, 8> out{};
    CHECK(ring.read(out.data(), 3) == 3);
    CHECK(out[2] == 3.0f);
    CHECK(ring.skip(2) == 2);
    CHECK(ring.read(out.data(), 8) == 3);
    CHECK(out[0] == 6.0f);
    CHECK(out[1] == 1.0f);
    CHECK(out[2] == 2.0f);
    CHECK(ring.available() == 0);
    CHECK(ring.space() == 8);
}

TEST_CASE("The input can be heard through an insert, and a mono input on both sides", "[engine][record]")
{
    Rig rig;
    rig.block(0.25f, -0.25f); // not monitored: nothing comes out
    CHECK(rig.left[100] == 0.0f);

    rig.engine.setInputMonitor(0, 1.0f);
    rig.block(0.25f, -0.25f); // fades in over the block
    CHECK(rig.left[kBlock - 1] == Approx(0.25f));
    rig.block(0.25f, -0.25f);
    CHECK(rig.left[10] == Approx(0.25f));
    CHECK(rig.right[10] == Approx(-0.25f));

    // Mono: the one channel on both sides.
    std::fill(rig.inLeft.begin(), rig.inLeft.end(), 0.5f);
    rig.engine.pushAudioInput(rig.inLeft.data(), nullptr, kBlock);
    rig.engine.process(rig.out.data(), 2, kBlock);
    CHECK(rig.left[10] == Approx(0.5f));
    CHECK(rig.right[10] == Approx(0.5f));

    // Nothing came in for a block: silence, counted as a dropout.
    const auto before = rig.engine.inputDropouts();
    rig.silentBlock();
    CHECK(rig.left[10] == 0.0f);
    CHECK(rig.engine.inputDropouts() == before + 1);

    rig.engine.setInputMonitor(-1);
    rig.block(0.25f, 0.25f);
    rig.block(0.25f, 0.25f);
    CHECK(rig.left[10] == 0.0f);
}

TEST_CASE("Recording writes the input to a file and places it where the song was", "[engine][record]")
{
    TempFolder folder;
    Rig rig;
    rig.engine.setAudioRecording(true);
    CHECK(rig.engine.isRecordingAudio());
    rig.block(0.9f, 0.9f); // stopped: not recorded
    rig.engine.play();
    for (int b = 0; b < 40; ++b)
        rig.block(0.01f * static_cast<float>(b + 1), -0.01f * static_cast<float>(b + 1));
    rig.engine.stop();
    rig.block(0.9f, 0.9f);

    vloom::AudioTake take;
    std::string error;
    REQUIRE(take.open(folder.file("take.wav"), 48000, &error));
    REQUIRE(take.finish(rig.engine, &error));
    CHECK(take.frames() == 40 * kBlock);
    const auto segments = take.segments();
    REQUIRE(segments.size() == 1);
    CHECK(segments[0].firstFrame == 0);
    CHECK(segments[0].frames == 40 * kBlock);
    CHECK(segments[0].startTicks == Approx(0.0));
    CHECK(segments[0].ticksPerFrame == Approx(kTicksPerFrame));
    CHECK(segments[0].newTake);

    const auto file = vloom::readWavFile(folder.file("take.wav"));
    REQUIRE(file.buffer.has_value());
    REQUIRE(file.buffer->channels() == 2);
    REQUIRE(file.buffer->frames() == 40 * kBlock);
    CHECK(file.buffer->channel(0)[5 * kBlock + 10] == Approx(0.06f));
    CHECK(file.buffer->channel(1)[5 * kBlock + 10] == Approx(-0.06f));
}

TEST_CASE("A recording is moved back by the round trip and the input waiting", "[engine][record]")
{
    TempFolder folder;
    Rig rig;
    rig.engine.setAudioRecordLatency(512);
    rig.engine.play();
    for (int b = 0; b < 10; ++b)
        rig.block(0.0f, 0.0f); // 2560 frames: 102.4 ticks
    // One block more waits in the engine from now on.
    std::fill(rig.inLeft.begin(), rig.inLeft.end(), 0.0f);
    rig.engine.pushAudioInput(rig.inLeft.data(), rig.inRight.data(), kBlock);
    rig.engine.setAudioRecording(true);
    for (int b = 0; b < 10; ++b)
        rig.block(0.1f, 0.1f);
    rig.engine.stop();
    rig.silentBlock();

    vloom::AudioTake take;
    REQUIRE(take.open(folder.file("late.wav"), 48000));
    REQUIRE(take.finish(rig.engine));
    const auto segments = take.segments();
    REQUIRE(segments.size() == 1);
    CHECK(take.marks()[0].shiftFrames == 512 + kBlock);
    CHECK(segments[0].startTicks == Approx(102.4 - (512 + kBlock) * kTicksPerFrame));
    CHECK(segments[0].frames == 10 * kBlock);
}

TEST_CASE("Recording in a loop gives each pass a stretch of its own", "[engine][record]")
{
    TempFolder folder;
    Rig rig;
    rig.engine.setLoop(0.0, 960.0); // 24000 frames
    rig.engine.setAudioRecordLatency(1000);
    rig.engine.setAudioRecording(true);
    rig.engine.play();
    for (int b = 0; b < 120; ++b) // 30720 frames
        rig.block(0.2f, 0.2f);
    rig.engine.stop();
    rig.silentBlock();

    vloom::AudioTake take;
    REQUIRE(take.open(folder.file("loop.wav"), 48000));
    REQUIRE(take.finish(rig.engine));
    const auto segments = take.segments();
    REQUIRE(segments.size() == 2);
    // The first 1000 frames were heard before the song's start: left out.
    CHECK(segments[0].firstFrame == 1000);
    CHECK(segments[0].startTicks == Approx(0.0));
    CHECK(segments[0].frames == 24000);
    CHECK(segments[0].pass == 0);
    // The jump back shows in the input 1000 frames after the engine made it.
    CHECK(segments[1].firstFrame == 25000);
    CHECK(segments[1].startTicks == Approx(0.0));
    CHECK(segments[1].frames == 30720 - 25000);
    CHECK(segments[1].pass == 1);
    CHECK_FALSE(segments[1].newTake);
}

TEST_CASE("Starting the song again, or moving it, starts a new stretch", "[engine][record]")
{
    TempFolder folder;
    Rig rig;
    rig.engine.setAudioRecording(true);
    rig.engine.play();
    for (int b = 0; b < 10; ++b)
        rig.block(0.1f, 0.1f);
    rig.engine.locate(9600.0);
    for (int b = 0; b < 10; ++b)
        rig.block(0.2f, 0.2f);
    rig.engine.stop();
    rig.silentBlock();
    rig.engine.locate(0.0);
    rig.engine.play();
    for (int b = 0; b < 5; ++b)
        rig.block(0.3f, 0.3f);
    rig.engine.stop();
    rig.silentBlock();

    vloom::AudioTake take;
    REQUIRE(take.open(folder.file("takes.wav"), 48000));
    REQUIRE(take.finish(rig.engine));
    const auto segments = take.segments();
    REQUIRE(segments.size() == 3);
    CHECK(segments[0].newTake);
    CHECK(segments[0].frames == 10 * kBlock);
    CHECK_FALSE(segments[1].newTake);
    CHECK(segments[1].firstFrame == 10 * kBlock);
    CHECK(segments[1].startTicks == Approx(9600.0));
    CHECK(segments[2].newTake);
    CHECK(segments[2].firstFrame == 20 * kBlock);
    CHECK(segments[2].startTicks == Approx(0.0));
    CHECK(segments[2].frames == 5 * kBlock);
}

TEST_CASE("A recording not taken in time loses blocks, and goes on in the right place", "[engine][record]")
{
    TempFolder folder;
    Rig rig;
    rig.engine.setAudioRecording(true);
    rig.engine.play();
    const int fits = static_cast<int>(vloom::Engine::kRecordRingFrames) / kBlock;
    for (int b = 0; b < fits + 50; ++b)
        rig.block(0.1f, 0.1f);
    CHECK(rig.engine.recordedAudioLost() == 50 * kBlock);

    vloom::AudioTake take;
    REQUIRE(take.open(folder.file("lost.wav"), 48000));
    REQUIRE(take.collect(rig.engine));
    rig.block(0.1f, 0.1f); // fits again
    rig.engine.stop();
    rig.silentBlock();
    REQUIRE(take.finish(rig.engine));
    const auto segments = take.segments();
    REQUIRE(segments.size() == 2);
    CHECK(segments[1].firstFrame == fits * kBlock);
    CHECK(segments[1].startTicks == Approx(static_cast<double>((fits + 50) * kBlock) * kTicksPerFrame));
}

TEST_CASE("Taking input, monitoring and recording it do not allocate", "[engine][record]")
{
    Rig rig;
    rig.engine.setInputMonitor(0, 0.5f);
    rig.engine.setLoop(0.0, 960.0);
    rig.engine.setAudioRecording(true);
    rig.engine.play();
    std::vector<float> drain(2 * 4096);
    std::array<vloom::AudioRecordMark, 16> marks{};
    long long allocations = 0;
    for (int b = 0; b < 300; ++b) {
        std::fill(rig.inLeft.begin(), rig.inLeft.end(), 0.1f);
        const vloom::test::AllocationCounter counter;
        rig.engine.pushAudioInput(rig.inLeft.data(), b % 2 == 0 ? rig.inRight.data() : nullptr, kBlock);
        rig.engine.process(rig.out.data(), 2, kBlock);
        allocations += counter.count();
        rig.engine.takeRecordedAudio(drain);
        rig.engine.takeAudioMarks(marks);
    }
    CHECK(allocations == 0);
}

TEST_CASE("An audio input that is not open reports nothing and refuses to start", "[io][record]")
{
    // No device is opened here: the tests never touch a microphone.
    vloom::AudioInput input;
    CHECK_FALSE(input.isOpen());
    CHECK_FALSE(input.isRunning());
    CHECK(input.sampleRate() == 0);
    CHECK(input.channels() == 0);
    CHECK(input.deviceName().empty());
    input.stop();
    input.close();
    std::string error;
    CHECK_FALSE(input.start(&error));
    CHECK_FALSE(error.empty());
    error.clear();
    CHECK_FALSE(input.open(vloom::AudioInputConfig{}, nullptr, &error));
    CHECK_FALSE(error.empty());
    vloom::AudioInputConfig far;
    far.deviceIndex = 10000;
    error.clear();
    CHECK_FALSE(input.open(far, [](const float* const*, int, int) { }, &error));
    CHECK_FALSE(error.empty());
}

TEST_CASE("Switching the monitor off or to another insert fades it, without a click", "[engine][record]")
{
    Rig rig;
    rig.engine.setInputMonitor(0, 1.0f);
    rig.block(0.25f, 0.25f);
    rig.block(0.25f, 0.25f);
    rig.engine.setInputMonitor(-1);
    rig.block(0.25f, 0.25f);
    CHECK(rig.left[0] > 0.24f); // from where it was ...
    CHECK(rig.left[kBlock - 1] == Approx(0.0f).margin(1e-6)); // ... down to nothing over the block
    rig.block(0.25f, 0.25f);
    CHECK(rig.left[0] == 0.0f);
}

TEST_CASE("A tempo change while recording starts a stretch at the new tempo", "[engine][record]")
{
    TempFolder folder;
    Rig rig;
    rig.engine.setAudioRecording(true);
    rig.engine.play();
    for (int b = 0; b < 10; ++b) // 102.4 ticks
        rig.block(0.1f, 0.1f);
    rig.engine.setTempo(60.0);
    for (int b = 0; b < 10; ++b)
        rig.block(0.1f, 0.1f);
    rig.engine.stop();
    rig.silentBlock();

    vloom::AudioTake take;
    REQUIRE(take.open(folder.file("tempo.wav"), 48000));
    REQUIRE(take.finish(rig.engine));
    const auto segments = take.segments();
    REQUIRE(segments.size() == 2);
    CHECK(segments[0].ticksPerFrame == Approx(kTicksPerFrame));
    CHECK(segments[1].firstFrame == 10 * kBlock);
    CHECK(segments[1].startTicks == Approx(102.4));
    CHECK(segments[1].ticksPerFrame == Approx(kTicksPerFrame / 2.0));
}
