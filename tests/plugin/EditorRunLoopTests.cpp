// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "plugin/EditorRunLoop.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

using vloom::plugin_editor::RunLoop;

TEST_CASE("An editor run loop fires due timers with their ids", "[plugin][editor]")
{
    RunLoop loop;
    std::vector<std::uint32_t> fired;
    const std::uint32_t now
        = loop.addTimer(std::chrono::milliseconds{0}, [&](std::uint32_t id) { fired.push_back(id); });
    const std::uint32_t later
        = loop.addTimer(std::chrono::hours{1}, [&](std::uint32_t id) { fired.push_back(id); });
    REQUIRE(now != 0);
    REQUIRE(later != 0);
    CHECK(now != later);

    loop.run();
    loop.run();
    CHECK(fired == std::vector<std::uint32_t>{now, now}); // the hourly one is not due

    CHECK(loop.removeTimer(now));
    CHECK_FALSE(loop.removeTimer(now));
    loop.run();
    CHECK(fired.size() == 2);
    CHECK(loop.addTimer(std::chrono::milliseconds{0}, {}) == 0); // no callback
}

TEST_CASE("An editor run loop lets callbacks remove timers, and caps entries", "[plugin][editor]")
{
    RunLoop loop;
    int first = 0;
    int second = 0;
    std::uint32_t secondId = 0;
    loop.addTimer(std::chrono::milliseconds{0}, [&](std::uint32_t self) {
        ++first;
        loop.removeTimer(self);
        loop.removeTimer(secondId); // removes another one mid-run
    });
    secondId = loop.addTimer(std::chrono::milliseconds{0}, [&](std::uint32_t) { ++second; });
    loop.run();
    CHECK(first == 1);
    CHECK(second == 0);
    CHECK(loop.empty());

    for (std::size_t i = 0; i < RunLoop::kMaxEntries; ++i)
        REQUIRE(loop.addTimer(std::chrono::seconds{1}, [](std::uint32_t) { }) != 0);
    CHECK(loop.addTimer(std::chrono::seconds{1}, [](std::uint32_t) { }) == 0);
}

#if !defined(_WIN32)
TEST_CASE("An editor run loop calls file descriptor handlers when they are ready", "[plugin][editor]")
{
    int ends[2] = {-1, -1};
    REQUIRE(::pipe(ends) == 0);
    RunLoop loop;
    std::vector<std::uint32_t> ready;
    REQUIRE(loop.addFd(ends[0], RunLoop::kRead, [&](std::uint32_t what) { ready.push_back(what); }));
    CHECK_FALSE(loop.addFd(ends[0], RunLoop::kRead, [](std::uint32_t) { })); // once per descriptor

    loop.run();
    CHECK(ready.empty()); // nothing to read yet
    const char byte = 1;
    REQUIRE(::write(ends[1], &byte, 1) == 1);
    loop.run();
    REQUIRE(ready.size() == 1);
    CHECK((ready[0] & RunLoop::kRead) != 0);

    CHECK(loop.modifyFd(ends[0], 0)); // no longer interested
    ready.clear();
    loop.run();
    CHECK(ready.empty());
    CHECK(loop.removeFd(ends[0]));
    CHECK_FALSE(loop.removeFd(ends[0]));
    ::close(ends[0]);
    ::close(ends[1]);
}
#endif
