// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "support/Allocations.hpp"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <vector>

// The "does not allocate" tests are only worth something if the counter sees
// allocations at all.
TEST_CASE("The allocation counter sees new and vector growth", "[support]")
{
    if (!vloom::test::allocationCountingEnabled())
        SKIP("operator new is not replaceable in this build (AddressSanitizer on Windows)");
    const vloom::test::AllocationCounter counter;
    CHECK(counter.count() == 0);
    auto box = std::make_unique<int>(1);
    const long long afterNew = counter.count();
    CHECK(afterNew == 1);
    std::vector<int> v;
    v.push_back(*box);
    // Not an exact number: MSVC debug builds also allocate an iterator proxy.
    CHECK(counter.count() > afterNew);
}
