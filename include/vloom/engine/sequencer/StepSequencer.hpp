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
    /// Playback starts (or jumps) to `ticks`: held notes are released (their
    /// ends belong to the old position). Song mode: the audio clips already
    /// under way there start part-way through, `framesPerTick` converting
    /// the distance; clips starting exactly at `ticks` are left to
    /// triggerStep(). The channels of the audio clips fade out what they
    /// were playing first.
    void resumeAt(double ticks, double framesPerTick) noexcept;

    static constexpr std::int64_t kNoEvent = INT64_MAX;
    /// The first tick at or after `from` (counted from the song start, like
    /// steps) where a note starts; kNoEvent if none.
    [[nodiscard]] std::int64_t nextNoteStart(std::int64_t from) const noexcept;
    /// Starts the notes starting at `tick`. Each is held until endNotes()
    /// reaches its end.
    void startNotes(std::int64_t tick) noexcept;
    /// The earliest tick a sounding note ends at; kNoEvent if none.
    [[nodiscard]] std::int64_t nextNoteEnd() const noexcept;
    /// Releases the notes ending at or before `upTo`.
    void endNotes(std::int64_t upTo) noexcept;
    /// Speed factor of everything but audio clips (the master pitch), live;
    /// 1 = none.
    void setPitch(double ratio) noexcept { pitch_ = ratio; }
    /// The output rate, for the synth. Call while audio is stopped.
    void setSampleRate(double sampleRate) noexcept { sampleRate_ = sampleRate; }
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
    double pitch_ = 1.0; ///< audio thread
    double sampleRate_ = 48000.0;
    /// Plays one step of `channel` (absolute `step`): its sample, or on a
    /// synth its root key for one step.
    void playStep(const ChannelState& channel, std::int64_t step) const noexcept;
    /// Calls `visit(channel, note, absoluteStart)` for every playable note of
    /// the current state that starts at or after `from`: in pattern mode the
    /// next occurrence of each note, in song mode each note of each clip.
    template <typename Visit> void forEachNote(std::int64_t from, Visit&& visit) const noexcept;
    /// Starts `note` on `channel`, ending at `end` ticks.
    void playNote(const ChannelState& channel, const Note& note, std::int64_t end) const noexcept;
    /// The audio clip's channel, if it can sound.
    [[nodiscard]] const ChannelState* clipChannel(const SongAudioClip& clip) const noexcept;
};

} // namespace vloom
