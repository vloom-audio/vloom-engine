// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

#include "vloom/engine/core/SnapshotExchange.hpp"
#include "vloom/engine/sequencer/SequencerState.hpp"

#include <cstdint>
#include <memory>

namespace vloom {

/// Plays the channel rack: triggers each channel's sample on its active steps,
/// renders each channel into its own buffer, then mixes it with the
/// channel's volume and pan into the bus of its mixer insert. The Engine owns
/// one and schedules the steps sample-accurately; this class does not know
/// about time.
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
    /// start). Pattern mode: the pattern loops every stepCount steps. Song
    /// mode: the pattern clips covering the step, and the audio clips that
    /// start on it.
    void triggerStep(std::int64_t step) noexcept;
    /// Song mode: playback starts (or jumps) to `ticks`, between steps or on
    /// one: the audio clips already under way there start part-way through,
    /// `framesPerTick` converting the distance. Clips starting exactly at
    /// `ticks` are left to triggerStep(). The channels of the audio clips
    /// fade out what they were playing first.
    void resumeAt(double ticks, double framesPerTick) noexcept;
    /// Renders every channel's voices into its own buffer at
    /// [startFrame, startFrame + numFrames), including channels already
    /// removed whose notes are still fading.
    void render(int startFrame, int numFrames) noexcept;
    /// Once per block, after render(): adds each channel's buffer, with its
    /// volume and pan ramped from the last block, into the stereo bus of its
    /// insert (`buses[2 * insert]`, `buses[2 * insert + 1]`, insert 0 = the
    /// master bus) and clears the buffer. Marks the buses it wrote in `used`.
    void mix(int numFrames, float* const* buses, bool* used) noexcept;
    /// Fades out every note and audio clip (transport stop).
    void stopAll() noexcept;

private:
    [[nodiscard]] bool inCurrent(const ChannelVoices* voices) const noexcept;
    [[nodiscard]] bool sampleInCurrent(const SampleBuffer* sample) const noexcept;
    /// True when any sampler the audio thread still renders (current or
    /// draining states) plays `sample`.
    [[nodiscard]] bool samplePlaying(const SampleBuffer* sample) const noexcept;
    [[nodiscard]] bool canRelease(const SequencerState& old) const noexcept;

    SnapshotExchange<SequencerState> exchange_;
    const SequencerState* current_ = nullptr; ///< audio thread
    std::uint64_t pass_ = 0; ///< audio thread: render() calls so far
    std::uint64_t block_ = 0; ///< audio thread: mix() calls so far
    /// The audio clip's channel, if it can sound.
    [[nodiscard]] const ChannelState* clipChannel(const SongAudioClip& clip) const noexcept;
};

} // namespace vloom
