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
    /// Sorts the notes by start (the audio thread searches them), then hands
    /// the state over.
    void publish(std::unique_ptr<SequencerState> state);
    std::size_t collect() { return exchange_.collect(); }
    /// Silences every voice of the current and draining states at once.
    /// Only while the audio thread is stopped (Engine::prepare()).
    void resetVoices() noexcept;
    /// Activates every channel's instrument plugin (current, draining and
    /// pending states) for `sampleRate`. Only while the audio thread is stopped
    /// (Engine::prepare()).
    void activatePlugins(double sampleRate, int maxBlockFrames);

    // ---- audio thread
    /// Picks up a newly published state. Call at the start of every block.
    void beginBlock() noexcept;
    /// Hands states no voice needs any more back for deletion. Call at the end of every block.
    void endBlock() noexcept;

    /// Ticks per step of pattern mode's step grid; 0 when there is nothing
    /// to schedule on it: no state, or song mode (whose clips sit on any
    /// tick, so their steps come with the notes: nextNoteStart()).
    [[nodiscard]] int ticksPerStep() const noexcept;
    /// Pattern mode: plays absolute step `step` (counted from the song
    /// start) of the channels' steps; the pattern loops every stepCount
    /// steps. Song mode: nothing (see startNotes()).
    void triggerStep(std::int64_t step) noexcept;
    /// Playback starts (or jumps) to `ticks`: held notes are released (their
    /// ends belong to the old position). Song mode: the audio clips already
    /// under way there start part-way through, `framesPerTick` converting
    /// the distance; clips starting less than a frame before `ticks`, or
    /// later, are left to startNotes(). The channels of the audio clips fade out what they
    /// were playing first.
    void resumeAt(double ticks, double framesPerTick) noexcept;

    /// The sample an audio clip plays (its own, else its channel's); null: none.
    [[nodiscard]] static const SampleBuffer* clipSample(
        const SongAudioClip& clip, const ChannelState& channel) noexcept;
    /// An audio clip's playback speed (1 when its rate is out of range).
    [[nodiscard]] static double clipRate(const SongAudioClip& clip) noexcept;
    static constexpr std::int64_t kNoEvent = INT64_MAX;
    /// The furthest tick anything plays at (Transport::kMaxTicks): clips
    /// reaching further are ignored, notes are held at most this long.
    static constexpr std::int64_t kMaxTicks = 100'000'000'000;
    /// The first tick at or after `from` (counted from the song start) where
    /// a note starts; in song mode also where a pattern clip's step or an
    /// audio clip starts. kNoEvent if none.
    [[nodiscard]] std::int64_t nextNoteStart(std::int64_t from) const noexcept;
    /// Starts the notes starting at `tick`, each held until endNotes()
    /// reaches its end. Song mode: also the pattern clips' steps on `tick`
    /// and the audio clips starting there; `framesPerTick` (the tempo now)
    /// places an audio clip's offset in its sample.
    void startNotes(std::int64_t tick, double framesPerTick = 0.0) noexcept;
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
    /// Once per block, after render(): runs each channel's instrument plugin
    /// over the block (with the notes it got, `transport` as at the block's
    /// start), then adds each channel's buffer, with its volume and pan
    /// ramped from the last block, into the stereo bus of its insert
    /// (`buses[2 * insert]`, `buses[2 * insert + 1]`, insert 0 = the master
    /// bus) and clears the buffer. Marks the buses it wrote in `used`.
    void mix(int numFrames, float* const* buses, bool* used, const TransportInfo& transport = {}) noexcept;
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
    /// Audio thread: the frame of the block events fire at now (where the
    /// next render() starts), for the note events of instrument plugins.
    int eventFrame_ = 0;
    /// Plays one step of `channel`: its sample, or on a synth or plugin its
    /// root key until tick `end`.
    void playStep(const ChannelState& channel, std::int64_t end) const noexcept;
    /// The tick an audio clip stops at (kNoEvent: the sample's end).
    [[nodiscard]] std::int64_t audioClipEnd(const SongAudioClip& clip) const noexcept;
    /// Starts `note` on `channel`, ending at `end` ticks.
    void playNote(const ChannelState& channel, const Note& note, std::int64_t end) const noexcept;
    /// The audio clip's channel, if it can sound.
    [[nodiscard]] const ChannelState* clipChannel(const SongAudioClip& clip) const noexcept;
};

} // namespace vloom
