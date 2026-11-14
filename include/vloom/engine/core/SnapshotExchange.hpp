// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

#include "vloom/engine/core/SpscQueue.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <memory>

namespace vloom {

/// Hands immutable snapshots of state (channels, patterns, samples...) from
/// one control thread to the audio thread, and old ones back for deletion,
/// without locks or allocation on the audio thread (docs/adr/0005).
///
///  control thread                     audio thread
///  publish(new) --> pending -------->  acquire(): becomes current;
///                                      the old current starts draining
///  collect(): deletes  <-- retired <--  release(done): draining snapshots
///                                      no sound refers to any more
///
/// "Draining" covers the time a replaced snapshot may still be needed: a
/// voice can still be playing a sample only the old snapshot owns. The audio
/// thread decides with release() when that is over.
///
/// Only the control thread deletes snapshots, so T's destructor (freeing
/// samples, vectors...) never runs on the audio thread.
template <typename T, std::size_t MaxDraining = 16, std::size_t RetiredCapacity = 64> class SnapshotExchange {
public:
    SnapshotExchange() = default;
    /// Deletes every snapshot. Only while no audio thread uses the exchange.
    ~SnapshotExchange()
    {
        delete pending_.load(std::memory_order_acquire);
        delete current_;
        for (std::size_t i = 0; i < drainingCount_; ++i)
            delete draining_[i];
        collect();
    }
    SnapshotExchange(const SnapshotExchange&) = delete;
    SnapshotExchange& operator=(const SnapshotExchange&) = delete;

    // ---- control thread ---------------------------------------------------

    /// Makes `next` the state the audio thread picks up at its next acquire().
    /// A snapshot published earlier but not picked up yet is deleted: the
    /// latest one wins.
    void publish(std::unique_ptr<const T> next)
    {
        delete pending_.exchange(next.release(), std::memory_order_acq_rel);
        collect();
    }

    /// Calls `fn(const T&)` for every snapshot the audio thread may play:
    /// the current one, the draining ones and the one published but not
    /// picked up yet. Only while the audio thread is stopped (e.g. to
    /// prepare what they hold for a new sample rate).
    template <typename Fn> void forEachSnapshot(Fn&& fn) const
    {
        if (current_ != nullptr)
            fn(*current_);
        forEachDraining(fn);
        if (const T* next = pending_.load(std::memory_order_acquire))
            fn(*next);
    }

    /// Deletes the snapshots the audio thread has finished with. Call it
    /// regularly (publish() does too). Returns how many were deleted.
    std::size_t collect()
    {
        std::size_t count = 0;
        const T* done = nullptr;
        while (retired_.pop(done)) {
            delete done;
            ++count;
        }
        return count;
    }

    // ---- audio thread -----------------------------------------------------

    /// Picks up a newly published snapshot, if any, and returns the current
    /// one (null before the first). The replaced snapshot starts draining.
    /// While MaxDraining snapshots are draining, a new one waits.
    const T* acquire() noexcept
    {
        if (drainingCount_ < MaxDraining) {
            if (const T* next = pending_.exchange(nullptr, std::memory_order_acq_rel)) {
                if (current_ != nullptr)
                    draining_[drainingCount_++] = current_;
                current_ = next;
            }
        }
        return current_;
    }

    [[nodiscard]] const T* current() const noexcept { return current_; }
    [[nodiscard]] std::size_t drainingCount() const noexcept { return drainingCount_; }

    /// Calls `fn(const T&)` for every draining snapshot.
    template <typename Fn> void forEachDraining(Fn&& fn) const
    {
        for (std::size_t i = 0; i < drainingCount_; ++i)
            fn(*draining_[i]);
    }

    /// Hands every draining snapshot for which `done(const T&)` is true back
    /// to the control thread. One that does not fit in the retired queue
    /// stays draining until the next call.
    template <typename Done> void release(Done&& done)
    {
        for (std::size_t i = 0; i < drainingCount_;) {
            if (done(*draining_[i]) && retired_.push(draining_[i])) {
                draining_[i] = draining_[--drainingCount_];
                draining_[drainingCount_] = nullptr;
            } else {
                ++i;
            }
        }
    }

private:
    std::atomic<const T*> pending_{nullptr};
    const T* current_ = nullptr; ///< audio thread
    std::array<const T*, MaxDraining> draining_{}; ///< audio thread
    std::size_t drainingCount_ = 0; ///< audio thread
    SpscQueue<const T*, RetiredCapacity> retired_; ///< audio thread -> control thread
};

} // namespace vloom
