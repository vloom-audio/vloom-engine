// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include <vloom/engine/core/SpscQueue.hpp>

#include <catch2/catch_test_macros.hpp>

#include <thread>

TEST_CASE("SpscQueue keeps FIFO order", "[core]")
{
    vloom::SpscQueue<int, 8> q;
    for (int i = 0; i < 5; ++i)
        REQUIRE(q.push(i));

    int v = -1;
    for (int i = 0; i < 5; ++i) {
        REQUIRE(q.pop(v));
        CHECK(v == i);
    }
    CHECK_FALSE(q.pop(v));
    CHECK(q.empty());
}

TEST_CASE("SpscQueue reports full", "[core]")
{
    vloom::SpscQueue<int, 4> q;
    STATIC_REQUIRE(decltype(q)::capacity() == 3);
    CHECK(q.push(1));
    CHECK(q.push(2));
    CHECK(q.push(3));
    CHECK_FALSE(q.push(4));
}

TEST_CASE("SpscQueue survives a producer and consumer thread", "[core]")
{
    constexpr int kCount = 100000;
    vloom::SpscQueue<int, 64> q;

    std::thread producer([&] {
        for (int i = 0; i < kCount;)
            if (q.push(i))
                ++i;
    });

    int expected = 0;
    bool ordered = true;
    while (expected < kCount) {
        int v = 0;
        if (q.pop(v)) {
            ordered = ordered && (v == expected);
            ++expected;
        }
    }
    producer.join();
    CHECK(ordered);
}
