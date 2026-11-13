// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

#include "vloom/engine/dsp/Sampler.hpp"
#include "vloom/engine/mixer/MixerState.hpp"
#include "vloom/engine/sample/SampleBuffer.hpp"
#include "vloom/engine/time/MusicalTime.hpp"

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
        , left(kMaxBlockFrames, 0.0f)
        , right(kMaxBlockFrames, 0.0f)
    {
    }

    Sampler sampler;

    // ---- audio thread
    std::uint64_t renderedPass = 0; ///< avoids rendering it twice in one pass
    std::uint64_t mixedBlock = 0; ///< avoids mixing it twice in one block
    std::vector<float> left, right; ///< the channel's signal in the block being rendered; zero otherwise
    bool written = false; ///< something was rendered into left / right this block
    bool rampStarted = false;
    StereoGain applied{}; ///< gain at the end of the last block: the next ramp starts there
};

/// One channel of the channel rack, as the audio thread sees it.
struct ChannelState {
    std::shared_ptr<const SampleBuffer> sample; ///< at the engine's sample rate (sample/SampleLoader)
    std::shared_ptr<ChannelVoices> voices; ///< required to sound; keep it across edits
    float gain = 1.0f; ///< channel volume, linear 0 .. 4; changes ramp over one block, ringing notes too
    float pan = 0.0f; ///< balance -1 .. +1, live like the gain
    int insert = 0; ///< mixer insert it plays into, 1 .. kMaxInserts; 0 (or out of range) = the master
    bool muted = false; ///< no new notes; ringing ones finish
    std::vector<std::uint8_t> steps; ///< non-zero = the step plays; shorter than stepCount = off
};

/// One pattern's steps for song mode: [channel][step], non-zero = plays.
/// A channel missing here, or a step past the end of its row, is off.
struct SongPattern {
    std::vector<std::vector<std::uint8_t>> steps;
};

/// A pattern placed in the playlist: plays its steps from `startStep` for
/// `lengthSteps` steps (steps counted from the song start).
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
