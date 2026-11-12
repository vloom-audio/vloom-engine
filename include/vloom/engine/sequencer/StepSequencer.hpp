// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

#include "vloom/engine/core/SnapshotExchange.hpp"
#include "vloom/engine/sequencer/SequencerState.hpp"

#include <cstdint>
#include <memory>

namespace vloom {

/// Plays the channel rack: triggers each channel's sample on its active steps
/// and renders the voices. The Engine owns one and schedules the steps
/// sample-accurately; this class does not know about time.
///
/// Control thread: publish(), collect(). Audio thread: everything else, all
/// real-time safe.
class StepSequencer {
public:
    // ---- control thread
    void publish(std::unique_ptr<const SequencerState> state) { exchange_.publish(std::move(state)); }
    std::size_t collect() { return exchange_.collect(); }

    // ---- audio thread
    /// Picks up a newly published state. Call at the start of every block.
    void beginBlock() noexcept;
    /// Hands states no voice needs any more back for deletion. Call at the end of every block.
    void endBlock() noexcept;

    /// Ticks per step of the current state; 0 when there is none (nothing to schedule).
    [[nodiscard]] int ticksPerStep() const noexcept;
    /// Starts the notes of absolute step `step` (counted from the song
    /// start; the pattern loops every stepCount steps).
    void triggerStep(std::int64_t step) noexcept;
    /// Adds every channel's voices into out[ch][startFrame .. +numFrames),
    /// including channels already removed whose notes are still fading.
    void render(float* const* out, int numChannels, int startFrame, int numFrames) noexcept;
    /// Fades out every note (transport stop).
    void stopAll() noexcept;

private:
    [[nodiscard]] bool inCurrent(const ChannelVoices* voices) const noexcept;
    [[nodiscard]] bool canRelease(const SequencerState& old) const noexcept;

    SnapshotExchange<SequencerState> exchange_;
    const SequencerState* current_ = nullptr; ///< audio thread
    std::uint64_t pass_ = 0; ///< audio thread: render() calls so far
};

} // namespace vloom
