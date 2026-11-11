// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include <vloom/engine/time/MusicalTime.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("toBarBeatTick starts at 1.1.0", "[time]")
{
    const auto bbt = vloom::toBarBeatTick(0.0, 4);
    CHECK(bbt.bar == 1);
    CHECK(bbt.beat == 1);
    CHECK(bbt.tick == 0);
}

TEST_CASE("toBarBeatTick splits ticks into bars and beats", "[time]")
{
    const auto bbt = vloom::toBarBeatTick(5.0 * vloom::kPpq + 12.0, 4);
    CHECK(bbt.bar == 2);
    CHECK(bbt.beat == 2);
    CHECK(bbt.tick == 12);
}

TEST_CASE("toBarBeatTick respects the time signature", "[time]")
{
    const auto bbt = vloom::toBarBeatTick(3.0 * vloom::kPpq, 3);
    CHECK(bbt.bar == 2);
    CHECK(bbt.beat == 1);
}

TEST_CASE("toBarBeatTick clamps negative positions", "[time]")
{
    const auto bbt = vloom::toBarBeatTick(-100.0, 4);
    CHECK(bbt.bar == 1);
    CHECK(bbt.tick == 0);
}
