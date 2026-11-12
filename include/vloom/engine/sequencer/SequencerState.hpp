// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

#include "vloom/engine/dsp/Sampler.hpp"
#include "vloom/engine/sample/SampleBuffer.hpp"
#include "vloom/engine/time/MusicalTime.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace vloom {

/// Playback state of one channel that outlives snapshots (ADR 0005): every
/// SequencerState that contains the channel points at the same object, so
/// edits never cut a ringing note. Create it on the control thread when the
/// channel is created; afterwards only the audio thread touches it.
class ChannelVoices {
public:
    explicit ChannelVoices(int polyphony = Sampler::kDefaultVoices)
        : sampler(polyphony)
    {
    }

    Sampler sampler;
    std::uint64_t renderedPass = 0; ///< audio thread: avoids rendering it twice in one pass
};

/// One channel of the channel rack, as the audio thread sees it.
struct ChannelState {
    std::shared_ptr<const SampleBuffer> sample; ///< at the engine's sample rate (sample/SampleLoader)
    std::shared_ptr<ChannelVoices> voices; ///< required to sound; keep it across edits
    float gain = 1.0f; ///< linear, applied to notes as they start
    float pan = 0.0f; ///< -1 .. +1, applied to notes as they start
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
