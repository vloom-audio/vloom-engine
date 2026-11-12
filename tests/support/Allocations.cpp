// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
//
// Replaces the global (unaligned) operator new / delete for the test binary
// so tests can count allocations. Aligned new keeps the library version.
#include "support/Allocations.hpp"

#include <cstddef>
#include <cstdlib>
#include <new>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define VLOOM_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#define VLOOM_ASAN 1
#endif
// Windows ASan defines operator new itself; elsewhere ours takes precedence.
#if defined(_WIN32) && defined(VLOOM_ASAN)
#define VLOOM_COUNT_ALLOCATIONS 0
#else
#define VLOOM_COUNT_ALLOCATIONS 1
#endif

namespace {
thread_local long long allocations = 0;

[[maybe_unused]] void* allocate(std::size_t size)
{
    ++allocations;
    if (void* p = std::malloc(size == 0 ? 1 : size))
        return p;
    throw std::bad_alloc();
}
} // namespace

long long vloom::test::allocationsOnThisThread() noexcept { return allocations; }
bool vloom::test::allocationCountingEnabled() noexcept { return VLOOM_COUNT_ALLOCATIONS != 0; }

#if VLOOM_COUNT_ALLOCATIONS
void* operator new(std::size_t size) { return allocate(size); }
void* operator new[](std::size_t size) { return allocate(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
#endif
