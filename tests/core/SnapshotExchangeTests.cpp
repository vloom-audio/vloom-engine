// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "support/Allocations.hpp"

#include <vloom/engine/core/SnapshotExchange.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <memory>
#include <thread>

namespace {

/// Counts live instances, to prove every snapshot is deleted exactly once.
struct Snap {
    static inline std::atomic<int> alive{0};
    int value;
    explicit Snap(int v)
        : value(v)
    {
        ++alive;
    }
    ~Snap() { --alive; }
    Snap(const Snap&) = delete;
    Snap& operator=(const Snap&) = delete;
};

std::unique_ptr<const Snap> snap(int v) { return std::make_unique<const Snap>(v); }

} // namespace

TEST_CASE("The audio side picks up what the control side publishes", "[core]")
{
    {
        vloom::SnapshotExchange<Snap> exchange;
        CHECK(exchange.acquire() == nullptr);

        exchange.publish(snap(1));
        const Snap* first = exchange.acquire();
        REQUIRE(first != nullptr);
        CHECK(first->value == 1);
        CHECK(exchange.acquire() == first); // stays until a new one comes
        CHECK(exchange.drainingCount() == 0);

        exchange.publish(snap(2));
        CHECK(exchange.acquire()->value == 2);
        CHECK(exchange.drainingCount() == 1); // the first one drains
        CHECK(Snap::alive == 2);
    }
    CHECK(Snap::alive == 0); // the destructor frees all
}

TEST_CASE("The latest publish wins; skipped snapshots are deleted on the control side", "[core]")
{
    vloom::SnapshotExchange<Snap> exchange;
    exchange.publish(snap(1));
    exchange.publish(snap(2));
    exchange.publish(snap(3));
    CHECK(Snap::alive == 1);
    CHECK(exchange.acquire()->value == 3);
}

TEST_CASE("Draining snapshots are deleted only once released and collected", "[core]")
{
    vloom::SnapshotExchange<Snap> exchange;
    exchange.publish(snap(1));
    exchange.acquire();
    exchange.publish(snap(2));
    exchange.acquire();

    int seen = 0;
    exchange.forEachDraining([&](const Snap& s) { seen = s.value; });
    CHECK(seen == 1);

    exchange.release([](const Snap&) { return false; }); // still in use
    CHECK(exchange.drainingCount() == 1);
    CHECK(exchange.collect() == 0);

    exchange.release([](const Snap& s) { return s.value == 1; });
    CHECK(exchange.drainingCount() == 0);
    CHECK(Snap::alive == 2); // retired, not yet deleted: that is the control side's job
    CHECK(exchange.collect() == 1);
    CHECK(Snap::alive == 1);
}

TEST_CASE("A full draining list holds new snapshots back", "[core]")
{
    vloom::SnapshotExchange<Snap, 2> exchange;
    for (int i = 1; i <= 3; ++i) {
        exchange.publish(snap(i));
        exchange.acquire();
    }
    CHECK(exchange.drainingCount() == 2);
    exchange.publish(snap(4));
    CHECK(exchange.acquire()->value == 3); // 4 waits
    exchange.release([](const Snap&) { return true; });
    CHECK(exchange.acquire()->value == 4);
}

TEST_CASE("The audio side does not allocate or free", "[core]")
{
    vloom::SnapshotExchange<Snap> exchange;
    exchange.publish(snap(1));
    exchange.acquire();
    exchange.publish(snap(2));
    const int aliveBefore = Snap::alive;

    const vloom::test::AllocationCounter counter;
    exchange.acquire();
    exchange.release([](const Snap&) { return true; });
    CHECK(counter.count() == 0);
    CHECK(Snap::alive == aliveBefore); // nothing deleted on this side
}

TEST_CASE("Snapshots cross threads intact and are all freed", "[core]")
{
    // Under TSan (CI) this also checks the memory ordering.
    constexpr int kSnapshots = 20000;
    {
        vloom::SnapshotExchange<Snap> exchange;
        std::atomic<bool> done{false};
        std::atomic<bool> ordered{true};
        std::atomic<int> lastSeen{0}; ///< written by the audio thread only

        std::thread audio([&] {
            int last = 0;
            while (!done.load(std::memory_order_acquire)) {
                if (const Snap* s = exchange.acquire()) {
                    if (s->value < last)
                        ordered = false;
                    last = s->value;
                    lastSeen.store(last, std::memory_order_release);
                }
                exchange.release([](const Snap&) { return true; });
            }
        });
        for (int i = 1; i <= kSnapshots; ++i)
            exchange.publish(snap(i));
        // Wait until the audio side has picked up the last one.
        while (lastSeen.load(std::memory_order_acquire) != kSnapshots)
            exchange.collect();
        done = true;
        audio.join();
        exchange.collect();
        CHECK(ordered.load());
    }
    CHECK(Snap::alive == 0);
}
