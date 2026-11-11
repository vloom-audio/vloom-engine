// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "support/AudioAssertions.hpp"

#include <vloom/engine/Engine.hpp>
#include <vloom/engine/render/OfflineRenderer.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

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
