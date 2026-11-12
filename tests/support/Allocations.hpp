// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

namespace vloom::test {

/// Calls to the global operator new (and new[]) made by the calling thread
/// since it started. The test binary replaces operator new to count them.
[[nodiscard]] long long allocationsOnThisThread() noexcept;

/// False where operator new cannot be replaced (AddressSanitizer on Windows
/// brings its own): then nothing is counted and counts stay 0.
[[nodiscard]] bool allocationCountingEnabled() noexcept;

/// Counts the heap allocations made on this thread while it lives: use it
/// to prove that real-time code does not allocate.
///
///     vloom::test::AllocationCounter counter;
///     sampler.render(...);
///     CHECK(counter.count() == 0);
class AllocationCounter {
public:
    [[nodiscard]] long long count() const noexcept { return allocationsOnThisThread() - start_; }

private:
    long long start_ = allocationsOnThisThread();
};

} // namespace vloom::test
