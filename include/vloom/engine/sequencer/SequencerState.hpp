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

/// Immutable snapshot of the channel rack, published with
/// Engine::setSequencerState(). Build a new one for every edit; samples and
/// voices are shared between snapshots, so copies are cheap.
struct SequencerState {
    std::vector<ChannelState> channels;
    int stepCount = 16; ///< pattern length in steps; the pattern loops
    int ticksPerStep = kPpq / 4; ///< 16th notes
};

} // namespace vloom
