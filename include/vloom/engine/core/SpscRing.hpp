// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <vector>

namespace vloom {

/// A lock-free ring of floats between one writer thread and one reader
/// thread: audio that a device captures for the engine, or that the engine
/// records for the disk. Wait-free on both sides; only the constructor
/// allocates.
class SpscRing {
public:
    /// Room for `capacity` floats, rounded up to a power of two.
    explicit SpscRing(std::size_t capacity)
        : buffer_(roundUp(capacity), 0.0f)
        , mask_(buffer_.size() - 1)
    {
    }
    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    [[nodiscard]] std::size_t capacity() const noexcept { return buffer_.size(); }

    /// Writer: floats that can be written now.
    [[nodiscard]] std::size_t space() const noexcept
    {
        return buffer_.size()
            - (head_.load(std::memory_order_relaxed) - tail_.load(std::memory_order_acquire));
    }
    /// Writer: writes up to `count` floats; returns how many fitted.
    std::size_t write(const float* data, std::size_t count) noexcept
    {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t room = buffer_.size() - (head - tail_.load(std::memory_order_acquire));
        count = std::min(count, room);
        for (std::size_t i = 0; i < count; ++i)
            buffer_[(head + i) & mask_] = data[i];
        head_.store(head + count, std::memory_order_release);
        return count;
    }

    /// Reader: floats waiting.
    [[nodiscard]] std::size_t available() const noexcept
    {
        return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_relaxed);
    }
    /// Reader: reads up to `count` floats; returns how many there were.
    std::size_t read(float* out, std::size_t count) noexcept
    {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        count = std::min(count, head_.load(std::memory_order_acquire) - tail);
        for (std::size_t i = 0; i < count; ++i)
            out[i] = buffer_[(tail + i) & mask_];
        tail_.store(tail + count, std::memory_order_release);
        return count;
    }
    /// Reader: drops up to `count` floats; returns how many.
    std::size_t skip(std::size_t count) noexcept
    {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        count = std::min(count, head_.load(std::memory_order_acquire) - tail);
        tail_.store(tail + count, std::memory_order_release);
        return count;
    }

private:
    static std::size_t roundUp(std::size_t capacity) noexcept
    {
        std::size_t size = 2;
        while (size < capacity)
            size *= 2;
        return size;
    }

    std::vector<float> buffer_;
    std::size_t mask_;
    // Counters that only grow (wrapping is harmless: only differences are used).
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
};

} // namespace vloom
