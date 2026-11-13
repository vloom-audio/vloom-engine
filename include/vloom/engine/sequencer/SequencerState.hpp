// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

#include "vloom/engine/dsp/Sampler.hpp"
#include "vloom/engine/dsp/Synth.hpp"
#include "vloom/engine/mixer/MixerState.hpp"
#include "vloom/engine/sample/SampleBuffer.hpp"
#include "vloom/engine/time/MusicalTime.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

namespace vloom {

/// Playback state of one channel that outlives snapshots (ADR 0005): every
/// SequencerState that contains the channel points at the same object, so
/// edits never cut a ringing note. Create it on the control thread when the
/// channel is created (its buffers are allocated here); afterwards only the
/// audio thread touches it.
class ChannelVoices {
public:
    explicit ChannelVoices(int polyphony = Sampler::kDefaultVoices)
        : sampler(polyphony)
        , synth(polyphony)
        , left(kMaxBlockFrames, 0.0f)
        , right(kMaxBlockFrames, 0.0f)
    {
    }

    Sampler sampler;
    Synth synth;

    // Both instruments at once: a channel switched from one to the other
    // lets the first one's notes ring out.
    [[nodiscard]] int activeVoices() const noexcept { return sampler.activeVoices() + synth.activeVoices(); }
    [[nodiscard]] std::int64_t nextRelease() const noexcept
    {
        return std::min(sampler.nextRelease(), synth.nextRelease());
    }
    void release(std::int64_t upTo) noexcept
    {
        sampler.release(upTo);
        synth.release(upTo);
    }
    void releaseHeld() noexcept
    {
        sampler.releaseHeld();
        synth.releaseHeld();
    }
    void stopAll() noexcept
    {
        sampler.stopAll();
        synth.stopAll();
    }

    // ---- audio thread
    std::uint64_t renderedPass = 0; ///< avoids rendering it twice in one pass
    std::uint64_t mixedBlock = 0; ///< avoids mixing it twice in one block
    std::vector<float> left, right; ///< the channel's signal in the block being rendered; zero otherwise
    bool written = false; ///< something was rendered into left / right this block
    bool rampStarted = false;
    StereoGain applied{}; ///< gain at the end of the last block: the next ramp starts there
};

/// A piano-roll note. Times are in ticks (kPpq per quarter note) from the
/// start of its pattern.
struct Note {
    static constexpr int kMinKey = 0;
    static constexpr int kMaxKey = 127;
    static constexpr int kMiddleC = 60; ///< MIDI note 60 (C5 in FL Studio's naming)

    std::int64_t start = 0; ///< from 0 to the pattern's length (excluded)
    std::int64_t length = 0; ///< > 0; may reach past the pattern's end
    int key = kMiddleC; ///< MIDI note number, kMinKey .. kMaxKey
    float velocity = 100.0f / 128.0f; ///< 0 .. 1; scales the note's gain linearly
};

/// What a channel plays its steps and notes with.
enum class Instrument : std::uint8_t {
    Sampler, ///< its sample (Sampler)
    Synth, ///< the built-in synth (Synth), with ChannelState::synth
};

/// One channel of the channel rack, as the audio thread sees it.
struct ChannelState {
    Instrument instrument = Instrument::Sampler;
    /// Instrument::Synth: its sound. A step plays rootKey for one step.
    SynthSettings synth;
    std::shared_ptr<const SampleBuffer> sample; ///< at the engine's sample rate (sample/SampleLoader)
    std::shared_ptr<ChannelVoices> voices; ///< required to sound; keep it across edits
    float gain = 1.0f; ///< channel volume, linear 0 .. 4; changes ramp over one block, ringing notes too
    float pan = 0.0f; ///< balance -1 .. +1, live like the gain
    int insert = 0; ///< mixer insert it plays into, 1 .. kMaxInserts; 0 (or out of range) = the master
    bool muted = false; ///< no new notes; ringing ones finish
    std::vector<std::uint8_t> steps; ///< non-zero = the step plays; shorter than stepCount = off
    /// Pattern mode's piano roll, any order. A note is held for its length,
    /// then released; it plays the sample at its key relative to rootKey
    /// (one semitone = a 2^(1/12) speed change). Steps play at rootKey.
    /// Notes out of range (start, length, key) are skipped.
    std::vector<Note> notes;
    int rootKey = Note::kMiddleC; ///< the key the sample plays unchanged at
};

/// One pattern for song mode, [channel][...]. A channel missing here, or a
/// step past the end of its row, is off.
struct SongPattern {
    std::vector<std::vector<std::uint8_t>> steps; ///< non-zero = plays
    std::vector<std::vector<Note>> notes; ///< like ChannelState::notes
};

/// A pattern placed in the playlist: plays its steps and notes from
/// `startStep` for `lengthSteps` steps (steps counted from the song start).
/// Notes starting inside the clip play for their whole length.
struct SongPatternClip {
    int pattern = 0; ///< index into SequencerState::patterns
    std::int64_t startStep = 0;
    std::int64_t lengthSteps = 0;
};

/// A channel's sample placed in the playlist (FL Studio's audio clip): plays
/// once from `startStep`, at the sample's own speed (tempo does not stretch
/// it), through the channel like a step would: its volume, pan, mute and
/// mixer insert.
struct SongAudioClip {
    int channel = 0; ///< index into SequencerState::channels; its sample plays
    std::int64_t startStep = 0;
};

/// Immutable snapshot of the channel rack, published with
/// Engine::setSequencerState(). Build a new one for every edit; samples and
/// voices are shared between snapshots, so copies are cheap.
///
/// Pattern mode (song = false) loops the channels' own `steps` every
/// stepCount steps. Song mode plays the playlist instead: the pattern clips
/// trigger the channels with their pattern's steps, the audio clips play
/// their samples; nothing loops.
struct SequencerState {
    std::vector<ChannelState> channels;
    int stepCount = 16; ///< pattern length in steps; the pattern loops (pattern mode)
    int ticksPerStep = kPpq / 4; ///< 16th notes

    bool song = false;
    std::vector<SongPattern> patterns; ///< song mode
    std::vector<SongPatternClip> patternClips; ///< song mode
    std::vector<SongAudioClip> audioClips; ///< song mode
};

} // namespace vloom
