// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>

namespace vloom {

/// Wait-free single-producer / single-consumer ring buffer.
///
/// One thread may call push(), one other thread may call pop(). Neither call
/// allocates, locks or blocks, so pop() is safe on the real-time audio thread.
/// Capacity must be a power of two; one slot is kept free, so the queue holds
/// at most Capacity - 1 items.
template <typename T, std::size_t Capacity> class SpscQueue {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>, "SpscQueue items must be trivially copyable");

public:
    /// Producer side. Returns false when the queue is full.
    bool push(const T& item) noexcept
    {
        const auto head = head_.load(std::memory_order_relaxed);
        const auto next = (head + 1) & kMask;
        if (next == tail_.load(std::memory_order_acquire))
            return false;
        slots_[head] = item;
        head_.store(next, std::memory_order_release);
        return true;
    }

    /// Consumer side. Returns false when the queue is empty.
    bool pop(T& out) noexcept
    {
        const auto tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire))
            return false;
        out = slots_[tail];
        tail_.store((tail + 1) & kMask, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return tail_.load(std::memory_order_acquire) == head_.load(std::memory_order_acquire);
    }

    static constexpr std::size_t capacity() noexcept { return Capacity - 1; }

private:
    static constexpr std::size_t kMask = Capacity - 1;
    static constexpr std::size_t kCacheLine = 64;

    alignas(kCacheLine) std::atomic<std::size_t> head_{0}; // written by producer
    alignas(kCacheLine) std::atomic<std::size_t> tail_{0}; // written by consumer
    alignas(kCacheLine) std::array<T, Capacity> slots_{};
};

} // namespace vloom
