// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
//
// Replaces the global operator new / delete for the test binary, aligned
// forms included (alignas(64) types such as SpscQueue use them), so tests
// can count allocations. The nothrow forms call these in every standard
// library we build with.
#include "support/Allocations.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <new>
#if defined(_WIN32)
#include <malloc.h>
#endif

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

[[maybe_unused]] void* allocateAligned(std::size_t size, std::align_val_t alignment)
{
    ++allocations;
    const auto align = static_cast<std::size_t>(alignment);
    // aligned_alloc wants a size that is a multiple of the alignment.
    const std::size_t rounded = (std::max<std::size_t>(size, 1) + align - 1) / align * align;
#if defined(_WIN32)
    void* p = _aligned_malloc(rounded, align);
#else
    void* p = std::aligned_alloc(align, rounded);
#endif
    if (p != nullptr)
        return p;
    throw std::bad_alloc();
}

[[maybe_unused]] void freeAligned(void* p) noexcept
{
#if defined(_WIN32)
    _aligned_free(p);
#else
    std::free(p);
#endif
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
void* operator new(std::size_t size, std::align_val_t alignment) { return allocateAligned(size, alignment); }
void* operator new[](std::size_t size, std::align_val_t alignment)
{
    return allocateAligned(size, alignment);
}
void operator delete(void* p, std::align_val_t) noexcept { freeAligned(p); }
void operator delete[](void* p, std::align_val_t) noexcept { freeAligned(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { freeAligned(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { freeAligned(p); }
#endif
