// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

#include "vloom/engine/mixer/MixerState.hpp"
#include "vloom/engine/render/SongExport.hpp"
#include "vloom/engine/sequencer/SequencerState.hpp"

#include <cstdint>
#include <functional>
#include <span>
#include <string>

namespace vloom::detail {

/// Where renderSong() puts what it renders. Each returns false, with an
/// error, to stop the render.
struct SongRenderOutput {
    /// Before the first block, once the job is known to be good:
    /// `bodyFrames` is the range's length, the tail not included. Optional.
    std::function<bool(std::uint64_t bodyFrames, std::string* error)> begin;
    /// Each block, interleaved (options.channels wide). Required.
    std::function<bool(std::span<const float> block, std::string* error)> write;
    /// After the last block. Optional.
    std::function<bool(std::string* error)> finish;
};

/// What exportSong() and the bounces (render/Bounce.hpp) share: renders
/// `song` from options.startTicks to options.endTicks (and its tail) with an
/// Engine and voices of its own, leaving out the delay compensation's
/// lead-in. result.frames counts the frames handed to `output`.
SongExportResult renderSong(const SequencerState& song, const MixerState* mixer, double sampleRate,
    const SongExportOptions& options, const SongRenderOutput& output, const SongExportProgress& progress);

} // namespace vloom::detail
