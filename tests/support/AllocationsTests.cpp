// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "support/Allocations.hpp"

#include <catch2/catch_test_macros.hpp>

#include <new>

// The "does not allocate" tests are only worth something if the counter sees
// allocations at all.
//
// The allocation functions are called directly: optimisers may remove a
// new-expression whose memory never escapes (allowed since C++14, and GCC
// and Clang do at -O2), but not a call to ::operator new itself.
TEST_CASE("The allocation counter sees operator new and new[]", "[support]")
{
    if (!vloom::test::allocationCountingEnabled())
        SKIP("operator new is not replaceable in this build (AddressSanitizer on Windows)");
    const vloom::test::AllocationCounter counter;
    CHECK(counter.count() == 0);

    void* one = ::operator new(16);
    CHECK(counter.count() == 1);
    void* many = ::operator new[](64);
    CHECK(counter.count() == 2);

    ::operator delete(one);
    ::operator delete[](many);
    CHECK(counter.count() == 2); // freeing is not counted
}
