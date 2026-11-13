// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "support/Allocations.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
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

TEST_CASE("The allocation counter sees aligned operator new too", "[support]")
{
    // Audit finding: alignas(64) types (SpscQueue) went through the library's
    // aligned new, which the counter did not replace.
    if (!vloom::test::allocationCountingEnabled())
        SKIP("operator new is not replaceable in this build (AddressSanitizer on Windows)");
    const vloom::test::AllocationCounter counter;
    void* one = ::operator new(10, std::align_val_t{64});
    CHECK(reinterpret_cast<std::uintptr_t>(one) % 64 == 0);
    void* many = ::operator new[](100, std::align_val_t{128});
    CHECK(reinterpret_cast<std::uintptr_t>(many) % 128 == 0);
    CHECK(counter.count() == 2);
    ::operator delete(one, std::align_val_t{64});
    ::operator delete[](many, std::align_val_t{128});
}
