// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include <vloom/engine/time/Transport.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using Catch::Approx;

TEST_CASE("Transport advances one beat per 60/bpm seconds", "[time]")
{
    vloom::Transport t;
    t.prepare(48000.0);
    t.setTempo(120.0);
    t.play();

    t.advance(24000); // 0.5 s at 120 BPM = one quarter note
    CHECK(t.positionTicks() == Approx(vloom::kPpq));
}

TEST_CASE("Transport does not move while stopped", "[time]")
{
    vloom::Transport t;
    t.prepare(44100.0);
    t.advance(44100);
    CHECK(t.positionTicks() == 0.0);
}

TEST_CASE("Tempo changes keep the current position", "[time]")
{
    vloom::Transport t;
    t.prepare(48000.0);
    t.setTempo(120.0);
    t.play();
    t.advance(24000);

    t.setTempo(60.0);
    CHECK(t.positionTicks() == Approx(vloom::kPpq));
    t.advance(48000); // 1 s at 60 BPM = one more beat
    CHECK(t.positionTicks() == Approx(2.0 * vloom::kPpq));
}

TEST_CASE("Tempo is clamped to a sane range", "[time]")
{
    vloom::Transport t;
    t.setTempo(0.0);
    CHECK(t.tempo() > 0.0);
    t.setTempo(100000.0);
    CHECK(t.tempo() < 1000.0);
}
