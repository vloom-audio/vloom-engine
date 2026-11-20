// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

#include "vloom/engine/plugin/AudioProcessor.hpp"

#include <vloom/dsp/Sample.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

/// What the built-ins that play samples share (ADR 0008): reading a file
/// into a vloom-dsp SampleBuffer, and the sample paths in their state.
namespace vloom::builtin {

/// Reads `path` (through `resolver`, if set) as a WAV file, mixes more
/// than two channels down to stereo (even channels left, odd right) and
/// makes a dsp::SampleBuffer at the file's own rate: the processor resamples
/// as it plays. Null with the reason in `*error` (may be null) on failure.
/// Never throws. Not for the audio thread.
[[nodiscard]] std::shared_ptr<const dsp::SampleBuffer> readSampleFile(
    const std::string& path, const SampleFileResolver& resolver, std::string* error);

/// The sample paths' part of a built-in's state, after the parameters:
/// "SMP1", a 4-byte count, then per slot with a file its 4-byte index, the
/// 4-byte length of its path and the path (UTF-8), all little endian.
/// Empty paths are left out.
void appendSamplePaths(std::vector<std::uint8_t>& state, std::span<const std::string> paths);

/// Reads appendSamplePaths()' part into `paths` (one per slot, `slots` of
/// them; slots it does not list are empty, unknown slots are skipped).
/// False if it is not such a part, or is cut short or too long.
[[nodiscard]] bool readSamplePaths(
    std::span<const std::uint8_t> part, int slots, std::vector<std::string>& paths);

/// Longest path a state may hold, in bytes.
inline constexpr std::uint32_t kMaxSamplePathBytes = 32768;

} // namespace vloom::builtin
