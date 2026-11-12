// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "support/AudioAssertions.hpp"

#include <vloom/engine/Engine.hpp>
#include <vloom/engine/render/OfflineRenderer.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace {

std::vector<float> renderMetronome(double bpm, double seconds, int blockSize, bool play = true)
{
    vloom::Engine engine;
    engine.prepare(48000.0, blockSize);
    engine.setTempo(bpm);
    if (play)
        engine.play();

    vloom::OfflineRenderOptions opts;
    opts.seconds = seconds;
    opts.blockSize = blockSize;
    opts.channels = 2;
    return vloom::renderInterleaved(engine, opts);
}

} // namespace

TEST_CASE("Metronome clicks land exactly on beats, whatever the block size", "[engine]")
{
    const int blockSize = GENERATE(64, 256, 333, 512, 4096);
    const auto audio = renderMetronome(120.0, 2.0, blockSize);

    // 120 BPM at 48 kHz -> one beat every 24000 frames.
    const std::vector<std::size_t> expected{0, 24000, 48000, 72000};
    CHECK(vloom::test::findOnsets(audio, 2, 1000) == expected);
}

TEST_CASE("Rendering is deterministic", "[engine]")
{
    CHECK(renderMetronome(133.0, 1.5, 256) == renderMetronome(133.0, 1.5, 256));
}

TEST_CASE("A stopped engine outputs silence", "[engine]")
{
    CHECK(vloom::test::isSilent(renderMetronome(120.0, 1.0, 512, false)));
}

namespace {

/// Peak of a one-block render at `gain`, after one block to settle the ramp.
float peakAtGain(float gain)
{
    vloom::Engine engine;
    engine.prepare(48000.0, 512);
    engine.play();
    std::vector<float> l(512), r(512);
    float* out[] = {l.data(), r.data()};
    engine.setMasterGain(gain);
    engine.process(out, 2, 512); // ramps from 1 to `gain`
    engine.locate(0.0);
    engine.process(out, 2, 512); // click at frame 0, at the steady gain
    float peak = 0.0f;
    for (float s : l)
        peak = std::max(peak, std::abs(s));
    return peak;
}

} // namespace

TEST_CASE("Master gain scales the output", "[engine]")
{
    const float unity = peakAtGain(1.0f);
    REQUIRE(unity > 0.0f);
    CHECK(peakAtGain(0.5f) == Catch::Approx(unity * 0.5f).epsilon(1e-6));
    CHECK(peakAtGain(0.0f) == 0.0f);
}

TEST_CASE("Master gain is clamped and ignores NaN", "[engine]")
{
    const float unity = peakAtGain(1.0f);
    CHECK(peakAtGain(100.0f) == Catch::Approx(unity * vloom::Engine::kMaxMasterGain).epsilon(1e-6));
    CHECK(peakAtGain(-1.0f) == 0.0f);
    CHECK(peakAtGain(std::numeric_limits<float>::quiet_NaN()) == Catch::Approx(unity).epsilon(1e-6));
}

TEST_CASE("Master gain changes ramp instead of jumping", "[engine]")
{
    vloom::Engine engine;
    engine.prepare(48000.0, 512);
    engine.play();
    std::vector<float> l(512), r(512);
    float* out[] = {l.data(), r.data()};
    engine.process(out, 2, 512); // click at frame 0 at unity gain; keep its tail ringing
    engine.locate(0.0);
    engine.setMasterGain(0.0f);
    engine.process(out, 2, 512); // new click at frame 0, faded towards 0 over the block
    CHECK(std::abs(l.front()) <= 1.0f);
    CHECK(l.back() == 0.0f); // reaches the target on the last frame
}

namespace {

/// Peaks of left and right for one settled click at `pan`.
std::pair<float, float> stereoPeaksAtPan(float pan)
{
    vloom::Engine engine;
    engine.prepare(48000.0, 512);
    engine.play();
    std::vector<float> l(512), r(512);
    float* out[] = {l.data(), r.data()};
    engine.setMasterPan(pan);
    engine.process(out, 2, 512); // ramps to the new balance
    engine.locate(0.0);
    engine.process(out, 2, 512);
    float pl = 0.0f;
    float pr = 0.0f;
    for (int i = 0; i < 512; ++i) {
        pl = std::max(pl, std::abs(l[static_cast<std::size_t>(i)]));
        pr = std::max(pr, std::abs(r[static_cast<std::size_t>(i)]));
    }
    return {pl, pr};
}

} // namespace

TEST_CASE("Master pan balances left and right", "[engine]")
{
    const auto [centreL, centreR] = stereoPeaksAtPan(0.0f);
    REQUIRE(centreL > 0.0f);
    CHECK(centreL == Catch::Approx(centreR));

    const auto [leftL, leftR] = stereoPeaksAtPan(-1.0f);
    CHECK(leftL == Catch::Approx(centreL)); // the kept side stays at full gain
    CHECK(leftR == 0.0f);

    const auto [halfL, halfR] = stereoPeaksAtPan(0.5f);
    CHECK(halfL == Catch::Approx(centreL * 0.5f).epsilon(1e-6));
    CHECK(halfR == Catch::Approx(centreR));

    const auto [nanL, nanR] = stereoPeaksAtPan(std::numeric_limits<float>::quiet_NaN());
    CHECK(nanL == Catch::Approx(centreL));
    CHECK(nanR == Catch::Approx(centreR));
}

TEST_CASE("Output peaks are held until taken", "[engine]")
{
    vloom::Engine engine;
    engine.prepare(48000.0, 512);
    std::vector<float> l(512), r(512);
    float* out[] = {l.data(), r.data()};

    engine.process(out, 2, 512); // stopped: silence
    CHECK(engine.takeOutputPeak(0) == 0.0f);

    engine.play();
    engine.process(out, 2, 512); // click on frame 0
    engine.process(out, 2, 512); // quieter tail must not lower the held peak
    float expected = 0.0f;
    for (float s : l)
        expected = std::max(expected, std::abs(s));
    const float peak = engine.takeOutputPeak(0);
    CHECK(peak > 0.0f);
    CHECK(peak >= expected);
    CHECK(engine.takeOutputPeak(0) == 0.0f); // taken
    CHECK(engine.takeOutputPeak(1) > 0.0f);
    CHECK(engine.takeOutputPeak(2) == 0.0f); // not metered
    CHECK(engine.takeOutputPeak(-1) == 0.0f);
}

TEST_CASE("CPU load is reported after processing", "[engine]")
{
    vloom::Engine engine;
    engine.prepare(48000.0, 512);
    CHECK(engine.cpuLoad() == 0.0f);
    std::vector<float> l(512), r(512);
    float* out[] = {l.data(), r.data()};
    engine.play();
    for (int i = 0; i < 8; ++i)
        engine.process(out, 2, 512);
    CHECK(engine.cpuLoad() > 0.0f);
    CHECK(engine.cpuLoad() < 1.0f); // a metronome is far below real time
}

TEST_CASE("Status is published after each block", "[engine]")
{
    vloom::Engine engine;
    engine.prepare(48000.0, 512);
    engine.setTempo(90.0);
    engine.play();

    std::vector<float> l(512), r(512);
    float* out[] = {l.data(), r.data()};
    engine.process(out, 2, 512);

    CHECK(engine.isPlaying());
    CHECK(engine.tempo() == 90.0);
    CHECK(engine.positionTicks() > 0.0);

    engine.stop();
    const double pos = engine.positionTicks();
    engine.process(out, 2, 512);
    CHECK_FALSE(engine.isPlaying());
    CHECK(engine.positionTicks() == pos);
}

TEST_CASE("Non-finite tempo and positions are ignored", "[engine]")
{
    // Review finding: NaN reached the scheduler and hung the audio thread.
    vloom::Engine engine;
    engine.prepare(48000.0, 256);
    engine.setTempo(130.0);
    engine.setTempo(std::numeric_limits<double>::quiet_NaN());
    engine.setTempo(std::numeric_limits<double>::infinity());
    engine.locate(std::numeric_limits<double>::quiet_NaN());
    engine.locate(std::numeric_limits<double>::infinity());
    engine.play();

    vloom::OfflineRenderOptions opts;
    opts.seconds = 0.1;
    opts.blockSize = 256;
    vloom::renderInterleaved(engine, opts); // must return
    CHECK(engine.tempo() == 130.0);
    CHECK(std::isfinite(engine.positionTicks()));
    CHECK(engine.positionTicks() < vloom::kPpq);

    // Absurdly far positions are clamped, never overflowing the scheduler.
    engine.locate(1e30);
    vloom::renderInterleaved(engine, opts);
    CHECK(std::isfinite(engine.positionTicks()));
}
