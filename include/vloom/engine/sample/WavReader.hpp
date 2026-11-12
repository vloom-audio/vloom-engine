// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#pragma once

#include "vloom/engine/sample/SampleBuffer.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace vloom {

/// Bounds for files from outside (sample packs, downloads): anything larger is
/// rejected before memory is reserved for it.
struct WavReadLimits {
    std::uint64_t maxFileBytes = 1ULL << 30; ///< 1 GiB
    std::int64_t maxFrames = 192000LL * 60 * 20; ///< 20 minutes at 192 kHz
    int maxChannels = 8;
    /// Memory the decoded float samples may take (8-bit files grow 4x).
    std::uint64_t maxDecodedBytes = 1ULL << 30; ///< 1 GiB
};

struct WavReadResult {
    std::optional<SampleBuffer> buffer; ///< set on success
    std::string error; ///< set on failure, for the user
};

/// Decodes a RIFF/WAVE file held in memory.
///
/// Supported: PCM 8 (unsigned), 16, 24 and 32-bit integers, 32 and 64-bit
/// IEEE float, plain or WAVE_FORMAT_EXTENSIBLE, 1 to maxChannels channels.
///
/// Built for untrusted input: every chunk is bounds-checked against the
/// actual data, only the `fmt ` and `data` chunks are read (metadata such as
/// LIST, bext or cue is skipped unparsed), sizes are checked against
/// `limits` before allocating, sample rates must be 1 kHz .. 768 kHz, NaN /
/// infinite float samples become 0 and float samples are clamped to +/-1024.
/// A data chunk whose declared size runs past the end of the file (an
/// unfinished recording) is read up to the last whole frame.
[[nodiscard]] WavReadResult decodeWav(std::span<const std::uint8_t> bytes, const WavReadLimits& limits = {});

/// Reads and decodes a WAV file at `path` (UTF-8). Not for the audio thread.
[[nodiscard]] WavReadResult readWavFile(const std::string& path, const WavReadLimits& limits = {});

} // namespace vloom
